/*
 * Checking a disc image against the Redump lists: the "Check the image" row of the Add page, and the Settings group that
 * downloads and updates the lists. Hashing a whole DVD takes about half a minute, so it runs in a thread.
 */
#include "app.h"
#include "redump.h"

typedef struct {
    Ps2gmPlatform platform;
    char **paths;       /* the files to check, NULL when there is nothing (yet) */
    GtkWidget *row, *btn;
} Verify;

typedef struct {
    GtkWidget *row;     /* a reference is held while the thread runs */
    Verify *v;
    Ps2gmPlatform platform;
    char **paths;
    char *result;
    gint file, files;   /* which file is being hashed, out of how many */
} Check;

typedef struct {
    Check *c;
    int percent;
} Tick;

static void verify_free(gpointer p)
{
    Verify *v = p;

    g_strfreev(v->paths);
    g_free(v);
}

static void set_sub(GtkWidget *row, const char *text)
{
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), text);
}

static gboolean tick_idle(gpointer p)
{
    Tick *t = p;
    char *text = t->c->files > 1 ? g_strdup_printf(_("Checking file %d of %d: %d%%"), t->c->file, t->c->files, t->percent)
                                 : g_strdup_printf(_("Checking the image: %d%%"), t->percent);

    set_sub(t->c->row, text);
    g_free(text);
    g_free(t);
    return G_SOURCE_REMOVE;
}

static gboolean hash_progress(guint64 done, guint64 total, const char *what, gpointer p)
{
    Check *c = p;
    static gint last;
    int percent = total ? (int)(done * 100 / total) : 0;
    Tick *t;

    (void)what;
    if (percent == g_atomic_int_get(&last) && done != total)
        return TRUE;
    g_atomic_int_set(&last, percent);
    t = g_new0(Tick, 1);
    t->c = c;
    t->percent = percent;
    g_idle_add(tick_idle, t);
    return TRUE;
}

static void check_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    Check *c = data;
    GError *err = NULL;
    char *known = redump_info(c->platform), *game = NULL;
    guint i, matched = 0, n = g_strv_length(c->paths);

    (void)src;
    (void)cancel;
    if (known == NULL && !redump_update(c->platform, &err)) { /* never downloaded: get the list first */
        c->result = g_strdup_printf(_("Could not download the Redump list: %s"), err->message);
        g_clear_error(&err);
        goto out;
    }
    c->files = (gint)n;
    for (i = 0; i < n; i++) {
        char *md5 = NULL, *name = NULL;
        guint64 size;

        c->file = (gint)i + 1;
        if (!redump_hash_file(c->paths[i], &md5, &size, hash_progress, c, &err)) {
            c->result = g_strdup(err->message);
            g_clear_error(&err);
            goto out;
        }
        if (redump_lookup(c->platform, md5, size, &name) == REDUMP_MATCH) {
            matched++;
            if (game == NULL)
                game = g_strdup(name);
        }
        g_free(md5);
        g_free(name);
    }
    if (matched == n)
        c->result = n > 1 ? g_strdup_printf(_("Good dump, all %u files match: %s"), n, game) : g_strdup_printf(_("Good dump: %s"), game);
    else if (matched > 0)
        c->result = g_strdup_printf(_("Only %u of %u files match the Redump list (%s). The others are another dump or damaged."), matched, n, game);
    else
        c->result = g_strdup(_("Not in the Redump list. It may be another version of the game, a modified image, a damaged one, or a game Redump does not have."));
out:
    g_free(known);
    g_free(game);
    g_task_return_boolean(task, TRUE);
}

static void check_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Check *c = data;
    Verify *v = c->v;

    (void)src;
    (void)res;
    if (v->paths != NULL && g_strv_equal((const char *const *)c->paths, (const char *const *)v->paths)) { /* still the same image */
        set_sub(c->row, c->result ? c->result : "");
        gtk_widget_set_sensitive(v->btn, v->paths != NULL);
    }
    g_object_unref(c->row);
    g_strfreev(c->paths);
    g_free(c->result);
    g_free(c);
}

