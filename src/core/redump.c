#include "redump.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <glib/gstdio.h>

#define MAX_DAT_BYTES (256u * 1024 * 1024)

static const char *platform_key(Ps2gmPlatform p)
{
    return p == PS2GM_PS1 ? "ps1" : "ps2";
}

static const char *platform_url(Ps2gmPlatform p)
{
    return p == PS2GM_PS1 ? "http://redump.org/datfile/psx/" : "http://redump.org/datfile/ps2/";
}

static char *index_path(Ps2gmPlatform p)
{
    char *name = g_strdup_printf("%s.tsv", platform_key(p)), *path = g_build_filename(g_get_user_cache_dir(), "ps2gm", "redump", name, NULL);

    g_free(name);
    return path;
}

/* the index in memory: md5 -> "size<TAB>name"; dropped when the list is replaced */
static GHashTable *loaded[2];

static void forget(Ps2gmPlatform p)
{
    int i = p == PS2GM_PS1;

    g_clear_pointer(&loaded[i], g_hash_table_unref);
}

static GHashTable *load_index(Ps2gmPlatform p)
{
    int i = p == PS2GM_PS1;
    char *path, *text = NULL;
    gsize len;

    if (loaded[i] != NULL)
        return loaded[i];
    path = index_path(p);
    if (g_file_get_contents(path, &text, &len, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        guint k;

        loaded[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        for (k = 0; lines[k] != NULL; k++) {
            char *tab = strchr(lines[k], '\t');

            if (tab == NULL || tab - lines[k] != 32)
                continue;
            g_hash_table_replace(loaded[i], g_strndup(lines[k], 32), g_strdup(tab + 1));
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
    return loaded[i];
}

/* ---- the zip: the first file of an archive, stored or deflated ---------------------------------- */

static guint32 rd32(const guint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (guint32)p[3] << 24; }
static guint32 rd16(const guint8 *p) { return p[0] | p[1] << 8; }

static GBytes *zip_first_file(GBytes *zip, GError **error)
{
    gsize size, i;
    const guint8 *d = g_bytes_get_data(zip, &size);
    guint32 cd, method, csize, usize, local, start;
    guint8 *out;

    for (i = size >= 22 ? size - 22 : 0; i > 0 && rd32(d + i) != 0x06054b50; i--)
        ;
    if (size < 22 || rd32(d + i) != 0x06054b50 || i + 22 > size)
        goto bad;
    cd = rd32(d + i + 16);
    if ((gsize)cd + 46 > size || rd32(d + cd) != 0x02014b50)
        goto bad;
    method = rd16(d + cd + 10);
    csize = rd32(d + cd + 20);
    usize = rd32(d + cd + 24);
    local = rd32(d + cd + 42);
    if ((gsize)local + 30 > size || rd32(d + local) != 0x04034b50 || usize > MAX_DAT_BYTES)
        goto bad;
    start = local + 30 + rd16(d + local + 26) + rd16(d + local + 28);
    if ((gsize)start + csize > size)
        goto bad;
    out = g_malloc((gsize)usize + 1);
    if (method == 0 && csize == usize) {
        memcpy(out, d + start, usize);
    } else if (method == 8) {
        z_stream z = {0};
        int rc;

        if (inflateInit2(&z, -15) != Z_OK) {
            g_free(out);
            goto bad;
        }
        z.next_in = (Bytef *)(d + start);
        z.avail_in = csize;
        z.next_out = out;
        z.avail_out = usize;
        rc = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (rc != Z_STREAM_END || z.total_out != usize) {
            g_free(out);
            goto bad;
        }
    } else {
        g_free(out);
        goto bad;
    }
    out[usize] = '\0';
    return g_bytes_new_take(out, (gsize)usize);
bad:
    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("That is not a usable Redump list"));
    return NULL;
}

/* ---- the DAT: <game name="..."><rom name=".." size=".." md5=".."/></game> ------------------------- */

typedef struct {
    GString *out;
    char *game;
    guint count;
} DatState;

static void dat_start(GMarkupParseContext *c, const char *element, const char **names, const char **values, gpointer data, GError **error)
{
    DatState *s = data;
    guint i;

    (void)c;
    (void)error;
    if (!strcmp(element, "game") || !strcmp(element, "machine")) {
        g_free(s->game);
        s->game = NULL;
        for (i = 0; names[i] != NULL; i++)
            if (!strcmp(names[i], "name"))
                s->game = g_strdup(values[i]);
    } else if (!strcmp(element, "rom") && s->game != NULL) {
        const char *md5 = NULL, *size = NULL;

        for (i = 0; names[i] != NULL; i++) {
            if (!strcmp(names[i], "md5"))
                md5 = values[i];
            else if (!strcmp(names[i], "size"))
                size = values[i];
        }
        if (md5 != NULL && strlen(md5) == 32 && size != NULL) {
            char *lower = g_ascii_strdown(md5, -1);

            g_string_append_printf(s->out, "%s\t%s\t%s\n", lower, size, s->game);
            s->count++;
            g_free(lower);
        }
    }
}

gboolean redump_import_dat(Ps2gmPlatform platform, const char *xml, gsize len, GError **error)
{
    static const GMarkupParser parser = {dat_start, NULL, NULL, NULL, NULL};
    DatState s = {g_string_new(NULL), NULL, 0};
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &s, NULL);
    char *path = index_path(platform), *dir = g_path_get_dirname(path);
    gboolean ok = FALSE;

    if (!g_markup_parse_context_parse(ctx, xml, (gssize)len, NULL) || !g_markup_parse_context_end_parse(ctx, NULL) || s.count == 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("That is not a usable Redump list"));
        goto out;
    }
    g_mkdir_with_parents(dir, 0755);
    {
        char *tmp = g_strdup_printf("%s.part", path);

        if (!g_file_set_contents(tmp, s.out->str, (gssize)s.out->len, error) || g_rename(tmp, path) != 0) {
            if (error != NULL && *error == NULL)
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not save the Redump list"));
            g_free(tmp);
            goto out;
        }
        g_free(tmp);
    }
    forget(platform);
    ok = TRUE;
out:
    g_markup_parse_context_free(ctx);
    g_string_free(s.out, TRUE);
    g_free(s.game);
    g_free(path);
    g_free(dir);
    return ok;
}

gboolean redump_import_zip(Ps2gmPlatform platform, GBytes *zip, GError **error)
{
    GBytes *dat = zip_first_file(zip, error);
    gsize len;
    const char *xml;
    gboolean ok;

    if (dat == NULL)
        return FALSE;
    xml = g_bytes_get_data(dat, &len);
    ok = redump_import_dat(platform, xml, len, error);
    g_bytes_unref(dat);
    return ok;
}

gboolean redump_update(Ps2gmPlatform platform, GError **error)
{
    GBytes *zip = ps2gm_http_get(platform_url(platform), error);
    gboolean ok;

    if (zip == NULL)
        return FALSE;
    ok = redump_import_zip(platform, zip, error);
    g_bytes_unref(zip);
    return ok;
}

char *redump_info(Ps2gmPlatform platform)
{
    GHashTable *h = load_index(platform);
    char *path, *when, *result;
    GStatBuf st;
    GDateTime *t;

    if (h == NULL)
        return NULL;
    path = index_path(platform);
    if (g_stat(path, &st) != 0) {
        g_free(path);
        return NULL;
    }
    t = g_date_time_new_from_unix_local(st.st_mtime);
    when = g_date_time_format(t, "%Y-%m-%d");
    result = g_strdup_printf(_("%u images, downloaded %s"), g_hash_table_size(h), when);
    g_date_time_unref(t);
    g_free(when);
    g_free(path);
    return result;
}

/* ---- hashing and looking up --------------------------------------------------------------------- */

gboolean redump_hash_file(const char *path, char **md5, guint64 *size, Ps2gmProgress progress, gpointer user_data, GError **error)
{
    FILE *f = g_fopen(path, "rb");
    GChecksum *sum = g_checksum_new(G_CHECKSUM_MD5);
    guint8 *buf = g_malloc(4 << 20);
    GStatBuf st;
    guint64 done = 0, total = 0;
    size_t n;
    gboolean ok = FALSE;

    if (f == NULL || g_stat(path, &st) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), path);
        goto out;
    }
    total = (guint64)st.st_size;
    while ((n = fread(buf, 1, 4 << 20, f)) > 0) {
        g_checksum_update(sum, buf, (gssize)n);
        done += n;
        if (progress != NULL && !progress(done, total, _("Checking the image"), user_data)) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
            goto out;
        }
    }
    if (ferror(f)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), path);
        goto out;
    }
    *md5 = g_strdup(g_checksum_get_string(sum));
    *size = done;
    ok = TRUE;
out:
    if (f != NULL)
        fclose(f);
    g_checksum_free(sum);
    g_free(buf);
    return ok;
}

RedumpResult redump_lookup(Ps2gmPlatform platform, const char *md5, guint64 size, char **name)
{
    GHashTable *h = load_index(platform);
    const char *v;
    char *lower, *tab;
    guint64 want;

    if (h == NULL)
        return REDUMP_NO_LIST;
    lower = g_ascii_strdown(md5, -1);
    v = g_hash_table_lookup(h, lower);
    g_free(lower);
    if (v == NULL)
        return REDUMP_UNKNOWN;
    want = g_ascii_strtoull(v, &tab, 10);
    if (want != size || *tab != '\t')
        return REDUMP_UNKNOWN; /* the same hash with another size cannot be the same image */
    if (name != NULL)
        *name = g_strdup(tab + 1);
    return REDUMP_MATCH;
}
