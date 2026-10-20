/*
 * Transfers: installing a game takes minutes, so it runs in the background, one at a time, in a queue.
 * The Transfers page lists what is waiting, what is running (with its progress) and what is finished; meanwhile the
 * rest of the program stays usable, except what would touch the disk at the same time (see transfers_busy).
 */
#include "app.h"

typedef enum { TR_QUEUED, TR_RUNNING, TR_DONE, TR_FAILED, TR_CANCELLED } TrState;

typedef struct {
    char *kind, *title, *id;   /* id: the game's ID, to find its cover once it is installed */
    JobFn fn;
    JobDone done;
    gpointer data;
    GDestroyNotify free_data;
    GPtrArray *notes;
    GError *error;
    TrState state;
    gboolean ok;
    gint cancel_req;       /* set by the Cancel button, read by the worker thread */
    gint refs;             /* the list plus every progress message still on its way to the window */
    double frac;
    char *what, *finished_at;
    GtkWidget *row, *thumb, *bar, *status, *pct, *cancel_btn, *details_btn, *remove_btn;
} Transfer;

typedef struct {
    Transfer *t;
    double frac;
    char *what;
    gboolean cancelable;
} Msg;

static GPtrArray *list;             /* Transfer, in the order they were added */
static Transfer *running;
static guint batch_ok, batch_failed; /* finished since the queue was last empty */
static GtkWidget *listbox, *empty_box, *scroll, *sub_label, *clear_btn;

static void refresh_view(void);

static void transfer_unref(Transfer *t)
{
    if (!g_atomic_int_dec_and_test(&t->refs))
        return;
    g_ptr_array_free(t->notes, TRUE);
    g_clear_error(&t->error);
    g_free(t->kind);
    g_free(t->title);
    g_free(t->id);
    g_free(t->what);
    g_free(t->finished_at);
    g_free(t);
}

static guint count_state(gboolean active)
{
    guint i, n = 0;

    for (i = 0; list && i < list->len; i++) {
        TrState s = ((Transfer *)g_ptr_array_index(list, i))->state;

        if ((s == TR_QUEUED || s == TR_RUNNING) == active)
            n++;
    }
    return n;
}

/* while something is queued or running, nothing else may write to or read from the disk */
gboolean transfers_busy(void)
{
    return count_state(TRUE) > 0;
}

guint transfers_count(void)
{
    return count_state(TRUE);
}

/* "Title 43%" of the running transfer, for the status bar; NULL if none */
char *transfers_summary(void)
{
    guint waiting = count_state(TRUE);

    if (waiting == 0)
        return NULL;
    if (running != NULL) {
        char *s = g_strdup_printf(waiting > 1 ? _("Installing %s: %d%% (+%u waiting)") : _("Installing %s: %d%%"), running->title,
                                  (int)(running->frac * 100), waiting - 1);

        return s;
    }
    return g_strdup_printf(_("%u waiting"), waiting);
}

/* ---- the worker ------------------------------------------------------------------------------- */

/* the phases that stop cleanly (nothing is left on the disk); the copy to the disk itself cannot be interrupted */
static gboolean phase_cancelable(const char *what)
{
    return what != NULL && (!strcmp(what, _("Checking the disk")) || !strcmp(what, _("Unpacking")) || !strcmp(what, _("Compressing")) ||
                            !strcmp(what, _("Preparing")) || !strcmp(what, _("Writing VCD")));
}

