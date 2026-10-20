/*
 * Saves: the virtual memory cards (OPL's VMC folder on the PS2 disk, or any raw card file on this computer, for example
 * the ones on a USB stick). List the saves of a card, export one as .psu, import .psu files, delete a save.
 *
 * A card of the disk is copied to this computer, changed there (card code in src/core/mcfs.c, shared with the console
 * app savemgr) and put back safely (ops_vmc_store). Before every change a backup of the card is kept.
 */
#include <glib/gstdio.h>

#include "app.h"
#include "mcfs.h"

#define BACKUPS_KEPT 5

typedef struct {
    char *name, *title;
    guint size_kb, entries;
} SaveInfo;

static struct {
    GtkWidget *combo, *list, *status, *import_btn, *copy_btn, *backups_btn, *empty;
    GtkStringList *model;
    GPtrArray *card_names;  /* the cards on the disk; entry i is item i + 1 of the model (item 0 is the "choose" line) */
    char *work_path;        /* the card file being edited: a copy of the disk's card, or the user's own file */
    char *remote_name;      /* the card's name on the disk, NULL for a file on this computer */
    GPtrArray *saves;       /* SaveInfo */
    guint free_kb, total_kb;
    gboolean updating;      /* the model is being rebuilt: ignore the selection signal */
} S;

static void save_info_free(gpointer p)
{
    SaveInfo *s = p;

    g_free(s->name);
    g_free(s->title);
    g_free(s);
}

static const char *mc_error_text(int rc)
{
    switch (rc) {
    case MC_E_IO:       return _("The card file could not be read or written");
    case MC_E_FORMAT:   return _("That is not a PS2 memory card image");
    case MC_E_ECC:      return _("That card image has ECC data; only raw images (like the ones OPL makes) are supported");
    case MC_E_NOTFOUND: return _("The save is not on the card");
    case MC_E_EXISTS:   return _("A save with that name is already on the card");
    case MC_E_FULL:     return _("There is not enough room on the card");
    case MC_E_BADPSU:   return _("That is not a usable .psu save file");
    case MC_E_RDONLY:   return _("The card is read only");
    default:            return _("The card could not be changed");
    }
}

static char *backups_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "ps2gm", "vmc-backups", NULL);
}

/* ---- showing the card ---------------------------------------------------------------------------- */

static void show_card(void);

/* reads the saves of the card file in S.work_path */
static void load_card(void)
{
    mc_t mc;
    struct mc_save *l;
    int n, i, rc;

    g_clear_pointer(&S.saves, g_ptr_array_unref);
    S.free_kb = S.total_kb = 0;
    rc = S.work_path ? mc_open(&mc, S.work_path, 0) : MC_E_IO;
    if (rc != MC_OK) {
        if (S.work_path != NULL) { /* a file that is not a card: say why, and show nothing */
            gtk_label_set_text(GTK_LABEL(S.status), mc_error_text(rc));
            g_clear_pointer(&S.work_path, g_free);
            g_clear_pointer(&S.remote_name, g_free);
        }
        show_card();
        return;
    }
    l = g_new0(struct mc_save, 512);
    n = mc_list(&mc, l, 512);
    S.saves = g_ptr_array_new_with_free_func(save_info_free);
    for (i = 0; i < n; i++) {
        SaveInfo *s = g_new0(SaveInfo, 1);
        char title[64] = "";

        mc_title(&mc, l[i].name, title, sizeof(title));
        s->name = g_strdup(l[i].name);
        s->title = g_strdup(title);
        s->size_kb = l[i].size_kb;
        s->entries = l[i].entries;
        g_ptr_array_add(S.saves, s);
    }
    S.free_kb = mc_free_kb(&mc);
    mc_close(&mc);
    g_free(l);
    {
        GStatBuf st;

        S.total_kb = g_stat(S.work_path, &st) == 0 ? (guint)(st.st_size / 1024) : 0;
    }
    show_card();
}

static void on_export(GtkButton *b, gpointer data);
static void on_delete(GtkButton *b, gpointer data);

static GtkWidget *icon_button(const char *icon, const char *tooltip, GCallback cb, const char *name)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);

    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    g_object_set_data_full(G_OBJECT(b), "save", g_strdup(name), g_free);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

