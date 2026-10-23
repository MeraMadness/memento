#include <signal.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "backend.h"

struct _Backend {
    GSubprocess *proc;
    GOutputStream *in;
    GDataInputStream *out;
    GMutex lock;
};

Backend *backend_start(const char *device, gboolean use_pkexec, const char *helper_path, GError **error)
{
    Backend *b;
    gchar *self = NULL, *dir = NULL, *helper;
    const char *argv[5];
    int n = 0;
    gchar *line;

    signal(SIGPIPE, SIG_IGN); /* a helper that died must not take the application with it */
    if (helper_path == NULL) {
        self = g_file_read_link("/proc/self/exe", NULL);
        dir = self ? g_path_get_dirname(self) : g_strdup(".");
        helper = g_build_filename(dir, "ps2gm-helper", NULL);
    } else
        helper = g_strdup(helper_path);
    if (use_pkexec)
        argv[n++] = "pkexec";
    argv[n++] = helper;
    argv[n++] = device;
    argv[n] = NULL;

    b = g_new0(Backend, 1);
    g_mutex_init(&b->lock);
    b->proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    g_free(self);
    g_free(dir);
    g_free(helper);
    if (b->proc == NULL) {
        g_free(b);
        return NULL;
    }
    b->in = g_subprocess_get_stdin_pipe(b->proc);
    b->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(b->proc));
    line = g_data_input_stream_read_line(b->out, NULL, NULL, NULL);
    if (line == NULL || strcmp(line, "READY") != 0) {
        if (line != NULL && g_str_has_prefix(line, "ERR\t"))
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("%s"), line + 4);
        else {
            /* the helper never said READY: show what pkexec/the helper wrote, and the way around a missing agent */
            gchar buf[2048] = {0}, *why;
            gssize n;

            g_subprocess_force_exit(b->proc);
            n = g_input_stream_read(g_subprocess_get_stderr_pipe(b->proc), buf, sizeof(buf) - 1, NULL, NULL);
            why = g_strstrip(g_strndup(buf, n > 0 ? n : 0));
            if (use_pkexec)
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL,
                            _("The disk helper did not start%s%s.\n\nIf no password window appeared, this desktop session has no "
                            "polkit authentication agent. Alternative: give your user access to the disk once with\n"
                            "    sudo setfacl -m u:%s:rw %s\nand press Connect again (no password is needed after that)."),
                            *why ? ": " : "", why, g_get_user_name(), device);
            else
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("The disk helper did not start%s%s"), *why ? ": " : "", why);
            g_free(why);
        }
        g_free(line);
        backend_stop(b);
        return NULL;
    }
    g_free(line);
    return b;
}

void backend_stop(Backend *b)
{
    if (b == NULL)
        return;
    if (b->proc != NULL) {
        const char *quit = "QUIT\n";

        /* if the helper is still running this ends it; if it already exited the write just fails */
        g_output_stream_write_all(b->in, quit, strlen(quit), NULL, NULL, NULL);
        g_output_stream_flush(b->in, NULL, NULL);
        g_subprocess_wait(b->proc, NULL, NULL);
        g_object_unref(b->proc);
    }
    g_clear_object(&b->out);
    g_mutex_clear(&b->lock);
    g_free(b);
}

gboolean backend_run(Backend *b, char *const *fields, BackendLineFn cb, gpointer user_data, GError **error)
{
    gchar *cmd = g_strjoinv("\t", (gchar **)fields), *line;
    gboolean ok = FALSE;

    g_mutex_lock(&b->lock);
    {
        gchar *full = g_strconcat(cmd, "\n", NULL);
        gboolean wrote = g_output_stream_write_all(b->in, full, strlen(full), NULL, NULL, error) &&
                         g_output_stream_flush(b->in, NULL, error);

        g_free(full);
        if (!wrote)
            goto out;
    }
    for (;;) {
        line = g_data_input_stream_read_line(b->out, NULL, NULL, error);
        if (line == NULL) {
            if (error && *error == NULL)
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("The disk helper stopped unexpectedly"));
            goto out;
        }
        if (!strcmp(line, "END")) {
            g_free(line);
            ok = TRUE;
            goto out;
        }
        if (g_str_has_prefix(line, "ERR\t")) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("%s"), line + 4);
            g_free(line);
            goto out;
        }
        if (cb)
            cb(line, user_data);
        g_free(line);
    }
