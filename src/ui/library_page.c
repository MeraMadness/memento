/*
 * Library page: the games on the disk as a grid of covers (or a list) with search, filters and sorting, and a details
 * panel for the selected game with everything you can do to it. Everything shown comes from the disk (the pictures are
 * copied to a cache on this computer when the disk is read, see load_library).
 */
#include "app.h"

typedef struct {
    Ps2gmPlatform platform;
    char *id;       /* game ID; NULL for a PS1 image whose name has none */
    char *title;
    char *media;    /* CD / DVD; NULL for PS1 */
    char *vcd_name; /* PS1: the file in __.POPS */
    guint64 size_kb;
    GtkWidget *card, *row; /* the cover card in the grid and the row in the list */
} Entry;

static void entry_free(gpointer p)
{
    Entry *e = p;

    g_free(e->id);
    g_free(e->title);
    g_free(e->media);
    g_free(e->vcd_name);
    g_free(e);
}

/* a game is identified by its platform and its file/ID: stays the same when the list is rebuilt */
static char *entry_key(const Entry *e)
{
    return g_strdup_printf("%s:%s", e->platform == PS2GM_PS2 ? "ps2" : "ps1", e->vcd_name ? e->vcd_name : e->id);
}

/* ---- reading the disk -------------------------------------------------------------------------- */

static void add_art_name(GPtrArray *names, GHashTable *seen, const char *id, const char *kind)
{
    char *name = g_strdup_printf("%s_%s.png", id, kind);

    if (g_hash_table_add(seen, name))
        g_ptr_array_add(names, name);
}

gboolean load_library(Ps2gmProgress cb, gpointer pd, GError **error)
{
    Library *lib;
    GPtrArray *names = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint i;

    cb(0, 3, _("Reading the disk"), pd);
    lib = backend_library(A.backend, error);
    if (lib == NULL) {
        g_ptr_array_free(names, TRUE);
        g_hash_table_destroy(seen);
        return FALSE;
    }

    /* the covers of all games and the banner picture (background) of each one */
    for (i = 0; i < lib->games->len; i++) {
        LibGame *g = g_ptr_array_index(lib->games, i);

        add_art_name(names, seen, g->id, "COV");
        add_art_name(names, seen, g->id, "BG");
    }
    for (i = 0; i < lib->vcds->len; i++) {
        char *id = NULL;

        if (vcd_name_parse(((LibFile *)g_ptr_array_index(lib->vcds, i))->name, &id, NULL)) {
            add_art_name(names, seen, id, "COV");
            add_art_name(names, seen, id, "BG");
        }
        g_free(id);
    }
    g_ptr_array_add(names, NULL);
    cb(1, 3, _("Reading the pictures"), pd);
    g_free(A.art_cache);
    A.art_cache = backend_art_cache_dir(A.device);
    backend_cache_art(A.backend, lib, (char *const *)names->pdata, A.art_cache, NULL); /* pictures are optional */
    g_ptr_array_free(names, TRUE);
    g_hash_table_destroy(seen);

    library_free(A.lib);
    A.lib = lib;
    return TRUE;
}

static gboolean job_refresh(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    (void)data;
    (void)notes;
    return load_library(cb, pd, error);
}

static void after_refresh(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        rebuild_library();
}

void refresh_library(void)
{
    if (A.backend != NULL)
        run_job(_("Reading the PS2 disk"), job_refresh, NULL, NULL, after_refresh);
}

/* ---- removing and art: what the buttons ask ----------------------------------------------------- */

typedef struct {
    char *a, *b;
    Ps2gmPlatform platform;
    gboolean replace; /* art: overwrite files that already exist */
    gboolean with_folder; /* removing a PS1 game: also its memory card folder */
    GtkWidget *folder_check; /* the switch of the question; only valid while that dialog is open */
} Target;

static void target_free(gpointer p)
{
    Target *t = p;

    g_free(t->a);
    g_free(t->b);
    g_free(t);
}

static void on_remove_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    Target *t = data;

    (void)d;
    if (!strcmp(response, "remove")) {
        if (t->platform == PS2GM_PS2)
            do_remove_ps2(t->a, t->b);
        else
            do_remove_ps1(t->a, t->folder_check && gtk_check_button_get_active(GTK_CHECK_BUTTON(t->folder_check)));
    }
    target_free(t);
}

static void ask_remove(Ps2gmPlatform platform, const char *a, const char *b, const char *name)
{
    Target *t = g_new0(Target, 1);
    char *body = g_strdup_printf(platform == PS2GM_PS1 ? _("\"%s\" will be deleted from the PS2 disk. This cannot be undone.\nIts memory card and saves are kept, unless you tick the box.")
                                                         : _("\"%s\" will be deleted from the PS2 disk. This cannot be undone."),
                                 name);
    AdwDialog *d = adw_alert_dialog_new(_("Remove this game?"), body);

    t->platform = platform;
    t->a = g_strdup(a);
    t->b = g_strdup(b);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "remove", _("Remove"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
    if (platform == PS2GM_PS1) { /* the memory card folder: off by default, because the saves on the card are lost with it */
        t->folder_check = gtk_check_button_new_with_label(_("Also delete its memory card, settings and fix files (the saves on the card are lost)"));
        adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), t->folder_check);
    }
    g_signal_connect(d, "response", G_CALLBACK(on_remove_response), t);
    adw_dialog_present(d, GTK_WIDGET(A.win));
    g_free(body);
}

static Entry *selected_entry(void)
{
    guint i;

    for (i = 0; A.selected && A.entries && i < A.entries->len; i++) {
        Entry *e = g_ptr_array_index(A.entries, i);
        char *key = entry_key(e);
        gboolean same = !strcmp(key, A.selected);

        g_free(key);
        if (same)
            return e;
    }
    return NULL;
}