static gboolean msg_idle(gpointer p)
{
    Msg *m = p;

    if (m->t->state == TR_RUNNING) {
        m->t->frac = m->frac;
        g_free(m->t->what);
        m->t->what = g_strdup(m->what);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m->t->bar), m->frac);
        gtk_label_set_text(GTK_LABEL(m->t->status), m->what);
        {
            char *pct = g_strdup_printf("%d%%", (int)(m->frac * 100));

            gtk_label_set_text(GTK_LABEL(m->t->pct), pct);
            g_free(pct);
        }
        gtk_widget_set_sensitive(m->t->cancel_btn, m->cancelable && !g_atomic_int_get(&m->t->cancel_req));
        update_statusbar(FALSE);
    }
    transfer_unref(m->t);
    g_free(m->what);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static gboolean transfer_progress(guint64 done, guint64 total, const char *what, gpointer p)
{
    Transfer *t = p;
    Msg *m = g_new0(Msg, 1);
    gboolean cancelable = phase_cancelable(what);

    m->t = t;
    g_atomic_int_inc(&t->refs);
    m->frac = total ? (double)done / total : 0;
    m->what = g_strdup(what);
    m->cancelable = cancelable;
    g_idle_add(msg_idle, m);
    return !(cancelable && g_atomic_int_get(&t->cancel_req));
}

static void thread_run(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Transfer *t = data;

    (void)src;
    (void)c;
    t->ok = t->fn(t->data, transfer_progress, t, t->notes, &t->error);
    g_task_return_boolean(task, t->ok);
}

static void pump(void);

/* a desktop notification when the queue ends while the window is in the background (a long install is easy to forget) */
static void notify_finished(void)
{
    GNotification *n;
    char *body;

    if (A.app == NULL || g_getenv("PS2GM_SCRIPT") || (A.win != NULL && gtk_window_is_active(GTK_WINDOW(A.win))))
        return;
    if (batch_failed == 0)
        body = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u game installed.", "%u games installed.", batch_ok), batch_ok);
    else
        body = g_strdup_printf(_("%u installed, %u did not work."), batch_ok, batch_failed);
    n = g_notification_new(_("Transfers finished"));
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(A.app), "transfers", n);
    g_object_unref(n);
    g_free(body);
}

static void thread_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Transfer *t = data;
    GDateTime *now = g_date_time_new_now_local();

    (void)src;
    (void)res;
    t->finished_at = g_date_time_format(now, "%H:%M");
    g_date_time_unref(now);
    t->state = t->ok ? TR_DONE : (g_atomic_int_get(&t->cancel_req) ? TR_CANCELLED : TR_FAILED);
    t->frac = t->ok ? 1.0 : t->frac;
    if (t->ok)
        batch_ok++;
    else if (t->state == TR_FAILED)
        batch_failed++;
    if (t->done)
        t->done(t->ok, t->data);
    if (t->free_data)
        t->free_data(t->data);
    t->data = NULL;
    running = NULL;
    active_jobs--;
    refresh_view();
    pump();
    if (!transfers_busy() && (batch_ok || batch_failed)) { /* the whole queue is finished: tell, and read the disk once */
        notify_finished();
        if (batch_ok)
            refresh_library();
        batch_ok = batch_failed = 0;
    }
    update_statusbar(FALSE);
    transfer_unref(t);
}

static void pump(void)
{
    guint i;

    if (running != NULL)
        return;
    for (i = 0; list && i < list->len; i++) {
        Transfer *t = g_ptr_array_index(list, i);

        if (t->state == TR_QUEUED) {
            GTask *task;

            t->state = TR_RUNNING;
            running = t;
            g_atomic_int_inc(&t->refs); /* held until thread_done */
            task = g_task_new(NULL, NULL, thread_done, t);
            g_task_set_task_data(task, t, NULL);
            g_task_run_in_thread(task, thread_run);
            g_object_unref(task);
            refresh_view();
            return;
        }
    }
}

/* ---- the rows ---------------------------------------------------------------------------------- */

static void on_cancel(GtkButton *b, gpointer p)
{
    Transfer *t = p;

    (void)b;
    if (t->state == TR_QUEUED) {
        t->state = TR_CANCELLED;
        g_atomic_int_set(&t->cancel_req, 1);
        if (t->free_data)
            t->free_data(t->data);
        t->data = NULL;
        active_jobs--;
        refresh_view();
        update_statusbar(FALSE);
    } else if (t->state == TR_RUNNING) {
        g_atomic_int_set(&t->cancel_req, 1);
        gtk_widget_set_sensitive(t->cancel_btn, FALSE);
        gtk_label_set_text(GTK_LABEL(t->status), _("Cancelling..."));
    }
}

