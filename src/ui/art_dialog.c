/*
 * The art dialog of one game: download everything, or change a single picture (front cover, background...)
 * by downloading it again, choosing a file on this computer, or giving a web address.
 */
#include "app.h"

typedef struct {
    char *id, *title;
    Ps2gmPlatform platform;
    AdwDialog *dialog;
    AdwComboRow *combo;
    GtkWidget *url_row;
} ArtDlg;

typedef struct {
    char *id, *key, *source; /* source NULL: download the usual picture */
    Ps2gmPlatform platform;
    gboolean whole;
} SetArt;

static void setart_free(gpointer p)
{
    SetArt *s = p;

    g_free(s->id);
    g_free(s->key);
    g_free(s->source);
    g_free(s);
}

static gboolean job_set_art(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    SetArt *s = data;

    (void)notes;
    return ops_set_art(A.ops, s->id, s->platform, s->key, s->source, s->whole, cb, pd, error);
}

void do_set_art(Ps2gmPlatform platform, const char *id, const char *key, const char *source, gboolean whole)
{
    SetArt *s = g_new0(SetArt, 1);

    s->whole = whole;
    s->platform = platform;
    s->id = g_strdup(id);
    s->key = g_strdup(key);
    s->source = g_strdup(source);
    run_job(_("Changing the picture"), job_set_art, s, setart_free, after_change);
}

static const char *selected_key(ArtDlg *d)
{
    guint n, sel = adw_combo_row_get_selected(d->combo);
    const ArtType *types = art_types(&n);

    return sel < n ? types[sel].key : types[0].key;
}

/* copies what is needed before closing the dialog (closing frees ArtDlg) */
static void start_set_art(ArtDlg *d, const char *source)
{
    do_set_art(d->platform, d->id, selected_key(d), source, FALSE);
    adw_dialog_close(d->dialog);
}

typedef struct {
    Ps2gmPlatform platform;
    char *id;
} CoverFor;

static void cover_for_free(gpointer p)
{
    CoverFor *c = p;

    g_free(c->id);
    g_free(c);
}

static void on_cover_picked(const char *path, gboolean whole, gpointer data)
{
    CoverFor *c = data;

    do_set_art(c->platform, c->id, "COV", path, whole);
}

static void on_cover_carousel(GtkButton *b, gpointer data)
{
    ArtDlg *d = data;
    CoverFor *c = g_new0(CoverFor, 1);

    (void)b;
    c->platform = d->platform;
    c->id = g_strdup(d->id);
    show_cover_picker(d->platform, d->id, d->title, on_cover_picked, c, cover_for_free);
    adw_dialog_close(d->dialog); /* frees d; the picker has its own copies */
}

static void on_all_missing(GtkButton *b, gpointer data)
{
    ArtDlg *d = data;

    (void)b;
    do_update_art(d->id, d->platform, d->title, FALSE);
    adw_dialog_close(d->dialog);
}

static void on_all_replace(GtkButton *b, gpointer data)
{
    ArtDlg *d = data;

    (void)b;
    do_update_art(d->id, d->platform, d->title, TRUE);
    adw_dialog_close(d->dialog);
}

static void on_one_download(GtkButton *b, gpointer data)
{
    (void)b;
    start_set_art(data, NULL);
}

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path;

    if (f == NULL)
        return;
    path = g_file_get_path(f);
    g_object_unref(f);
    if (path != NULL)
        start_set_art(data, path);
    g_free(path);
}

static void on_one_file(GtkButton *b, gpointer data)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    gtk_file_dialog_set_title(dlg, _("Choose a picture"));
    gtk_file_filter_set_name(flt, _("Pictures (PNG, JPG, BMP)"));
    gtk_file_filter_add_mime_type(flt, "image/png");
    gtk_file_filter_add_mime_type(flt, "image/jpeg");
    gtk_file_filter_add_mime_type(flt, "image/bmp");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, data);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

static void on_url_apply(AdwEntryRow *row, gpointer data)
{
    const char *url = gtk_editable_get_text(GTK_EDITABLE(row));

    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
        show_error(_("Not a web address"), _("The address of the picture must start with http:// or https://"));
        return;
    }
    start_set_art(data, url);
}

static void on_dialog_closed(AdwDialog *dialog, gpointer data)
{
    ArtDlg *d = data;

    (void)dialog;
    g_free(d->id);
    g_free(d->title);
    g_free(d);
}

void show_art_dialog(Ps2gmPlatform platform, const char *id, const char *title)
{
    ArtDlg *d = g_new0(ArtDlg, 1);
    GtkWidget *view = adw_toolbar_view_new(), *header = adw_header_bar_new();
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *all = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *one = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkStringList *names = gtk_string_list_new(NULL);
    GtkWidget *row, *btn;
    const ArtType *types;
    guint i, n;
    char *sub;

    d->id = g_strdup(id);
    d->title = g_strdup(title);
    d->platform = platform;

    {
        AdwPreferencesGroup *cover = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

        adw_preferences_group_set_title(cover, _("Front cover"));
        row = button_row(_("Choose from several covers"), _("Flat and 3D box art found online, or add your own"), _("Carousel..."), NULL, &btn);
        gtk_widget_add_css_class(btn, "suggested-action");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_cover_carousel), d);
        adw_preferences_group_add(cover, row);
        adw_preferences_page_add(page, cover);
    }
    adw_preferences_group_set_title(all, _("All pictures"));
    row = button_row(_("Download what is missing"), _("Fills the gaps and keeps what is on the disk"), _("Download"), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_all_missing), d);
    adw_preferences_group_add(all, row);
    row = button_row(_("Download everything again"), _("Replaces the pictures on the disk, also the ones you chose"), _("Replace"), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_all_replace), d);
    adw_preferences_group_add(all, row);

    adw_preferences_group_set_title(one, _("Change one picture"));
    types = art_types(&n);
    for (i = 0; i < n; i++)
        gtk_string_list_append(names, _(types[i].label));
    d->combo = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->combo), _("Picture"));
    adw_combo_row_set_model(d->combo, G_LIST_MODEL(names));
    adw_preferences_group_add(one, GTK_WIDGET(d->combo));
    row = button_row(_("From the internet"), _("The usual source, again"), _("Download"), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_one_download), d);
    adw_preferences_group_add(one, row);
    row = button_row(_("From a file on this computer"), _("PNG, JPG or BMP; it is resized for the PS2"), _("Choose..."), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_one_file), d);
    adw_preferences_group_add(one, row);
    d->url_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->url_row), _("From a web address (press Enter)"));
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(d->url_row), TRUE);
    g_signal_connect(d->url_row, "apply", G_CALLBACK(on_url_apply), d);
    adw_preferences_group_add(one, d->url_row);

    adw_preferences_page_add(page, all);
    adw_preferences_page_add(page, one);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));

    d->dialog = adw_dialog_new();
    sub = g_strdup_printf(_("Art: %s"), title && *title ? title : id);
    adw_dialog_set_title(d->dialog, sub);
    g_free(sub);
    adw_dialog_set_child(d->dialog, view);
    adw_dialog_set_content_width(d->dialog, 480);
    adw_dialog_set_content_height(d->dialog, 560);
    g_signal_connect(d->dialog, "closed", G_CALLBACK(on_dialog_closed), d);
    adw_dialog_present(d->dialog, GTK_WIDGET(A.win));
}