/* path of a cached picture, or NULL if the disk has none */
static char *cached_art(const char *id, const char *kind)
{
    char *path;

    if (id == NULL || A.art_cache == NULL)
        return NULL;
    path = g_strdup_printf("%s/%s_%s.png", A.art_cache, id, kind);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        return NULL;
    }
    return path;
}


/*
 * The discs of a multi-disc PS1 game installed by this app are named "<title> (Disc N)".
 * Returns the .VCD names of all the discs of e's game in disc order (g_strfreev), or NULL if e is not one of several.
 */
static char **disc_group(const Entry *e)
{
    GRegex *re = g_regex_new("^(.*) \\(Disc (\\d+)\\)$", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    GPtrArray *found = g_ptr_array_new(); /* Entry * */
    char *prefix = NULL, **names = NULL;
    guint i;

    if (e->platform != PS2GM_PS1 || e->vcd_name == NULL || !g_regex_match(re, e->title, 0, &mi))
        goto out;
    prefix = g_match_info_fetch(mi, 1);
    for (i = 0; i < A.entries->len; i++) {
        Entry *o = g_ptr_array_index(A.entries, i);
        GMatchInfo *m2 = NULL;

        if (o->platform == PS2GM_PS1 && o->vcd_name && g_regex_match(re, o->title, 0, &m2)) {
            char *p2 = g_match_info_fetch(m2, 1);

            if (!strcmp(p2, prefix))
                g_ptr_array_add(found, o);
            g_free(p2);
        }
        g_match_info_free(m2);
    }
    if (found->len >= 2 && found->len <= 4) {
        GPtrArray *sorted = g_ptr_array_new();

        /* disc order = the numbers in the titles */
        for (i = 1; i <= 9 && sorted->len < found->len; i++) {
            guint k;

            for (k = 0; k < found->len; k++) {
                Entry *o = g_ptr_array_index(found, k);
                GMatchInfo *m3 = NULL;

                if (g_regex_match(re, o->title, 0, &m3)) {
                    char *num = g_match_info_fetch(m3, 2);

                    if (g_ascii_strtoull(num, NULL, 10) == i)
                        g_ptr_array_add(sorted, g_strdup(o->vcd_name));
                    g_free(num);
                }
                g_match_info_free(m3);
            }
        }
        if (sorted->len == found->len) {
            g_ptr_array_add(sorted, NULL);
            names = (char **)g_ptr_array_free(sorted, FALSE);
        } else
            g_ptr_array_free(sorted, TRUE);
    }
out:
    g_match_info_free(mi);
    g_regex_unref(re);
    g_free(prefix);
    g_ptr_array_free(found, TRUE);
    return names;
}


/* ---- what a game's ID says -------------------------------------------------------------------- */

static const char *region_of(const char *id)
{
    static const struct {
        const char *prefix, *name;
    } map[] = {
        {"SLUS", "NTSC-U"}, {"SCUS", "NTSC-U"}, {"SLES", "PAL"}, {"SCES", "PAL"}, {"SLED", "PAL"}, {"SCED", "PAL"},
        {"SLPS", "NTSC-J"}, {"SLPM", "NTSC-J"}, {"SCPS", "NTSC-J"}, {"SCPM", "NTSC-J"}, {"SIPS", "NTSC-J"}, {"PAPX", "NTSC-J"},
        {"PBPX", "NTSC-J"}, {"SLAJ", "Asia"}, {"SCAJ", "Asia"}, {"SLKA", "Korea"}, {"SCKA", "Korea"}, {"SCCS", "China"},
    };
    guint i;

    for (i = 0; id != NULL && i < G_N_ELEMENTS(map); i++)
        if (!strncmp(id, map[i].prefix, 4))
            return map[i].name;
    return NULL;
}

static GtkWidget *tag(const char *text, const char *css_class)
{
    GtkWidget *l = gtk_label_new(text);

    gtk_widget_add_css_class(l, "tag");
    gtk_widget_add_css_class(l, css_class);
    return l;
}

char *library_cover_path(const char *id)
{
    return cached_art(id, "COV");
}

/* the cover with the platform strip on top; w x h is the size of the frame */
static GtkWidget *cover_frame(const Entry *e, int w, int h, gboolean strip)
{
    GtkWidget *overlay = gtk_overlay_new(), *content;
    char *path = cached_art(e->id, "COV");
    int iw = 0, ih = 0;
    double ratio = e->platform == PS2GM_PS1 ? 1.0 : 1.4; /* height / width: a CD case is square, a DVD case is tall */

    /* the frame has the shape of the picture (read from the file header), so there are no black bars; h is the tallest allowed */
    if (path != NULL && gdk_pixbuf_get_file_info(path, &iw, &ih) != NULL && iw > 0 && ih > 0)
        ratio = (double)ih / iw;
    ratio = CLAMP(ratio, 0.8, (double)h / w);
    h = (int)(w * ratio + 0.5);

    if (path != NULL) {
        GtkWidget *clamp = adw_clamp_new();

        content = gtk_picture_new_for_filename(path);
        gtk_picture_set_content_fit(GTK_PICTURE(content), GTK_CONTENT_FIT_COVER);
        gtk_picture_set_can_shrink(GTK_PICTURE(content), TRUE);
        gtk_widget_add_css_class(content, "cover");
        /* a picture reports its own size as its natural width: cap it so every frame is the same */
        adw_clamp_set_maximum_size(ADW_CLAMP(clamp), w);
        adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), w);
        adw_clamp_set_child(ADW_CLAMP(clamp), content);
        content = clamp;
    } else {
        content = gtk_image_new_from_icon_name("image-missing-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(content), 36);
        gtk_widget_add_css_class(content, "cover-missing");
    }
    gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
    if (strip) {
        GtkWidget *s = gtk_label_new(e->platform == PS2GM_PS2 ? "PS2" : "PS1");

        gtk_widget_add_css_class(s, "cover-strip");
        gtk_widget_add_css_class(s, e->platform == PS2GM_PS2 ? "ps2" : "ps1");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_widget_set_valign(s, GTK_ALIGN_END); /* at the bottom: the top of a cover often has its own logo */
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), s);
    }
    gtk_widget_set_size_request(overlay, w, h);
    gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(overlay, GTK_ALIGN_END); /* in a taller slot the covers stand on the same line */
    gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(overlay, "cover-frame");
    g_free(path);
    return overlay;
}

