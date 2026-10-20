/* Add games page: install a PS2 ISO, convert and install a PS1 BIN/CUE game. */
#include <glib/gstdio.h>

#include "app.h"
#include "ziso.h"

/* ---- the cover of the game about to be installed -------------------------------------------- */

static void slot_set(CoverSlot *s, const char *path, gboolean whole)
{
    g_free(s->path);
    s->path = g_strdup(path);
    s->whole = whole;
    gtk_picture_set_filename(GTK_PICTURE(s->thumb), path);
    gtk_widget_set_visible(s->thumb, path != NULL);
    gtk_widget_set_visible(s->reset_btn, path != NULL);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->row), path ? _("Your choice") : _("Automatic: the usual cover is downloaded"));
}

/* script mode and tests: the same as choosing a cover in the carousel */
void set_install_cover(Ps2gmPlatform platform, const char *path, gboolean whole)
{
    slot_set(platform == PS2GM_PS2 ? &A.iso_cover : &A.ps1_cover, path, whole);
}

static void on_iso_cover_chosen(const char *path, gboolean whole, gpointer data)
{
    (void)data;
    slot_set(&A.iso_cover, path, whole);
}

static void on_ps1_cover_chosen(const char *path, gboolean whole, gpointer data)
{
    (void)data;
    slot_set(&A.ps1_cover, path, whole);
}

static void on_choose_iso_cover(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (A.iso_ok)
        show_cover_picker(PS2GM_PS2, A.iso_info.id, gtk_editable_get_text(GTK_EDITABLE(A.iso_title_row)), on_iso_cover_chosen, NULL, NULL);
}

static void on_choose_ps1_cover(GtkButton *b, gpointer d)
{
    Ps1Disc *first = A.discs->len ? g_ptr_array_index(A.discs, 0) : NULL;

    (void)b;
    (void)d;
    if (first != NULL && first->id != NULL)
        show_cover_picker(PS2GM_PS1, first->id, gtk_editable_get_text(GTK_EDITABLE(A.ps1_title_row)), on_ps1_cover_chosen, NULL, NULL);
}

static void on_reset_iso_cover(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    slot_set(&A.iso_cover, NULL, FALSE);
}

static void on_reset_ps1_cover(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    slot_set(&A.ps1_cover, NULL, FALSE);
}

static void make_cover_row(CoverSlot *s, AdwPreferencesGroup *g, GCallback choose, GCallback reset)
{
    s->row = adw_action_row_new();
    s->thumb = gtk_picture_new();
    s->choose_btn = gtk_button_new_with_label(_("Choose..."));
    s->reset_btn = gtk_button_new_from_icon_name("edit-clear-symbolic");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->row), _("Cover"));
    gtk_picture_set_content_fit(GTK_PICTURE(s->thumb), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(s->thumb), TRUE);
    gtk_widget_set_size_request(s->thumb, 34, 48);
    gtk_widget_set_valign(s->thumb, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(s->row), s->thumb);
    gtk_widget_set_valign(s->choose_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(s->reset_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(s->reset_btn, "flat");
    gtk_widget_set_tooltip_text(s->reset_btn, _("Back to the automatic cover"));
    g_signal_connect(s->choose_btn, "clicked", choose, NULL);
    g_signal_connect(s->reset_btn, "clicked", reset, NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(s->row), s->reset_btn);
    adw_action_row_add_suffix(ADW_ACTION_ROW(s->row), s->choose_btn);
    gtk_widget_set_sensitive(s->choose_btn, FALSE);
    slot_set(s, NULL, FALSE);
    adw_preferences_group_add(g, s->row);
}

/* ---- add games: PS2 ---------------------------------------------------------------------------- */

/* several images chosen at once: what was found in each, and why one is left out (skip), if it is */
typedef struct {
    char *path, *id, *title, *media, *skip;
    guint64 size_kb, zso_kb;
    gboolean zso;
} BatchItem;

static GPtrArray *batch;          /* BatchItem; NULL when a single game is chosen */
static GtkWidget *batch_group;    /* the list of the games, in the PS2 card */
static GPtrArray *batch_rows;     /* its rows (widgets, not owned) */
static void batch_reset(void);
static void batch_refresh_ui(void);
static void install_batch(void);

/* the estimate of what compressing an ISO would save, worked out in the background after choosing it */
static guint estimate_generation;

typedef struct {
    guint generation;
    char *path;
    guint64 size_kb;
    double ratio;
    gboolean ok;
} Estimate;

static void estimate_free(gpointer p)
{
    Estimate *e = p;

    g_free(e->path);
    g_free(e);
}

static void estimate_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Estimate *e = data;

    (void)src;
    (void)c;
    e->ok = ziso_estimate(e->path, &e->ratio, NULL);
    g_task_return_boolean(task, e->ok);
}

static void set_pack_subtitle(const char *extra)
{
    char *text = g_strdup_printf("%s\n%s",
                                 _("Makes the game much smaller on the PS2 disk, but OPL needs ZSO support: try one game first. Takes a few minutes and some free space on this computer."),
                                 extra);

    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_pack_row), text);
    g_free(text);
}

