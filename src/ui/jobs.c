/* Long running work: a worker thread behind a progress dialog. */
#include "app.h"

/* ---- jobs: a worker thread behind a progress dialog -------------------------------------------- */

typedef struct {
    JobFn fn;
    JobDone done;
    gpointer data;
    GDestroyNotify free_data;
    GPtrArray *notes;
    AdwDialog *dialog;
    GtkWidget *bar, *label;
    gboolean ok;
    GError *error;
    gint refs;         /* the job itself plus every progress message still on its way to the window */
    gboolean finished; /* set in the window thread: the dialog is gone, progress messages are dropped */
} Job;

typedef struct {
    Job *job;
    double frac;
    char *what;
} ProgressMsg;

static void job_unref(Job *j)
{
    if (!g_atomic_int_dec_and_test(&j->refs))
        return;
    g_ptr_array_free(j->notes, TRUE);
    g_clear_error(&j->error);
    g_free(j);
}

static gboolean progress_idle(gpointer p)
{
    ProgressMsg *m = p;

    /* a message can arrive after the job is over and its dialog destroyed */
    if (!m->job->finished) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m->job->bar), m->frac);
        gtk_label_set_text(GTK_LABEL(m->job->label), m->what);
    }
    job_unref(m->job);
    g_free(m->what);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static gboolean job_progress(guint64 done, guint64 total, const char *what, gpointer p)
{
    ProgressMsg *m = g_new0(ProgressMsg, 1);

    m->job = p;
    g_atomic_int_inc(&m->job->refs);
    m->frac = total ? (double)done / total : 0;
    m->what = g_strdup(what);
    g_idle_add(progress_idle, m);
    return TRUE;
}

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Job *j = data;

    (void)src;
    (void)c;
    j->ok = j->fn(j->data, job_progress, j, j->notes, &j->error);
    g_task_return_boolean(task, j->ok);
}

static void job_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    Job *j = data;
    GString *msg = g_string_new(NULL);
    guint i;

    (void)src;
    (void)res;
    j->finished = TRUE;
    adw_dialog_force_close(j->dialog);
    if (!j->ok && j->error != NULL)
        show_error(_("That did not work"), j->error->message);
    else if (j->notes->len > 0) {
        for (i = 0; i < j->notes->len; i++)
            g_string_append_printf(msg, "%s%s", i ? "\n" : "", (char *)g_ptr_array_index(j->notes, i));
        show_error(_("Done, with a few notes"), msg->str);
    }
    g_string_free(msg, TRUE);
    if (j->done)
        j->done(j->ok, j->data);
    if (j->free_data)
        j->free_data(j->data);
    active_jobs--;
    job_unref(j);
}

void run_job(const char *title, JobFn fn, gpointer data, GDestroyNotify free_data, JobDone done)
{
    Job *j;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GTask *task;

    /* an install is writing to the disk: a second tool on the same disk could damage it, so wait for the transfers */
    if (transfers_busy()) {
        toast(_("Wait for the transfers to finish first"));
        if (free_data)
            free_data(data);
        return;
    }
    j = g_new0(Job, 1);

    j->refs = 1;
    j->fn = fn;
    j->data = data;
    j->free_data = free_data;
    j->done = done;
    j->notes = g_ptr_array_new_with_free_func(g_free);
    j->dialog = adw_dialog_new();
    j->bar = gtk_progress_bar_new();
    j->label = gtk_label_new(_("Starting..."));
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    {
        GtkWidget *t = gtk_label_new(title);

        gtk_widget_add_css_class(t, "title-3");
        gtk_box_append(GTK_BOX(box), t);
    }
    gtk_box_append(GTK_BOX(box), j->bar);
    gtk_box_append(GTK_BOX(box), j->label);
    adw_dialog_set_child(j->dialog, box);
    adw_dialog_set_content_width(j->dialog, 420);
    adw_dialog_set_can_close(j->dialog, FALSE);
    adw_dialog_present(j->dialog, GTK_WIDGET(A.win));

    active_jobs++;
    task = g_task_new(NULL, NULL, job_finished, j);
    g_task_set_task_data(task, j, NULL);
    g_task_run_in_thread(task, job_thread);
    g_object_unref(task);
}
