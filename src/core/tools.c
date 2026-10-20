#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "tools.h"

#include <stdlib.h>
#include <sys/stat.h>

/* ---- setup ------------------------------------------------------------------------------------- */

gboolean ps2gm_path_root_safe(const char *path, char **why)
{
    char *real = realpath(path, NULL), *cur;

    if (why != NULL)
        *why = NULL;
    if (real == NULL) {
        if (why != NULL)
            *why = g_strdup_printf(_("%s cannot be found"), path);
        return FALSE;
    }
    cur = g_strdup(real);
    for (;;) {
        struct stat st;
        char *slash;

        if (lstat(cur, &st) != 0 || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            if (why != NULL)
                *why = g_strdup_printf(_("%s is not owned by root or can be changed by other users"), cur);
            g_free(cur);
            free(real);
            return FALSE;
        }
        if (!strcmp(cur, "/"))
            break;
        slash = strrchr(cur, '/');
        if (slash == cur)
            slash[1] = '\0'; /* the folder above is "/" */
        else
            *slash = '\0';
    }
    g_free(cur);
    free(real);
    return TRUE;
}

gboolean ps2gm_tools_init(Ps2gmTools *t, const char *dir, GError **error)
{
    gchar *self = NULL;

    memset(t, 0, sizeof(*t));
    if (dir == NULL) {
        self = g_file_read_link("/proc/self/exe", NULL);
        t->dir = self ? g_path_get_dirname(self) : g_strdup(".");
        g_free(self);
    } else
        t->dir = g_strdup(dir);
    t->hdl_dump = g_build_filename(t->dir, "hdl_dump", NULL);
    t->pfsshell = g_build_filename(t->dir, "pfsshell", NULL);
    if (!g_file_test(t->hdl_dump, G_FILE_TEST_IS_EXECUTABLE) ||
        !g_file_test(t->pfsshell, G_FILE_TEST_IS_EXECUTABLE)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("hdl_dump and pfsshell were not found in %s"), t->dir);
        ps2gm_tools_clear(t);
        return FALSE;
    }
    return TRUE;
}

void ps2gm_tools_clear(Ps2gmTools *t)
{
    g_clear_pointer(&t->dir, g_free);
    g_clear_pointer(&t->hdl_dump, g_free);
    g_clear_pointer(&t->pfsshell, g_free);
}

