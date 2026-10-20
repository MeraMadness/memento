#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include "art.h"

#define COVER_URL "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/%s.jpg"
#define ARCHIVE_URL "https://archive.org/download/OPLM_ART_2023_11/OPLM_ART_2023_11.zip/%s/%s/%s_%s"
#define GAMEINDEX_URL "https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/GameIndex.yaml"

char *ps2gm_serial(const char *id)
{
    gchar *s = g_strdup(id);
    gchar *dot = strchr(s, '.');

    if (dot != NULL)
        memmove(dot, dot + 1, strlen(dot)); /* drop the dot */
    for (gchar *p = s; *p; p++)
        if (*p == '_')
            *p = '-';
    return s;
}

/* ---- HTTP -------------------------------------------------------------------------------------- */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    g_byte_array_append((GByteArray *)ud, (const guint8 *)ptr, size * nmemb);
    return size * nmemb;
}

/*
 * NULL without an error set means "not available" (404, or archive.org's 503 for a file that is not in
 * the zip). A network failure is an error. 429/503 are retried briefly, they can also mean "busy".
 */
GBytes *ps2gm_http_get(const char *url, GError **error)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        CURL *c = curl_easy_init();
        GByteArray *buf = g_byte_array_new();
        long code = 0;
        CURLcode rc;

        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "ps2gm/0.1 (personal PS2 game manager)");
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, buf);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);

        if (rc == CURLE_OK && code == 200)
            return g_byte_array_free_to_bytes(buf);
        g_byte_array_free(buf, TRUE);
        if (rc == CURLE_OK && code != 429 && code != 503)
            return NULL; /* not there */
        if (attempt == 2) {
            if (rc != CURLE_OK)
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_NET, _("Download failed (%s): %s"), curl_easy_strerror(rc), url);
            return NULL; /* busy or missing: no art, not a failure */
        }
        g_usleep((attempt + 1) * 1000000);
    }
    return NULL;
}

/* ---- images ------------------------------------------------------------------------------------ */