/* the status line and the list for what was read */
static void show_card(void)
{
    guint i;
    gboolean have = S.work_path != NULL && S.saves != NULL;

    gtk_list_box_remove_all(GTK_LIST_BOX(S.list));
    gtk_widget_set_visible(S.list, have && S.saves->len > 0);
    gtk_widget_set_visible(S.empty, have && S.saves->len == 0);
    gtk_widget_set_sensitive(S.import_btn, have);
    gtk_widget_set_sensitive(S.copy_btn, have);
    if (!have)
        return;
    {
        char *total = g_strdup_printf("%u MB", S.total_kb / 1024), *free_t = g_strdup_printf("%.1f MB", S.free_kb / 1024.0), *text;

        text = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%s card · %s free · %u save", "%s card · %s free · %u saves", S.saves->len),
                               total, free_t, S.saves->len);
        gtk_label_set_text(GTK_LABEL(S.status), text);
        g_free(total);
        g_free(free_t);
        g_free(text);
    }
    for (i = 0; i < S.saves->len; i++) {
        SaveInfo *s = g_ptr_array_index(S.saves, i);
        GtkWidget *row = adw_action_row_new();
        char *sub = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%s · %u KB · %u file", "%s · %u KB · %u files", s->entries),
                                    s->name, s->size_kb, s->entries);

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), s->title && *s->title ? s->title : s->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon_button("document-save-symbolic", _("Export as .psu"), G_CALLBACK(on_export), s->name));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon_button("user-trash-symbolic", _("Delete from the card"), G_CALLBACK(on_delete), s->name));
        gtk_list_box_append(GTK_LIST_BOX(S.list), row);
        g_free(sub);
    }
}

/* ---- changing the card: backup, change, put back --------------------------------------------------- */

typedef struct {
    gboolean import;
    GPtrArray *items;       /* import: .psu paths; delete: the save name */
    char *work_path, *remote_name;
} Change;

static void change_free(gpointer p)
{
    Change *c = p;

    g_ptr_array_free(c->items, TRUE);
    g_free(c->work_path);
    g_free(c->remote_name);
    g_free(c);
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* a copy of the card before it is changed, with the date in its name; only the last few are kept */
static void make_backup(const char *card_path)
{
    char *dir = backups_dir(), *base = g_path_get_basename(card_path);
    char *stem = g_strndup(base, strlen(base) > 4 ? strlen(base) - 4 : strlen(base)), *prefix = g_strdup_printf("%s-", stem);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S"), *dest = g_strdup_printf("%s/%s-%s.bin", dir, stem, stamp);
    GFile *from = g_file_new_for_path(card_path), *to = g_file_new_for_path(dest);
    GPtrArray *mine = g_ptr_array_new_with_free_func(g_free);
    GDir *d;
    const char *n;

    g_mkdir_with_parents(dir, 0755);
    g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
    d = g_dir_open(dir, 0, NULL);
    while (d && (n = g_dir_read_name(d)) != NULL)
        if (g_str_has_prefix(n, prefix) && g_str_has_suffix(n, ".bin") && strlen(n) == strlen(stem) + 1 + 15 + 4)
            g_ptr_array_add(mine, g_strdup(n));
    if (d)
        g_dir_close(d);
    g_ptr_array_sort(mine, cmp_names);
    while (mine->len > BACKUPS_KEPT) { /* the names sort by date: the first ones are the oldest */
        char *old = g_build_filename(dir, g_ptr_array_index(mine, 0), NULL);

        g_unlink(old);
        g_free(old);
        g_ptr_array_remove_index(mine, 0);
    }
    g_ptr_array_free(mine, TRUE);
    g_object_unref(from);
    g_object_unref(to);
    g_date_time_unref(now);
    g_free(prefix);
    g_free(stamp);
    g_free(dest);
    g_free(stem);
    g_free(base);
    g_free(dir);
}

static gboolean job_change(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Change *c = data;
    mc_t mc;
    guint i, done = 0;
    int rc;

    (void)cb;
    (void)pd;
    make_backup(c->work_path);
    rc = mc_open(&mc, c->work_path, 1);
    if (rc != MC_OK) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, "%s", mc_error_text(rc));
        return FALSE;
    }
    for (i = 0; i < c->items->len; i++) {
        const char *item = g_ptr_array_index(c->items, i);
        char *base = g_path_get_basename(item);

        rc = c->import ? mc_import_psu(&mc, item, NULL) : mc_delete(&mc, item);
        if (rc == MC_OK)
            done++;
        else
            g_ptr_array_add(notes, g_strdup_printf("%s: %s", base, mc_error_text(rc)));
        g_free(base);
        if (rc == MC_E_IO || rc == MC_E_FULL) /* nothing more can be added */
            break;
    }
    rc = mc_close(&mc);
    if (rc != MC_OK) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, "%s", mc_error_text(rc));
        return FALSE;
    }
    if (done == 0)
        return TRUE; /* nothing changed: the reasons are in the notes */
    if (c->remote_name != NULL)
        return ops_vmc_store(A.ops, c->remote_name, c->work_path, error);
    return TRUE;
}

