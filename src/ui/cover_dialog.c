/*
 * The cover picker: a carousel of the front covers that can be found online for a game, plus pictures you add
 * yourself (a file, or a web address). Used before installing a game (Add games) and for a game already on the disk.
 * Nothing is written to the disk here: the chosen picture is handed to the caller.
 */
#include <glib/gstdio.h>

#include "app.h"

typedef struct {
    char *path, *label;
    gboolean whole; /* keep the whole picture instead of cropping it to the cover frame */
    GtkWidget *widget;
} CoverPage;

typedef struct {
    Ps2gmPlatform platform;
    char *id, *title, *dir;
    CoverChosen cb;
    gpointer data;
    GDestroyNotify free_data;
    gint refs; /* the dialog and the downloads still running */
    gboolean open;
    guint user_files;

    AdwDialog *dialog;
    GtkWidget *stack, *whole_row, *use_btn, *prev_btn, *next_btn, *label, *url_row;
    AdwCarousel *carousel;
    GPtrArray *pages; /* CoverPage, in carousel order */
    guint current;
} Picker;

static void page_free(gpointer p)
{
    CoverPage *c = p;

    g_free(c->path);
    g_free(c->label);
    g_free(c);
}

static void picker_unref(Picker *p)
{
    GDir *d;
    const char *n;

    if (!g_atomic_int_dec_and_test(&p->refs))
        return;
    /* the downloaded candidates are only needed while the dialog is open */
    d = g_dir_open(p->dir, 0, NULL);
    while (d && (n = g_dir_read_name(d)) != NULL) {
        char *f = g_build_filename(p->dir, n, NULL);

        g_unlink(f);
        g_free(f);
    }
    if (d)
        g_dir_close(d);
    g_rmdir(p->dir);
    g_ptr_array_free(p->pages, TRUE);
    if (p->free_data)
        p->free_data(p->data);
    g_free(p->id);
    g_free(p->title);
    g_free(p->dir);
    g_free(p);
}

/* ---- the carousel -------------------------------------------------------------------------------- */

static void update_controls(Picker *p)
{
    gboolean any = p->pages->len > 0;

    gtk_widget_set_sensitive(p->use_btn, any);
    gtk_widget_set_sensitive(p->whole_row, any);
    gtk_widget_set_sensitive(p->prev_btn, any && p->current > 0);
    gtk_widget_set_sensitive(p->next_btn, any && p->current + 1 < p->pages->len);
    if (any) {
        CoverPage *c = g_ptr_array_index(p->pages, p->current);
        char *text = g_strdup_printf(_("%s  (%u of %u)"), c->label, p->current + 1, p->pages->len);

        gtk_label_set_text(GTK_LABEL(p->label), text);
        g_free(text);
        g_signal_handlers_block_matched(p->whole_row, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
        adw_switch_row_set_active(ADW_SWITCH_ROW(p->whole_row), c->whole);
        g_signal_handlers_unblock_matched(p->whole_row, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
    } else
        gtk_label_set_text(GTK_LABEL(p->label), "");
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), any ? "carousel" : "empty");
}

static void on_page_changed(AdwCarousel *c, guint index, gpointer data)
{
    Picker *p = data;

    (void)c;
    p->current = index;
    update_controls(p);
}

static void on_whole_toggled(GObject *row, GParamSpec *ps, gpointer data)
{
    Picker *p = data;

    (void)ps;
    if (p->pages->len > 0)
        ((CoverPage *)g_ptr_array_index(p->pages, p->current))->whole = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
}

static void on_prev(GtkButton *b, gpointer data)
{
    Picker *p = data;

    (void)b;
    if (p->current > 0)
        adw_carousel_scroll_to(p->carousel, ((CoverPage *)g_ptr_array_index(p->pages, p->current - 1))->widget, TRUE);
}

static void on_next(GtkButton *b, gpointer data)
{
    Picker *p = data;

    (void)b;
    if (p->current + 1 < p->pages->len)
        adw_carousel_scroll_to(p->carousel, ((CoverPage *)g_ptr_array_index(p->pages, p->current + 1))->widget, TRUE);
}