static void on_details(GtkButton *b, gpointer p)
{
    Transfer *t = p;
    GString *msg = g_string_new(NULL);
    guint i;

    (void)b;
    if (t->state == TR_FAILED && t->error != NULL) {
        show_error(_("That did not work"), t->error->message);
    } else {
        for (i = 0; i < t->notes->len; i++)
            g_string_append_printf(msg, "%s%s", i ? "\n" : "", (char *)g_ptr_array_index(t->notes, i));
        show_error(_("A few notes"), msg->str);
    }
    g_string_free(msg, TRUE);
}

static void on_remove_row(GtkButton *b, gpointer p)
{
    Transfer *t = p;

    (void)b;
    if (t->state == TR_QUEUED || t->state == TR_RUNNING)
        return;
    gtk_list_box_remove(GTK_LIST_BOX(listbox), t->row);
    g_ptr_array_remove(list, t); /* drops the list's reference */
    refresh_view();
}

static void on_clear_finished(GtkButton *b, gpointer d)
{
    guint i;

    (void)b;
    (void)d;
    for (i = list ? list->len : 0; i > 0; i--) {
        Transfer *t = g_ptr_array_index(list, i - 1);

        if (t->state != TR_QUEUED && t->state != TR_RUNNING) {
            gtk_list_box_remove(GTK_LIST_BOX(listbox), t->row);
            g_ptr_array_remove_index(list, i - 1);
        }
    }
    refresh_view();
}