gboolean ps2gm_arg_ok(const char *s)
{
    gsize len;

    if (s == NULL || *s == '\0')
        return FALSE;
    len = strlen(s);
    if (len > 200 || !g_utf8_validate(s, -1, NULL))
        return FALSE;
    if (strpbrk(s, "\"\r\n\\") != NULL)
        return FALSE;
    return strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

gboolean ps2gm_remote_path_ok(const char *path)
{
    gchar **parts;
    int i;
    gboolean ok = TRUE;

    if (path == NULL || *path == '\0') /* the partition root */
        return TRUE;
    if (*path == '/')
        return FALSE;
    parts = g_strsplit(path, "/", -1);
    for (i = 0; parts[i] != NULL; i++)
        if (!ps2gm_arg_ok(parts[i]))
            ok = FALSE;
    g_strfreev(parts);
    return ok;
}

/* ---- running a tool ---------------------------------------------------------------------------- */

static gchar *to_utf8(const guint8 *data, gsize len)
{
    gchar *s = g_strndup((const gchar *)data, len);
    gchar *v = g_utf8_make_valid(s, -1);

    g_free(s);
    return v;
}

/*
 * Runs argv with optional stdin text; output = stdout+stderr merged.
 *
 * The tools write their normal output through a block-buffered pipe (4 KB chunks) but their prompts and
 * messages unbuffered, so in a long listing a prompt can end up in the middle of a line and hide an entry.
 * stdbuf -oL makes them write whole lines, which cannot be split.
 */
/* the stdbuf program: where the build says (PS2GM_STDBUF), the usual places, and the user's PATH only when not running as root */
static char *find_stdbuf(void)
{
    static const char *fixed[] = {"/usr/bin/stdbuf", "/bin/stdbuf", NULL};
    int i;

#ifdef PS2GM_STDBUF
    if (g_file_test(PS2GM_STDBUF, G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup(PS2GM_STDBUF);
#endif
    for (i = 0; fixed[i] != NULL; i++)
        if (g_file_test(fixed[i], G_FILE_TEST_IS_EXECUTABLE))
            return g_strdup(fixed[i]);
    return geteuid() == 0 ? NULL : g_find_program_in_path("stdbuf");
}

static gboolean run_capture(const char *const *argv, const char *stdin_text, gchar **out, int *status,
                            GError **error)
{
    GSubprocess *p;
    GBytes *in = NULL, *outb = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE;
    gchar *stdbuf = find_stdbuf();
    GPtrArray *full = g_ptr_array_new();
    int i;

    if (stdbuf != NULL) {
        g_ptr_array_add(full, stdbuf);
        g_ptr_array_add(full, (gpointer) "-oL");
    }
    for (i = 0; argv[i] != NULL; i++)
        g_ptr_array_add(full, (gpointer)argv[i]);
    g_ptr_array_add(full, NULL);
    if (stdin_text != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    p = g_subprocess_newv((const gchar *const *)full->pdata, flags, error);
    g_ptr_array_free(full, TRUE);
    g_free(stdbuf);
    if (p == NULL)
        return FALSE;
    if (stdin_text != NULL)
        in = g_bytes_new(stdin_text, strlen(stdin_text));
    if (!g_subprocess_communicate(p, in, NULL, &outb, NULL, error)) {
        g_object_unref(p);
        if (in)
            g_bytes_unref(in);
        return FALSE;
    }
    if (out) {
        gsize n;
        const guint8 *d = g_bytes_get_data(outb, &n);

        *out = to_utf8(d, n);
    }
    if (status)
        *status = g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : -1;
    if (in)
        g_bytes_unref(in);
    g_bytes_unref(outb);
    g_object_unref(p);
    return TRUE;
}

static gboolean match_line(GRegex *re, const char *line, GMatchInfo **mi)
{
    return g_regex_match(re, line, 0, mi);
}

/* ---- hdl_dump ---------------------------------------------------------------------------------- */

void hdl_game_free(gpointer p)
{
    HdlGame *g = p;

    g_free(g->media);
    g_free(g->flags);
    g_free(g->id);
    g_free(g->title);
    g_free(g);
}

void hdl_iso_info_clear(HdlIsoInfo *i)
{
    g_clear_pointer(&i->media, g_free);
    g_clear_pointer(&i->label, g_free);
    g_clear_pointer(&i->id, g_free);
}

GPtrArray *hdl_list_games(const Ps2gmTools *t, const char *device, GError **error)
{
    const char *argv[] = {t->hdl_dump, "hdl_toc", device, NULL};
    gchar *out = NULL, **lines;
    int status = 0, i;
    GRegex *re;
    GPtrArray *games;

    if (!ps2gm_arg_ok(device))
        return g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad device name")), NULL;
    if (!run_capture(argv, NULL, &out, &status, error))
        return NULL;
    if (status != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("hdl_dump failed (%d): %s"), status, g_strstrip(out));
        g_free(out);
        return NULL;
    }
    /* CD|DVD  <n>KB  <flags...>  SLES_539.08  <title> */
    re = g_regex_new("^(CD|DVD)\\s+(\\d+)KB\\s+(.*?)\\s*([A-Z]{4}_\\d{3}\\.\\d{2})\\s+(.*?)\\s*$", 0, 0, NULL);
    games = g_ptr_array_new_with_free_func(hdl_game_free);
    lines = g_strsplit(out, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        GMatchInfo *mi = NULL;

        if (match_line(re, lines[i], &mi)) {
            HdlGame *g = g_new0(HdlGame, 1);
            gchar *kb = g_match_info_fetch(mi, 2);

            g->media = g_match_info_fetch(mi, 1);
            g->size_kb = g_ascii_strtoull(kb, NULL, 10);
            g->flags = g_match_info_fetch(mi, 3);
            g->id = g_match_info_fetch(mi, 4);
            g->title = g_match_info_fetch(mi, 5);
            g_free(kb);
            g_ptr_array_add(games, g);
        }
        g_match_info_free(mi);
    }
    g_strfreev(lines);
    g_regex_unref(re);
    g_free(out);
    return games;
}

gboolean hdl_disk_space(const Ps2gmTools *t, const char *device, guint64 *total_mb, guint64 *used_mb,
                        guint64 *free_mb, GError **error)
{
    const char *argv[] = {t->hdl_dump, "toc", device, NULL};
    gchar *out = NULL;
    int status = 0;
    GRegex *re;
    GMatchInfo *mi = NULL;
    gboolean ok = FALSE;

    if (!ps2gm_arg_ok(device) || !run_capture(argv, NULL, &out, &status, error))
        return FALSE;
    re = g_regex_new("[Tt]otal(?: slice size)?:?\\s*(\\d+)MB,\\s*used:?\\s*(\\d+)MB,\\s*available:?\\s*(\\d+)MB",
                     0, 0, NULL);
    if (status == 0 && g_regex_match(re, out, 0, &mi)) {
        gchar *a = g_match_info_fetch(mi, 1), *b = g_match_info_fetch(mi, 2), *c = g_match_info_fetch(mi, 3);

        *total_mb = g_ascii_strtoull(a, NULL, 10);
        *used_mb = g_ascii_strtoull(b, NULL, 10);
        *free_mb = g_ascii_strtoull(c, NULL, 10);
        g_free(a);
        g_free(b);
        g_free(c);
        ok = TRUE;
    } else
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("Cannot read the disk size from hdl_dump"));
    g_match_info_free(mi);
    g_regex_unref(re);
    g_free(out);
    return ok;
}

gboolean hdl_parse_cdvd_info(const char *text, HdlIsoInfo *info)
{
    GRegex *re = g_regex_new("^(dual-layer\\s+)?(\\S+)\\s+(\\d+)KB\\s+\"(.*)\"\\s+\"(.*)\"", G_REGEX_MULTILINE, 0, NULL);
    GMatchInfo *mi = NULL;
    gboolean ok = FALSE;

    memset(info, 0, sizeof(*info));
    if (g_regex_match(re, text, 0, &mi)) {
        gchar *dual = g_match_info_fetch(mi, 1), *kb = g_match_info_fetch(mi, 3);

        info->dual_layer = dual != NULL && *dual != '\0';
        info->media = g_match_info_fetch(mi, 2);
        info->size_kb = g_ascii_strtoull(kb, NULL, 10);
        info->label = g_match_info_fetch(mi, 4);
        info->id = g_match_info_fetch(mi, 5);
        g_free(dual);
        g_free(kb);
        ok = TRUE;
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    return ok;
}

gboolean hdl_iso_info(const Ps2gmTools *t, const char *iso, HdlIsoInfo *info, GError **error)
{
    const char *argv[] = {t->hdl_dump, "cdvd_info2", iso, NULL};
    gchar *out = NULL;
    int status = 0;
    gboolean ok = FALSE;

    memset(info, 0, sizeof(*info));
    if (!run_capture(argv, NULL, &out, &status, error))
        return FALSE;
    if (status == 0 && hdl_parse_cdvd_info(out, info))
        ok = TRUE;
    else
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("Not a PlayStation 2 disc image: %s"), g_strstrip(out));
    g_free(out);
    return ok;
}

gboolean hdl_install(const Ps2gmTools *t, const char *device, const char *media, const char *title,
                     const char *iso, const char *id, Ps2gmProgress progress, gpointer user_data,
                     GError **error)
{
    const char *cmd = !g_strcmp0(media, "CD") ? "inject_cd" : "inject_dvd";
    /*
     * hdl_dump installs "<name>.zso" instead of "<name>.iso" when such a file sits next to it, and a ZSO that is not
     * a whole number of sectors long loses its end that way. A plain image is therefore given through a link in a
     * private folder, where no such neighbour can exist. A .zso is given as it is.
     */
    gchar *link_dir = NULL, *link_path = NULL, *real = NULL;
    const char *use = iso;
    const char *argv[7];
    GSubprocess *p;
    GInputStream *in;
    guint8 buf[512];
    gssize n;
    int last = -1;
    GString *log = g_string_new(NULL);
    gboolean ok = FALSE;

    if (!ps2gm_arg_ok(device) || !ps2gm_arg_ok(title) || !ps2gm_arg_ok(id) || !ps2gm_arg_ok(iso)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad characters in the title or path"));
        goto out;
    }
    if (!ps2gm_is_zso_name(iso)) {
        link_dir = g_dir_make_tmp("ps2gm-link-XXXXXX", NULL);
        real = g_canonicalize_filename(iso, NULL);
        if (link_dir != NULL) {
            link_path = g_build_filename(link_dir, "game.iso", NULL);
            if (symlink(real, link_path) == 0)
                use = link_path;
        }
    }
    argv[0] = t->hdl_dump;
    argv[1] = cmd;
    argv[2] = device;
    argv[3] = title;
    argv[4] = use;
    argv[5] = id;
    argv[6] = NULL;
    p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, error);
    if (p == NULL)
        goto out;
    in = g_subprocess_get_stdout_pipe(p);
    while ((n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL)) > 0) {
        gssize i;

        g_string_append_len(log, (const char *)buf, n);
        if (log->len > 4096)
            g_string_erase(log, 0, log->len - 2048);
        /* hdl_dump prints "  0% 13% 27% ..." without line breaks: the last number is the progress */
        for (i = 0; i < n; i++) {
            if (buf[i] == '%') {
                gssize j = i;
                int v = 0, mul = 1;

                while (j > 0 && g_ascii_isdigit(buf[j - 1])) {
                    v += (buf[j - 1] - '0') * mul;
                    mul *= 10;
                    j--;
                }
                if (mul > 1 && v != last && v <= 100) {
                    last = v;
                    if (progress && !progress(v, 100, _("Installing to the PS2 disk"), user_data)) {
                        g_subprocess_force_exit(p);
                        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
                        g_object_unref(p);
                        goto out;
                    }
                }
            }
        }
    }
    g_subprocess_wait(p, NULL, NULL);
    if (g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0)
        ok = TRUE;
    else
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("hdl_dump failed: %s"), g_strstrip(log->str));
    g_object_unref(p);