/* ---- the details panel -------------------------------------------------------------------------- */

static GtkWidget *detail_row(const char *key, GtkWidget **value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *k = gtk_label_new(key), *v = gtk_label_new("");

    gtk_widget_add_css_class(k, "detail-key");
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    gtk_widget_set_hexpand(k, TRUE);
    gtk_widget_add_css_class(v, "detail-val");
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), k);
    gtk_box_append(GTK_BOX(row), v);
    *value = v;
    return row;
}

/* the cover of the selected game, small (it is enlarged and blurred anyway: less to draw) behind the details panel; none: plain */
static void set_backdrop(const char *path)
{
    GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file_at_scale(path, 96, 136, FALSE, NULL) : NULL;

    if (pb == NULL) {
        gtk_picture_set_paintable(GTK_PICTURE(A.det_backdrop), NULL);
        if (A.win_backdrop != NULL)
            gtk_picture_set_paintable(GTK_PICTURE(A.win_backdrop), NULL);
        return;
    }
    {
        GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), (gsize)gdk_pixbuf_get_rowstride(pb) * (gsize)gdk_pixbuf_get_height(pb));
        GdkTexture *tex = gdk_memory_texture_new(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                                                 gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes,
                                                 (gsize)gdk_pixbuf_get_rowstride(pb));

        gtk_picture_set_paintable(GTK_PICTURE(A.det_backdrop), GDK_PAINTABLE(tex));
        if (A.win_backdrop != NULL) /* the window is built after this page */
            gtk_picture_set_paintable(GTK_PICTURE(A.win_backdrop), GDK_PAINTABLE(tex));
        g_object_unref(tex);
        g_bytes_unref(bytes);
    }
    g_object_unref(pb);
}

static void update_details(void)
{
    Entry *e = selected_entry();
    GtkWidget *child;
    char **group = e ? disc_group(e) : NULL;

    while ((child = gtk_widget_get_first_child(A.det_cover_box)) != NULL)
        gtk_box_remove(GTK_BOX(A.det_cover_box), child);
    while ((child = gtk_widget_get_first_child(A.det_tags)) != NULL)
        gtk_box_remove(GTK_BOX(A.det_tags), child);
    gtk_widget_set_visible(A.det_content, e != NULL);
    gtk_widget_set_visible(A.det_empty, e == NULL);
    gtk_widget_set_visible(A.det_settings, e != NULL && e->platform == PS2GM_PS2); /* PS1 games run in POPStarter */
    gtk_widget_set_visible(A.det_cheats, e != NULL && e->platform == PS2GM_PS2);
    gtk_widget_set_visible(A.det_fixes, e != NULL && e->platform == PS2GM_PS1 && e->vcd_name != NULL);
    gtk_widget_set_visible(A.det_share, group != NULL);
    g_strfreev(group);
    if (e == NULL) {
        set_backdrop(NULL);
        gtk_label_set_text(GTK_LABEL(A.det_empty_title), A.entries && A.entries->len ? _("Nothing matches") : _("No games yet"));
        gtk_label_set_text(GTK_LABEL(A.det_empty_sub), A.entries && A.entries->len ? _("Change the search or the filter.") : _("Use Add game to install your first one."));
        return;
    }
    {
        char *size = size_text(e->size_kb), *cht = e->id ? g_strdup_printf("%s.cht", e->id) : NULL, *bg = cached_art(e->id, "BG"), *cov = cached_art(e->id, "COV");
        const char *region = region_of(e->id);

        set_backdrop(cov);
        gtk_box_append(GTK_BOX(A.det_cover_box), cover_frame(e, 170, 236, FALSE)); /* no PS2/PS1 strip here: the cover often has its own logo, and Platform is listed below */
        gtk_label_set_text(GTK_LABEL(A.det_title), e->title);
        gtk_label_set_text(GTK_LABEL(A.det_id), e->id ? e->id : _("No game ID in the file name"));
        gtk_label_set_text(GTK_LABEL(A.det_vals[0]), e->platform == PS2GM_PS2 ? "PlayStation 2" : "PlayStation");
        gtk_label_set_text(GTK_LABEL(A.det_vals[1]), region ? region : _("Unknown"));
        gtk_label_set_text(GTK_LABEL(A.det_vals[2]), e->platform == PS2GM_PS2 ? (e->media ? e->media : "?") : _("POPStarter image"));
        gtk_label_set_text(GTK_LABEL(A.det_vals[3]), size);
        if (cht != NULL && A.lib != NULL && g_hash_table_contains(A.lib->cheats, cht))
            gtk_box_append(GTK_BOX(A.det_tags), tag(_("cheats"), "tag-info"));
        if (e->id != NULL && cov == NULL)
            gtk_box_append(GTK_BOX(A.det_tags), tag(_("no cover"), "tag-warn"));
        if (e->id != NULL && bg == NULL)
            gtk_box_append(GTK_BOX(A.det_tags), tag(_("no background"), "tag-warn"));
        gtk_widget_set_visible(A.det_tags, gtk_widget_get_first_child(A.det_tags) != NULL);
        gtk_widget_set_sensitive(A.det_art, e->id != NULL);
        /* the main button: the settings of a PS2 game; for a PS1 game (no settings here) the pictures */
        if (e->platform == PS2GM_PS2) {
            gtk_widget_remove_css_class(A.det_art, "suggested-action");
            gtk_widget_add_css_class(A.det_art, "btn-secondary");
        } else {
            gtk_widget_add_css_class(A.det_art, "suggested-action");
            gtk_widget_remove_css_class(A.det_art, "btn-secondary");
        }
        g_free(size);
        g_free(cht);
        g_free(bg);
        g_free(cov);
    }
}