static void estimate_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Estimate *e = data;

    (void)src;
    (void)res;
    if (e->generation == estimate_generation && A.iso_ok && !A.iso_is_zso) { /* still the game that is chosen */
        if (e->ok) {
            int percent = (int)((1.0 - e->ratio) * 100 + 0.5);
            char *saved = size_text((guint64)(e->size_kb * (1.0 - e->ratio))), *line;

            if (percent < 10)
                line = g_strdup_printf(_("Estimate for this game: only about %d%% smaller (%s). Probably not worth it."), percent < 0 ? 0 : percent, saved);
            else
                line = g_strdup_printf(_("Estimate for this game: it would be about %d%% smaller (%s less)."), percent, saved);
            set_pack_subtitle(line);
            g_free(line);
            g_free(saved);
        } else
            set_pack_subtitle("");
    }
    estimate_free(e);
}

static void start_estimate(void)
{
    Estimate *e = g_new0(Estimate, 1);
    GTask *task;

    e->generation = ++estimate_generation;
    e->path = g_strdup(A.iso_path);
    e->size_kb = A.iso_info.size_kb;
    set_pack_subtitle(_("Estimating how much it would save..."));
    task = g_task_new(NULL, NULL, estimate_done, e);
    g_task_set_task_data(task, e, NULL);
    g_task_run_in_thread(task, estimate_thread);
    g_object_unref(task);
}

void update_iso_widgets(void)
{
    if (batch != NULL) {
        batch_refresh_ui();
        return;
    }
    if (A.iso_ok) {
        char *sz = size_text(A.iso_info.size_kb), *sub = g_strdup_printf("%s  ·  %s  ·  \"%s\"", A.iso_info.id,
                                                                         !g_strcmp0(A.iso_info.media, "?") ? "disc" : A.iso_info.media, sz);

        adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_id_row), sub);
        g_free(sz);
        g_free(sub);
    } else
        adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_id_row), _("Choose an ISO file first"));
    gtk_widget_set_sensitive(A.iso_go, A.iso_ok && A.backend != NULL);
    gtk_widget_set_sensitive(A.iso_cover.choose_btn, A.iso_ok);
    gtk_widget_set_visible(A.iso_keep_row, A.iso_ok && A.iso_is_zso);
    gtk_widget_set_visible(A.iso_pack_row, A.iso_ok && !A.iso_is_zso);
    if (A.iso_ok && !A.iso_is_zso) {
        char *one[2] = {A.iso_path, NULL};

        verify_set_files(A.iso_verify, one, NULL);
    } else
        verify_set_files(A.iso_verify, NULL, A.iso_ok ? _("Only plain .iso images can be checked") : NULL);
}

void load_iso(const char *path)
{
    GError *err = NULL;

    batch_reset(); /* one game: back to the single game rows */

    g_free(A.iso_path);
    A.iso_path = g_strdup(path);
    A.iso_is_zso = FALSE;
    slot_set(&A.iso_cover, NULL, FALSE); /* a cover belongs to one game */
    hdl_iso_info_clear(&A.iso_info);
    if (ziso_is_zso(path)) {
        /* compressed: the game is read straight out of the file, nothing is unpacked yet */
        ZisoGame g;
        GStatBuf st;

        A.iso_ok = ziso_game_info(path, &g, &err);
        if (A.iso_ok) {
            A.iso_info.media = g_strdup(g.media);
            A.iso_info.id = g.id; /* taken over */
            A.iso_info.label = g.label;
            A.iso_info.size_kb = g.size_kb;
            A.iso_is_zso = TRUE;
            A.iso_zso_kb = g_stat(path, &st) == 0 ? (guint64)st.st_size / 1024 : g.size_kb;
        }
    } else
        A.iso_ok = hdl_iso_info(&A.tools, A.iso_path, &A.iso_info, &err);
    if (!A.iso_ok) {
        show_error(_("Not a PS2 game image"), err->message);
        g_clear_error(&err);
        g_clear_pointer(&A.iso_path, g_free);
    } else {
        char *t = ps2_title_lookup(A.iso_info.id);
        char *base = g_path_get_basename(A.iso_path), *dot = strrchr(base, '.');

        if (dot)
            *dot = '\0';
        gtk_editable_set_text(GTK_EDITABLE(A.iso_title_row), t ? t : (A.iso_info.label && *A.iso_info.label ? A.iso_info.label : base));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_file_row), A.iso_path);
        g_free(t);
        g_free(base);
        if (!A.iso_is_zso)
            start_estimate();
    }
    estimate_generation += A.iso_ok ? 0 : 1; /* a failed load: any estimate still running is for a file that is no longer chosen */
    update_iso_widgets();
}

static void on_iso_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    GPtrArray *paths;
    guint i, n;

    (void)data;
    if (files == NULL)
        return;
    paths = g_ptr_array_new_with_free_func(g_free);
    n = g_list_model_get_n_items(files);
    for (i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(files, i);

        g_ptr_array_add(paths, g_file_get_path(f));
        g_object_unref(f);
    }
    g_object_unref(files);
    g_ptr_array_add(paths, NULL);
    load_isos((char **)paths->pdata);
    g_ptr_array_free(paths, TRUE);
}