out:
    if (link_path != NULL)
        g_unlink(link_path);
    if (link_dir != NULL)
        g_rmdir(link_dir);
    g_free(link_path);
    g_free(link_dir);
    g_free(real);
    g_string_free(log, TRUE);
    return ok;
}

/* names of the HDL game partitions ("0x1337" entries of hdl_dump toc) */
static GPtrArray *hdl_partition_names(const Ps2gmTools *t, const char *device, GError **error)
{
    const char *argv[] = {t->hdl_dump, "toc", device, NULL};
    gchar *out = NULL, **lines;
    int status = 0, i;
    GRegex *re = g_regex_new("^0x1337\\s+[0-9A-Fa-f]+[.:*]*\\s+\\d+\\s+\\d+MB\\s+(\\S.*?)\\s*$", 0, 0, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

    if (!run_capture(argv, NULL, &out, &status, error) || status != 0) {
        if (status != 0)
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("hdl_dump toc failed"));
        g_ptr_array_free(names, TRUE);
        g_regex_unref(re);
        g_free(out);
        return NULL;
    }
    lines = g_strsplit(out, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        GMatchInfo *mi = NULL;

        if (g_regex_match(re, lines[i], 0, &mi))
            g_ptr_array_add(names, g_match_info_fetch(mi, 1));
        g_match_info_free(mi);
    }
    g_strfreev(lines);
    g_regex_unref(re);
    g_free(out);
    return names;
}