static void after_change_card(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        toast(_("Done"));
    load_card(); /* the work file is what the disk has now, or what the file now holds */
}

static void start_change(gboolean import, GPtrArray *items)
{
    Change *c = g_new0(Change, 1);

    c->import = import;
    c->items = items;
    c->work_path = g_strdup(S.work_path);
    c->remote_name = g_strdup(S.remote_name);
    run_job(import ? _("Importing saves") : _("Deleting the save"), job_change, c, change_free, after_change_card);
}

/* ---- the buttons ---------------------------------------------------------------------------------- */

static void on_psus_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    GPtrArray *items;
    guint i, n;

    (void)data;
    if (files == NULL)
        return;
    items = g_ptr_array_new_with_free_func(g_free);
    n = g_list_model_get_n_items(files);
    for (i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(files, i);

        g_ptr_array_add(items, g_file_get_path(f));
        g_object_unref(f);
    }
    g_object_unref(files);
    if (items->len > 0 && S.work_path != NULL)
        start_change(TRUE, items);
    else
        g_ptr_array_free(items, TRUE);
}

void saves_import_psus(char **paths) /* also for the test script */
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; paths[i] != NULL; i++)
        g_ptr_array_add(items, g_strdup(paths[i]));
    if (S.work_path != NULL)
        start_change(TRUE, items);
    else
        g_ptr_array_free(items, TRUE);
}

static void on_import(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose the saves to put on the card (.psu)"));
    gtk_file_filter_set_name(flt, _("PS2 saves (.psu)"));
    gtk_file_filter_add_suffix(flt, "psu");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(A.win), NULL, on_psus_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

static void free_closure_data(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void on_delete_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    char *name = data;

    (void)d;
    if (!strcmp(response, "delete") && S.work_path != NULL) {
        GPtrArray *items = g_ptr_array_new_with_free_func(g_free);

        g_ptr_array_add(items, g_strdup(name));
        start_change(FALSE, items);
    }
}

void saves_delete(const char *name) /* also for the test script */
{
    AdwDialog *d;
    char *body;

    if (S.work_path == NULL)
        return;
    body = g_strdup_printf(_("%s will be removed from the card. A backup of the card is kept first."), name);
    d = adw_alert_dialog_new(_("Delete this save?"), body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "delete", _("Delete"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect_data(d, "response", G_CALLBACK(on_delete_response), g_strdup(name), free_closure_data, 0);
    adw_dialog_present(d, GTK_WIDGET(A.win));
    g_free(body);
}

static void on_delete(GtkButton *b, gpointer d)
{
    (void)d;
    saves_delete(g_object_get_data(G_OBJECT(b), "save"));
}

static void on_export_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *name = data;

    if (f != NULL && S.work_path != NULL) {
        char *path = g_file_get_path(f);
        mc_t mc;
        int rc = mc_open(&mc, S.work_path, 0);

        if (rc == MC_OK) {
            rc = mc_export_psu(&mc, name, path);
            mc_close(&mc);
        }
        if (rc == MC_OK)
            toast(_("Save exported"));
        else
            show_error(_("The save could not be exported"), mc_error_text(rc));
        g_free(path);
    }
    if (f != NULL)
        g_object_unref(f);
    g_free(name);
}

static void on_export(GtkButton *b, gpointer d)
{
    const char *name = g_object_get_data(G_OBJECT(b), "save");
    GtkFileDialog *dlg = gtk_file_dialog_new();
    char *initial = g_strdup_printf("%s.psu", name);

    (void)d;
    gtk_file_dialog_set_title(dlg, _("Export the save"));
    gtk_file_dialog_set_initial_name(dlg, initial);
    gtk_file_dialog_save(dlg, GTK_WINDOW(A.win), NULL, on_export_chosen, g_strdup(name));
    g_object_unref(dlg);
    g_free(initial);
}

static void on_copy_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);

    (void)data;
    if (f != NULL && S.work_path != NULL) {
        GFile *from = g_file_new_for_path(S.work_path);
        GError *err = NULL;

        if (g_file_copy(from, f, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err))
            toast(_("Card copied"));
        else {
            show_error(_("The card could not be copied"), err->message);
            g_clear_error(&err);
        }
        g_object_unref(from);
    }
    if (f != NULL)
        g_object_unref(f);
}