static void on_choose_iso(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose PS2 game images (several at once are installed one after the other)"));
    gtk_file_filter_set_name(flt, _("Disc images (.iso, .zso)"));
    gtk_file_filter_add_suffix(flt, "iso");
    gtk_file_filter_add_suffix(flt, "zso");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(A.win), NULL, on_iso_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

typedef struct {
    char *iso, *media, *id, *title, *cover; /* cover: a picture chosen in the carousel, or NULL */
    guint64 size_kb;                        /* what is copied to the disk: the file size of a compressed game */
    gboolean art, cover_whole;
    gboolean unpack_first;                  /* a .zso that is to go on the disk as a normal ISO */
    gboolean pack_first;                    /* an .iso that is to go on the disk compressed (ZSO) */
    gboolean zso_pad;                       /* a .zso to install as it is, but not a whole number of sectors long */
} Ps2Job;

static void ps2job_free(gpointer p)
{
    Ps2Job *j = p;

    g_free(j->iso);
    g_free(j->media);
    g_free(j->id);
    g_free(j->title);
    g_free(j->cover);
    g_free(j);
}

/* puts the cover chosen in the carousel on the disk, over the automatic one; a problem is a note, not a failure */
static void apply_cover(const char *cover, gboolean whole, Ps2gmPlatform platform, const char *id, GPtrArray *notes)
{
    GError *err = NULL;

    if (cover != NULL && id != NULL && !ops_set_art(A.ops, id, platform, "COV", cover, whole, NULL, NULL, &err)) {
        g_ptr_array_add(notes, g_strdup_printf("Your cover for %s could not be used: %s", id, err->message));
        g_clear_error(&err);
    }
}

static gboolean job_install_ps2(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Ps2Job *j = data;
    char *temp = NULL;
    gboolean ok;

    if (j->unpack_first) {
        /* for an OPL without ZSO support: a normal ISO goes on the disk; the temporary one is deleted afterwards */
        temp = g_strdup_printf("%s/ps2gm/unpacked/%s.iso", g_get_user_cache_dir(), j->id);
        if (!ziso_unpack(j->iso, temp, cb, pd, error)) {
            g_free(temp);
            return FALSE;
        }
    }
    if (j->zso_pad) {
        /* a ZSO that is not a whole number of sectors long would lose its tail in hdl_dump: install a padded copy */
        GStatBuf st;

        temp = g_strdup_printf("%s/ps2gm/packed/%s.zso", g_get_user_cache_dir(), j->id);
        if (!ziso_pad_copy(j->iso, temp, cb, pd, error)) {
            g_free(temp);
            return FALSE;
        }
        if (g_stat(temp, &st) == 0)
            j->size_kb = (guint64)st.st_size / 1024;
    }
    if (j->pack_first) {
        /* compress to a temporary ZSO, install that, delete it: the disk gets the compressed data */
        GStatBuf st;

        temp = g_strdup_printf("%s/ps2gm/packed/%s.zso", g_get_user_cache_dir(), j->id);
        if (!ziso_pack(j->iso, temp, cb, pd, error)) {
            g_free(temp);
            return FALSE;
        }
        if (g_stat(temp, &st) == 0)
            j->size_kb = (guint64)st.st_size / 1024; /* what the disk must have room for */
    }
    ok = ops_install_ps2(A.ops, temp ? temp : j->iso, j->media, j->size_kb, j->id, j->title, j->art, notes, cb, pd, error);
    if (temp != NULL) {
        g_unlink(temp);
        g_free(temp);
    }
    if (!ok)
        return FALSE;
    apply_cover(j->cover, j->cover_whole, PS2GM_PS2, j->id, notes);
    return TRUE;
}

/* the toast when a transfer ends; the disk is read again by the queue once everything is done */
static void after_transfer(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        toast(_("Installed"));
    else
        toast(_("An installation did not work, see Transfers"));
}

void on_install_ps2(GtkButton *b, gpointer d)
{
    Ps2Job *j;

    (void)b;
    (void)d;
    if (batch != NULL) {
        install_batch();
        return;
    }
    j = g_new0(Ps2Job, 1);
    j->iso = g_strdup(A.iso_path);
    j->media = g_strdup(A.iso_info.media);
    j->id = g_strdup(A.iso_info.id);
    j->size_kb = A.iso_info.size_kb;
    j->title = g_strdup(gtk_editable_get_text(GTK_EDITABLE(A.iso_title_row)));
    j->art = adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_art_row));
    j->cover = g_strdup(A.iso_cover.path);
    j->cover_whole = A.iso_cover.whole;
    slot_set(&A.iso_cover, NULL, FALSE);
    j->pack_first = !A.iso_is_zso && adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_pack_row));
    j->unpack_first = A.iso_is_zso && !adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_keep_row));
    if (A.iso_is_zso && !j->unpack_first) {
        j->size_kb = A.iso_zso_kb; /* the compressed size is what the disk must have room for */
        j->zso_pad = ziso_needs_padding(A.iso_path);
    }
    queue_transfer("PS2", j->title, j->id, job_install_ps2, j, ps2job_free, after_transfer);
}

/* ---- several PS2 games at once ------------------------------------------------------------------- */

static void batch_item_free(gpointer p)
{
    BatchItem *b = p;

    g_free(b->path);
    g_free(b->id);
    g_free(b->title);
    g_free(b->media);
    g_free(b->skip);
    g_free(b);
}