gboolean hdl_partition_size(const Ps2gmTools *t, const char *device, const char *name, guint *parts,
                            guint64 *size_mb, GError **error)
{
    const char *argv[] = {t->hdl_dump, "toc", device, NULL};
    gchar *out = NULL, **lines;
    int status = 0, i;
    GRegex *re = g_regex_new("^0x[0-9A-Fa-f]{4}\\s+[0-9A-Fa-f]+[.:*]*\\s+(\\d+)\\s+(\\d+)MB\\s+(\\S.*?)\\s*$", 0, 0, NULL);
    gboolean ok = FALSE;

    if (!ps2gm_arg_ok(device) || !run_capture(argv, NULL, &out, &status, error) || status != 0) {
        g_free(out);
        g_regex_unref(re);
        return FALSE;
    }
    lines = g_strsplit(out, "\n", -1);
    for (i = 0; lines[i] != NULL && !ok; i++) {
        GMatchInfo *mi = NULL;

        if (g_regex_match(re, lines[i], 0, &mi)) {
            gchar *n = g_match_info_fetch(mi, 3);

            if (!g_strcmp0(n, name)) {
                gchar *p = g_match_info_fetch(mi, 1), *s = g_match_info_fetch(mi, 2);

                *parts = (guint)g_ascii_strtoull(p, NULL, 10);
                *size_mb = g_ascii_strtoull(s, NULL, 10);
                g_free(p);
                g_free(s);
                ok = TRUE;
            }
            g_free(n);
        }
        g_match_info_free(mi);
    }
    if (!ok)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("Partition %s not found"), name);
    g_strfreev(lines);
    g_regex_unref(re);
    g_free(out);
    return ok;
}