static void on_copy(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    char *initial = S.remote_name ? g_strdup(S.remote_name) : S.work_path ? g_path_get_basename(S.work_path) : g_strdup("card.bin");

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Save a copy of the card"));
    gtk_file_dialog_set_initial_name(dlg, initial);
    gtk_file_dialog_save(dlg, GTK_WINDOW(A.win), NULL, on_copy_chosen, NULL);
    g_object_unref(dlg);
    g_free(initial);
}

static void on_backups(GtkButton *b, gpointer d)
{
    char *dir = backups_dir();
    GFile *f;
    GtkFileLauncher *l;

    (void)b;
    (void)d;
    g_mkdir_with_parents(dir, 0755);
    f = g_file_new_for_path(dir);
    l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, GTK_WINDOW(A.win), NULL, NULL, NULL);
    g_object_unref(l);
    g_object_unref(f);
    g_free(dir);
}

/* ---- choosing the card ----------------------------------------------------------------------------- */

typedef struct {
    char *name, *dest;
} Fetch;

static void fetch_free(gpointer p)
{
    Fetch *f = p;

    g_free(f->name);
    g_free(f->dest);
    g_free(f);
}

static gboolean job_fetch(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Fetch *f = data;

    (void)cb;
    (void)pd;
    (void)notes;
    return ops_vmc_fetch(A.ops, f->name, f->dest, error);
}

static void after_fetch(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        load_card();
    else {
        g_clear_pointer(&S.work_path, g_free);
        g_clear_pointer(&S.remote_name, g_free);
        show_card();
    }
}

/* opens a card of the disk: it is copied to the cache of this computer first */
void saves_open_remote(const char *name)
{
    Fetch *f = g_new0(Fetch, 1);
    char *dir = g_build_filename(g_get_user_cache_dir(), "ps2gm", "vmc", NULL);

    g_mkdir_with_parents(dir, 0700);
    f->name = g_strdup(name);
    f->dest = g_build_filename(dir, name, NULL);
    g_free(dir);
    g_free(S.work_path);
    g_free(S.remote_name);
    S.work_path = g_strdup(f->dest);
    S.remote_name = g_strdup(name);
    S.updating = TRUE; /* show the card in the selector without opening it a second time */
    for (guint i = 0; i < S.card_names->len; i++)
        if (!g_strcmp0(g_ptr_array_index(S.card_names, i), name))
            adw_combo_row_set_selected(ADW_COMBO_ROW(S.combo), i + 1);
    S.updating = FALSE;
    run_job(_("Reading the memory card"), job_fetch, f, fetch_free, after_fetch);
}

void saves_open_file(const char *path)
{
    g_free(S.work_path);
    g_free(S.remote_name);
    S.work_path = g_strdup(path);
    S.remote_name = NULL;
    load_card();
}

static void on_card_selected(GObject *combo, GParamSpec *ps, gpointer data)
{
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));

    (void)ps;
    (void)data;
    if (S.updating || sel == 0 || sel > S.card_names->len)
        return;
    saves_open_remote(g_ptr_array_index(S.card_names, sel - 1));
}

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);

    (void)data;
    if (f != NULL) {
        char *path = g_file_get_path(f);

        saves_open_file(path);
        g_free(path);
        g_object_unref(f);
    }
}