static GtkWidget *make_row(Transfer *t)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14), *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *name = gtk_label_new(t->title), *badge = gtk_label_new(t->kind);

    t->thumb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); /* the cover, once the game is installed */
    gtk_widget_set_visible(t->thumb, FALSE);
    gtk_widget_set_valign(t->thumb, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(badge, "badge");
    gtk_widget_add_css_class(badge, !g_strcmp0(t->kind, "PS1") ? "badge-ps1" : "badge-ps2");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(name, "card-title");
    t->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(t->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t->status), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(t->status, "card-sub");
    t->bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(t->bar, "chip-bar");
    gtk_box_append(GTK_BOX(texts), name);
    gtk_box_append(GTK_BOX(texts), t->bar);
    gtk_box_append(GTK_BOX(texts), t->status);
    gtk_widget_set_hexpand(texts, TRUE);
    t->pct = gtk_label_new("");
    gtk_widget_add_css_class(t->pct, "detail-val");
    t->details_btn = gtk_button_new_with_label(_("Details"));
    t->cancel_btn = gtk_button_new_with_label(_("Cancel"));
    t->remove_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(t->remove_btn, _("Remove from the list"));
    gtk_widget_add_css_class(t->remove_btn, "flat");
    gtk_widget_add_css_class(t->details_btn, "btn-secondary");
    gtk_widget_add_css_class(t->cancel_btn, "btn-secondary");
    gtk_widget_set_valign(t->details_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(t->cancel_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(t->remove_btn, GTK_ALIGN_CENTER);
    g_signal_connect(t->cancel_btn, "clicked", G_CALLBACK(on_cancel), t);
    g_signal_connect(t->details_btn, "clicked", G_CALLBACK(on_details), t);
    g_signal_connect(t->remove_btn, "clicked", G_CALLBACK(on_remove_row), t);
    gtk_box_append(GTK_BOX(row), t->thumb);
    gtk_box_append(GTK_BOX(row), badge);
    gtk_box_append(GTK_BOX(row), texts);
    gtk_box_append(GTK_BOX(row), t->pct);
    gtk_box_append(GTK_BOX(row), t->details_btn);
    gtk_box_append(GTK_BOX(row), t->cancel_btn);
    gtk_box_append(GTK_BOX(row), t->remove_btn);
    gtk_widget_add_css_class(row, "transfer");
    return row;
}

/* sets every row to what its transfer is now, and the header, the empty state and the tab */
static void refresh_view(void)
{
    guint i, active = count_state(TRUE), finished = list ? list->len - active : 0;
    char *sub, *a, *f;

    for (i = 0; list && i < list->len; i++) {
        Transfer *t = g_ptr_array_index(list, i);
        gboolean fin = t->state != TR_QUEUED && t->state != TR_RUNNING;
        char *text = NULL;

        gtk_widget_set_visible(t->bar, !fin || t->state == TR_DONE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(t->bar), t->state == TR_DONE ? 1.0 : (t->state == TR_QUEUED ? 0 : t->frac));
        gtk_widget_set_visible(t->pct, t->state == TR_RUNNING);
        gtk_widget_set_visible(t->cancel_btn, !fin);
        gtk_widget_set_sensitive(t->cancel_btn, t->state == TR_QUEUED || (t->state == TR_RUNNING && phase_cancelable(t->what) && !t->cancel_req));
        gtk_widget_set_visible(t->remove_btn, fin);
        gtk_widget_set_visible(t->details_btn, (t->state == TR_DONE && t->notes->len > 0) || t->state == TR_FAILED);
        switch (t->state) {
        case TR_QUEUED:
            text = g_strdup(_("Waiting in the queue"));
            break;
        case TR_DONE:
            text = g_strdup_printf(t->notes->len ? _("Done at %s, with a few notes") : _("Done at %s"), t->finished_at);
            break;
        case TR_CANCELLED:
            text = g_strdup(_("Cancelled"));
            break;
        case TR_FAILED:
            text = g_strdup_printf(_("Did not work: %s"), t->error ? t->error->message : "?");
            break;
        default:
            break;
        }
        if (text != NULL) {
            gtk_label_set_text(GTK_LABEL(t->status), text);
            gtk_widget_set_tooltip_text(t->status, t->state == TR_FAILED ? text : NULL);
            g_free(text);
        }
    }
    a = g_strdup_printf(_("%u active"), active);
    f = g_strdup_printf(_("%u finished"), finished);
    sub = g_strdup_printf("%s · %s", a, f);
    gtk_label_set_text(GTK_LABEL(sub_label), sub);
    gtk_widget_set_visible(clear_btn, finished > 0);
    gtk_widget_set_visible(scroll, list && list->len > 0);
    gtk_widget_set_visible(empty_box, !list || list->len == 0);
    tabs_set_badge(active);
    g_free(a);
    g_free(f);
    g_free(sub);
}

/* adds a job to the queue; it starts when the ones before it are finished. kind: "PS2" or "PS1" */
void queue_transfer(const char *kind, const char *title, const char *id, JobFn fn, gpointer data, GDestroyNotify free_data, JobDone done)
{
    Transfer *t = g_new0(Transfer, 1);

    if (list == NULL)
        list = g_ptr_array_new_with_free_func((GDestroyNotify)transfer_unref);
    t->refs = 1;
    t->kind = g_strdup(kind);
    t->title = g_strdup(title);
    t->id = g_strdup(id);
    t->fn = fn;
    t->data = data;
    t->free_data = free_data;
    t->done = done;
    t->notes = g_ptr_array_new_with_free_func(g_free);
    t->state = TR_QUEUED;
    t->row = make_row(t);
    g_ptr_array_add(list, t);
    gtk_list_box_append(GTK_LIST_BOX(listbox), t->row);
    active_jobs++;
    refresh_view();
    pump();
    update_statusbar(FALSE);
    adw_view_stack_set_visible_child_name(A.stack, "transfers");
}

/* once the disk is read again the covers of the finished installs are in the cache: show them in the rows */
void transfers_show_covers(void)
{
    guint i;

    for (i = 0; list && i < list->len; i++) {
        Transfer *t = g_ptr_array_index(list, i);
        char *path = t->state == TR_DONE && t->id ? library_cover_path(t->id) : NULL;

        if (path != NULL && gtk_widget_get_first_child(t->thumb) == NULL) {
            /* a small copy of the picture: a GtkPicture would ask for the size of the whole file */
            GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 40, 56, FALSE, NULL);

            if (pb != NULL) {
                GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), (gsize)gdk_pixbuf_get_rowstride(pb) * (gsize)gdk_pixbuf_get_height(pb));
                GdkTexture *tex = gdk_memory_texture_new(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                                                         gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes,
                                                         (gsize)gdk_pixbuf_get_rowstride(pb));
                GtkWidget *pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));

                gtk_picture_set_can_shrink(GTK_PICTURE(pic), FALSE);
                gtk_widget_set_overflow(pic, GTK_OVERFLOW_HIDDEN);
                gtk_widget_add_css_class(pic, "cover");
                gtk_box_append(GTK_BOX(t->thumb), pic);
                gtk_widget_set_visible(t->thumb, TRUE);
                g_object_unref(tex);
                g_bytes_unref(bytes);
                g_object_unref(pb);
            }
        }
        g_free(path);
    }
}