gboolean hdl_remove_game(const Ps2gmTools *t, const char *device, const char *id, const char *title,
                         GError **error)
{
    GPtrArray *names = hdl_partition_names(t, device, error);
    GRegex *re = g_regex_new("^(\\S+): \\[(.*)\\], (?:CD|DVD)", G_REGEX_MULTILINE, 0, NULL);
    gchar *found = NULL;
    guint i, matches = 0;
    gboolean ok = FALSE;

    if (names == NULL) {
        g_regex_unref(re);
        return FALSE;
    }
    if (!ps2gm_arg_ok(device) || !ps2gm_arg_ok(id) || !ps2gm_arg_ok(title)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad characters in the game name"));
        goto out;
    }
    for (i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        const char *argv[] = {t->hdl_dump, "info", device, name, NULL};
        gchar *out = NULL;
        int status = 0;
        GMatchInfo *mi = NULL;

        if (!ps2gm_arg_ok(name) || !run_capture(argv, NULL, &out, &status, NULL) || status != 0) {
            g_free(out);
            continue;
        }
        if (g_regex_match(re, out, 0, &mi)) {
            gchar *gid = g_match_info_fetch(mi, 1), *gtitle = g_match_info_fetch(mi, 2);

            if (!g_strcmp0(gid, id) && !g_strcmp0(gtitle, title)) {
                matches++;
                g_free(found);
                found = g_strdup(name);
            }
            g_free(gid);
            g_free(gtitle);
        }
        g_match_info_free(mi);
        g_free(out);
    }
    if (matches != 1) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED,
                    matches == 0 ? _("No installed game matches %s \"%s\"") : _("Several installed games match %s \"%s\"; nothing was deleted"),
                    id, title);
        goto out;
    }
    {
        const char *argv[] = {t->pfsshell, NULL};
        gchar *script = g_strdup_printf("device \"%s\"\nrmpart \"%s\"\nexit\n", device, found), *text = NULL;
        int status = 0;
        GPtrArray *after;

        if (!run_capture(argv, script, &text, &status, error)) {
            g_free(script);
            goto out;
        }
        g_free(script);
        g_free(text);
        /* verify: the partition must be gone */
        after = hdl_partition_names(t, device, error);
        if (after == NULL)
            goto out;
        ok = TRUE;
        for (i = 0; i < after->len; i++)
            if (!g_strcmp0(g_ptr_array_index(after, i), found))
                ok = FALSE;
        g_ptr_array_free(after, TRUE);
        if (!ok)
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("The partition %s could not be removed"), found);
    }
out:
    g_free(found);
    g_regex_unref(re);
    g_ptr_array_free(names, TRUE);
    return ok;
}

/* ---- pfsshell ---------------------------------------------------------------------------------- */

void pfs_entry_free(gpointer p)
{
    PfsEntry *e = p;

    g_free(e->name);
    g_free(e);
}

/* text after the last prompt ("# ") on a line: pfsshell glues prompts in front of its output */
static const char *after_prompt(const char *line)
{
    const char *p = strstr(line, "# ");
    const char *last = NULL;

    while (p != NULL) {
        last = p + 2;
        p = strstr(p + 2, "# ");
    }
    if (last != NULL)
        return last;
    return g_str_has_prefix(line, "> ") ? line + 2 : line;
}

/* "cd" into each component of dir (creating them when create is set), then print the working directory */
static void script_enter(GString *s, const char *dir, gboolean create)
{
    gchar **parts;
    int i;

    if (dir == NULL || *dir == '\0')
        return;
    parts = g_strsplit(dir, "/", -1);
    for (i = 0; parts[i] != NULL; i++) {
        if (create)
            g_string_append_printf(s, "mkdir \"%s\"\n", parts[i]);
        g_string_append_printf(s, "cd \"%s\"\n", parts[i]);
    }
    g_strfreev(parts);
}

static gboolean run_pfs(const Ps2gmTools *t, const char *device, const char *partition, const char *body,
                        gchar **out, GError **error)
{
    const char *argv[] = {t->pfsshell, NULL};
    GString *script = g_string_new(NULL);
    int status = 0;
    gboolean ok;

    if (!ps2gm_arg_ok(device) || !ps2gm_arg_ok(partition)) {
        g_string_free(script, TRUE);
        return g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad device or partition name")), FALSE;
    }
    g_string_append_printf(script, "device \"%s\"\nmount \"%s\"\n%sumount\nexit\n", device, partition, body);
    ok = run_capture(argv, script->str, out, &status, error);
    g_string_free(script, TRUE);
    return ok;
}

/* the line printed by "pwd" after the requested cd's must show the requested directory */
static gboolean pwd_is(const char *out, const char *dir)
{
    gchar *want = g_strconcat("/", dir ? dir : "", NULL), **lines = g_strsplit(out, "\n", -1);
    gboolean found = FALSE;
    int i;

    for (i = 0; lines[i] != NULL && !found; i++)
        found = g_strcmp0(g_strstrip((gchar *)after_prompt(lines[i])), want) == 0;
    g_strfreev(lines);
    g_free(want);
    return found;
}