/* adds a picture to the carousel; FALSE (and nothing added) if it is not a picture */
static gboolean add_page(Picker *p, const char *path, const char *label, gboolean whole, gboolean show_it)
{
    GdkTexture *probe = gdk_texture_new_from_filename(path, NULL);
    CoverPage *c;
    GtkWidget *pic, *frame;

    if (probe == NULL)
        return FALSE;
    g_object_unref(probe);
    c = g_new0(CoverPage, 1);
    c->path = g_strdup(path);
    c->label = g_strdup(label);
    c->whole = whole;
    pic = gtk_picture_new_for_filename(path);
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_widget_add_css_class(pic, "cover");
    gtk_widget_set_size_request(pic, 260, 360);
    frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(frame), pic);
    gtk_widget_set_margin_start(frame, 40);
    gtk_widget_set_margin_end(frame, 40);
    c->widget = frame;
    g_ptr_array_add(p->pages, c);
    adw_carousel_append(p->carousel, frame);
    if (show_it) {
        p->current = p->pages->len - 1;
        adw_carousel_scroll_to(p->carousel, frame, TRUE);
    }
    update_controls(p);
    return TRUE;
}

/* ---- finding the covers online ------------------------------------------------------------------- */

static void candidates_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Picker *p = data;

    (void)src;
    (void)c;
    g_task_return_pointer(task, art_cover_candidates(p->id, p->platform, p->dir), (GDestroyNotify)g_ptr_array_unref);
}

static void candidates_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Picker *p = data;
    GPtrArray *list = g_task_propagate_pointer(G_TASK(res), NULL);
    guint i;

    (void)src;
    if (p->open && list != NULL) {
        for (i = 0; i < list->len; i++) {
            CoverCandidate *c = g_ptr_array_index(list, i);

            add_page(p, c->path, _(c->label), c->whole, FALSE); /* labels are marked with N_ in art.c */
        }
        if (p->pages->len == 0)
            update_controls(p);
    }
    if (list != NULL)
        g_ptr_array_unref(list);
    picker_unref(p);
}

/* ---- adding your own ------------------------------------------------------------------------------ */

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    Picker *p = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    char *path, *base;

    if (f != NULL && p->open) {
        path = g_file_get_path(f);
        base = path ? g_path_get_basename(path) : NULL;
        if (path == NULL || !add_page(p, path, base, FALSE, TRUE))
            show_error(_("That is not a picture"), _("Choose a PNG, JPG or BMP image."));
        g_free(path);
        g_free(base);
    }
    if (f != NULL)
        g_object_unref(f);
    picker_unref(p);
}

static void on_add_file(GtkButton *b, gpointer data)
{
    Picker *p = data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    gtk_file_dialog_set_title(dlg, _("Choose a cover picture"));
    gtk_file_filter_set_name(flt, _("Pictures (PNG, JPG, BMP)"));
    gtk_file_filter_add_mime_type(flt, "image/png");
    gtk_file_filter_add_mime_type(flt, "image/jpeg");
    gtk_file_filter_add_mime_type(flt, "image/bmp");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_atomic_int_inc(&p->refs);
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_file_chosen, p);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

typedef struct {
    Picker *p;
    char *url, *path;
    gboolean ok;
} UrlJob;

static void url_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    UrlJob *j = data;
    GBytes *bytes = ps2gm_http_get(j->url, NULL);

    (void)src;
    (void)c;
    if (bytes != NULL) {
        gsize n;
        const void *d = g_bytes_get_data(bytes, &n);

        j->ok = g_file_set_contents(j->path, d, n, NULL);
        g_bytes_unref(bytes);
    }
    g_task_return_boolean(task, j->ok);
}