static void select_entry(Entry *e)
{
    guint i;

    g_free(A.selected);
    A.selected = e ? entry_key(e) : NULL;
    for (i = 0; A.entries && i < A.entries->len; i++) {
        Entry *o = g_ptr_array_index(A.entries, i);

        if (o->card != NULL) {
            if (o == e)
                gtk_widget_add_css_class(o->card, "selected-card");
            else
                gtk_widget_remove_css_class(o->card, "selected-card");
        }
        if (o->row != NULL) {
            if (o == e)
                gtk_widget_add_css_class(o->row, "selected-row");
            else
                gtk_widget_remove_css_class(o->row, "selected-row");
        }
    }
    update_details();
}

/* selects the game with this exact title (script mode and tests); FALSE if there is none */
gboolean select_game_by_title(const char *title)
{
    guint i;

    for (i = 0; A.entries && i < A.entries->len; i++) {
        Entry *e = g_ptr_array_index(A.entries, i);

        if (!strcmp(e->title, title)) {
            select_entry(e);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_card_clicked(GtkButton *b, gpointer data)
{
    (void)data;
    select_entry(g_object_get_data(G_OBJECT(b), "entry"));
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    (void)data;
    select_entry(g_object_get_data(G_OBJECT(row), "entry"));
}

/* the buttons of the panel: they act on the selected game */

static void on_det_art(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();

    (void)b;
    (void)data;
    if (e != NULL && e->id != NULL)
        show_art_dialog(e->platform, e->id, e->title);
}

static void on_det_cheats(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();

    (void)b;
    (void)data;
    if (e != NULL && e->platform == PS2GM_PS2 && e->id != NULL)
        show_cheats_dialog(e->id, e->title);
}

static void on_det_fixes(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();

    (void)b;
    (void)data;
    if (e != NULL && e->platform == PS2GM_PS1 && e->vcd_name != NULL)
        show_ps1fix_dialog(e->vcd_name, e->title);
}

static void on_det_settings(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();

    (void)b;
    (void)data;
    if (e != NULL && e->platform == PS2GM_PS2 && e->id != NULL)
        show_settings_dialog(e->id, e->title);
}

static void on_det_remove(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();

    (void)b;
    (void)data;
    if (e == NULL)
        return;
    if (e->platform == PS2GM_PS2)
        ask_remove(PS2GM_PS2, e->id, e->title, e->title);
    else
        ask_remove(PS2GM_PS1, e->vcd_name, NULL, e->title);
}

static void on_share_response(AdwAlertDialog *d, const char *response, gpointer data);

static gboolean job_share(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    (void)notes;
    cb(0, 1, _("Sharing the memory card"), pd);
    return ops_share_memcard(A.ops, data, error);
}

static void on_det_share(GtkButton *b, gpointer data)
{
    Entry *e = selected_entry();
    char **group = e ? disc_group(e) : NULL;
    AdwDialog *d;

    (void)b;
    (void)data;
    if (group == NULL)
        return;
    d = adw_alert_dialog_new(_("Share one memory card?"),
                             _("All the discs of this game will use the memory card of the first disc, so a save made on one disc "
                             "is there on the others.\n\nSaves already on the other discs' own cards are not merged: they stay "
                             "on the disk, but the game stops using them."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", _("Cancel"), "share", _("Share"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "share", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect(d, "response", G_CALLBACK(on_share_response), group); /* group is freed there */
    adw_dialog_present(d, GTK_WIDGET(A.win));
}

static void on_share_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    char **group = data;

    (void)d;
    if (!strcmp(response, "share"))
        run_job(_("Sharing the memory card"), job_share, group, (GDestroyNotify)g_strfreev, after_change);
    else
        g_strfreev(group);
}


/* ---- search, filters, sorting ------------------------------------------------------------------- */

/* does the game match the platform filter, the "which games" filter and the search text? */
static gboolean entry_matches(const Entry *e)
{
    const char *q = A.search ? gtk_editable_get_text(GTK_EDITABLE(A.search)) : "";

    if (A.filter == 1 && e->platform != PS2GM_PS2)
        return FALSE;
    if (A.filter == 2 && e->platform != PS2GM_PS1)
        return FALSE;
    if (A.filter2 == 1) { /* with cheats */
        char *cht = e->id ? g_strdup_printf("%s.cht", e->id) : NULL;
        gboolean has = cht && A.lib && g_hash_table_contains(A.lib->cheats, cht);

        g_free(cht);
        if (!has)
            return FALSE;
    } else if (A.filter2 == 2) { /* without a cover */
        char *c = cached_art(e->id, "COV");
        gboolean missing = c == NULL;

        g_free(c);
        if (!missing)
            return FALSE;
    }
    if (*q && !g_str_match_string(q, e->title, TRUE) && !(e->id && g_str_match_string(q, e->id, TRUE)))
        return FALSE;
    return TRUE;
}

static int cmp_entries(const Entry *x, const Entry *y)
{
    char *kx = g_utf8_casefold(x->title, -1), *ky = g_utf8_casefold(y->title, -1);
    int by_name = g_utf8_collate(kx, ky), r;

    g_free(kx);
    g_free(ky);
    switch (A.sort) {
    case 1: /* Z-A */
        return -by_name;
    case 2: /* largest first */
        r = x->size_kb < y->size_kb ? 1 : (x->size_kb > y->size_kb ? -1 : 0);
        return r ? r : by_name;
    case 3: /* smallest first */
        r = x->size_kb < y->size_kb ? -1 : (x->size_kb > y->size_kb ? 1 : 0);
        return r ? r : by_name;
    default:
        return by_name;
    }
}

static int cmp_for_sort(gconstpointer a, gconstpointer b)
{
    return cmp_entries(*(Entry *const *)a, *(Entry *const *)b);
}

static Entry *entry_of_child(GtkFlowBoxChild *c)
{
    return g_object_get_data(G_OBJECT(gtk_flow_box_child_get_child(c)), "entry");
}

static gboolean flow_filter(GtkFlowBoxChild *child, gpointer data)
{
    Entry *e = entry_of_child(child);

    (void)data;
    return e == NULL || entry_matches(e);
}

static int flow_sort(GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer data)
{
    (void)data;
    return cmp_entries(entry_of_child(a), entry_of_child(b));
}

static gboolean list_filter(GtkListBoxRow *row, gpointer data)
{
    Entry *e = g_object_get_data(G_OBJECT(row), "entry");

    (void)data;
    return e == NULL || entry_matches(e);
}

static int list_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data)
{
    (void)data;
    return cmp_entries(g_object_get_data(G_OBJECT(a), "entry"), g_object_get_data(G_OBJECT(b), "entry"));
}

/* the line under the title: how many games match, and of which platforms */
static void update_subtitle(void)
{
    guint shown = 0, i;
    gboolean ps1 = FALSE, ps2 = FALSE;
    char *count, *platforms, *text;

    for (i = 0; A.entries && i < A.entries->len; i++) {
        Entry *e = g_ptr_array_index(A.entries, i);

        if (e->platform == PS2GM_PS2)
            ps2 = TRUE;
        else
            ps1 = TRUE;
        if (entry_matches(e))
            shown++;
    }
    count = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u title", "%u titles", shown), shown);
    platforms = g_strdup(ps1 && ps2 ? "PS1 + PS2" : (ps2 ? "PS2" : (ps1 ? "PS1" : "")));
    text = *platforms ? g_strdup_printf("%s · %s", count, platforms) : g_strdup(count);
    gtk_label_set_text(GTK_LABEL(A.count_label), text);
    gtk_widget_set_visible(A.empty_label, A.entries && A.entries->len > 0 && shown == 0);
    g_free(count);
    g_free(platforms);
    g_free(text);
}

/* after a change of search, filter or sort: the selected game must still be one that is shown, else take the first that is */
static void on_filter_changed(void)
{
    Entry *keep = selected_entry(), *pick = NULL;
    GPtrArray *sorted;
    guint i;

    if (A.flow == NULL || A.list == NULL) /* the page is still being built */
        return;
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(A.flow));
    gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(A.flow));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(A.list));
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(A.list));
    update_subtitle();
    if (keep != NULL && entry_matches(keep))
        return;
    sorted = g_ptr_array_new();
    for (i = 0; A.entries && i < A.entries->len; i++)
        g_ptr_array_add(sorted, g_ptr_array_index(A.entries, i));
    g_ptr_array_sort(sorted, cmp_for_sort);
    for (i = 0; i < sorted->len && pick == NULL; i++)
        if (entry_matches(g_ptr_array_index(sorted, i)))
            pick = g_ptr_array_index(sorted, i);
    g_ptr_array_free(sorted, TRUE);
    select_entry(pick);
}

