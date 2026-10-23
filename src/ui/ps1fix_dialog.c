/*
 * The "PS1 fixes" dialog of a PS1 game: switches for the POPStarter workarounds that help with video hangs and slowdowns, and
 * a text box for any other line of CHEATS.TXT (other settings, raw cheat codes). The file is kept in the game's own folder on
 * the disk, __common/POPS/<VCD name>/CHEATS.TXT, where POPStarter reads it. Opening the dialog first reads the file.
 */
#include "app.h"
#include "ps1fix.h"

typedef struct {
    char *vcd, *title;
    char *text;      /* what the disk has now; NULL if there is no file */
    GPtrArray *files; /* LibFile: the TROJAN_#.BIN / PATCH_#.BIN files in the game's folder (only while the dialog is being built) */
    /* the dialog */
    AdwDialog *dialog;
    GtkTextBuffer *buffer;
    GtkWidget *sw_cache1, *sw_subcd, *sw_code, *sw_undo;
} Fix;

static void fix_free(gpointer p)
{
    Fix *f = p;

    g_free(f->vcd);
    g_free(f->title);
    g_free(f->text);
    if (f->files != NULL)
        g_ptr_array_free(f->files, TRUE);
    g_free(f);
}

typedef struct {
    char *vcd, *text; /* text NULL: remove the file */
} SaveFix;

static void savefix_free(gpointer p)
{
    SaveFix *s = p;

    g_free(s->vcd);
    g_free(s->text);
    g_free(s);
}

static gboolean job_save_fix(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    SaveFix *s = data;

    (void)notes;
    cb(0, 1, s->text ? _("Saving the fixes") : _("Removing the fixes"), pd);
    return ops_set_ps1_fixes(A.ops, s->vcd, s->text, error);
}

static void after_save_fix(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        toast(_("Done"));
}

static char *buffer_text(Fix *f)
{
    GtkTextIter a, b;

    gtk_text_buffer_get_bounds(f->buffer, &a, &b);
    return gtk_text_buffer_get_text(f->buffer, &a, &b, FALSE);
}

/* the file as the dialog now describes it; FALSE (and an error dialog) when a line cannot be written */
static gboolean compose(Fix *f, char **text)
{
    Ps1Fixes *fx = ps1fix_parse(f->text); /* only for the line ends the file already has */
    GError *err = NULL;
    gboolean ok;

    g_free(fx->other);
    fx->other = buffer_text(f);
    fx->cache1 = adw_switch_row_get_active(ADW_SWITCH_ROW(f->sw_cache1));
    fx->subcdstatus = adw_switch_row_get_active(ADW_SWITCH_ROW(f->sw_subcd));
    fx->codecache0 = adw_switch_row_get_active(ADW_SWITCH_ROW(f->sw_code));
    fx->undo_fixes = adw_switch_row_get_active(ADW_SWITCH_ROW(f->sw_undo));
    ok = ps1fix_compose(fx, text, &err);
    if (!ok) {
        show_error(_("These lines cannot be saved"), err->message);
        g_clear_error(&err);
    }
    ps1fix_free(fx);
    return ok;
}

static void on_save(GtkButton *b, gpointer data)
{
    Fix *f = data;
    char *text = NULL;
    SaveFix *s;

    (void)b;
    if (!compose(f, &text))
        return;
    s = g_new0(SaveFix, 1);
    s->vcd = g_strdup(f->vcd);
    s->text = text; /* NULL when nothing is switched on and the box is empty: that removes the file */
    adw_dialog_close(f->dialog); /* frees f */
    run_job(text ? _("Saving the fixes") : _("Removing the fixes"), job_save_fix, s, savefix_free, after_save_fix);
}

static void on_remove(GtkButton *b, gpointer data)
{
    Fix *f = data;
    SaveFix *s = g_new0(SaveFix, 1);

    (void)b;
    s->vcd = g_strdup(f->vcd);
    adw_dialog_close(f->dialog);
    run_job(_("Removing the fixes"), job_save_fix, s, savefix_free, after_save_fix);
}

/* ---- the fix files ----------------------------------------------------------------------------- */

typedef struct {
    char *vcd, *title, *path, *name; /* add: path of the file on this computer; remove: name in the game's folder */
} FileOp;