static gboolean batch_installable(const BatchItem *b)
{
    return b->skip == NULL;
}

/* back to the rows of a single game */
static void batch_reset(void)
{
    if (batch == NULL)
        return;
    g_clear_pointer(&batch, g_ptr_array_unref);
    gtk_widget_set_visible(batch_group, FALSE);
    gtk_widget_set_visible(A.iso_id_row, TRUE);
    gtk_widget_set_visible(A.iso_verify, TRUE);
    gtk_widget_set_visible(A.iso_title_row, TRUE);
    gtk_widget_set_visible(A.iso_cover.row, TRUE);
    gtk_button_set_label(GTK_BUTTON(A.iso_go), _("Install on the PS2 disk"));
}

static void on_batch_remove(GtkButton *b, gpointer data)
{
    guint i = GPOINTER_TO_UINT(data);

    (void)b;
    if (batch == NULL || i >= batch->len)
        return;
    g_ptr_array_remove_index(batch, i);
    if (batch->len == 0) {
        batch_reset();
        update_iso_widgets();
    } else
        batch_refresh_ui();
}

static void batch_refresh_ui(void)
{
    guint i, n = 0;
    guint64 total_kb = 0;
    gboolean any_zso = FALSE, any_iso = FALSE;
    char *label, *desc;

    /* the rows of the group: remove them all, then add one per file */
    if (batch_rows == NULL)
        batch_rows = g_ptr_array_new();
    for (i = 0; i < batch_rows->len; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(batch_group), g_ptr_array_index(batch_rows, i));
    g_ptr_array_set_size(batch_rows, 0);
    for (i = 0; batch && i < batch->len; i++) {
        BatchItem *b = g_ptr_array_index(batch, i);
        GtkWidget *row = adw_action_row_new(), *rm = gtk_button_new_from_icon_name("window-close-symbolic");
        char *base = g_path_get_basename(b->path), *sz = size_text(b->zso ? b->zso_kb : b->size_kb), *sub;

        if (batch_installable(b)) {
            sub = g_strdup_printf("%s  ·  %s  ·  %s", b->id, sz, base);
            n++;
            total_kb += b->zso ? b->zso_kb : b->size_kb;
            if (b->zso)
                any_zso = TRUE;
            else
                any_iso = TRUE;
        } else {
            sub = g_strdup_printf("%s  ·  %s", b->skip, base);
            gtk_widget_add_css_class(row, "dim-label");
        }
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), b->title ? b->title : base);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        gtk_widget_set_tooltip_text(rm, _("Remove from the list"));
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
        g_signal_connect(rm, "clicked", G_CALLBACK(on_batch_remove), GUINT_TO_POINTER(i));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(batch_group), row);
        g_ptr_array_add(batch_rows, row);
        g_free(base);
        g_free(sz);
        g_free(sub);
    }
    {
        char *total = size_text(total_kb), *free_t = A.lib ? mb_text(A.lib->free_mb) : g_strdup("?");

        desc = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u game · %s in total · %s free on the disk", "%u games · %s in total · %s free on the disk", n), n, total, free_t);
        if (A.lib != NULL && total_kb / 1024 + 20 * n > A.lib->free_mb) {
            char *more = g_strdup_printf("%s\n%s", desc, _("Not enough room for all of them: the last ones will fail when their turn comes."));

            g_free(desc);
            desc = more;
        }
        g_free(total);
        g_free(free_t);
    }
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(batch_group), desc);
    gtk_widget_set_visible(batch_group, TRUE);
    gtk_widget_set_visible(A.iso_id_row, FALSE); /* the single game rows make no sense for a list */
    gtk_widget_set_visible(A.iso_verify, FALSE);
    gtk_widget_set_visible(A.iso_title_row, FALSE);
    gtk_widget_set_visible(A.iso_cover.row, FALSE);
    gtk_widget_set_visible(A.iso_keep_row, any_zso);
    gtk_widget_set_visible(A.iso_pack_row, any_iso);
    if (any_iso)
        set_pack_subtitle(_("It applies to every .iso of the list."));
    label = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "Install %u game", "Install %u games", n), n);
    gtk_button_set_label(GTK_BUTTON(A.iso_go), label);
    gtk_widget_set_sensitive(A.iso_go, n > 0 && A.backend != NULL);
    gtk_widget_set_sensitive(A.iso_cover.choose_btn, FALSE);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_file_row), _("Several images chosen"));
    g_free(label);
    g_free(desc);
}