static void on_search_changed(GtkSearchEntry *s, gpointer data)
{
    (void)s;
    (void)data;
    on_filter_changed();
}

static void on_dropdown_changed(GObject *dd, GParamSpec *ps, gpointer data)
{
    int which = GPOINTER_TO_INT(data);
    int sel = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));

    (void)ps;
    if (which == 0)
        A.filter = sel;
    else if (which == 1)
        A.filter2 = sel;
    else
        A.sort = sel;
    on_filter_changed();
}

/* grid or list (script mode and tests: the toggle buttons do the same) */
void library_set_list_view(gboolean list)
{
    A.list_view = list;
    gtk_stack_set_visible_child_name(GTK_STACK(A.view_stack), list ? "list" : "grid");
}

static void on_view_toggled(GtkToggleButton *b, gpointer data)
{
    if (!gtk_toggle_button_get_active(b))
        return;
    library_set_list_view(GPOINTER_TO_INT(data) == 1);
}

/* ---- the cards and the rows ---------------------------------------------------------------------- */

static GtkWidget *make_card(Entry *e)
{
    GtkWidget *btn = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *title = gtk_label_new(e->title), *sub;
    char *size = size_text(e->size_kb), *subtext = g_strdup_printf("%s · %s", e->platform == PS2GM_PS2 ? "PS2" : "PS1", size);

    gtk_widget_set_size_request(box, 130, -1); /* every card is as wide as its cover: long titles are cut, not widened */
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(title), 1);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 1);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "card-title");
    sub = gtk_label_new(subtext);
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_widget_add_css_class(sub, "card-sub");
    {
        GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* as tall as the tallest cover, so every title is on the same line */

        gtk_widget_set_size_request(slot, 130, 178);
        GtkWidget *cf = cover_frame(e, 130, 178, TRUE);

        gtk_widget_set_vexpand(cf, TRUE); /* with valign END: the cover stands on the bottom of the slot */
        gtk_box_append(GTK_BOX(slot), cf);
        gtk_box_append(GTK_BOX(box), slot);
    }
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), sub);
    gtk_button_set_child(GTK_BUTTON(btn), box);
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "game-card");
    gtk_widget_set_tooltip_text(btn, e->title);
    g_object_set_data(G_OBJECT(btn), "entry", e);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_card_clicked), NULL);
    g_free(size);
    g_free(subtext);
    return btn;
}