static void fileop_free(gpointer p)
{
    FileOp *o = p;

    g_free(o->vcd);
    g_free(o->title);
    g_free(o->path);
    g_free(o->name);
    g_free(o);
}

static gboolean job_file_op(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    FileOp *o = data;

    (void)notes;
    cb(0, 1, o->path ? _("Copying the fix file to the disk") : _("Removing the fix file"), pd);
    return o->path ? ops_add_fix_file(A.ops, o->vcd, o->path, error) : ops_remove_fix_file(A.ops, o->vcd, o->name, error);
}

/* the list of files changed: open the dialog again so it shows what the disk has now */
static void after_file_op(gboolean ok, gpointer data)
{
    FileOp *o = data;

    if (ok)
        toast(_("Done"));
    show_ps1fix_dialog(o->vcd, o->title);
}

static void start_file_op(Fix *f, const char *path, const char *name)
{
    FileOp *o = g_new0(FileOp, 1);

    o->vcd = g_strdup(f->vcd);
    o->title = g_strdup(f->title);
    o->path = g_strdup(path);
    o->name = g_strdup(name);
    adw_dialog_close(f->dialog); /* frees f */
    run_job(path ? _("Adding the fix file") : _("Removing the fix file"), job_file_op, o, fileop_free, after_file_op);
}

static void on_remove_file(GtkButton *b, gpointer data)
{
    start_file_op(data, NULL, g_object_get_data(G_OBJECT(b), "name"));
}

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);

    if (file != NULL) {
        char *path = g_file_get_path(file);

        start_file_op(data, path, NULL);
        g_free(path);
        g_object_unref(file);
    }
}

static void on_add_file(GtkButton *b, gpointer data)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    gtk_file_dialog_set_title(dlg, _("Choose a fix file (TROJAN_#.BIN or PATCH_#.BIN)"));
    gtk_file_filter_set_name(flt, _("Fix files (.BIN)"));
    gtk_file_filter_add_suffix(flt, "BIN");
    gtk_file_filter_add_suffix(flt, "bin");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, data);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

static void on_closed(AdwDialog *d, gpointer data)
{
    (void)d;
    fix_free(data);
}

static GtkWidget *switch_row(const char *title, const char *directive, const char *what, gboolean on)
{
    GtkWidget *row = adw_switch_row_new();
    char *sub = g_strdup_printf("%s: %s", directive, what);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), on);
    g_free(sub);
    return row;
}