/* reads the images chosen (one or many); one file is the usual single game, more than one a list */
void load_isos(char **paths)
{
    guint i, n = g_strv_length(paths);

    if (n == 0)
        return;
    if (n == 1) {
        load_iso(paths[0]);
        return;
    }
    g_clear_pointer(&batch, g_ptr_array_unref);
    batch = g_ptr_array_new_with_free_func(batch_item_free);
    g_free(A.iso_path); /* no single game is chosen now */
    A.iso_path = NULL;
    A.iso_ok = FALSE;
    slot_set(&A.iso_cover, NULL, FALSE);
    for (i = 0; i < n; i++) {
        BatchItem *b = g_new0(BatchItem, 1);
        GError *err = NULL;
        char *name, *base, *dot;
        guint k;

        b->path = g_strdup(paths[i]);
        if (ziso_is_zso(b->path)) {
            ZisoGame g;
            GStatBuf st;

            if (ziso_game_info(b->path, &g, &err)) {
                b->zso = TRUE;
                b->id = g.id;
                b->media = g_strdup(g.media);
                b->size_kb = g.size_kb;
                b->zso_kb = g_stat(b->path, &st) == 0 ? (guint64)st.st_size / 1024 : g.size_kb;
                name = g.label;
            } else
                name = NULL;
        } else {
            HdlIsoInfo info = {0};

            if (hdl_iso_info(&A.tools, b->path, &info, &err)) {
                b->id = info.id;
                b->media = g_strdup(info.media);
                b->size_kb = info.size_kb;
                name = info.label;
                g_free(info.media);
            } else
                name = NULL;
        }
        if (err != NULL) {
            b->skip = g_strdup(_("Not a PS2 game image"));
            g_clear_error(&err);
        } else {
            char *t = ps2_title_lookup(b->id);

            base = g_path_get_basename(b->path);
            dot = strrchr(base, '.');
            if (dot)
                *dot = '\0';
            b->title = t ? t : g_strdup(name && *name ? name : base);
            g_free(base);
            for (k = 0; A.lib && k < A.lib->games->len; k++)
                if (!g_strcmp0(((LibGame *)g_ptr_array_index(A.lib->games, k))->id, b->id))
                    b->skip = g_strdup(_("Already on the PS2 disk"));
            for (k = 0; b->skip == NULL && k < batch->len; k++) {
                BatchItem *o = g_ptr_array_index(batch, k);

                if (batch_installable(o) && !g_strcmp0(o->id, b->id))
                    b->skip = g_strdup(_("Same game as another file in the list"));
            }
        }
        g_free(name);
        g_ptr_array_add(batch, b);
    }
    update_iso_widgets();
}

/* every game of the list becomes a transfer; the ones left out stay out */
static void install_batch(void)
{
    guint i, queued = 0;
    gboolean art = adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_art_row));
    gboolean keep = adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_keep_row)), pack = adw_switch_row_get_active(ADW_SWITCH_ROW(A.iso_pack_row));

    for (i = 0; batch && i < batch->len; i++) {
        BatchItem *b = g_ptr_array_index(batch, i);
        Ps2Job *j;

        if (!batch_installable(b))
            continue;
        j = g_new0(Ps2Job, 1);
        j->iso = g_strdup(b->path);
        j->media = g_strdup(b->media);
        j->id = g_strdup(b->id);
        j->title = g_strdup(b->title);
        j->art = art;
        j->size_kb = b->size_kb;
        j->pack_first = !b->zso && pack;
        j->unpack_first = b->zso && !keep;
        if (b->zso && keep) {
            j->size_kb = b->zso_kb;
            j->zso_pad = ziso_needs_padding(b->path);
        }
        queue_transfer("PS2", j->title, j->id, job_install_ps2, j, ps2job_free, after_transfer);
        queued++;
    }
    batch_reset();
    update_iso_widgets();
    if (queued > 0) {
        char *msg = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u game added to Transfers", "%u games added to Transfers", queued), queued);

        toast(msg);
        g_free(msg);
    }
}

/* ---- add games: PS1 ---------------------------------------------------------------------------- */

static void ps1disc_free(gpointer p)
{
    Ps1Disc *d = p;

    g_free(d->cue_path);
    g_free(d->id);
    g_free(d);
}

void rebuild_disc_rows(void)
{
    guint i;

    for (i = 0; i < A.ps1_disc_rows->len; i++)
        adw_preferences_group_remove(A.ps1_disc_group, g_ptr_array_index(A.ps1_disc_rows, i));
    g_ptr_array_set_size(A.ps1_disc_rows, 0);
    for (i = 0; i < A.discs->len; i++) {
        Ps1Disc *d = g_ptr_array_index(A.discs, i);
        char *name = g_path_get_basename(d->cue_path);
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        {
            char *title = g_strdup_printf("Disc %u: %s", i + 1, name);

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
            g_free(title);
        }
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), d->id ? d->id : _("Game ID not found in this image"));
        adw_preferences_group_add(A.ps1_disc_group, row);
        g_ptr_array_add(A.ps1_disc_rows, row);
        g_free(name);
    }
    gtk_widget_set_visible(GTK_WIDGET(A.ps1_disc_group), A.discs->len > 0);
    {
        gboolean ready = A.discs->len > 0 && A.backend != NULL;

        for (i = 0; i < A.discs->len; i++)
            if (((Ps1Disc *)g_ptr_array_index(A.discs, i))->id == NULL)
                ready = FALSE;
        gtk_widget_set_sensitive(A.ps1_go, ready);
        gtk_widget_set_sensitive(A.ps1_cover.choose_btn, ready);
    }
}

static int cmp_disc(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((*(Ps1Disc **)a)->cue_path, (*(Ps1Disc **)b)->cue_path);
}

