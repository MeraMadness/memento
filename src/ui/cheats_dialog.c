/*
 * The cheats dialog of a PS2 game: edit the cheat list (OPL's PS2RD format), load it from a file, save it to the
 * disk (+OPL/CHT/<ID>.cht) and switch the cheat engine on for the game. Opening it first reads the list from the disk.
 */
#include "app.h"

typedef struct {
    char *id, *title;
    char *text;      /* what the disk has now; NULL if no cheats yet */
    /* the dialog */
    AdwDialog *dialog;
    GtkTextBuffer *buffer;
    GtkWidget *status_row, *turn_on_row;
} Cheats;

static void cheats_free(gpointer p)
{
    Cheats *c = p;

    g_free(c->id);
    g_free(c->title);
    g_free(c->text);
    g_free(c);
}

static char *buffer_text(Cheats *c)
{
    GtkTextIter a, b;

    gtk_text_buffer_get_bounds(c->buffer, &a, &b);
    return gtk_text_buffer_get_text(c->buffer, &a, &b, FALSE);
}

static char *cheats_count_text(guint n)
{
    return g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u cheat", "%u cheats", n), n);
}

static char *codes_count_text(guint n)
{
    return g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u code line", "%u code lines", n), n);
}

/* the live "N cheats" line under the text */
static void on_text_changed(GtkTextBuffer *buf, gpointer data)
{
    Cheats *c = data;
    char *text = buffer_text(c), *clean = NULL, *msg;
    GError *err = NULL;
    CheatInfo ci;

    (void)buf;
    if (*text == '\0')
        msg = g_strdup(_("Empty. Paste cheats here or load a file."));
    else if (cheats_normalize(text, &clean, &ci, &err))
    {
        char *c1 = cheats_count_text(ci.cheats), *c2 = codes_count_text(ci.codes);

        msg = g_strdup_printf(_("%s, %s"), c1, c2);
        g_free(c1);
        g_free(c2);
    }
    else
        msg = g_strdup(err->message);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(c->status_row), msg);
    g_free(msg);
    g_free(clean);
    g_free(text);
    g_clear_error(&err);
}

/* ---- save and remove ------------------------------------------------------------------------- */

typedef struct {
    char *id, *text; /* text NULL: remove */
    gboolean turn_on;
    CheatInfo info;
} SaveJob;

static void savejob_free(gpointer p)
{
    SaveJob *j = p;

    g_free(j->id);
    g_free(j->text);
    g_free(j);
}

static gboolean job_save(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    SaveJob *j = data;

    (void)notes;
    cb(0, 1, j->text ? _("Saving the cheats") : _("Removing the cheats"), pd);
    return ops_set_cheats(A.ops, j->id, j->text, j->turn_on, &j->info, error);
}

static void on_save(GtkButton *b, gpointer data)
{
    Cheats *c = data;
    char *text = buffer_text(c), *clean = NULL;
    GError *err = NULL;
    SaveJob *j;

    (void)b;
    if (!cheats_normalize(text, &clean, NULL, &err)) { /* the same check again, to say what is wrong */
        show_error(_("These cheats cannot be saved"), err->message);
        g_clear_error(&err);
        g_free(text);
        return;
    }
    g_free(clean);
    j = g_new0(SaveJob, 1);
    j->id = g_strdup(c->id);
    j->text = text;
    j->turn_on = adw_switch_row_get_active(ADW_SWITCH_ROW(c->turn_on_row));
    adw_dialog_close(c->dialog); /* frees c */
    run_job(_("Saving the cheats"), job_save, j, savejob_free, after_change);
}

static void on_remove(GtkButton *b, gpointer data)
{
    Cheats *c = data;
    SaveJob *j = g_new0(SaveJob, 1);

    (void)b;
    j->id = g_strdup(c->id);
    adw_dialog_close(c->dialog);
    run_job(_("Removing the cheats"), job_save, j, savejob_free, after_change);
}

/* ---- load from a file ------------------------------------------------------------------------ */

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    Cheats *c = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    gchar *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    char *path;

    if (f == NULL)
        return;
    path = g_file_get_path(f);
    g_object_unref(f);
    if (path != NULL && g_file_get_contents(path, &contents, &len, &err)) {
        if (g_utf8_validate(contents, len, NULL))
            gtk_text_buffer_set_text(c->buffer, contents, (int)len);
        else
            show_error(_("That is not a text file"), _("Cheat lists are plain text. Save the list as a text (.txt or .cht) file and try again."));
    } else if (err != NULL) {
        show_error(_("Cannot read that file"), err->message);
    }
    g_clear_error(&err);
    g_free(contents);
    g_free(path);
}