/* ---- the page ---------------------------------------------------------------------------------- */

GtkWidget *build_transfers_page(void)
{
    GtkWidget *clamp = adw_clamp_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16), *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *eyebrow = gtk_label_new(NULL), *title = gtk_label_new(_("Transfers"));
    GdkPaintable *art = pixel_art("ps2-disc.png", 110);
    char *eb = g_utf8_strup(_("In the background"), -1);

    gtk_label_set_text(GTK_LABEL(eyebrow), eb);
    g_free(eb);
    gtk_widget_add_css_class(eyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0);
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    sub_label = gtk_label_new("");
    gtk_widget_add_css_class(sub_label, "page-sub");
    gtk_label_set_xalign(GTK_LABEL(sub_label), 0);
    gtk_box_append(GTK_BOX(titles), eyebrow);
    gtk_box_append(GTK_BOX(titles), title);
    gtk_box_append(GTK_BOX(titles), sub_label);
    gtk_widget_set_hexpand(titles, TRUE);
    clear_btn = gtk_button_new_with_label(_("Clear finished"));
    gtk_widget_add_css_class(clear_btn, "btn-secondary");
    gtk_widget_add_css_class(clear_btn, "pill");
    gtk_widget_set_valign(clear_btn, GTK_ALIGN_CENTER);
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_finished), NULL);
    gtk_box_append(GTK_BOX(head), titles);
    gtk_box_append(GTK_BOX(head), clear_btn);

    listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(listbox, "transparent-list");
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);
    gtk_widget_set_vexpand(scroll, TRUE);

    empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(empty_box, TRUE);
    gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
    if (art != NULL) {
        GtkWidget *pic = gtk_picture_new_for_paintable(art);

        gtk_picture_set_can_shrink(GTK_PICTURE(pic), FALSE);
        gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(empty_box), pic);
        g_object_unref(art);
    }
    {
        GtkWidget *t = gtk_label_new(_("No transfers yet")), *s = gtk_label_new(_("Games you install show their progress here. You can keep using Memento meanwhile."));

        gtk_widget_add_css_class(t, "details-title");
        gtk_widget_add_css_class(s, "page-sub");
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_justify(GTK_LABEL(s), GTK_JUSTIFY_CENTER);
        gtk_box_append(GTK_BOX(empty_box), t);
        gtk_box_append(GTK_BOX(empty_box), s);
    }

    gtk_widget_set_margin_top(page, 22);
    gtk_widget_set_margin_bottom(page, 16);
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    gtk_box_append(GTK_BOX(page), head);
    gtk_box_append(GTK_BOX(page), scroll);
    gtk_box_append(GTK_BOX(page), empty_box);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_child(ADW_CLAMP(clamp), page);
    refresh_view();
    return clamp;
}