void load_cues(char **paths, guint n)
{
    guint i;
    char *guess = NULL;

    g_ptr_array_set_size(A.discs, 0);
    slot_set(&A.ps1_cover, NULL, FALSE);
    for (i = 0; i < n && i < 4; i++) {
        Ps1Disc *d = g_new0(Ps1Disc, 1);
        Ps1Cue *cue;
        GError *err = NULL;

        d->cue_path = g_strdup(paths[i]);
        cue = ps1cue_parse(d->cue_path, &err);
        if (cue == NULL) {
            show_error(_("That disc image cannot be used"), err->message);
            g_clear_error(&err);
            ps1disc_free(d);
            continue;
        }
        d->id = ps1_detect_game_id(((Ps1File *)g_ptr_array_index(cue->files, 0))->path);
        ps1cue_free(cue);
        g_ptr_array_add(A.discs, d);
    }
    g_ptr_array_sort(A.discs, cmp_disc);

    /* title guess from the first file name: "Game (Disc 1).cue" -> "Game" */
    if (A.discs->len > 0) {
        char *base = g_path_get_basename(((Ps1Disc *)g_ptr_array_index(A.discs, 0))->cue_path), *dot = strrchr(base, '.');
        GRegex *re = g_regex_new("\\s*[(\\[]\\s*(?:Disc|CD)\\s*\\d+[^)\\]]*[)\\]]|\\s*[(\\[][^)\\]]*[)\\]]", G_REGEX_CASELESS, 0, NULL);

        if (dot)
            *dot = '\0';
        guess = g_regex_replace(re, base, -1, 0, "", 0, NULL);
        g_regex_unref(re);
        gtk_editable_set_text(GTK_EDITABLE(A.ps1_title_row), guess && *guess ? guess : base);
        g_free(base);
        g_free(guess);
    }
    {
        char *sub = A.discs->len ? g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u disc selected", "%u discs selected", A.discs->len), A.discs->len) : g_strdup(_("Choose the .cue file of each disc"));

        adw_action_row_set_subtitle(ADW_ACTION_ROW(A.ps1_files_row), sub);
        g_free(sub);
    }
    {
        /* the BIN files of every disc: they are what the Redump list has hashes for */
        GPtrArray *bins = g_ptr_array_new_with_free_func(g_free);

        for (i = 0; i < A.discs->len; i++) {
            Ps1Cue *cue = ps1cue_parse(((Ps1Disc *)g_ptr_array_index(A.discs, i))->cue_path, NULL);
            guint k;

            for (k = 0; cue != NULL && k < cue->files->len; k++)
                g_ptr_array_add(bins, g_strdup(((Ps1File *)g_ptr_array_index(cue->files, k))->path));
            ps1cue_free(cue);
        }
        g_ptr_array_add(bins, NULL);
        verify_set_files(A.ps1_verify, (char **)bins->pdata, NULL);
        g_ptr_array_free(bins, TRUE);
    }
    rebuild_disc_rows();
}

static void on_cues_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    guint i, n;
    char **paths;

    (void)data;
    if (files == NULL)
        return;
    n = g_list_model_get_n_items(files);
    paths = g_new0(char *, n + 1);
    for (i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(files, i);

        paths[i] = g_file_get_path(f);
        g_object_unref(f);
    }
    g_object_unref(files);
    load_cues(paths, n);
    g_strfreev(paths);
}

static void on_choose_cues(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *flt = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose the PS1 disc images (.cue). Select all discs of a multi-disc game"));
    gtk_file_filter_set_name(flt, _("CUE sheets (.cue)"));
    gtk_file_filter_add_suffix(flt, "cue");
    g_list_store_append(filters, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(A.win), NULL, on_cues_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(flt);
    g_object_unref(dlg);
}

typedef struct {
    GPtrArray *discs;
    char *title, *cover;
    gboolean art, cover_whole;
} Ps1Job;

static void ps1job_free(gpointer p)
{
    Ps1Job *j = p;

    g_ptr_array_free(j->discs, TRUE);
    g_free(j->title);
    g_free(j->cover);
    g_free(j);
}

static gboolean job_install_ps1(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Ps1Job *j = data;

    guint i;

    if (!ops_install_ps1(A.ops, j->discs, j->title, j->art, notes, cb, pd, error))
        return FALSE;
    for (i = 0; i < j->discs->len; i++) /* every disc of the game gets the chosen cover */
        apply_cover(j->cover, j->cover_whole, PS2GM_PS1, ((Ps1Disc *)g_ptr_array_index(j->discs, i))->id, notes);
    return TRUE;
}

void on_install_ps1(GtkButton *b, gpointer d)
{
    Ps1Job *j = g_new0(Ps1Job, 1);
    guint i;

    (void)b;
    (void)d;
    j->discs = g_ptr_array_new_with_free_func(ps1disc_free);
    for (i = 0; i < A.discs->len; i++) {
        Ps1Disc *s = g_ptr_array_index(A.discs, i), *c = g_new0(Ps1Disc, 1);

        c->cue_path = g_strdup(s->cue_path);
        c->id = g_strdup(s->id);
        g_ptr_array_add(j->discs, c);
    }
    j->title = g_strdup(gtk_editable_get_text(GTK_EDITABLE(A.ps1_title_row)));
    j->art = adw_switch_row_get_active(ADW_SWITCH_ROW(A.ps1_art_row));
    j->cover = g_strdup(A.ps1_cover.path);
    j->cover_whole = A.ps1_cover.whole;
    slot_set(&A.ps1_cover, NULL, FALSE);
    queue_transfer("PS1", j->title, j->discs->len ? ((Ps1Disc *)g_ptr_array_index(j->discs, 0))->id : NULL, job_install_ps1, j, ps1job_free, after_transfer);
}

/* ---- the page: two cards, one per kind of game ------------------------------------------------- */

static gboolean on_drop_iso(GtkDropTarget *dt, const GValue *value, double x, double y, gpointer data)
{
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value)), *l;

    (void)dt;
    (void)x;
    (void)y;
    (void)data;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    for (l = files; l != NULL; l = l->next) {
        char *path = g_file_get_path(l->data);

        if (path != NULL && (g_str_has_suffix(path, ".iso") || g_str_has_suffix(path, ".ISO") || g_str_has_suffix(path, ".zso") ||
                             g_str_has_suffix(path, ".ZSO")))
            g_ptr_array_add(paths, path);
        else
            g_free(path);
    }
    g_slist_free(files);
    if (paths->len == 0) {
        g_ptr_array_free(paths, TRUE);
        show_error(_("That is not an .iso or .zso file"), _("Drop the .iso or .zso disc image of a PS2 game here."));
        return FALSE;
    }
    g_ptr_array_add(paths, NULL);
    load_isos((char **)paths->pdata);
    g_ptr_array_free(paths, TRUE);
    return TRUE;
}