static GPtrArray *parse_ls(const char *out)
{
    GRegex *re = g_regex_new("([-d])[rwx-]{9}\\s+(\\d+)\\s+\\d{4}-\\d{2}-\\d{2}\\s+\\d{2}:\\d{2}\\s+(.+?)\\s*$", 0, 0, NULL);
    GPtrArray *list = g_ptr_array_new_with_free_func(pfs_entry_free);
    gchar **lines = g_strsplit(out, "\n", -1);
    int i;

    for (i = 0; lines[i] != NULL; i++) {
        GMatchInfo *mi = NULL;

        if (g_regex_match(re, lines[i], 0, &mi)) {
            PfsEntry *e = g_new0(PfsEntry, 1);
            gchar *sz = g_match_info_fetch(mi, 2);
            gchar *name = g_match_info_fetch(mi, 3);
            gsize len = strlen(name);

            {
                gchar *kind = g_match_info_fetch(mi, 1);

                e->is_dir = kind[0] == 'd';
                g_free(kind);
            }
            if (e->is_dir && len > 0 && name[len - 1] == '/')
                name[len - 1] = '\0';
            e->size = g_ascii_strtoull(sz, NULL, 10);
            e->name = name;
            g_free(sz);
            if (!strcmp(e->name, ".") || !strcmp(e->name, "..") || !strcmp(e->name, "./") || !strcmp(e->name, "../"))
                pfs_entry_free(e);
            else
                g_ptr_array_add(list, e);
        }
        g_match_info_free(mi);
    }
    g_strfreev(lines);
    g_regex_unref(re);
    return list;
}

GPtrArray *pfs_list(const Ps2gmTools *t, const char *device, const char *partition, const char *dir,
                    GError **error)
{
    GString *body = g_string_new(NULL);
    gchar *out = NULL;
    GPtrArray *list = NULL;

    if (!ps2gm_remote_path_ok(dir)) {
        g_string_free(body, TRUE);
        return g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad folder name")), NULL;
    }
    script_enter(body, dir, FALSE);
    g_string_append(body, "pwd\nls -l\n");
    if (run_pfs(t, device, partition, body->str, &out, error)) {
        if (pwd_is(out, dir))
            list = parse_ls(out);
        else
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, dir ? dir : "");
    }
    g_string_free(body, TRUE);
    g_free(out);
    return list;
}