static gboolean save_png(GBytes *data, const char *path, int w, int h, GError **error)
{
    GInputStream *in = g_memory_input_stream_new_from_bytes(data);
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream(in, NULL, error), *scaled = NULL;
    gboolean ok;

    g_object_unref(in);
    if (pb == NULL)
        return FALSE;
    if (w > 0 && h > 0) {
        scaled = gdk_pixbuf_scale_simple(pb, w, h, GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = scaled;
    }
    ok = gdk_pixbuf_save(pb, path, "png", error, NULL);
    g_object_unref(pb);
    return ok;
}

typedef struct {
    const char *remote; /* file suffix in the archive */
    const char *local;  /* OPL art type */
} ArtKind;

static const ArtKind kinds[] = {
    {"COV2.jpg", "COV2"}, {"ICO.png", "ICO"}, {"LGO.png", "LGO"}, {"LAB.jpg", "LAB"},
    {"BG_00.jpg", "BG"},  {"SCR_00.jpg", "SCR"}, {"SCR_01.jpg", "SCR2"},
};

static gboolean skipped(char *const *skip, const char *name)
{
    int i;

    for (i = 0; skip && skip[i]; i++)
        if (!strcmp(skip[i], name))
            return TRUE;
    return FALSE;
}

char **art_fetch(const char *id, Ps2gmPlatform platform, const char *out_dir, char *const *skip,
                 Ps2gmProgress progress, gpointer user_data, GError **error)
{
    GPtrArray *made = g_ptr_array_new_with_free_func(g_free);
    const char *folder = platform == PS2GM_PS1 ? "PS1" : "PS2";
    gchar *name, *path, *url, *serial;
    GBytes *data;
    GError *err = NULL;
    guint i;

    g_mkdir_with_parents(out_dir, 0755);

    /* front cover: the PS2 cover project first (clean, 140x200), else the archive's */
    name = g_strdup_printf("%s_COV.png", id);
    if (!skipped(skip, name)) {
        data = NULL;
        if (platform == PS2GM_PS2) {
            serial = ps2gm_serial(id);
            url = g_strdup_printf(COVER_URL, serial);
            data = ps2gm_http_get(url, &err);
            g_free(url);
            g_free(serial);
        }
        if (data == NULL && err == NULL) {
            url = g_strdup_printf(ARCHIVE_URL, folder, id, id, "COV.jpg");
            data = ps2gm_http_get(url, &err);
            g_free(url);
        }
        if (err) {
            g_propagate_error(error, err);
            g_free(name);
            g_ptr_array_free(made, TRUE);
            return NULL;
        }
        if (data != NULL) {
            path = g_build_filename(out_dir, name, NULL);
            /* PS2 covers are DVD-case shaped (140x200); PS1 covers are square jewel cases and must not be stretched */
            if (save_png(data, path, platform == PS2GM_PS1 ? 0 : 140, platform == PS2GM_PS1 ? 0 : 200, NULL))
                g_ptr_array_add(made, g_strdup(name));
            g_free(path);
            g_bytes_unref(data);
        }
    }
    g_free(name);

    for (i = 0; i < G_N_ELEMENTS(kinds); i++) {
        if (progress && !progress(i, G_N_ELEMENTS(kinds), kinds[i].local, user_data))
            break;
        name = g_strdup_printf("%s_%s.png", id, kinds[i].local);
        if (!skipped(skip, name)) {
            url = g_strdup_printf(ARCHIVE_URL, folder, id, id, kinds[i].remote);
            data = ps2gm_http_get(url, &err);
            g_free(url);
            if (err) { /* a network error on one image should not lose the others */
                g_clear_error(&err);
            } else if (data != NULL) {
                path = g_build_filename(out_dir, name, NULL);
                if (save_png(data, path, 0, 0, NULL))
                    g_ptr_array_add(made, g_strdup(name));
                g_free(path);
                g_bytes_unref(data);
            }
        }
        g_free(name);
    }
    g_ptr_array_add(made, NULL);
    return (char **)g_ptr_array_free(made, FALSE);
}

/* ---- pictures of your own and single pictures ---------------------------------------------------- */

static const ArtType art_type_list[] = {
    {"COV", N_("Front cover"), 140, 200},
    {"COV2", N_("Back cover"), 242, 344},
    {"BG", N_("Background"), 640, 480},
    {"LGO", N_("Logo"), 300, 125},
    {"ICO", N_("Disc icon"), 64, 64},
    {"SCR", N_("Screenshot 1"), 320, 240},
    {"SCR2", N_("Screenshot 2"), 320, 240},
};

const ArtType *art_types(guint *count)
{
    if (count)
        *count = G_N_ELEMENTS(art_type_list);
    return art_type_list;
}

const ArtType *art_type_find(const char *key)
{
    guint i;

    for (i = 0; key && i < G_N_ELEMENTS(art_type_list); i++)
        if (!strcmp(art_type_list[i].key, key))
            return &art_type_list[i];
    return NULL;
}

gboolean art_import_bytes(GBytes *data, const char *id, const char *key, Ps2gmPlatform platform,
                          gboolean whole, const char *out_dir, char **out_name, GError **error)
{
    const ArtType *type = art_type_find(key);
    GInputStream *in;
    GdkPixbuf *pb, *out;
    gchar *name, *path;
    int w, h, mw, mh;
    gboolean ok;

    if (type == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Unknown kind of picture: %s"), key ? key : "(none)");
        return FALSE;
    }
    in = g_memory_input_stream_new_from_bytes(data);
    pb = gdk_pixbuf_new_from_stream(in, NULL, NULL);
    g_object_unref(in);
    if (pb == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That is not a picture I can read (use PNG, JPG or BMP)"));
        return FALSE;
    }
    w = gdk_pixbuf_get_width(pb);
    h = gdk_pixbuf_get_height(pb);
    mw = type->max_w;
    mh = type->max_h;
    if (platform == PS2GM_PS1 && !strcmp(key, "COV"))
        mw = mh = 200; /* PS1 covers are square: no cover frame shape to fill */

    if (platform == PS2GM_PS2 && !strcmp(key, "COV") && !whole) {
        /* fill the 7:10 frame: cut the picture to that shape (centered), then scale */
        int cw = w, ch = h, cx = 0, cy = 0;

        if (w * 10 > h * 7) { /* too wide */
            cw = h * 7 / 10;
            cx = (w - cw) / 2;
        } else { /* too tall */
            ch = w * 10 / 7;
            cy = (h - ch) / 2;
        }
        if (cw < 1 || ch < 1) {
            g_object_unref(pb);
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That picture is too small"));
            return FALSE;
        }
        {
            GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(pb, cx, cy, cw, ch);

            out = gdk_pixbuf_scale_simple(sub, mw, mh, GDK_INTERP_HYPER);
            g_object_unref(sub);
        }
    } else if (w > mw || h > mh) {
        double f = MIN((double)mw / w, (double)mh / h);

        out = gdk_pixbuf_scale_simple(pb, MAX(1, (int)(w * f)), MAX(1, (int)(h * f)), GDK_INTERP_HYPER);
    } else
        out = g_object_ref(pb);
    g_object_unref(pb);

    g_mkdir_with_parents(out_dir, 0755);
    name = g_strdup_printf("%s_%s.png", id, key);
    path = g_build_filename(out_dir, name, NULL);
    ok = gdk_pixbuf_save(out, path, "png", error, NULL);
    g_object_unref(out);
    g_free(path);
    if (ok && out_name)
        *out_name = name;
    else
        g_free(name);
    return ok;
}

gboolean art_import_file(const char *path, const char *id, const char *key, Ps2gmPlatform platform,
                         gboolean whole, const char *out_dir, char **out_name, GError **error)
{
    gchar *contents = NULL;
    gsize len = 0;
    GBytes *bytes;
    gboolean ok;

    if (!g_file_get_contents(path, &contents, &len, error))
        return FALSE;
    if (len > 64 * 1024 * 1024) {
        g_free(contents);
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That file is too big for a picture"));
        return FALSE;
    }
    bytes = g_bytes_new_take(contents, len);
    ok = art_import_bytes(bytes, id, key, platform, whole, out_dir, out_name, error);
    g_bytes_unref(bytes);
    return ok;
}

gboolean art_import_url(const char *url, const char *id, const char *key, Ps2gmPlatform platform,
                        gboolean whole, const char *out_dir, char **out_name, GError **error)
{
    GBytes *data;
    gboolean ok;

    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("The address must start with http:// or https://"));
        return FALSE;
    }
    data = ps2gm_http_get(url, error);
    if (data == NULL) {
        if (error && *error == NULL)
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_NET, _("Nothing to download at that address"));
        return FALSE;
    }
    ok = art_import_bytes(data, id, key, platform, whole, out_dir, out_name, error);
    g_bytes_unref(data);
    return ok;
}