out:
    g_mutex_unlock(&b->lock);
    g_free(cmd);
    return ok;
}

/* ---- library ----------------------------------------------------------------------------------- */

static void lib_game_free(gpointer p)
{
    LibGame *g = p;

    g_free(g->media);
    g_free(g->id);
    g_free(g->title);
    g_free(g);
}

static void lib_file_free(gpointer p)
{
    LibFile *f = p;

    g_free(f->name);
    g_free(f);
}

void library_free(Library *lib)
{
    if (lib == NULL)
        return;
    g_ptr_array_free(lib->games, TRUE);
    g_ptr_array_free(lib->vcds, TRUE);
    g_ptr_array_free(lib->apps, TRUE);
    g_ptr_array_free(lib->vmcs, TRUE);
    g_hash_table_destroy(lib->art);
    g_hash_table_destroy(lib->cfgs);
    g_hash_table_destroy(lib->cheats);
    g_hash_table_destroy(lib->popsdirs);
    g_free(lib);
}

static void library_line(const char *line, gpointer ud)
{
    Library *lib = ud;
    gchar **f = g_strsplit(line, "\t", -1);
    guint n = g_strv_length(f);

    if (!strcmp(f[0], "SPACE") && n == 4) {
        lib->total_mb = g_ascii_strtoull(f[1], NULL, 10);
        lib->used_mb = g_ascii_strtoull(f[2], NULL, 10);
        lib->free_mb = g_ascii_strtoull(f[3], NULL, 10);
    } else if (!strcmp(f[0], "OPLSPACE") && n == 3) {
        lib->opl_size_mb = g_ascii_strtoull(f[1], NULL, 10);
        lib->opl_free_mb = g_ascii_strtoull(f[2], NULL, 10);
    } else if (!strcmp(f[0], "POPSSPACE") && n == 3) {
        lib->pops_size_mb = g_ascii_strtoull(f[1], NULL, 10);
        lib->pops_free_mb = g_ascii_strtoull(f[2], NULL, 10);
    } else if (!strcmp(f[0], "GAME") && n >= 5) {
        LibGame *g = g_new0(LibGame, 1);

        g->media = g_strdup(f[1]);
        g->size_kb = g_ascii_strtoull(f[2], NULL, 10);
        g->id = g_strdup(f[3]);
        g->title = g_strdup(f[4]);
        g_ptr_array_add(lib->games, g);
    } else if (!strcmp(f[0], "VCD") && n >= 3) {
        LibFile *v = g_new0(LibFile, 1);

        v->name = g_strdup(f[1]);
        v->size = g_ascii_strtoull(f[2], NULL, 10);
        g_ptr_array_add(lib->vcds, v);
    } else if (!strcmp(f[0], "APP") && n >= 3) {
        LibFile *a = g_new0(LibFile, 1);

        a->name = g_strdup(f[1]);
        a->size = g_ascii_strtoull(f[2], NULL, 10);
        g_ptr_array_add(lib->apps, a);
    } else if (!strcmp(f[0], "VMC") && n >= 3) {
        LibFile *v = g_new0(LibFile, 1);

        v->name = g_strdup(f[1]);
        v->size = g_ascii_strtoull(f[2], NULL, 10);
        g_ptr_array_add(lib->vmcs, v);
    } else if (!strcmp(f[0], "ART") && n >= 2) {
        guint64 *size = g_new(guint64, 1);

        *size = n >= 3 ? g_ascii_strtoull(f[2], NULL, 10) : 0;
        g_hash_table_replace(lib->art, g_strdup(f[1]), size);
    }
    else if (!strcmp(f[0], "CFG") && n >= 2)
        g_hash_table_add(lib->cfgs, g_strdup(f[1]));
    else if (!strcmp(f[0], "CHT") && n >= 2)
        g_hash_table_add(lib->cheats, g_strdup(f[1]));
    else if (!strcmp(f[0], "POPSDIR") && n >= 2)
        g_hash_table_add(lib->popsdirs, g_strdup(f[1]));
    g_strfreev(f);
}