static void url_done(GObject *src, GAsyncResult *res, gpointer data)
{
    UrlJob *j = data;

    (void)src;
    (void)res;
    if (j->p->open) {
        gtk_widget_set_sensitive(j->p->url_row, TRUE);
        if (!j->ok || !add_page(j->p, j->path, _("From the web address"), FALSE, TRUE))
            show_error(_("No picture there"), _("Nothing that looks like a picture could be downloaded from that address."));
        else
            gtk_editable_set_text(GTK_EDITABLE(j->p->url_row), "");
    }
    picker_unref(j->p);
    g_free(j->url);
    g_free(j->path);
    g_free(j);
}

static void on_url_apply(AdwEntryRow *row, gpointer data)
{
    Picker *p = data;
    const char *url = gtk_editable_get_text(GTK_EDITABLE(row));
    UrlJob *j;
    GTask *task;

    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
        show_error(_("Not a web address"), _("The address of the picture must start with http:// or https://"));
        return;
    }
    j = g_new0(UrlJob, 1);
    j->p = p;
    j->url = g_strdup(url);
    j->path = g_strdup_printf("%s/web-%u.img", p->dir, ++p->user_files);
    g_atomic_int_inc(&p->refs);
    gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE); /* until it is downloaded */
    task = g_task_new(NULL, NULL, url_done, j);
    g_task_set_task_data(task, j, NULL);
    g_task_run_in_thread(task, url_thread);
    g_object_unref(task);
}

/* ---- choosing -------------------------------------------------------------------------------------- */

static void on_use(GtkButton *b, gpointer data)
{
    Picker *p = data;
    CoverPage *c;
    char *pending_dir, *base, *dest;
    GFile *from, *to;
    GError *err = NULL;
    const char *dot;

    (void)b;
    if (p->pages->len == 0)
        return;
    c = g_ptr_array_index(p->pages, p->current);
    /* a copy that outlives the dialog (the candidates are deleted when it closes) */
    pending_dir = g_build_filename(g_get_user_cache_dir(), "ps2gm", "pending", NULL);
    g_mkdir_with_parents(pending_dir, 0755);
    dot = strrchr(c->path, '.');
    base = g_strdup_printf("%s-cover%s", p->id, dot && strlen(dot) <= 5 ? dot : ".img");
    dest = g_build_filename(pending_dir, base, NULL);
    from = g_file_new_for_path(c->path);
    to = g_file_new_for_path(dest);
    if (!g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err)) {
        show_error(_("Cannot keep that picture"), err->message);
        g_clear_error(&err);
    } else {
        if (p->cb != NULL)
            p->cb(dest, c->whole, p->data);
        adw_dialog_close(p->dialog);
    }
    g_object_unref(from);
    g_object_unref(to);
    g_free(pending_dir);
    g_free(base);
    g_free(dest);
}

static void on_closed(AdwDialog *d, gpointer data)
{
    Picker *p = data;

    (void)d;
    p->open = FALSE;
    picker_unref(p);
}

/* ---- the dialog ------------------------------------------------------------------------------------ */

static GtkWidget *round_button(const char *icon, GCallback cb, Picker *p)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);

    gtk_widget_add_css_class(b, "circular");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", cb, p);
    return b;
}