static gboolean on_drop_cues(GtkDropTarget *dt, const GValue *value, double x, double y, gpointer data)
{
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value)), *l;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    (void)dt;
    (void)x;
    (void)y;
    (void)data;
    for (l = files; l != NULL; l = l->next) {
        char *path = g_file_get_path(l->data);

        if (path != NULL && g_str_has_suffix(path, ".cue"))
            g_ptr_array_add(paths, path);
        else
            g_free(path);
    }
    g_slist_free(files);
    if (paths->len == 0) {
        g_ptr_array_free(paths, TRUE);
        show_error(_("No .cue file"), _("Drop the .cue file of each disc of a PS1 game here (all discs together for a multi-disc game)."));
        return FALSE;
    }
    load_cues((char **)paths->pdata, paths->len);
    g_ptr_array_free(paths, TRUE);
    return TRUE;
}

static GdkDragAction on_drop_hover(GtkDropTarget *dt, double x, double y, gpointer data)
{
    (void)x;
    (void)y;
    (void)data;
    gtk_widget_add_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt)), "drop-hover");
    return GDK_ACTION_COPY;
}

static void on_drop_leave(GtkDropTarget *dt, gpointer data)
{
    (void)data;
    gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt)), "drop-hover");
}

/* a card: badge and heading, a hint about dropping files, the rows, then the install button */
static GtkWidget *make_card(const char *badge, const char *badge_class, const char *heading, const char *hint,
                            GtkWidget *group, GtkWidget *extra, GtkWidget *go, const char *go_note, GCallback on_drop)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14), *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *b = gtk_label_new(badge), *h = gtk_label_new(heading);
    GtkWidget *hl = gtk_label_new(hint), *note = gtk_label_new(go_note);
    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);

    gtk_widget_add_css_class(card, "add-card");
    gtk_widget_add_css_class(b, "badge");
    gtk_widget_add_css_class(b, badge_class);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(h, "section-title");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_widget_add_css_class(hl, "card-sub");
    gtk_label_set_xalign(GTK_LABEL(hl), 0);
    gtk_label_set_wrap(GTK_LABEL(hl), TRUE);
    gtk_box_append(GTK_BOX(texts), h);
    gtk_box_append(GTK_BOX(texts), hl);
    gtk_box_append(GTK_BOX(head), b);
    gtk_box_append(GTK_BOX(head), texts);
    gtk_box_append(GTK_BOX(card), head);
    gtk_box_append(GTK_BOX(card), group);
    if (extra != NULL)
        gtk_box_append(GTK_BOX(card), extra);

    {
        /* when the cards stand side by side the shorter one is stretched: keep its button at the bottom */
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_widget_set_vexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(card), spacer);
    }
    gtk_widget_add_css_class(go, "pill");
    gtk_widget_add_css_class(go, "suggested-action");
    gtk_widget_set_sensitive(go, FALSE);
    gtk_widget_set_margin_top(go, 4);
    gtk_box_append(GTK_BOX(card), go);
    gtk_widget_add_css_class(note, "card-sub");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_box_append(GTK_BOX(card), note);

    g_signal_connect(dt, "drop", on_drop, NULL);
    g_signal_connect(dt, "enter", G_CALLBACK(on_drop_hover), NULL);
    g_signal_connect(dt, "leave", G_CALLBACK(on_drop_leave), NULL);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(dt));
    return card;
}

static void on_back_to_library(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    adw_view_stack_set_visible_child_name(A.stack, "library");
}