gboolean art_fetch_one(const char *id, Ps2gmPlatform platform, const char *key, const char *out_dir,
                       char **out_name, GError **error)
{
    const char *folder = platform == PS2GM_PS1 ? "PS1" : "PS2";
    const char *remote = NULL;
    GBytes *data = NULL;
    GError *err = NULL;
    gchar *url;
    gboolean ok;
    guint i;

    if (art_type_find(key) == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Unknown kind of picture: %s"), key ? key : "(none)");
        return FALSE;
    }
    if (!strcmp(key, "COV")) {
        if (platform == PS2GM_PS2) {
            gchar *serial = ps2gm_serial(id);

            url = g_strdup_printf(COVER_URL, serial);
            data = ps2gm_http_get(url, &err);
            g_free(url);
            g_free(serial);
        }
        remote = "COV.jpg";
    } else
        for (i = 0; i < G_N_ELEMENTS(kinds); i++)
            if (!strcmp(kinds[i].local, key))
                remote = kinds[i].remote;
    if (data == NULL && err == NULL && remote != NULL) {
        url = g_strdup_printf(ARCHIVE_URL, folder, id, id, remote);
        data = ps2gm_http_get(url, &err);
        g_free(url);
    }
    if (err) {
        g_propagate_error(error, err);
        return FALSE;
    }
    if (data == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_NET, _("No %s picture was found online for %s"),
                    _(art_type_find(key)->label), id);
        return FALSE;
    }
    ok = art_import_bytes(data, id, key, platform, FALSE, out_dir, out_name, error);
    g_bytes_unref(data);
    return ok;
}

/* ---- cover candidates -------------------------------------------------------------------------- */

#define COVER3D_URL "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/3d/%s.png"

void cover_candidate_free(gpointer p)
{
    CoverCandidate *c = p;

    g_free(c->label);
    g_free(c->path);
    g_free(c);
}