Library *backend_library(Backend *b, GError **error)
{
    Library *lib = g_new0(Library, 1);
    char *cmd[] = {"LIBRARY", NULL};

    lib->games = g_ptr_array_new_with_free_func(lib_game_free);
    lib->vcds = g_ptr_array_new_with_free_func(lib_file_free);
    lib->apps = g_ptr_array_new_with_free_func(lib_file_free);
    lib->vmcs = g_ptr_array_new_with_free_func(lib_file_free);
    lib->art = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    lib->cfgs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    lib->cheats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    lib->popsdirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!backend_run(b, cmd, library_line, lib, error)) {
        library_free(lib);
        return NULL;
    }
    return lib;
}

gboolean library_has_art(const Library *lib, const char *id)
{
    gchar *name = g_strdup_printf("%s_COV.png", id);
    gboolean has = g_hash_table_contains(lib->art, name);

    g_free(name);
    return has;
}

guint64 library_art_size(const Library *lib, const char *name)
{
    const guint64 *size = g_hash_table_lookup(lib->art, name);

    return size ? *size : 0;
}

char *backend_art_cache_dir(const char *device)
{
    gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, device ? device : "", -1), *short_sum = g_strndup(sum, 10), *dir;

    dir = g_build_filename(g_get_user_cache_dir(), "ps2gm", "art", short_sum, NULL);
    g_free(sum);
    g_free(short_sum);
    return dir;
}

typedef struct {
    const char *cache_dir;
    guint copied;
} GotCtx;

static void got_line(const char *line, gpointer ud)
{
    GotCtx *g = ud;
    gchar **f = g_strsplit(line, "\t", -1);

    if (g_strv_length(f) == 3 && !strcmp(f[0], "GOT") && strchr(f[1], '/') == NULL) {
        gchar *dst = g_build_filename(g->cache_dir, f[1], NULL), *data = NULL;
        gsize len = 0;

        if (g_file_get_contents(f[2], &data, &len, NULL) && g_file_set_contents(dst, data, len, NULL))
            g->copied++;
        g_free(data);
        g_free(dst);
    }
    g_strfreev(f);
}

typedef struct {
    char *path;
} GotPath;

static void got_path_line(const char *line, gpointer ud)
{
    GotPath *g = ud;
    gchar **f = g_strsplit(line, "\t", -1);

    if (g_strv_length(f) == 3 && !strcmp(f[0], "GOT") && g->path == NULL)
        g->path = g_strdup(f[2]);
    g_strfreev(f);
}

gboolean backend_read_text(Backend *b, const char *folder, const char *name, char **text, GError **error)
{
    char *cmd[] = {"GET", (char *)folder, (char *)name, NULL};
    GotPath got = {NULL};
    gsize len = 0;
    gboolean ok;

    *text = NULL;
    if (!backend_run(b, cmd, got_path_line, &got, error))
        return FALSE;
    if (got.path == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("%s/%s is not on the disk"), folder, name);
        return FALSE;
    }
    ok = g_file_get_contents(got.path, text, &len, error);
    g_free(got.path);
    return ok;
}

static void fix_file_line(const char *line, gpointer ud)
{
    GPtrArray *files = ud;
    gchar **f = g_strsplit(line, "\t", -1);

    if (g_strv_length(f) == 3 && !strcmp(f[0], "FIXFILE")) {
        LibFile *a = g_new0(LibFile, 1);

        a->name = g_strdup(f[1]);
        a->size = g_ascii_strtoull(f[2], NULL, 10);
        g_ptr_array_add(files, a);
    }
    g_strfreev(f);
}