static GtkWidget *make_row(Entry *e)
{
    GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14), *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new(e->title), *id = gtk_label_new(e->id ? e->id : ""), *size_l;
    char *size = size_text(e->size_kb);

    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(id), 0);
    gtk_widget_add_css_class(id, "card-sub");
    gtk_box_append(GTK_BOX(texts), title);
    gtk_box_append(GTK_BOX(texts), id);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    size_l = gtk_label_new(size);
    gtk_widget_add_css_class(size_l, "detail-val");
    {
        GtkWidget *cf = cover_frame(e, 42, 58, FALSE);

        gtk_widget_set_valign(cf, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), cf);
    }
    gtk_box_append(GTK_BOX(box), texts);
    {
        GtkWidget *t = tag(e->platform == PS2GM_PS2 ? "PS2" : "PS1", e->platform == PS2GM_PS2 ? "tag-info" : "tag-warn");

        gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), t);
    }
    gtk_box_append(GTK_BOX(box), size_l);
    gtk_widget_add_css_class(box, "game-row");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "entry", e);
    g_free(size);
    return row;
}

void rebuild_library(void)
{
    guint i;
    Entry *keep;

    if (A.lib == NULL)
        return;
    update_topbar();
    update_devices_page();
    update_statusbar(TRUE);

    gtk_flow_box_remove_all(GTK_FLOW_BOX(A.flow));
    gtk_list_box_remove_all(GTK_LIST_BOX(A.list));
    if (A.entries != NULL)
        g_ptr_array_free(A.entries, TRUE);
    A.entries = g_ptr_array_new_with_free_func(entry_free);

    for (i = 0; i < A.lib->games->len; i++) {
        LibGame *g = g_ptr_array_index(A.lib->games, i);
        Entry *e = g_new0(Entry, 1);

        e->platform = PS2GM_PS2;
        e->id = g_strdup(g->id);
        e->title = g_strdup(g->title);
        e->media = g_strdup(g->media);
        e->size_kb = g->size_kb;
        g_ptr_array_add(A.entries, e);
    }
    for (i = 0; i < A.lib->vcds->len; i++) {
        LibFile *f = g_ptr_array_index(A.lib->vcds, i);
        Entry *e = g_new0(Entry, 1);
        char *title = NULL;

        e->platform = PS2GM_PS1;
        e->vcd_name = g_strdup(f->name);
        e->size_kb = f->size / 1024;
        if (vcd_name_parse(f->name, &e->id, &title) && title && *title)
            e->title = title;
        else {
            g_free(title);
            e->title = g_strndup(f->name, strlen(f->name) > 4 ? strlen(f->name) - 4 : strlen(f->name)); /* without .VCD */
        }
        g_ptr_array_add(A.entries, e);
    }
    for (i = 0; i < A.entries->len; i++) {
        Entry *e = g_ptr_array_index(A.entries, i);

        e->card = make_card(e);
        e->row = make_row(e);
        gtk_flow_box_append(GTK_FLOW_BOX(A.flow), e->card);
        gtk_list_box_append(GTK_LIST_BOX(A.list), e->row);
    }
    keep = selected_entry();
    if (keep == NULL && A.entries->len > 0) { /* nothing chosen yet: the first one in the order shown */
        GPtrArray *sorted = g_ptr_array_new();

        for (i = 0; i < A.entries->len; i++)
            g_ptr_array_add(sorted, g_ptr_array_index(A.entries, i));
        g_ptr_array_sort(sorted, cmp_for_sort);
        keep = g_ptr_array_index(sorted, 0);
        g_ptr_array_free(sorted, TRUE);
    }
    select_entry(keep);
    on_filter_changed();
    rebuild_apps(); /* the same disk reading also holds the list of programs */
    transfers_show_covers();
    rebuild_saves();
}

/* ---- removal and art jobs ---------------------------------------------------------------------- */

static gboolean job_remove(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Target *t = data;

    (void)notes;
    cb(0, 1, _("Removing"), pd);
    return t->platform == PS2GM_PS2 ? ops_remove_ps2(A.ops, t->a, t->b, error) : ops_remove_ps1(A.ops, t->a, t->with_folder, notes, error);
}

void after_change(gboolean ok, gpointer data)
{
    (void)data;
    if (ok)
        toast(_("Done"));
    refresh_library();
}

void do_remove_ps2(const char *id, const char *title)
{
    Target *t = g_new0(Target, 1);

    t->platform = PS2GM_PS2;
    t->a = g_strdup(id);
    t->b = g_strdup(title);
    run_job(_("Removing the game"), job_remove, t, target_free, after_change);
}

void do_remove_ps1(const char *name, gboolean with_folder)
{
    Target *t = g_new0(Target, 1);

    t->with_folder = with_folder;
    t->platform = PS2GM_PS1;
    t->a = g_strdup(name);
    run_job(_("Removing the game"), job_remove, t, target_free, after_change);
}

static gboolean job_art(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Target *t = data;

    return ops_update_art(A.ops, t->a, t->platform, t->b, t->replace, notes, cb, pd, error);
}

void do_update_art(const char *id, Ps2gmPlatform platform, const char *title, gboolean replace)
{
    Target *t = g_new0(Target, 1);

    t->platform = platform;
    t->replace = replace;
    t->a = g_strdup(id);
    t->b = g_strdup(title);
    run_job(_("Downloading the art"), job_art, t, target_free, after_change);
}

void on_refresh(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    refresh_library();
}


/* ---- the page ---------------------------------------------------------------------------------- */

static GtkWidget *panel_button(const char *label, GCallback cb, const char *css_class)
{
    GtkWidget *b = gtk_button_new_with_label(label);

    gtk_widget_add_css_class(b, "pill");
    gtk_widget_add_css_class(b, css_class);
    gtk_widget_set_hexpand(b, TRUE);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *dropdown(const char *const *items, int which)
{
    GtkWidget *dd = gtk_drop_down_new_from_strings(items);

    gtk_widget_set_size_request(dd, 108, -1);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(on_dropdown_changed), GINT_TO_POINTER(which));
    return dd;
}