static void present_fix(gboolean ok, gpointer data)
{
    Fix *f;
    Ps1Fixes *fx;
    GtkWidget *view = adw_toolbar_view_new(), *header = adw_header_bar_new(), *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *info = gtk_label_new(NULL), *text, *btn, *row, *outer;
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *group2 = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *group3 = ADW_PREFERENCES_GROUP(adw_preferences_group_new()); /* the buttons, below the text box */
    AdwPreferencesGroup *group4 = ADW_PREFERENCES_GROUP(adw_preferences_group_new()); /* the fix files */
    guint i;
    char *title;

    if (!ok)
        return; /* the error was shown by the job; the data is freed with the job */
    /* the job's data is freed after this call: the dialog works on its own copy */
    f = g_new0(Fix, 1);
    f->vcd = g_strdup(((Fix *)data)->vcd);
    f->title = g_strdup(((Fix *)data)->title);
    f->text = g_strdup(((Fix *)data)->text);
    fx = ps1fix_parse(f->text);

    gtk_label_set_wrap(GTK_LABEL(info), TRUE);
    gtk_label_set_xalign(GTK_LABEL(info), 0);
    gtk_widget_add_css_class(info, "dim-label");
    gtk_label_set_text(GTK_LABEL(info),
                       _("POPStarter reads these settings from CHEATS.TXT in the game's own folder. They come from the community's "
                       "documentation of POPStarter, not from the program's makers, and not every version supports all of them. "
                       "Change one thing at a time and test the game after each change."));

    adw_preferences_group_set_title(group, _("Workarounds"));
    f->sw_cache1 = switch_row(_("Smaller read buffer"), "$CACHE1", _("a buffer of 1 sector instead of 16. May help video that hangs."), fx->cache1);
    f->sw_subcd = switch_row(_("CD status workaround"), "$SUBCDSTATUS", _("for hangs and timeouts."), fx->subcdstatus);
    f->sw_code = switch_row(_("Recompiler cache workaround"), "$CODECACHE_ADDON_0", _("for slowdowns and random hangs. Use it only for the game that needs it."), fx->codecache0);
    f->sw_undo = switch_row(_("Turn off the built-in fixes"), "$UNDO_GAME_FIXES", _("finds out whether a fix that POPStarter applies by itself hurts this game."), fx->undo_fixes);
    adw_preferences_group_add(group, f->sw_cache1);
    adw_preferences_group_add(group, f->sw_subcd);
    adw_preferences_group_add(group, f->sw_code);
    adw_preferences_group_add(group, f->sw_undo);

    adw_preferences_group_set_title(group2, _("Other lines"));
    adw_preferences_group_set_description(group2, _("One per line: other $ settings and raw cheat codes. Plain text only."));
    text = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_NONE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 8);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 8);
    f->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    gtk_text_buffer_set_text(f->buffer, fx->other ? fx->other : "", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_widget_add_css_class(scroll, "card");
    adw_preferences_group_add(group2, scroll);
    ps1fix_free(fx);

    adw_preferences_group_set_title(group4, _("Fix files"));
    adw_preferences_group_set_description(group4,
                                          _("TROJAN_#.BIN and PATCH_#.BIN files from a fix pack go in the game's folder. Some of them change how the game "
                                          "behaves with its memory card, so keep a copy of the card first. Use one pack's files at a time."));
    for (i = 0; ((Fix *)data)->files && i < ((Fix *)data)->files->len; i++) {
        LibFile *lf = g_ptr_array_index(((Fix *)data)->files, i);
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        char *sz = size_text(lf->size / 1024 + (lf->size % 1024 ? 1 : 0));

        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lf->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sz);
        gtk_widget_set_tooltip_text(rm, _("Remove from the game's folder"));
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
        g_object_set_data_full(G_OBJECT(rm), "name", g_strdup(lf->name), g_free);
        g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_file), f);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm);
        adw_preferences_group_add(group4, row);
        g_free(sz);
    }
    row = button_row(_("Add a fix file"), _("Copies a file from this computer into the game's folder on the disk"), _("Choose..."), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_file), f);
    adw_preferences_group_add(group4, row);

    row = button_row(_("Save"), _("Writes CHEATS.TXT into the game's folder on the disk"), _("Save to the disk"), NULL, &btn);
    gtk_widget_add_css_class(btn, "suggested-action");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_save), f);
    adw_preferences_group_add(group3, row);
    if (f->text != NULL) {
        row = button_row(_("Remove"), _("Deletes CHEATS.TXT, so the game runs without these settings"), _("Remove the file"), NULL, &btn);
        gtk_widget_add_css_class(btn, "destructive-action");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_remove), f);
        adw_preferences_group_add(group3, row);
    }

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_box_append(GTK_BOX(box), info);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group4));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group2));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group3));
    outer = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer), box);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), outer);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    f->dialog = adw_dialog_new();
    title = g_strdup_printf(_("PS1 fixes: %s"), f->title && *f->title ? f->title : f->vcd);
    adw_dialog_set_title(f->dialog, title);
    g_free(title);
    adw_dialog_set_child(f->dialog, view);
    adw_dialog_set_content_width(f->dialog, 560);
    adw_dialog_set_content_height(f->dialog, 720);
    g_signal_connect(f->dialog, "closed", G_CALLBACK(on_closed), f);
    adw_dialog_present(f->dialog, GTK_WIDGET(A.win));
}

static gboolean job_read(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Fix *f = data;

    (void)notes;
    cb(0, 1, _("Reading the fixes from the disk"), pd);
    return ops_get_ps1_fixes(A.ops, f->vcd, &f->text, error) && ops_list_fix_files(A.ops, f->vcd, &f->files, error);
}

void show_ps1fix_dialog(const char *vcd_name, const char *title)
{
    Fix *f = g_new0(Fix, 1);

    f->vcd = g_strdup(vcd_name);
    f->title = g_strdup(title);
    run_job(_("Reading the fixes"), job_read, f, fix_free, present_fix);
}