void show_cover_picker(Ps2gmPlatform platform, const char *id, const char *title, CoverChosen cb, gpointer data,
                       GDestroyNotify free_data)
{
    Picker *p = g_new0(Picker, 1);
    GtkWidget *view = adw_toolbar_view_new(), *header = adw_header_bar_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *stage = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *loading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *spinner = gtk_spinner_new(), *dots = adw_carousel_indicator_dots_new(), *empty = adw_status_page_new();
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *group = adw_preferences_group_new(), *row, *btn;
    GTask *task;
    char *heading;

    p->platform = platform;
    p->id = g_strdup(id);
    p->title = g_strdup(title);
    p->dir = g_build_filename(g_get_user_cache_dir(), "ps2gm", "covers", id, NULL);
    p->cb = cb;
    p->data = data;
    p->free_data = free_data;
    p->refs = 2; /* the dialog + the search that starts below */
    p->open = TRUE;
    p->pages = g_ptr_array_new_with_free_func(page_free);
    g_mkdir_with_parents(p->dir, 0755);

    p->carousel = ADW_CAROUSEL(adw_carousel_new());
    gtk_widget_set_hexpand(GTK_WIDGET(p->carousel), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(p->carousel), -1, 380);
    adw_carousel_indicator_dots_set_carousel(ADW_CAROUSEL_INDICATOR_DOTS(dots), p->carousel);
    g_signal_connect(p->carousel, "page-changed", G_CALLBACK(on_page_changed), p);
    p->prev_btn = round_button("go-previous-symbolic", G_CALLBACK(on_prev), p);
    p->next_btn = round_button("go-next-symbolic", G_CALLBACK(on_next), p);
    gtk_box_append(GTK_BOX(stage), p->prev_btn);
    gtk_box_append(GTK_BOX(stage), GTK_WIDGET(p->carousel));
    gtk_box_append(GTK_BOX(stage), p->next_btn);
    gtk_box_append(GTK_BOX(wrap), stage);
    gtk_box_append(GTK_BOX(wrap), dots);

    gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
    gtk_widget_set_size_request(spinner, 32, 32);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(loading), spinner);
    gtk_box_append(GTK_BOX(loading), gtk_label_new(_("Looking for covers online...")));
    gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), _("No cover found online"));
    adw_status_page_set_description(ADW_STATUS_PAGE(empty), _("Add a picture of your own below."));
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "image-missing-symbolic");

    p->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(p->stack), loading, "loading");
    gtk_stack_add_named(GTK_STACK(p->stack), wrap, "carousel");
    gtk_stack_add_named(GTK_STACK(p->stack), empty, "empty");
    gtk_widget_set_size_request(p->stack, -1, 420);

    p->label = gtk_label_new("");
    gtk_widget_add_css_class(p->label, "card-sub");

    p->whole_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->whole_row), _("Keep the whole picture"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(p->whole_row), _("Off: the picture is cut to the shape of the cover frame. On: it is only made smaller."));
    g_signal_connect(p->whole_row, "notify::active", G_CALLBACK(on_whole_toggled), p);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), p->whole_row);
    row = button_row(_("Add a picture from a file"), _("PNG, JPG or BMP"), _("Choose..."), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_file), p);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    p->url_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->url_row), _("Add a picture from a web address (press Enter)"));
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(p->url_row), TRUE);
    g_signal_connect(p->url_row, "apply", G_CALLBACK(on_url_apply), p);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), p->url_row);

    p->use_btn = gtk_button_new_with_label(_("Use this cover"));
    gtk_widget_add_css_class(p->use_btn, "pill");
    gtk_widget_add_css_class(p->use_btn, "suggested-action");
    gtk_widget_set_halign(p->use_btn, GTK_ALIGN_CENTER);
    g_signal_connect(p->use_btn, "clicked", G_CALLBACK(on_use), p);

    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_box_append(GTK_BOX(box), p->stack);
    gtk_box_append(GTK_BOX(box), p->label);
    gtk_box_append(GTK_BOX(box), group);
    gtk_box_append(GTK_BOX(box), p->use_btn);
    {
        GtkWidget *scroll = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
    }
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    p->dialog = adw_dialog_new();
    heading = g_strdup_printf(_("Cover: %s"), title && *title ? title : id);
    adw_dialog_set_title(p->dialog, heading);
    g_free(heading);
    adw_dialog_set_child(p->dialog, view);
    adw_dialog_set_content_width(p->dialog, 600);
    adw_dialog_set_content_height(p->dialog, 820);
    g_signal_connect(p->dialog, "closed", G_CALLBACK(on_closed), p);

    update_controls(p);
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "loading"); /* until the search is done */
    gtk_widget_set_sensitive(p->use_btn, FALSE);
    adw_dialog_present(p->dialog, GTK_WIDGET(A.win));

    task = g_task_new(NULL, NULL, candidates_done, p);
    g_task_set_task_data(task, p, NULL);
    g_task_run_in_thread(task, candidates_thread);
    g_object_unref(task);
}