gboolean pfs_put(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                 char *const *local_files, GError **error)
{
    GString *body = g_string_new(NULL);
    gchar *out = NULL;
    gboolean ok = FALSE;
    GPtrArray *expect = g_ptr_array_new_with_free_func(g_free), *list = NULL;
    int i;

    if (!ps2gm_remote_path_ok(remote_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad folder name"));
        goto out;
    }
    script_enter(body, remote_dir, TRUE);
    g_string_append(body, "pwd\n");
    for (i = 0; local_files[i] != NULL; i++) {
        gchar *dir = g_path_get_dirname(local_files[i]), *base = g_path_get_basename(local_files[i]);
        GStatBuf st;

        if (!ps2gm_arg_ok(dir) || !ps2gm_arg_ok(base) || g_stat(local_files[i], &st) != 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Cannot use file %s"), local_files[i]);
            g_free(dir);
            g_free(base);
            goto out;
        }
        /* replace a file of the same name: remove it first (the error for a missing file is harmless) */
        g_string_append_printf(body, "lcd \"%s\"\nrm \"%s\"\nput \"%s\"\n", dir, base, base);
        g_ptr_array_add(expect, g_strdup_printf("%s\t%" G_GUINT64_FORMAT, base, (guint64)st.st_size));
        g_free(dir);
        g_free(base);
    }
    g_string_append(body, "ls -l\n");

    if (!run_pfs(t, device, partition, body->str, &out, error))
        goto out;
    if (!pwd_is(out, remote_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create or enter %s/%s"), partition, remote_dir ? remote_dir : "");
        goto out;
    }
    /* verify: every file must now be listed with its size */
    list = parse_ls(out);
    for (i = 0; (guint)i < expect->len; i++) {
        gchar **kv = g_strsplit(g_ptr_array_index(expect, i), "\t", 2);
        gboolean found = FALSE;
        guint j;

        for (j = 0; j < list->len; j++) {
            PfsEntry *e = g_ptr_array_index(list, j);

            if (!e->is_dir && !strcmp(e->name, kv[0]) && e->size == g_ascii_strtoull(kv[1], NULL, 10))
                found = TRUE;
        }
        if (!found) {
            /* a listing can still be garbled: look again, in a fresh session, before calling it a failure */
            GPtrArray *again = pfs_list(t, device, partition, remote_dir, NULL);

            for (j = 0; again != NULL && j < again->len; j++) {
                PfsEntry *e = g_ptr_array_index(again, j);

                if (!e->is_dir && !strcmp(e->name, kv[0]) && e->size == g_ascii_strtoull(kv[1], NULL, 10))
                    found = TRUE;
            }
            if (again)
                g_ptr_array_free(again, TRUE);
        }
        if (!found) {
            gchar *base_name = g_strdup(kv[0]);

            /* a copy that stopped half way (disk full) must not stay on the disk: remove what was written */
            for (j = 0; j < (guint)expect->len; j++) {
                gchar **other = g_strsplit(g_ptr_array_index(expect, j), "\t", 2);

                pfs_remove(t, device, partition, remote_dir, other[0], NULL); /* best effort */
                g_strfreev(other);
            }
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO,
                        _("%s was not written completely to %s (disk full?). The files of this copy were removed again."),
                        base_name, partition);
            g_free(base_name);
            g_strfreev(kv);
            goto out;
        }
        g_strfreev(kv);
    }
    ok = TRUE;
out:
    if (list)
        g_ptr_array_free(list, TRUE);
    g_ptr_array_free(expect, TRUE);
    g_string_free(body, TRUE);
    g_free(out);
    return ok;
}

gboolean pfs_get(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                 char *const *names, const char *local_dir, GError **error)
{
    GString *body = g_string_new(NULL);
    gchar *out = NULL;
    gboolean ok = FALSE;
    int i;

    if (!ps2gm_remote_path_ok(remote_dir) || !ps2gm_arg_ok(local_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad folder name"));
        goto out;
    }
    for (i = 0; names[i] != NULL; i++)
        if (!ps2gm_arg_ok(names[i])) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad file name"));
            goto out;
        }
    script_enter(body, remote_dir, FALSE);
    g_string_append_printf(body, "pwd\nlcd \"%s\"\n", local_dir);
    for (i = 0; names[i] != NULL; i++)
        g_string_append_printf(body, "get \"%s\"\n", names[i]);
    if (!run_pfs(t, device, partition, body->str, &out, error))
        goto out;
    if (!pwd_is(out, remote_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, remote_dir ? remote_dir : "");
        goto out;
    }
    ok = TRUE; /* files that could not be read are simply missing in local_dir; the caller checks */
out:
    g_string_free(body, TRUE);
    g_free(out);
    return ok;
}

static double to_mib(double v, const char *unit)
{
    if (!strcmp(unit, "GiB"))
        return v * 1024;
    if (!strcmp(unit, "KiB"))
        return v / 1024;
    if (!strcmp(unit, "TiB"))
        return v * 1024 * 1024;
    return v;
}

gboolean pfs_space(const Ps2gmTools *t, const char *device, const char *partition, guint64 *size_mb,
                   guint64 *free_mb, GError **error)
{
    gchar *out = NULL, **lines;
    GRegex *re = g_regex_new("(\\S+)\\s+([\\d.]+)(KiB|MiB|GiB|TiB)\\s+([\\d.]+)(KiB|MiB|GiB|TiB)\\s+([\\d.]+)(KiB|MiB|GiB|TiB)\\s+\\d+%", 0, 0, NULL);
    gboolean ok = FALSE;
    int i;

    if (!run_pfs(t, device, partition, "df\n", &out, error)) {
        g_regex_unref(re);
        return FALSE;
    }
    lines = g_strsplit(out, "\n", -1);
    for (i = 0; lines[i] != NULL && !ok; i++) {
        GMatchInfo *mi = NULL;

        if (g_regex_match(re, lines[i], 0, &mi)) {
            gchar *name = g_match_info_fetch(mi, 1);

            /* the device line starts with hdd0:, the partition line with the partition name */
            if (!g_strcmp0(name, partition)) {
                gchar *sz = g_match_info_fetch(mi, 2), *su = g_match_info_fetch(mi, 3);
                gchar *fr = g_match_info_fetch(mi, 6), *fu = g_match_info_fetch(mi, 7);

                *size_mb = (guint64)to_mib(g_ascii_strtod(sz, NULL), su);
                *free_mb = (guint64)to_mib(g_ascii_strtod(fr, NULL), fu);
                g_free(sz);
                g_free(su);
                g_free(fr);
                g_free(fu);
                ok = TRUE;
            }
            g_free(name);
        }
        g_match_info_free(mi);
    }
    if (!ok)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("Cannot read the free space of %s"), partition);
    g_strfreev(lines);
    g_regex_unref(re);
    g_free(out);
    return ok;
}