static void on_load(GtkButton *b, gpointer data)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    gtk_file_dialog_set_title(dlg, _("Choose a cheat list"));
    gtk_file_filter_set_name(flt, _("Cheat lists (.cht, .txt)"));
    gtk_file_filter_add_suffix(flt, "cht");
    gtk_file_filter_add_suffix(flt, "txt");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, data);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

/* ---- the dialog ------------------------------------------------------------------------------ */

static void on_closed(AdwDialog *d, gpointer data)
{
    (void)d;
    cheats_free(data);
}

static void present_cheats(gboolean ok, gpointer data)
{
    Cheats *c = data;
    GtkWidget *view = adw_toolbar_view_new(), *header = adw_header_bar_new(), *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *info = gtk_label_new(NULL), *text, *btn, *row;
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    char *title;

    if (!ok)
        return; /* the error was shown by the job; c is freed with the job data */
    /* the job's data is freed after this call: the dialog works on its own copy */
    c = g_new0(Cheats, 1);
    c->id = g_strdup(((Cheats *)data)->id);
    c->title = g_strdup(((Cheats *)data)->title);
    c->text = g_strdup(((Cheats *)data)->text);

    gtk_label_set_wrap(GTK_LABEL(info), TRUE);
    gtk_label_set_xalign(GTK_LABEL(info), 0);
    gtk_widget_add_css_class(info, "dim-label");
    gtk_label_set_markup(GTK_LABEL(info),
                         _("One cheat per block: a line with its <b>name</b>, then its code lines, like\n"
                         "<tt>Infinite Health\n10B8DAFA 00003F00</tt>\n"
                         "Lines starting with <tt>//</tt> or <tt>#</tt> are notes. Cheat lists in this format are shared on the "
                         "internet for most games; the app does not download them."));

    text = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_NONE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 8);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 8);
    c->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
    gtk_widget_set_size_request(scroll, -1, 260);
    gtk_widget_add_css_class(scroll, "card");

    c->status_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->status_row), _("Check"));
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(c->status_row), 3);
    adw_preferences_group_add(group, c->status_row);
    row = button_row(_("Load from a file"), _("A .cht or .txt cheat list"), _("Choose..."), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_load), c);
    adw_preferences_group_add(group, row);
    c->turn_on_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->turn_on_row), _("Turn the cheats on for this game"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(c->turn_on_row), _("Sets the game's OPL settings: cheat engine on, all cheats active"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(c->turn_on_row), TRUE);
    adw_preferences_group_add(group, c->turn_on_row);
    row = button_row(_("Save"), _("Writes the list to the disk (+OPL/CHT)"), _("Save to the disk"), NULL, &btn);
    gtk_widget_add_css_class(btn, "suggested-action");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_save), c);
    adw_preferences_group_add(group, row);
    if (c->text != NULL) {
        row = button_row(_("Remove"), _("Deletes the cheat list from the disk and switches the cheats off"), _("Remove cheats"), NULL, &btn);
        gtk_widget_add_css_class(btn, "destructive-action");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_remove), c);
        adw_preferences_group_add(group, row);
    }

    g_signal_connect(c->buffer, "changed", G_CALLBACK(on_text_changed), c);
    if (c->text != NULL)
        gtk_text_buffer_set_text(c->buffer, c->text, -1);
    else
        on_text_changed(c->buffer, c);

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_box_append(GTK_BOX(box), info);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));
    {
        GtkWidget *outer = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer), box);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), outer);
    }
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    c->dialog = adw_dialog_new();
    title = g_strdup_printf(_("Cheats: %s"), c->title && *c->title ? c->title : c->id);
    adw_dialog_set_title(c->dialog, title);
    g_free(title);
    adw_dialog_set_child(c->dialog, view);
    adw_dialog_set_content_width(c->dialog, 560);
    adw_dialog_set_content_height(c->dialog, 720);
    g_signal_connect(c->dialog, "closed", G_CALLBACK(on_closed), c);
    adw_dialog_present(c->dialog, GTK_WIDGET(A.win));
}

static gboolean job_read(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Cheats *c = data;

    (void)notes;
    cb(0, 1, _("Reading the cheats from the disk"), pd);
    return ops_get_cheats(A.ops, c->id, &c->text, error);
}

void show_cheats_dialog(const char *id, const char *title)
{
    Cheats *c = g_new0(Cheats, 1);

    c->id = g_strdup(id);
    c->title = g_strdup(title);
    run_job(_("Reading the cheats"), job_read, c, cheats_free, present_cheats);
}