static void on_check(GtkButton *b, gpointer data)
{
    Verify *v = data;
    Check *c;
    GTask *task;

    (void)b;
    if (v->paths == NULL)
        return;
    c = g_new0(Check, 1);
    c->row = g_object_ref(v->row);
    c->v = v;
    c->platform = v->platform;
    c->paths = g_strdupv(v->paths);
    gtk_widget_set_sensitive(v->btn, FALSE);
    set_sub(v->row, _("Starting..."));
    task = g_task_new(NULL, NULL, check_done, c);
    g_task_set_task_data(task, c, NULL);
    g_task_run_in_thread(task, check_thread);
    g_object_unref(task);
}

GtkWidget *build_verify_row(Ps2gmPlatform platform)
{
    Verify *v = g_new0(Verify, 1);
    GtkWidget *row = adw_action_row_new(), *b = gtk_button_new_with_label(_("Check"));

    v->platform = platform;
    v->row = row;
    v->btn = b;
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Check the image"));
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);
    set_sub(row, _("Compares it with the Redump list of good dumps"));
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(b, FALSE);
    g_signal_connect(b, "clicked", G_CALLBACK(on_check), v);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
    g_object_set_data_full(G_OBJECT(row), "verify", v, verify_free);
    return row;
}

/* a new image (paths, NULL-terminated) was chosen, or none (NULL); reason: why this kind of file cannot be checked, or NULL */
void verify_set_files(GtkWidget *row, char **paths, const char *reason)
{
    Verify *v = g_object_get_data(G_OBJECT(row), "verify");

    g_strfreev(v->paths);
    v->paths = paths && paths[0] ? g_strdupv(paths) : NULL;
    gtk_widget_set_sensitive(v->btn, v->paths != NULL);
    set_sub(row, reason ? reason : _("Compares it with the Redump list of good dumps"));
}

/* the Check button pressed (script mode and tests) */
void verify_start(GtkWidget *row)
{
    Verify *v = g_object_get_data(G_OBJECT(row), "verify");

    on_check(NULL, v);
}

/* ---- Settings: the lists themselves ------------------------------------------------------------- */

typedef struct {
    Ps2gmPlatform platform;
    GtkWidget *row, *btn;
    char *error;
} Update;

static void show_list_state(GtkWidget *row, Ps2gmPlatform platform)
{
    char *info = redump_info(platform);

    set_sub(row, info ? info : _("Not downloaded yet"));
    g_free(info);
}

static void update_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    Update *u = data;
    GError *err = NULL;

    (void)src;
    (void)cancel;
    if (!redump_update(u->platform, &err)) {
        u->error = g_strdup(err->message);
        g_clear_error(&err);
    }
    g_task_return_boolean(task, TRUE);
}

static void update_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Update *u = data;

    (void)src;
    (void)res;
    if (u->error != NULL)
        show_error(_("The Redump list could not be downloaded"), u->error);
    else
        toast(_("Redump list updated"));
    show_list_state(u->row, u->platform);
    gtk_widget_set_sensitive(u->btn, TRUE);
    g_object_unref(u->row);
    g_free(u->error);
    g_free(u);
}

static void on_update(GtkButton *b, gpointer data)
{
    Update *u = g_new0(Update, 1);
    GTask *task;

    u->platform = GPOINTER_TO_INT(data) ? PS2GM_PS1 : PS2GM_PS2;
    u->btn = GTK_WIDGET(b);
    u->row = g_object_ref(gtk_widget_get_ancestor(GTK_WIDGET(b), ADW_TYPE_ACTION_ROW));
    gtk_widget_set_sensitive(u->btn, FALSE);
    set_sub(u->row, _("Downloading..."));
    task = g_task_new(NULL, NULL, update_done, u);
    g_task_set_task_data(task, u, NULL);
    g_task_run_in_thread(task, update_thread);
    g_object_unref(task);
}

static GtkWidget *list_row(const char *title, Ps2gmPlatform platform)
{
    GtkWidget *row = adw_action_row_new(), *b = gtk_button_new_with_label(_("Download or update"));

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    show_list_state(row, platform);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", G_CALLBACK(on_update), GINT_TO_POINTER(platform == PS2GM_PS1));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
    return row;
}

GtkWidget *build_redump_group(void)
{
    GtkWidget *group = adw_preferences_group_new();

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), _("Redump lists"));
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          _("Hashes of good disc dumps from redump.org, used to check an image before installing it. Downloaded once, then used offline."));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), list_row("PlayStation 2", PS2GM_PS2));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), list_row("PlayStation", PS2GM_PS1));
    return group;
}