static void on_open_file(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose a memory card file"));
    gtk_file_filter_set_name(flt, _("Memory cards (.bin)"));
    gtk_file_filter_add_suffix(flt, "bin");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

/* the disk was read (or disconnected): the list of cards on it */
void rebuild_saves(void)
{
    guint i;

    S.updating = TRUE;
    gtk_string_list_splice(S.model, 0, g_list_model_get_n_items(G_LIST_MODEL(S.model)), NULL);
    g_ptr_array_set_size(S.card_names, 0);
    gtk_string_list_append(S.model, A.lib && A.lib->vmcs->len ? _("Choose a card on the disk...") : _("No cards on the disk"));
    for (i = 0; A.lib && i < A.lib->vmcs->len; i++) {
        LibFile *f = g_ptr_array_index(A.lib->vmcs, i);
        char *label;

        if (strstr(f->name, "ps2gm-new")) /* a leftover of an interrupted save: not a card to open */
            continue;
        label = g_strdup_printf("%s · %u MB", f->name, (guint)(f->size / (1024 * 1024)));
        g_ptr_array_add(S.card_names, g_strdup(f->name));
        gtk_string_list_append(S.model, label);
        if (!g_strcmp0(f->name, S.remote_name))
            adw_combo_row_set_selected(ADW_COMBO_ROW(S.combo), S.card_names->len);
        g_free(label);
    }
    if (S.remote_name == NULL || !A.lib)
        adw_combo_row_set_selected(ADW_COMBO_ROW(S.combo), 0);
    S.updating = FALSE;
    if (A.lib == NULL && S.remote_name != NULL) { /* the disk is gone: so is its card */
        g_clear_pointer(&S.work_path, g_free);
        g_clear_pointer(&S.remote_name, g_free);
        g_clear_pointer(&S.saves, g_ptr_array_unref);
        gtk_label_set_text(GTK_LABEL(S.status), "");
        show_card();
    }
}

/* ---- the page -------------------------------------------------------------------------------------- */

GtkWidget *build_saves_page(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new(), *clamp = adw_clamp_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    GtkWidget *eyebrow = gtk_label_new(NULL), *title = gtk_label_new(_("Saves")), *sub = gtk_label_new(_("Game saves on your virtual memory cards."));
    GtkWidget *group = adw_preferences_group_new(), *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *file_btn = gtk_button_new_with_label(_("Open a card file...")), *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char *eb = g_utf8_strup(_("Memory cards"), -1);

    gtk_label_set_text(GTK_LABEL(eyebrow), eb);
    g_free(eb);
    gtk_widget_add_css_class(eyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0);
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(sub, "page-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);

    S.model = gtk_string_list_new(NULL);
    S.card_names = g_ptr_array_new_with_free_func(g_free);
    S.combo = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(S.combo), _("Card"));
    adw_combo_row_set_model(ADW_COMBO_ROW(S.combo), G_LIST_MODEL(S.model));
    gtk_string_list_append(S.model, _("No cards on the disk"));
    g_signal_connect(S.combo, "notify::selected", G_CALLBACK(on_card_selected), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), S.combo);
    {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Another card file"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("For example a card on a USB stick connected to this computer"));
        gtk_widget_set_valign(file_btn, GTK_ALIGN_CENTER);
        g_signal_connect(file_btn, "clicked", G_CALLBACK(on_open_file), NULL);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), file_btn);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    S.status = gtk_label_new("");
    gtk_widget_add_css_class(S.status, "page-sub");
    gtk_label_set_xalign(GTK_LABEL(S.status), 0);
    gtk_widget_set_hexpand(S.status, TRUE);
    gtk_label_set_wrap(GTK_LABEL(S.status), TRUE);
    S.import_btn = gtk_button_new_with_label(_("Import saves..."));
    gtk_widget_add_css_class(S.import_btn, "suggested-action");
    gtk_widget_add_css_class(S.import_btn, "pill");
    g_signal_connect(S.import_btn, "clicked", G_CALLBACK(on_import), NULL);
    S.copy_btn = gtk_button_new_with_label(_("Save a copy of the card..."));
    gtk_widget_add_css_class(S.copy_btn, "btn-secondary");
    gtk_widget_add_css_class(S.copy_btn, "pill");
    g_signal_connect(S.copy_btn, "clicked", G_CALLBACK(on_copy), NULL);
    S.backups_btn = gtk_button_new_with_label(_("Backups"));
    gtk_widget_set_tooltip_text(S.backups_btn, _("Before every change a copy of the card is kept here (the last five)"));
    gtk_widget_add_css_class(S.backups_btn, "btn-secondary");
    gtk_widget_add_css_class(S.backups_btn, "pill");
    g_signal_connect(S.backups_btn, "clicked", G_CALLBACK(on_backups), NULL);
    gtk_box_append(GTK_BOX(head), S.status);
    gtk_box_append(GTK_BOX(tools), S.import_btn);
    gtk_box_append(GTK_BOX(tools), S.copy_btn);
    gtk_box_append(GTK_BOX(tools), S.backups_btn);

    S.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(S.list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(S.list, "boxed-list");
    gtk_widget_set_visible(S.list, FALSE);
    S.empty = gtk_label_new(_("This card has no saves yet."));
    gtk_widget_add_css_class(S.empty, "page-sub");
    gtk_widget_set_visible(S.empty, FALSE);

    gtk_widget_set_margin_top(page, 22);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    gtk_box_append(GTK_BOX(page), eyebrow);
    gtk_box_append(GTK_BOX(page), title);
    gtk_box_append(GTK_BOX(page), sub);
    gtk_box_append(GTK_BOX(page), group);
    gtk_box_append(GTK_BOX(page), head);
    gtk_box_append(GTK_BOX(page), tools);
    gtk_box_append(GTK_BOX(page), S.list);
    gtk_box_append(GTK_BOX(page), S.empty);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
    adw_clamp_set_child(ADW_CLAMP(clamp), page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    show_card();
    return scroll;
}
