/*
 * Programs page: the homebrew on the APPS partition (what OPL shows in its Apps tab). Add a PS2 program (.ELF) with a
 * button or by dropping it, update one while keeping the old version, rename, remove.
 */
#include "app.h"

typedef struct {
    char *a, *b; /* program name(s) or path */
    gboolean keep_old;
} AppJob;

static AdwPreferencesGroup *list_group;
static GPtrArray *list_rows;

static void appjob_free(gpointer p)
{
    AppJob *j = p;

    g_free(j->a);
    g_free(j->b);
    g_free(j);
}

static AppJob *appjob(const char *a, const char *b, gboolean keep_old)
{
    AppJob *j = g_new0(AppJob, 1);

    j->a = g_strdup(a);
    j->b = g_strdup(b);
    j->keep_old = keep_old;
    return j;
}

/* ---- the jobs --------------------------------------------------------------------------------- */

static gboolean job_add(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    AppJob *j = data;

    cb(0, 1, _("Copying the program to the PS2 disk"), pd);
    return ops_apps_add(A.ops, j->a, j->keep_old, notes, error);
}

static gboolean job_remove(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    AppJob *j = data;

    (void)notes;
    cb(0, 1, _("Removing"), pd);
    return ops_apps_remove(A.ops, j->a, error);
}

static gboolean job_rename(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    AppJob *j = data;

    (void)notes;
    cb(0, 1, _("Renaming"), pd);
    return ops_apps_rename(A.ops, j->a, j->b, error);
}

/* adds a program; asks what to do when one of that name is on the disk (skip_ask: the script mode and tests) */
static void on_replace_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    AppJob *j = data;

    (void)d;
    if (!strcmp(response, "keep") || !strcmp(response, "replace")) {
        j->keep_old = !strcmp(response, "keep");
        run_job(_("Adding the program"), job_add, j, appjob_free, after_change);
    } else
        appjob_free(j);
}

void apps_add_file(const char *path, gboolean ask, gboolean keep_old)
{
    GError *err = NULL;
    char *name = g_path_get_basename(path);
    gboolean exists = FALSE;
    guint i;

    if (!ps2gm_elf_check(path, &err)) {
        show_error(_("Not a PS2 program"), err->message);
        g_clear_error(&err);
        g_free(name);
        return;
    }
    for (i = 0; A.lib != NULL && i < A.lib->apps->len; i++)
        if (!strcmp(((LibFile *)g_ptr_array_index(A.lib->apps, i))->name, name))
            exists = TRUE;
    if (exists && ask) {
        char *backup = ops_apps_backup_name(name), *body = g_strdup_printf(_("A program called %s is already on the disk."), name);
        AdwDialog *d = adw_alert_dialog_new(_("Replace it?"), body);
        char *keep_label = g_strdup_printf(_("Replace, keep the old one as %s"), backup);

        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "replace", _("Replace"), "keep", keep_label, NULL);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "keep", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "keep");
        g_signal_connect(d, "response", G_CALLBACK(on_replace_response), appjob(path, NULL, TRUE));
        adw_dialog_present(d, GTK_WIDGET(A.win));
        g_free(backup);
        g_free(body);
        g_free(keep_label);
    } else
        run_job(_("Adding the program"), job_add, appjob(path, NULL, exists && keep_old), appjob_free, after_change);
    g_free(name);
}

void apps_remove_file(const char *name)
{
    run_job(_("Removing the program"), job_remove, appjob(name, NULL, FALSE), appjob_free, after_change);
}

void apps_rename_file(const char *from, const char *to)
{
    run_job(_("Renaming the program"), job_rename, appjob(from, to, FALSE), appjob_free, after_change);
}

/* ---- the list ---------------------------------------------------------------------------------- */

static void on_remove_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    (void)d;
    if (!strcmp(response, "remove"))
        apps_remove_file(data);
    g_free(data);
}

static void on_remove_clicked(GtkButton *b, gpointer data)
{
    const char *name = g_object_get_data(G_OBJECT(b), "app");
    char *body = g_strdup_printf(_("\"%s\" will be deleted from the PS2 disk. This cannot be undone."), name);
    AdwDialog *d = adw_alert_dialog_new(_("Remove this program?"), body);

    (void)data;
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "remove", _("Remove"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect(d, "response", G_CALLBACK(on_remove_response), g_strdup(name));
    adw_dialog_present(d, GTK_WIDGET(A.win));
    g_free(body);
}

typedef struct {
    char *from;
    GtkWidget *entry;
} RenameCtx;

static void on_rename_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    RenameCtx *r = data;

    (void)d;
    if (!strcmp(response, "rename")) {
        char *to = g_strdup(gtk_editable_get_text(GTK_EDITABLE(r->entry)));

        g_strstrip(to);
        if (*to != '\0' && strcmp(to, r->from) != 0) {
            if (!ps2gm_is_elf_name(to)) { /* forgot the ending: add it */
                char *with = g_strconcat(to, ".ELF", NULL);

                g_free(to);
                to = with;
            }
            apps_rename_file(r->from, to);
        }
        g_free(to);
    }
    g_free(r->from);
    g_free(r);
}