GtkWidget *build_add_page(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new(), *clamp = adw_clamp_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *back = gtk_button_new(), *back_content = adw_button_content_new();
    GtkWidget *title = gtk_label_new(_("Add games")), *sub = gtk_label_new(_("Install a game on the PS2 disk. You can also drop the files onto a card."));
    GtkWidget *cards = gtk_flow_box_new(), *c2, *c1, *go2, *go1;
    AdwPreferencesGroup *g2 = ADW_PREFERENCES_GROUP(adw_preferences_group_new()), *g1 = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    A.discs = g_ptr_array_new_with_free_func(ps1disc_free);
    A.ps1_disc_rows = g_ptr_array_new();

    /* PS2 */
    A.iso_file_row = button_row(_("Disc image"), _("Choose an .iso or .zso file"), _("Choose..."), G_CALLBACK(on_choose_iso), NULL);
    adw_preferences_group_add(g2, A.iso_file_row);
    A.iso_id_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.iso_id_row), _("Game"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_id_row), _("Choose an ISO file first"));
    adw_preferences_group_add(g2, A.iso_id_row);
    A.iso_verify = build_verify_row(PS2GM_PS2);
    adw_preferences_group_add(g2, A.iso_verify);
    A.iso_title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.iso_title_row), _("Title"));
    adw_preferences_group_add(g2, A.iso_title_row);
    make_cover_row(&A.iso_cover, g2, G_CALLBACK(on_choose_iso_cover), G_CALLBACK(on_reset_iso_cover));
    A.iso_keep_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.iso_keep_row), _("Keep it compressed on the disk"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_keep_row),
                                _("Saves space on the PS2 disk, but OPL needs ZSO support (recent versions have it). Off: the game is unpacked here and installed as a normal ISO."));
    adw_switch_row_set_active(ADW_SWITCH_ROW(A.iso_keep_row), TRUE);
    gtk_widget_set_visible(A.iso_keep_row, FALSE); /* only for .zso files */
    adw_preferences_group_add(g2, A.iso_keep_row);
    A.iso_pack_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.iso_pack_row), _("Compress it to ZSO while installing"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.iso_pack_row),
                                _("Makes the game much smaller on the PS2 disk, but OPL needs ZSO support: try one game first. Takes a few minutes and some free space on this computer."));
    gtk_widget_set_visible(A.iso_pack_row, FALSE); /* only for .iso files */
    adw_preferences_group_add(g2, A.iso_pack_row);
    A.iso_art_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.iso_art_row), _("Download covers and art"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(A.iso_art_row), TRUE);
    adw_preferences_group_add(g2, A.iso_art_row);
    batch_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(batch_group), _("Games to install"));
    gtk_widget_set_visible(batch_group, FALSE);
    go2 = A.iso_go = gtk_button_new_with_label(_("Install on the PS2 disk"));
    g_signal_connect(go2, "clicked", G_CALLBACK(on_install_ps2), NULL);
    c2 = make_card("PS2", "badge-ps2", _("PS2 game"), _("A disc image (.iso, or a compressed .zso). Drop it here or choose it below."), GTK_WIDGET(g2), batch_group, go2,
                   _("Copies the whole game; a DVD takes several minutes."), G_CALLBACK(on_drop_iso));

    /* PS1 */
    A.ps1_files_row = button_row(_("Disc images"), _("Choose the .cue file of each disc"), _("Choose..."), G_CALLBACK(on_choose_cues), NULL);
    adw_preferences_group_add(g1, A.ps1_files_row);
    A.ps1_verify = build_verify_row(PS2GM_PS1);
    adw_preferences_group_add(g1, A.ps1_verify);
    A.ps1_title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.ps1_title_row), _("Title"));
    adw_preferences_group_add(g1, A.ps1_title_row);
    make_cover_row(&A.ps1_cover, g1, G_CALLBACK(on_choose_ps1_cover), G_CALLBACK(on_reset_ps1_cover));
    A.ps1_art_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.ps1_art_row), _("Download covers and art"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(A.ps1_art_row), TRUE);
    adw_preferences_group_add(g1, A.ps1_art_row);
    A.ps1_disc_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(A.ps1_disc_group, _("Discs found"));
    gtk_widget_set_visible(GTK_WIDGET(A.ps1_disc_group), FALSE);
    go1 = A.ps1_go = gtk_button_new_with_label(_("Convert and install"));
    g_signal_connect(go1, "clicked", G_CALLBACK(on_install_ps1), NULL);
    c1 = make_card("PS1", "badge-ps1", _("PS1 game"), _("BIN/CUE images. Several discs of one game: select all their .cue files together."),
                   GTK_WIDGET(g1), GTK_WIDGET(A.ps1_disc_group), go1,
                   _("Makes the POPStarter image (.VCD) and copies it to the PS2 disk."), G_CALLBACK(on_drop_cues));

    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(back_content), "go-previous-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(back_content), _("Back to the library"));
    gtk_button_set_child(GTK_BUTTON(back), back_content);
    gtk_widget_add_css_class(back, "flat");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_to_library), NULL);
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(sub, "card-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE); /* long in some languages: must not widen the page */
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(cards), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(cards), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(cards), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(cards), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(cards), 18);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(cards), 18);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(cards), FALSE);
    gtk_flow_box_append(GTK_FLOW_BOX(cards), c2);
    gtk_flow_box_append(GTK_FLOW_BOX(cards), c1);

    gtk_widget_set_margin_top(page, 22);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_box_append(GTK_BOX(page), back);
    gtk_box_append(GTK_BOX(page), title);
    gtk_box_append(GTK_BOX(page), sub);
    gtk_box_append(GTK_BOX(page), cards);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1200);
    adw_clamp_set_child(ADW_CLAMP(clamp), page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    return scroll;
}