static void add_candidate(GPtrArray *list, const char *out_dir, const char *label, const char *url, gboolean whole)
{
    GBytes *data = ps2gm_http_get(url, NULL);
    const char *ext = g_str_has_suffix(url, ".png") ? "png" : "jpg";
    gchar *path;
    gsize n;
    const void *d;

    if (data == NULL)
        return;
    d = g_bytes_get_data(data, &n);
    path = g_strdup_printf("%s/candidate-%u.%s", out_dir, list->len + 1, ext);
    if (g_file_set_contents(path, d, n, NULL)) {
        CoverCandidate *c = g_new0(CoverCandidate, 1);

        c->label = g_strdup(label);
        c->path = path;
        c->whole = whole;
        g_ptr_array_add(list, c);
    } else
        g_free(path);
    g_bytes_unref(data);
}

GPtrArray *art_cover_candidates(const char *id, Ps2gmPlatform platform, const char *out_dir)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(cover_candidate_free);
    gchar *url;

    g_mkdir_with_parents(out_dir, 0755);
    if (platform == PS2GM_PS2) {
        gchar *serial = ps2gm_serial(id);

        url = g_strdup_printf(COVER_URL, serial);
        add_candidate(list, out_dir, N_("Flat cover"), url, FALSE);
        g_free(url);
        url = g_strdup_printf(COVER3D_URL, serial);
        add_candidate(list, out_dir, N_("3D box art"), url, TRUE);
        g_free(url);
        g_free(serial);
    }
    url = g_strdup_printf(ARCHIVE_URL, platform == PS2GM_PS1 ? "PS1" : "PS2", id, id, "COV.jpg");
    add_candidate(list, out_dir, N_("OPL Manager archive"), url, FALSE);
    g_free(url);
    return list;
}

/* ---- titles ------------------------------------------------------------------------------------ */

static GHashTable *titles;

static void load_titles(void)
{
    gchar *dir = g_build_filename(g_get_user_cache_dir(), "ps2gm", NULL);
    gchar *file = g_build_filename(dir, "GameIndex.yaml", NULL), *text = NULL, **lines;
    GStatBuf st;
    int i;
    char *cur = NULL;

    titles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_mkdir_with_parents(dir, 0755);
    if (g_stat(file, &st) != 0 || g_get_real_time() / G_USEC_PER_SEC - st.st_mtime > 30 * 86400) {
        GBytes *b = ps2gm_http_get(GAMEINDEX_URL, NULL);

        if (b != NULL) {
            gsize n;
            const char *d = g_bytes_get_data(b, &n);

            g_file_set_contents(file, d, n, NULL);
            g_bytes_unref(b);
        }
    }
    if (g_file_get_contents(file, &text, NULL, NULL)) {
        lines = g_strsplit(text, "\n", -1);
        for (i = 0; lines[i] != NULL; i++) {
            const char *l = lines[i];

            if (l[0] && !g_ascii_isspace(l[0]) && l[0] != '#') {
                size_t n = strlen(l);

                g_free(cur);
                cur = (n > 1 && l[n - 1] == ':') ? g_strndup(l, n - 1) : NULL;
            } else if (cur && g_str_has_prefix(l, "  name:") && !g_hash_table_contains(titles, cur)) {
                gchar *v = g_strstrip(g_strdup(l + 7));
                size_t n = strlen(v);

                if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                    memmove(v, v + 1, n - 2);
                    v[n - 2] = '\0';
                }
                g_hash_table_insert(titles, g_strdup(cur), v);
            }
        }
        g_free(cur);
        g_strfreev(lines);
        g_free(text);
    }
    g_free(dir);
    g_free(file);
}

char *ps2_title_lookup(const char *id)
{
    gchar *serial = ps2gm_serial(id);
    const char *t;
    char *r;

    if (titles == NULL)
        load_titles();
    t = g_hash_table_lookup(titles, serial);
    r = t ? g_strdup(t) : NULL;
    g_free(serial);
    return r;
}

/* ---- config ------------------------------------------------------------------------------------ */

gboolean cfg_write(const char *path, const char *title, const char *description, GError **error)
{
    GString *s = g_string_new(NULL);
    gboolean ok;

    g_string_append_printf(s, "Title=%s\n", title);
    if (description != NULL && *description != '\0')
        g_string_append_printf(s, "Description=%s\n", description);
    ok = g_file_set_contents(path, s->str, s->len, error);
    g_string_free(s, TRUE);
    return ok;
}