static void on_rename_clicked(GtkButton *b, gpointer data)
{
    const char *name = g_object_get_data(G_OBJECT(b), "app");
    AdwDialog *d = adw_alert_dialog_new(_("Rename this program"), _("The name shown in the Apps tab of OPL. It must end in .ELF."));
    RenameCtx *r = g_new0(RenameCtx, 1);

    (void)data;
    r->from = g_strdup(name);
    r->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(r->entry), name);
    gtk_entry_set_activates_default(GTK_ENTRY(r->entry), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), r->entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "rename", _("Rename"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "rename");
    g_signal_connect(d, "response", G_CALLBACK(on_rename_response), r);
    adw_dialog_present(d, GTK_WIDGET(A.win));
}

static GtkWidget *row_button(const char *icon, const char *tip, const char *name, GCallback cb)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);

    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "flat");
    g_object_set_data_full(G_OBJECT(b), "app", g_strdup(name), g_free);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static int cmp_files(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp((*(LibFile *const *)a)->name, (*(LibFile *const *)b)->name);
}

void rebuild_apps(void)
{
    GPtrArray *progs = g_ptr_array_new();
    guint i;

    if (list_group == NULL || A.lib == NULL)
        return;
    for (i = 0; i < list_rows->len; i++)
        adw_preferences_group_remove(list_group, g_ptr_array_index(list_rows, i));
    g_ptr_array_set_size(list_rows, 0);

    for (i = 0; i < A.lib->apps->len; i++)
        if (ps2gm_is_elf_name(((LibFile *)g_ptr_array_index(A.lib->apps, i))->name))
            g_ptr_array_add(progs, g_ptr_array_index(A.lib->apps, i));
    g_ptr_array_sort(progs, cmp_files);
    adw_preferences_group_set_description(list_group, progs->len ? NULL : _("No programs on the disk yet."));
    for (i = 0; i < progs->len; i++) {
        LibFile *f = g_ptr_array_index(progs, i);
        GtkWidget *row = adw_action_row_new();
        char *sz = size_text(f->size / 1024);

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), f->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sz);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), row_button("document-edit-symbolic", _("Rename"), f->name, G_CALLBACK(on_rename_clicked)));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), row_button("user-trash-symbolic", _("Remove from the disk"), f->name, G_CALLBACK(on_remove_clicked)));
        adw_preferences_group_add(list_group, row);
        g_ptr_array_add(list_rows, row);
        g_free(sz);
    }
    g_ptr_array_free(progs, TRUE);
}

/* ---- adding: button and drop --------------------------------------------------------------------- */

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;

    (void)data;
    if (f == NULL)
        return;
    path = g_file_get_path(f);
    g_object_unref(f);
    if (path != NULL)
        apps_add_file(path, TRUE, TRUE);
    g_free(path);
}

static void on_choose_clicked(GtkButton *b, gpointer data)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    (void)data;
    gtk_file_dialog_set_title(dlg, _("Choose a PS2 program"));
    gtk_file_filter_set_name(flt, _("PS2 programs (.ELF)"));
    gtk_file_filter_add_suffix(flt, "elf");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

static gboolean on_drop(GtkDropTarget *dt, const GValue *value, double x, double y, gpointer data)
{
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value)), *l;
    gboolean any = FALSE;

    (void)dt; (void)x; (void)y; (void)data;
    for (l = files; l != NULL; l = l->next) {
        char *path = g_file_get_path(l->data);

        if (path != NULL) {
            apps_add_file(path, TRUE, TRUE);
            any = TRUE;
        }
        g_free(path);
    }
    g_slist_free(files);
    return any;
}

static GdkDragAction on_drag_enter(GtkDropTarget *dt, double x, double y, gpointer data)
{
    (void)x; (void)y; (void)data;
    gtk_widget_add_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt)), "drop-hover");
    return GDK_ACTION_COPY;
}

static void on_drag_leave(GtkDropTarget *dt, gpointer data)
{
    (void)data;
    gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt)), "drop-hover");
}

GtkWidget *build_apps_page(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new(), *clamp = adw_clamp_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *title = gtk_label_new(_("Homebrew")), *sub = gtk_label_new(_("Homebrew programs on the PS2 disk. They show up in the Apps tab of OPL.")), *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *head = gtk_label_new(_("Add a program")), *hint = gtk_label_new(_("A PS2 program (.ELF). Drop it here or choose it. If it is already on the disk you can keep the old version.")), *btn;
    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);

    list_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    list_rows = g_ptr_array_new();

    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(sub, "card-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);

    gtk_widget_add_css_class(card, "add-card");
    gtk_widget_add_css_class(head, "section-title");
    gtk_label_set_xalign(GTK_LABEL(head), 0);
    gtk_widget_add_css_class(hint, "card-sub");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    btn = gtk_button_new_with_label(_("Choose..."));
    gtk_widget_add_css_class(btn, "pill");
    gtk_widget_add_css_class(btn, "suggested-action");
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_choose_clicked), NULL);
    gtk_box_append(GTK_BOX(card), head);
    gtk_box_append(GTK_BOX(card), hint);
    gtk_box_append(GTK_BOX(card), btn);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), NULL);
    g_signal_connect(dt, "enter", G_CALLBACK(on_drag_enter), NULL);
    g_signal_connect(dt, "leave", G_CALLBACK(on_drag_leave), NULL);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(dt));

    adw_preferences_group_set_title(list_group, _("On the disk"));

    gtk_widget_set_margin_top(page, 22);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_box_append(GTK_BOX(page), title);
    gtk_box_append(GTK_BOX(page), sub);
    gtk_box_append(GTK_BOX(page), card);
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(list_group));
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_child(ADW_CLAMP(clamp), page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    return scroll;
}