gboolean backend_list_fix_files(Backend *b, const char *folder, GPtrArray **files, GError **error)
{
    char *cmd[] = {"LIST_POPS", (char *)folder, NULL};

    *files = g_ptr_array_new_with_free_func(lib_file_free);
    if (!backend_run(b, cmd, fix_file_line, *files, error)) {
        g_ptr_array_free(*files, TRUE);
        *files = NULL;
        return FALSE;
    }
    return TRUE;
}

gboolean backend_read_pops_file(Backend *b, const char *folder, const char *name, char **text, GError **error)
{
    char *cmd[] = {"GET_POPS", (char *)folder, (char *)name, NULL};
    GotPath got = {NULL};
    gboolean ok = TRUE;

    *text = NULL;
    if (!backend_run(b, cmd, got_path_line, &got, error))
        return FALSE;
    if (got.path != NULL) {
        ok = g_file_get_contents(got.path, text, NULL, error);
        g_free(got.path);
    }
    return ok;
}

gboolean backend_read_pops_text(Backend *b, const char *folder, char **text, GError **error)
{
    return backend_read_pops_file(b, folder, "CHEATS.TXT", text, error);
}

gboolean backend_get_file(Backend *b, const char *folder, const char *name, char **path, GError **error)
{
    char *cmd[] = {"GET", (char *)folder, (char *)name, NULL};
    GotPath got = {NULL};

    *path = NULL;
    if (!backend_run(b, cmd, got_path_line, &got, error))
        return FALSE;
    if (got.path == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("%s/%s is not on the disk"), folder, name);
        return FALSE;
    }
    *path = got.path;
    return TRUE;
}

guint backend_cache_art(Backend *b, const Library *lib, char *const *names, const char *cache_dir, GError **error)
{
    GPtrArray *todo = g_ptr_array_new();
    GotCtx ctx = {cache_dir, 0};
    guint i, start;

    g_mkdir_with_parents(cache_dir, 0755);
    for (i = 0; names[i] != NULL; i++) {
        gchar *p = g_build_filename(cache_dir, names[i], NULL);
        GStatBuf st;
        guint64 on_disk = library_art_size(lib, names[i]);

        if (on_disk > 0 && !(g_stat(p, &st) == 0 && (guint64)st.st_size == on_disk))
            g_ptr_array_add(todo, names[i]);
        g_free(p);
    }
    /* small batches keep each helper answer short */
    for (start = 0; start < todo->len; start += 20) {
        GPtrArray *cmd = g_ptr_array_new();
        guint end = MIN(start + 20, todo->len), k;
        gboolean ok;

        g_ptr_array_add(cmd, "GET");
        g_ptr_array_add(cmd, "ART");
        for (k = start; k < end; k++)
            g_ptr_array_add(cmd, g_ptr_array_index(todo, k));
        g_ptr_array_add(cmd, NULL);
        ok = backend_run(b, (char *const *)cmd->pdata, got_line, &ctx, error);
        g_ptr_array_free(cmd, TRUE);
        if (!ok)
            break;
    }
    g_ptr_array_free(todo, TRUE);
    return ctx.copied;
}

gboolean vcd_name_parse(const char *name, char **id, char **title)
{
    gsize len = strlen(name);
    gchar *base;
    const char *p;
    int i;

    if (len < 5 || g_ascii_strcasecmp(name + len - 4, ".VCD") != 0)
        return FALSE;
    base = g_strndup(name, len - 4);
    p = base;
    if (strlen(p) >= 11) {
        gboolean ok = TRUE;

        for (i = 0; i < 11 && ok; i++)
            ok = i < 4 ? g_ascii_isupper(p[i]) : i == 4 ? p[i] == '_' : i == 8 ? p[i] == '.' : g_ascii_isdigit(p[i]);
        if (ok && (p[11] == '.' || p[11] == '\0')) {
            if (id)
                *id = g_strndup(p, 11);
            if (title)
                *title = g_strdup(p[11] == '.' ? p + 12 : "");
            g_free(base);
            return TRUE;
        }
    }
    g_free(base);
    return FALSE;
}