gboolean pfs_rename(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                    const char *from, const char *to, GError **error)
{
    GString *body = g_string_new(NULL);
    gchar *out = NULL;
    gboolean ok = FALSE, has_from = FALSE, has_to = FALSE;
    GPtrArray *list, *before = NULL;
    guint j;

    if (!ps2gm_remote_path_ok(remote_dir) || !ps2gm_arg_ok(from) || !ps2gm_arg_ok(to) || strchr(from, '/') || strchr(to, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad file name"));
        goto out;
    }
    before = pfs_list(t, device, partition, remote_dir, error);
    if (before == NULL)
        goto out;
    for (j = 0; j < before->len; j++)
        if (!strcmp(((PfsEntry *)g_ptr_array_index(before, j))->name, to)) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("A file named %s is already there"), to);
            goto out;
        }
    script_enter(body, remote_dir, FALSE);
    g_string_append_printf(body, "pwd\nrename \"%s\" \"%s\"\nls -l\n", from, to);
    if (!run_pfs(t, device, partition, body->str, &out, error))
        goto out;
    if (!pwd_is(out, remote_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, remote_dir ? remote_dir : "");
        goto out;
    }
    list = parse_ls(out);
    for (j = 0; j < list->len; j++) {
        const char *n = ((PfsEntry *)g_ptr_array_index(list, j))->name;

        if (!strcmp(n, from))
            has_from = TRUE;
        if (!strcmp(n, to))
            has_to = TRUE;
    }
    g_ptr_array_free(list, TRUE);
    if (has_to && !has_from)
        ok = TRUE;
    else
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not rename %s"), from);
out:
    if (before)
        g_ptr_array_free(before, TRUE);
    g_string_free(body, TRUE);
    g_free(out);
    return ok;
}

gboolean ps2gm_is_zso_name(const char *name)
{
    gsize n = name ? strlen(name) : 0;

    return n > 4 && !g_ascii_strcasecmp(name + n - 4, ".zso");
}

gboolean ps2gm_is_elf_name(const char *name)
{
    gsize n = name ? strlen(name) : 0;

    return n > 4 && !g_ascii_strcasecmp(name + n - 4, ".elf");
}

gboolean ps2gm_elf_check(const char *path, GError **error)
{
    guint8 h[20];
    FILE *f = g_fopen(path, "rb");
    gboolean ok;
    GStatBuf st;

    if (f == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), path, g_strerror(errno));
        return FALSE;
    }
    ok = fread(h, 1, sizeof(h), f) == sizeof(h);
    fclose(f);
    /* ELF magic, 32-bit (1), little endian (1), executable (2), MIPS (8): what the PS2's EE runs */
    ok = ok && !memcmp(h, "\x7f" "ELF", 4) && h[4] == 1 && h[5] == 1 && (h[16] | h[17] << 8) == 2 && (h[18] | h[19] << 8) == 8;
    if (!ok) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That is not a PS2 program (a PS2 .ELF file)"));
        return FALSE;
    }
    if (g_stat(path, &st) == 0 && st.st_size > 64 * 1024 * 1024) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That file is too big to be a PS2 program"));
        return FALSE;
    }
    return TRUE;
}

gboolean pfs_remove(const Ps2gmTools *t, const char *device, const char *partition,
                    const char *remote_dir, const char *name, GError **error)
{
    GString *body = g_string_new(NULL);
    gchar *out = NULL;
    gboolean ok = FALSE;
    GPtrArray *list;
    guint j;

    if (!ps2gm_remote_path_ok(remote_dir) || !ps2gm_arg_ok(name)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad file name"));
        goto out;
    }
    script_enter(body, remote_dir, FALSE);
    g_string_append_printf(body, "pwd\nrm \"%s\"\nls -l\n", name);
    if (!run_pfs(t, device, partition, body->str, &out, error))
        goto out;
    if (!pwd_is(out, remote_dir)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, remote_dir ? remote_dir : "");
        goto out;
    }
    list = parse_ls(out);
    ok = TRUE;
    for (j = 0; j < list->len; j++)
        if (!strcmp(((PfsEntry *)g_ptr_array_index(list, j))->name, name))
            ok = FALSE;
    g_ptr_array_free(list, TRUE);
    if (!ok)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not remove %s"), name);
out:
    g_string_free(body, TRUE);
    g_free(out);
    return ok;
}