static void on_add_game(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    adw_view_stack_set_visible_child_name(A.stack, "add");
}

GtkWidget *build_library_page(void)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *eyebrow = gtk_label_new(NULL), *title = gtk_label_new(_("Game library")), *add = gtk_button_new(), *add_content = adw_button_content_new();
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *toggles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *grid_scroll = gtk_scrolled_window_new(), *list_scroll = gtk_scrolled_window_new(), *tb_grid, *tb_list;
    GtkWidget *details = gtk_scrolled_window_new(), *dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    static const char *platform_items[4], *games_items[4], *sort_items[5];
    char *eb = g_utf8_strup(_("On the disk"), -1);
    int i;

    /* title block and the Add game button */
    gtk_label_set_text(GTK_LABEL(eyebrow), eb);
    gtk_widget_add_css_class(eyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0);
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    A.count_label = gtk_label_new("");
    gtk_widget_add_css_class(A.count_label, "page-sub");
    gtk_label_set_xalign(GTK_LABEL(A.count_label), 0);
    gtk_box_append(GTK_BOX(titles), eyebrow);
    gtk_box_append(GTK_BOX(titles), title);
    gtk_box_append(GTK_BOX(titles), A.count_label);
    gtk_widget_set_hexpand(titles, TRUE);
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(add_content), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(add_content), _("Add game"));
    gtk_button_set_child(GTK_BUTTON(add), add_content);
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_add_css_class(add, "pill");
    gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_game), NULL);
    gtk_box_append(GTK_BOX(head), titles);
    gtk_box_append(GTK_BOX(head), add);

    /* search, filters, sorting, the view */
    A.search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(A.search), _("Search games or serial..."));
    gtk_widget_set_hexpand(A.search, TRUE);
    gtk_widget_set_size_request(A.search, 170, -1);
    gtk_widget_add_css_class(A.search, "search-field");
    g_signal_connect(A.search, "search-changed", G_CALLBACK(on_search_changed), NULL);
    platform_items[0] = _("All platforms"); platform_items[1] = "PS2"; platform_items[2] = "PS1"; platform_items[3] = NULL;
    games_items[0] = _("All games"); games_items[1] = _("With cheats"); games_items[2] = _("Missing cover"); games_items[3] = NULL;
    sort_items[0] = _("Name A-Z"); sort_items[1] = _("Name Z-A"); sort_items[2] = _("Largest first"); sort_items[3] = _("Smallest first"); sort_items[4] = NULL;
    tb_grid = gtk_toggle_button_new();
    tb_list = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(tb_grid), "view-grid-symbolic");
    gtk_button_set_icon_name(GTK_BUTTON(tb_list), "view-list-symbolic");
    gtk_widget_set_tooltip_text(tb_grid, _("Covers"));
    gtk_widget_set_tooltip_text(tb_list, _("List"));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tb_list), GTK_TOGGLE_BUTTON(tb_grid));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_grid), TRUE);
    g_signal_connect(tb_grid, "toggled", G_CALLBACK(on_view_toggled), GINT_TO_POINTER(0));
    g_signal_connect(tb_list, "toggled", G_CALLBACK(on_view_toggled), GINT_TO_POINTER(1));
    gtk_widget_add_css_class(toggles, "view-toggle");
    gtk_box_append(GTK_BOX(toggles), tb_grid);
    gtk_box_append(GTK_BOX(toggles), tb_list);
    gtk_box_append(GTK_BOX(filters), A.search);
    gtk_box_append(GTK_BOX(filters), dropdown(platform_items, 0));
    gtk_box_append(GTK_BOX(filters), dropdown(games_items, 1));
    gtk_box_append(GTK_BOX(filters), dropdown(sort_items, 2));
    gtk_box_append(GTK_BOX(filters), toggles);
    gtk_widget_add_css_class(filters, "filterbar");

    /* the covers and the list */
    A.flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(A.flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(A.flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(A.flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(A.flow), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(A.flow), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(A.flow), 6);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(A.flow), FALSE);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(A.flow), flow_filter, NULL, NULL);
    gtk_flow_box_set_sort_func(GTK_FLOW_BOX(A.flow), flow_sort, NULL, NULL);
    gtk_widget_set_halign(A.flow, GTK_ALIGN_START);
    gtk_widget_set_valign(A.flow, GTK_ALIGN_START);
    gtk_widget_set_margin_end(A.flow, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroll), A.flow);
    A.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(A.list), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(A.list), TRUE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(A.list), list_filter, NULL, NULL);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(A.list), list_sort, NULL, NULL);
    gtk_widget_add_css_class(A.list, "transparent-list");
    g_signal_connect(A.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), A.list);
    A.view_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(A.view_stack), grid_scroll, "grid");
    gtk_stack_add_named(GTK_STACK(A.view_stack), list_scroll, "list");
    gtk_widget_set_vexpand(A.view_stack, TRUE);
    A.empty_label = gtk_label_new(_("No game matches the search or the filters."));
    gtk_widget_add_css_class(A.empty_label, "page-sub");
    gtk_widget_set_visible(A.empty_label, FALSE);

    gtk_widget_set_margin_top(left, 22);
    gtk_widget_set_margin_bottom(left, 16);
    gtk_widget_set_margin_start(left, 28);
    gtk_widget_set_margin_end(left, 20);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_box_append(GTK_BOX(left), head);
    gtk_box_append(GTK_BOX(left), filters);
    gtk_box_append(GTK_BOX(left), A.empty_label);
    gtk_box_append(GTK_BOX(left), A.view_stack);

    /* the details panel */
    A.det_cover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(A.det_cover_box, GTK_ALIGN_CENTER);
    A.det_tags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(A.det_tags, GTK_ALIGN_CENTER);
    A.det_title = gtk_label_new("");
    gtk_widget_add_css_class(A.det_title, "details-title");
    gtk_label_set_wrap(GTK_LABEL(A.det_title), TRUE);
    gtk_label_set_justify(GTK_LABEL(A.det_title), GTK_JUSTIFY_CENTER);
    A.det_id = gtk_label_new("");
    gtk_widget_add_css_class(A.det_id, "details-id");
    A.det_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(A.det_content), A.det_cover_box);
    gtk_box_append(GTK_BOX(A.det_content), A.det_tags);
    gtk_box_append(GTK_BOX(A.det_content), A.det_title);
    gtk_box_append(GTK_BOX(A.det_content), A.det_id);
    {
        GtkWidget *hr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class(hr, "hr");
        gtk_widget_set_margin_top(hr, 6);
        gtk_widget_set_margin_bottom(hr, 4);
        gtk_box_append(GTK_BOX(A.det_content), hr);
    }
    gtk_box_append(GTK_BOX(A.det_content), detail_row(_("Platform"), &A.det_vals[0]));
    gtk_box_append(GTK_BOX(A.det_content), detail_row(_("Region"), &A.det_vals[1]));
    gtk_box_append(GTK_BOX(A.det_content), detail_row(_("Disc"), &A.det_vals[2]));
    gtk_box_append(GTK_BOX(A.det_content), detail_row(_("Size"), &A.det_vals[3]));
    A.det_settings = panel_button(_("Game settings"), G_CALLBACK(on_det_settings), "suggested-action");
    A.det_art = panel_button(_("Pictures..."), G_CALLBACK(on_det_art), "btn-secondary");
    A.det_cheats = panel_button(_("Cheats..."), G_CALLBACK(on_det_cheats), "btn-secondary");
    A.det_fixes = panel_button(_("PS1 fixes..."), G_CALLBACK(on_det_fixes), "btn-secondary");
    A.det_share = panel_button(_("Share memory card"), G_CALLBACK(on_det_share), "btn-secondary");
    gtk_widget_set_tooltip_text(A.det_share, _("Make all the discs of this game use one memory card"));
    A.det_rm = panel_button(_("Remove"), G_CALLBACK(on_det_remove), "btn-danger");
    gtk_widget_set_margin_top(A.det_settings, 8);
    gtk_box_append(GTK_BOX(A.det_content), A.det_settings);
    gtk_box_append(GTK_BOX(A.det_content), A.det_art);
    gtk_box_append(GTK_BOX(A.det_content), A.det_cheats);
    gtk_box_append(GTK_BOX(A.det_content), A.det_fixes);
    gtk_box_append(GTK_BOX(A.det_content), A.det_share);
    gtk_box_append(GTK_BOX(A.det_content), A.det_rm);

    A.det_empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    A.det_empty_title = gtk_label_new("");
    gtk_widget_add_css_class(A.det_empty_title, "details-title");
    A.det_empty_sub = gtk_label_new("");
    gtk_widget_add_css_class(A.det_empty_sub, "page-sub");
    gtk_label_set_wrap(GTK_LABEL(A.det_empty_sub), TRUE);
    gtk_label_set_justify(GTK_LABEL(A.det_empty_sub), GTK_JUSTIFY_CENTER);
    {
        GdkPaintable *art = pixel_art("ps2-controller.png", 96);

        if (art != NULL) {
            GtkWidget *pic = gtk_picture_new_for_paintable(art);

            gtk_picture_set_can_shrink(GTK_PICTURE(pic), FALSE);
            gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_bottom(pic, 10);
            gtk_box_append(GTK_BOX(A.det_empty), pic);
            g_object_unref(art);
        }
    }
    gtk_widget_set_margin_top(A.det_empty, 80);
    gtk_box_append(GTK_BOX(A.det_empty), A.det_empty_title);
    gtk_box_append(GTK_BOX(A.det_empty), A.det_empty_sub);

    gtk_widget_set_margin_top(dbox, 22);
    gtk_widget_set_margin_bottom(dbox, 20);
    gtk_widget_set_margin_start(dbox, 22);
    gtk_widget_set_margin_end(dbox, 22);
    gtk_box_append(GTK_BOX(dbox), A.det_content);
    gtk_box_append(GTK_BOX(dbox), A.det_empty);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(details), dbox);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    {
        /* the panel in layers: blurred cover, dark veil, then the panel itself */
        GtkWidget *glass = gtk_overlay_new(), *scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        A.det_backdrop = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(A.det_backdrop), GTK_CONTENT_FIT_COVER);
        gtk_picture_set_can_shrink(GTK_PICTURE(A.det_backdrop), TRUE);
        gtk_widget_add_css_class(A.det_backdrop, "details-backdrop");
        gtk_widget_add_css_class(scrim, "details-scrim");
        gtk_overlay_set_child(GTK_OVERLAY(glass), A.det_backdrop);
        gtk_overlay_add_overlay(GTK_OVERLAY(glass), scrim);
        gtk_overlay_add_overlay(GTK_OVERLAY(glass), details);
        gtk_overlay_set_measure_overlay(GTK_OVERLAY(glass), details, TRUE); /* the panel decides the size, not the picture */
        gtk_widget_set_vexpand(details, TRUE);
        gtk_widget_set_size_request(glass, 330, -1);
        gtk_widget_set_overflow(glass, GTK_OVERFLOW_HIDDEN);
        gtk_widget_add_css_class(glass, "details");
        gtk_box_append(GTK_BOX(root), left);
        gtk_box_append(GTK_BOX(root), glass);
    }
    update_details();
    update_subtitle();
    for (i = 0; i < 0; i++)
        ;
    g_free(eb);
    return root;
}
