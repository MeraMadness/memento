#include <gio/gio.h>
#include <string.h>
#include <glib/gstdio.h>

#include "ops.h"
#include "tools.h"

#define OPL_MARGIN_MB 3

OpsContext *ops_new(Backend *backend, const char *work_dir)
{
    OpsContext *c = g_new0(OpsContext, 1);

    c->backend = backend;
    c->work_dir = work_dir ? g_strdup(work_dir) : g_build_filename(g_get_user_cache_dir(), "ps2gm", "work", NULL);
    return c;
}

void ops_free(OpsContext *c)
{
    if (c == NULL)
        return;
    g_free(c->work_dir);
    g_free(c);
}

char *ops_clean_title(const char *title)
{
    GString *s = g_string_new(NULL);
    const char *p;
    gboolean space = TRUE;

    for (p = title; *p;) {
        gunichar c = g_utf8_get_char(p);
        const char *next = g_utf8_next_char(p);

        if (c < 128 && c != ' ' && strchr("\"\\/:*?<>|\t\r\n", (int)c) != NULL) {
            /* characters that break file names or the disk tools: treated as a space */
            if (!space)
                g_string_append_c(s, ' ');
            space = TRUE;
        } else if (c == ' ') {
            if (!space)
                g_string_append_c(s, ' ');
            space = TRUE;
        } else {
            g_string_append_len(s, p, next - p);
            space = FALSE;
        }
        p = next;
    }
    while (s->len > 0 && s->str[s->len - 1] == ' ')
        g_string_truncate(s, s->len - 1);
    return g_string_free(s, FALSE);
}

/* ---- progress staging -------------------------------------------------------------------------- */

typedef struct {
    Ps2gmProgress cb;
    gpointer ud;
    guint64 base, span; /* thousandths */
    gboolean cancelled;
} Stage;

static gboolean stage_cb(guint64 done, guint64 total, const char *what, gpointer p)
{
    Stage *s = p;

    if (s->cb == NULL)
        return TRUE;
    if (!s->cb(s->base + (total ? s->span * done / total : 0), 1000, what, s->ud))
        s->cancelled = TRUE;
    return !s->cancelled;
}

static void report(Ps2gmProgress cb, gpointer ud, guint64 at, const char *what)
{
    if (cb)
        cb(at, 1000, what, ud);
}

static void note(GPtrArray *notes, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void note(GPtrArray *notes, const char *fmt, ...)
{
    va_list ap;

    if (notes == NULL)
        return;
    va_start(ap, fmt);
    g_ptr_array_add(notes, g_strdup_vprintf(fmt, ap));
    va_end(ap);
}

static gchar *make_work_dir(OpsContext *c, GError **error)
{
    gchar *tmpl;
    gchar *dir;

    if (g_mkdir_with_parents(c->work_dir, 0755) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create %s"), c->work_dir);
        return NULL;
    }
    tmpl = g_build_filename(c->work_dir, "job-XXXXXX", NULL);
    dir = g_mkdtemp(tmpl);
    if (dir == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create a work folder in %s"), c->work_dir);
        g_free(tmpl);
    }
    return dir;
}

static void remove_tree(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *n;

    if (d) {
        while ((n = g_dir_read_name(d)) != NULL) {
            gchar *p = g_build_filename(dir, n, NULL);

            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                remove_tree(p);
            else
                g_unlink(p);
            g_free(p);
        }
        g_dir_close(d);
    }
    g_rmdir(dir);
}

/* ---- art and config for one game --------------------------------------------------------------- */

static gboolean put_files(OpsContext *c, const char *partition, const char *dir, GPtrArray *files, GError **error)
{
    GPtrArray *cmd;
    gboolean ok;
    guint i;

    if (files->len == 0)
        return TRUE;
    cmd = g_ptr_array_new();
    g_ptr_array_add(cmd, "PUT");
    g_ptr_array_add(cmd, (gpointer)partition);
    g_ptr_array_add(cmd, (gpointer)dir);
    for (i = 0; i < files->len; i++)
        g_ptr_array_add(cmd, g_ptr_array_index(files, i));
    g_ptr_array_add(cmd, NULL);
    ok = backend_run(c->backend, (char *const *)cmd->pdata, NULL, NULL, error);
    g_ptr_array_free(cmd, TRUE);
    return ok;
}

static guint64 file_size(const char *path)
{
    GStatBuf st;

    return g_stat(path, &st) == 0 ? (guint64)st.st_size : 0;
}

/* downloads what is missing, copies it into +OPL/ART and writes +OPL/CFG/<ID>.cfg if there is none */
static gboolean put_art_and_cfg(OpsContext *c, const Library *lib, const char *work, const char *id,
                                Ps2gmPlatform platform, const char *title, gboolean replace, GPtrArray *notes,
                                Stage *st)
{
    gchar *artdir = g_build_filename(work, "art", id, NULL), *prefix = g_strdup_printf("%s_", id);
    GPtrArray *skip = g_ptr_array_new_with_free_func(g_free), *files = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter it;
    gpointer key;
    char **made;
    guint64 bytes = 0, i;
    gboolean ok = TRUE;
    GError *err = NULL;

    if (!replace) {
        g_hash_table_iter_init(&it, lib->art);
        while (g_hash_table_iter_next(&it, &key, NULL))
            if (g_str_has_prefix(key, prefix))
                g_ptr_array_add(skip, g_strdup(key));
    }
    g_ptr_array_add(skip, NULL);

    made = art_fetch(id, platform, artdir, (char *const *)skip->pdata, st ? stage_cb : NULL, st, &err);
    if (made == NULL) {
        note(notes, _("Art for %s could not be downloaded: %s"), id, err->message);
        g_clear_error(&err);
    } else {
        for (i = 0; made[i] != NULL; i++) {
            gchar *p = g_build_filename(artdir, made[i], NULL);

            bytes += file_size(p);
            g_ptr_array_add(files, p);
        }
        if (files->len == 0 && skip->len <= 1)
            note(notes, _("No art was found for %s"), id);
        else if (bytes / (1024 * 1024) + OPL_MARGIN_MB > lib->opl_free_mb) {
            note(notes, _("Art for %s was not copied: not enough free space on the +OPL partition (%lu MB free, %lu MB needed)"),
                 id, (gulong)lib->opl_free_mb, (gulong)(bytes / (1024 * 1024) + 1));
        } else if (!put_files(c, "+OPL", "ART", files, &err)) {
            note(notes, _("Art for %s could not be copied: %s"), id, err->message);
            g_clear_error(&err);
        }
        g_strfreev(made);
    }

    {
        gchar *cfgname = g_strdup_printf("%s.cfg", id);

        /* never overwrite an info file that exists: it may hold a description the user wrote */
        if (!g_hash_table_contains(lib->cfgs, cfgname)) {
            gchar *cfgpath = g_build_filename(work, cfgname, NULL);
            GPtrArray *one = g_ptr_array_new();

            if (cfg_write(cfgpath, title, NULL, NULL)) {
                g_ptr_array_add(one, cfgpath);
                if (!put_files(c, "+OPL", "CFG", one, &err)) {
                    note(notes, _("The info file for %s could not be copied: %s"), id, err->message);
                    g_clear_error(&err);
                }
            }
            g_ptr_array_free(one, TRUE);
            g_free(cfgpath);
        }
        g_free(cfgname);
    }
    g_ptr_array_free(skip, TRUE);
    g_ptr_array_free(files, TRUE);
    g_free(artdir);
    g_free(prefix);
    return ok;
}

gboolean ops_update_art(OpsContext *c, const char *id, Ps2gmPlatform platform, const char *title,
                        gboolean replace, GPtrArray *notes, Ps2gmProgress cb, gpointer ud, GError **error)
{
    Library *lib = backend_library(c->backend, error);
    gchar *work;
    Stage st = {cb, ud, 0, 1000, FALSE};
    gboolean ok;

    if (lib == NULL)
        return FALSE;
    work = make_work_dir(c, error);
    if (work == NULL) {
        library_free(lib);
        return FALSE;
    }
    ok = put_art_and_cfg(c, lib, work, id, platform, title, replace, notes, &st);
    remove_tree(work);
    g_free(work);
    library_free(lib);
    return ok;
}

gboolean ops_share_memcard(OpsContext *c, char *const *vcd_names, GError **error)
{
    guint n = g_strv_length((char **)vcd_names), i;
    gchar *work, *vmcfile, *first;
    gboolean ok = TRUE;

    if (n < 2 || n > 4) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("A shared memory card needs 2 to 4 discs"));
        return FALSE;
    }
    for (i = 0; i < n; i++)
        if (!ps2gm_arg_ok(vcd_names[i]) || !g_str_has_suffix(vcd_names[i], ".VCD") || strchr(vcd_names[i], '/')) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad disc name"));
            return FALSE;
        }
    first = g_strndup(vcd_names[0], strlen(vcd_names[0]) - 4);
    if (strlen(first) > 103) {
        g_free(first);
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("The name of the first disc is too long for POPStarter's VMCDIR.TXT"));
        return FALSE;
    }
    work = make_work_dir(c, error);
    if (work == NULL) {
        g_free(first);
        return FALSE;
    }
    vmcfile = g_build_filename(work, "VMCDIR.TXT", NULL);
    ok = g_file_set_contents(vmcfile, first, strlen(first), error);
    for (i = 0; ok && i < n; i++) {
        gchar *base = g_strndup(vcd_names[i], strlen(vcd_names[i]) - 4), *dir = g_strdup_printf("POPS/%s", base);
        GPtrArray *one = g_ptr_array_new();

        g_ptr_array_add(one, vmcfile);
        ok = put_files(c, "__common", dir, one, error);
        g_ptr_array_free(one, TRUE);
        g_free(base);
        g_free(dir);
    }
    remove_tree(work);
    g_free(work);
    g_free(vmcfile);
    g_free(first);
    return ok;
}

/* the text of the game's .cfg ("" if there is none) and writes a new text back */
static gboolean read_game_cfg(OpsContext *c, const char *id, char **text, GError **error)
{
    Library *lib = backend_library(c->backend, error);
    gchar *name = g_strdup_printf("%s.cfg", id);
    gboolean ok = TRUE;

    *text = NULL;
    if (lib == NULL) {
        g_free(name);
        return FALSE;
    }
    if (g_hash_table_contains(lib->cfgs, name))
        ok = backend_read_text(c->backend, "CFG", name, text, error);
    g_free(name);
    library_free(lib);
    return ok;
}

static gboolean write_game_cfg(OpsContext *c, const char *id, const char *text, GError **error)
{
    gchar *work = make_work_dir(c, error), *name, *path;
    GPtrArray *one = g_ptr_array_new();
    gboolean ok;

    if (work == NULL) {
        g_ptr_array_free(one, TRUE);
        return FALSE;
    }
    name = g_strdup_printf("%s.cfg", id);
    path = g_build_filename(work, name, NULL);
    ok = g_file_set_contents(path, text, -1, error);
    if (ok) {
        g_ptr_array_add(one, path);
        ok = put_files(c, "+OPL", "CFG", one, error);
    }
    g_ptr_array_free(one, TRUE);
    remove_tree(work);
    g_free(path);
    g_free(name);
    g_free(work);
    return ok;
}

gboolean ops_get_settings(OpsContext *c, const char *id, GameSettings *settings, GError **error)
{
    char *text = NULL;

    if (!ps2gm_arg_ok(id) || strchr(id, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID"));
        return FALSE;
    }
    if (!read_game_cfg(c, id, &text, error))
        return FALSE;
    game_settings_init(settings);
    game_settings_parse(text, settings);
    g_free(text);
    return TRUE;
}

gboolean ops_set_settings(OpsContext *c, const char *id, const GameSettings *settings, GError **error)
{
    char *old = NULL, *merged;
    gboolean ok;

    if (!ps2gm_arg_ok(id) || strchr(id, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID"));
        return FALSE;
    }
    if (!read_game_cfg(c, id, &old, error))
        return FALSE;
    merged = game_settings_apply(old, settings, error);
    g_free(old);
    if (merged == NULL)
        return FALSE;
    ok = write_game_cfg(c, id, merged, error);
    g_free(merged);
    return ok;
}

gboolean ops_reset_settings(OpsContext *c, const char *id, GError **error)
{
    char *old = NULL, *merged;
    gboolean ok;

    if (!ps2gm_arg_ok(id) || strchr(id, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID"));
        return FALSE;
    }
    if (!read_game_cfg(c, id, &old, error))
        return FALSE;
    merged = game_settings_reset(old);
    g_free(old);
    ok = write_game_cfg(c, id, merged, error);
    g_free(merged);
    return ok;
}

/* ---- virtual memory cards ---------------------------------------------------------------------- */

static gboolean vmc_listed(const Library *lib, const char *name, guint64 *size)
{
    guint i;

    for (i = 0; i < lib->vmcs->len; i++) {
        LibFile *f = g_ptr_array_index(lib->vmcs, i);

        if (!g_strcmp0(f->name, name)) {
            if (size != NULL)
                *size = f->size;
            return TRUE;
        }
    }
    return FALSE;
}

gboolean ops_vmc_fetch(OpsContext *c, const char *name, const char *dest_path, GError **error)
{
    char *tmp = NULL;
    GFile *from, *to;
    gboolean ok;

    if (!ps2gm_arg_ok(name) || !g_str_has_suffix(name, ".bin")) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad memory card name"));
        return FALSE;
    }
    if (!backend_get_file(c->backend, "VMC", name, &tmp, error))
        return FALSE;
    from = g_file_new_for_path(tmp);
    to = g_file_new_for_path(dest_path);
    ok = g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
    g_unlink(tmp);
    g_object_unref(from);
    g_object_unref(to);
    g_free(tmp);
    return ok;
}

gboolean ops_vmc_store(OpsContext *c, const char *name, const char *local_path, GError **error)
{
    Library *lib = NULL;
    gchar *base, *tmpname = NULL, *work = NULL, *staged = NULL;
    guint64 want = file_size(local_path), seen = 0;
    GPtrArray *one = g_ptr_array_new();
    gboolean ok = FALSE;
    GFile *from = NULL, *to = NULL;

    if (!ps2gm_arg_ok(name) || !g_str_has_suffix(name, ".bin") || strlen(name) < 5) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad memory card name"));
        goto out;
    }
    base = g_strndup(name, strlen(name) - 4);
    tmpname = g_strdup_printf("%s.ps2gm-new.bin", base);
    g_free(base);
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        goto out;
    if (!vmc_listed(lib, name, NULL)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("The card %s is not on the disk any more"), name);
        goto out;
    }
    if (vmc_listed(lib, tmpname, NULL) && !backend_run(c->backend, (char *[]){"REMOVE_FILE", "+OPL", "VMC", tmpname, NULL}, NULL, NULL, error))
        goto out; /* a leftover of an earlier interrupted save */
    work = make_work_dir(c, error);
    if (work == NULL)
        goto out;
    staged = g_build_filename(work, tmpname, NULL);
    from = g_file_new_for_path(local_path);
    to = g_file_new_for_path(staged);
    if (!g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error))
        goto out;
    g_ptr_array_add(one, staged);
    if (!put_files(c, "+OPL", "VMC", one, error))
        goto out;
    library_free(lib);
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        goto out;
    if (!vmc_listed(lib, tmpname, &seen) || seen != want) {
        backend_run(c->backend, (char *[]){"REMOVE_FILE", "+OPL", "VMC", tmpname, NULL}, NULL, NULL, NULL);
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("The card was not copied completely to the disk; the old card was not touched"));
        goto out;
    }
    if (!backend_run(c->backend, (char *[]){"REMOVE_FILE", "+OPL", "VMC", (char *)name, NULL}, NULL, NULL, error))
        goto out;
    if (!backend_run(c->backend, (char *[]){"RENAME_FILE", "+OPL", "VMC", tmpname, (char *)name, NULL}, NULL, NULL, error)) {
        {
            char *what = g_strdup_printf(_("The new card is on the disk as %s but could not take the place of %s"), tmpname, name);

            g_prefix_error(error, "%s: ", what);
            g_free(what);
        }
        goto out;
    }
    ok = TRUE;
out:
    g_ptr_array_free(one, TRUE);
    if (work != NULL) {
        remove_tree(work);
        g_free(work);
    }
    if (from)
        g_object_unref(from);
    if (to)
        g_object_unref(to);
    library_free(lib);
    g_free(tmpname);
    g_free(staged);
    return ok;
}

gboolean ops_get_cheats(OpsContext *c, const char *id, char **text, GError **error)
{
    Library *lib;
    gchar *name;
    gboolean ok = TRUE;

    *text = NULL;
    if (!ps2gm_arg_ok(id)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID"));
        return FALSE;
    }
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        return FALSE;
    name = g_strdup_printf("%s.cht", id);
    if (g_hash_table_contains(lib->cheats, name))
        ok = backend_read_text(c->backend, "CHT", name, text, error);
    g_free(name);
    library_free(lib);
    return ok;
}

/* writes the cheat keys into the game's config file, keeping everything else that is in it */
static gboolean set_cheat_config(OpsContext *c, const Library *lib, const char *work, const char *id, gboolean on, GError **error)
{
    gchar *cfgname = g_strdup_printf("%s.cfg", id), *old = NULL, *merged, *path;
    const char *pairs[] = {"$CheatsSource", "1", "$EnableCheat", on ? "1" : "0", "$CheatMode", "0", NULL};
    GPtrArray *one = g_ptr_array_new();
    gboolean ok;

    if (g_hash_table_contains(lib->cfgs, cfgname) && !backend_read_text(c->backend, "CFG", cfgname, &old, error)) {
        g_free(cfgname);
        g_ptr_array_free(one, TRUE);
        return FALSE;
    }
    merged = cfg_set_keys(old, pairs);
    path = g_build_filename(work, cfgname, NULL);
    ok = g_file_set_contents(path, merged, -1, error);
    if (ok) {
        g_ptr_array_add(one, path);
        ok = put_files(c, "+OPL", "CFG", one, error);
    }
    g_ptr_array_free(one, TRUE);
    g_free(path);
    g_free(merged);
    g_free(old);
    g_free(cfgname);
    return ok;
}

gboolean ops_set_cheats(OpsContext *c, const char *id, const char *text, gboolean turn_on, CheatInfo *info,
                        GError **error)
{
    Library *lib;
    gchar *work, *clean = NULL, *chtname, *path;
    gboolean ok = FALSE;

    if (!ps2gm_arg_ok(id) || strchr(id, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID"));
        return FALSE;
    }
    if (text != NULL && !cheats_normalize(text, &clean, info, error))
        return FALSE;
    lib = backend_library(c->backend, error);
    if (lib == NULL) {
        g_free(clean);
        return FALSE;
    }
    work = make_work_dir(c, error);
    if (work == NULL) {
        g_free(clean);
        library_free(lib);
        return FALSE;
    }
    chtname = g_strdup_printf("%s.cht", id);
    if (clean != NULL) {
        GPtrArray *one = g_ptr_array_new();

        path = g_build_filename(work, chtname, NULL);
        ok = g_file_set_contents(path, clean, -1, error);
        if (ok) {
            g_ptr_array_add(one, path);
            ok = put_files(c, "+OPL", "CHT", one, error);
        }
        g_ptr_array_free(one, TRUE);
        g_free(path);
        if (ok)
            ok = set_cheat_config(c, lib, work, id, turn_on, error);
    } else {
        ok = TRUE;
        if (g_hash_table_contains(lib->cheats, chtname)) {
            char *rm[] = {"REMOVE_FILE", "+OPL", "CHT", chtname, NULL};

            ok = backend_run(c->backend, rm, NULL, NULL, error);
        }
        if (ok) {
            gchar *cfgname = g_strdup_printf("%s.cfg", id);

            if (g_hash_table_contains(lib->cfgs, cfgname))
                ok = set_cheat_config(c, lib, work, id, FALSE, error); /* switch the engine off, keep the rest */
            g_free(cfgname);
        }
    }
    g_free(chtname);
    g_free(clean);
    remove_tree(work);
    g_free(work);
    library_free(lib);
    return ok;
}

/* ---- programs (APPS) --------------------------------------------------------------------------- */

char *ops_apps_backup_name(const char *name)
{
    gsize n = strlen(name);

    if (ps2gm_is_elf_name(name))
        return g_strdup_printf("%.*s-OLD%s", (int)(n - 4), name, name + n - 4);
    return g_strdup_printf("%s-OLD", name);
}

static gboolean apps_has(const Library *lib, const char *name)
{
    guint i;

    for (i = 0; i < lib->apps->len; i++)
        if (!strcmp(((LibFile *)g_ptr_array_index(lib->apps, i))->name, name))
            return TRUE;
    return FALSE;
}

static gboolean apps_cmd(OpsContext *c, const char *cmd, const char *a, const char *b, GError **error)
{
    char *argv[] = {(char *)cmd, "APPS", "", (char *)a, (char *)b, NULL};

    return backend_run(c->backend, argv, NULL, NULL, error);
}

gboolean ops_apps_add(OpsContext *c, const char *path, gboolean keep_old, GPtrArray *notes, GError **error)
{
    gchar *name = g_path_get_basename(path), *backup = NULL;
    Library *lib;
    GPtrArray *files;
    gboolean ok = FALSE, renamed = FALSE;

    if (!ps2gm_elf_check(path, error))
        goto out;
    if (!ps2gm_arg_ok(name) || !ps2gm_is_elf_name(name)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad characters in the name of the program"));
        goto out;
    }
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        goto out;
    if (keep_old && apps_has(lib, name)) {
        backup = ops_apps_backup_name(name);
        if ((apps_has(lib, backup) && !backend_run(c->backend, (char *[]){"REMOVE_FILE", "APPS", "", backup, NULL}, NULL, NULL, error)) ||
            !apps_cmd(c, "RENAME_FILE", name, backup, error)) {
            library_free(lib);
            goto out;
        }
        renamed = TRUE;
    }
    library_free(lib);

    files = g_ptr_array_new();
    g_ptr_array_add(files, (gpointer)path);
    ok = put_files(c, "APPS", "", files, error); /* a program of the same name is replaced */
    g_ptr_array_free(files, TRUE);
    if (!ok && renamed) {
        GError *e2 = NULL;

        /* put the old one back: the disk must not end up without it */
        if (!apps_cmd(c, "RENAME_FILE", backup, name, &e2)) {
            note(notes, _("The old %s could not be put back and is saved as %s"), name, backup);
            g_clear_error(&e2);
        }
    } else if (ok && renamed)
        note(notes, _("The old version was kept as %s"), backup);
out:
    g_free(name);
    g_free(backup);
    return ok;
}

gboolean ops_apps_remove(OpsContext *c, const char *name, GError **error)
{
    if (!ps2gm_is_elf_name(name)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Only programs (.elf) can be removed here"));
        return FALSE;
    }
    return backend_run(c->backend, (char *[]){"REMOVE_FILE", "APPS", "", (char *)name, NULL}, NULL, NULL, error);
}

gboolean ops_apps_rename(OpsContext *c, const char *from, const char *to, GError **error)
{
    if (!ps2gm_is_elf_name(from) || !ps2gm_is_elf_name(to) || !ps2gm_arg_ok(to) || strchr(to, '/')) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("The new name must end in .elf and have no special characters"));
        return FALSE;
    }
    return apps_cmd(c, "RENAME_FILE", from, to, error);
}

gboolean ops_set_art(OpsContext *c, const char *id, Ps2gmPlatform platform, const char *key, const char *source,
                     gboolean whole, Ps2gmProgress cb, gpointer ud, GError **error)
{
    Library *lib;
    gchar *work, *artdir, *name = NULL, *path;
    GPtrArray *files;
    gboolean ok;

    if (!ps2gm_arg_ok(id) || art_type_find(key) == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Bad game ID or kind of picture"));
        return FALSE;
    }
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        return FALSE;
    work = make_work_dir(c, error);
    if (work == NULL) {
        library_free(lib);
        return FALSE;
    }
    artdir = g_build_filename(work, "art", NULL);
    report(cb, ud, 100, source ? _("Preparing the picture") : _("Downloading the picture"));
    if (source == NULL)
        ok = art_fetch_one(id, platform, key, artdir, &name, error);
    else if (g_str_has_prefix(source, "http://") || g_str_has_prefix(source, "https://"))
        ok = art_import_url(source, id, key, platform, whole, artdir, &name, error);
    else
        ok = art_import_file(source, id, key, platform, whole, artdir, &name, error);
    if (ok) {
        path = g_build_filename(artdir, name, NULL);
        if (file_size(path) / (1024 * 1024) + OPL_MARGIN_MB > lib->opl_free_mb) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED,
                        _("Not enough free space on the +OPL partition for this picture (%lu MB free)"),
                        (gulong)lib->opl_free_mb);
            ok = FALSE;
        } else {
            report(cb, ud, 600, _("Copying to the PS2 disk"));
            files = g_ptr_array_new();
            g_ptr_array_add(files, path);
            ok = put_files(c, "+OPL", "ART", files, error); /* replaces the old file of the same name */
            g_ptr_array_free(files, TRUE);
        }
        g_free(path);
    }
    report(cb, ud, 1000, _("Done"));
    g_free(name);
    g_free(artdir);
    remove_tree(work);
    g_free(work);
    library_free(lib);
    return ok;
}

/* ---- PS2 --------------------------------------------------------------------------------------- */

typedef struct {
    Ps2gmProgress cb;
    gpointer ud;
} Forward;

/* the helper reports "PROGRESS<TAB>percent<TAB>text" while hdl_dump copies the disc */
static void on_install_line(const char *line, gpointer p)
{
    Forward *f = p;
    gchar **v = g_strsplit(line, "\t", 3);

    if (g_strv_length(v) >= 2 && !g_strcmp0(v[0], "PROGRESS") && f->cb)
        f->cb(50 + g_ascii_strtoull(v[1], NULL, 10) * 8, 1000, _("Installing to the PS2 disk"), f->ud);
    g_strfreev(v);
}

gboolean ops_install_ps2(OpsContext *c, const char *iso, const char *media, guint64 size_kb, const char *id,
                         const char *title_in, gboolean with_art, GPtrArray *notes, Ps2gmProgress cb,
                         gpointer ud, GError **error)
{
    Library *lib;
    gchar *title = ops_clean_title(title_in), *work = NULL;
    const char *kind = !g_strcmp0(media, "CD") ? "CD" : !g_strcmp0(media, "DVD") ? "DVD" : size_kb > 737280 ? "DVD" : "CD";
    gboolean ok = FALSE;
    guint i;

    report(cb, ud, 0, _("Checking the disk"));
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        goto out;
    if (title[0] == '\0') {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("The game needs a title"));
        goto out;
    }
    for (i = 0; i < lib->games->len; i++) {
        LibGame *g = g_ptr_array_index(lib->games, i);

        if (!g_strcmp0(g->id, id)) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("%s (%s) is already installed"), g->title, id);
            goto out;
        }
    }
    if (size_kb / 1024 + 20 > lib->free_mb) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Not enough free space on the PS2 disk (%lu MB free, %lu MB needed)"),
                    (gulong)lib->free_mb, (gulong)(size_kb / 1024 + 20));
        goto out;
    }
    {
        char *cmd[] = {"INSTALL_PS2", (char *)kind, title, (char *)iso, (char *)id, NULL};
        Forward fwd = {cb, ud};

        report(cb, ud, 50, _("Installing to the PS2 disk"));
        if (!backend_run(c->backend, cmd, on_install_line, &fwd, error))
            goto out;
    }
    if (with_art) {
        Stage st = {cb, ud, 850, 100, FALSE};

        report(cb, ud, 850, _("Downloading the art"));
        work = make_work_dir(c, error);
        if (work == NULL)
            goto out;
        library_free(lib);
        lib = backend_library(c->backend, error); /* fresh view: free space on +OPL, existing art */
        if (lib == NULL)
            goto out;
        put_art_and_cfg(c, lib, work, id, PS2GM_PS2, title, FALSE, notes, &st);
    }
    report(cb, ud, 1000, _("Done"));
    ok = TRUE;
out:
    if (work) {
        remove_tree(work);
        g_free(work);
    }
    library_free(lib);
    g_free(title);
    return ok;
}

/* ---- PS1 --------------------------------------------------------------------------------------- */

static guint64 ps1_vcd_bytes(const char *cue_path)
{
    Ps1Cue *cue = ps1cue_parse(cue_path, NULL);
    guint64 n = VCD_HEADER_SIZE, i;

    if (cue == NULL)
        return 0;
    for (i = 0; i < cue->files->len; i++)
        n += ((Ps1File *)g_ptr_array_index(cue->files, i))->size;
    ps1cue_free(cue);
    return n;
}

gboolean ops_install_ps1(OpsContext *c, GPtrArray *discs, const char *title_in, gboolean with_art,
                         GPtrArray *notes, Ps2gmProgress cb, gpointer ud, GError **error)
{
    Library *lib = NULL;
    gchar *title = ops_clean_title(title_in), *work = NULL;
    guint n = discs->len, i, k;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    guint64 need = 0;
    gboolean ok = FALSE;

    if (n < 1 || n > 4) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("A game can have 1 to 4 discs"));
        goto out;
    }
    if (title[0] == '\0') {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("The game needs a title"));
        goto out;
    }
    report(cb, ud, 0, _("Checking the disk"));
    lib = backend_library(c->backend, error);
    if (lib == NULL)
        goto out;

    for (i = 0; i < n; i++) {
        Ps1Disc *d = g_ptr_array_index(discs, i);
        gchar *name = n > 1 ? g_strdup_printf("%s.%s (Disc %u).VCD", d->id, title, i + 1) :
                              g_strdup_printf("%s.%s.VCD", d->id, title);

        if (strlen(name) > 85) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("The title is too long (POPStarter limits the file name)"));
            g_free(name);
            goto out;
        }
        for (k = 0; k < lib->vcds->len; k++)
            if (!g_ascii_strcasecmp(((LibFile *)g_ptr_array_index(lib->vcds, k))->name, name)) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("%s is already installed"), name);
                g_free(name);
                goto out;
            }
        g_ptr_array_add(names, name);
        need += ps1_vcd_bytes(d->cue_path);
    }
    if (need == 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("Cannot read the disc images"));
        goto out;
    }
    /* pops_size_mb == 0: the size could not be read, so do not guess; a copy that does not fit fails cleanly */
    if (lib->pops_size_mb > 0 && need / (1024 * 1024) + 5 > lib->pops_free_mb) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Not enough free space in the PS1 partition (__.POPS): %lu MB free, %lu MB needed"),
                    (gulong)lib->pops_free_mb, (gulong)(need / (1024 * 1024) + 5));
        goto out;
    }
    work = make_work_dir(c, error);
    if (work == NULL)
        goto out;

    for (i = 0; i < n; i++) {
        Ps1Disc *d = g_ptr_array_index(discs, i);
        const char *name = g_ptr_array_index(names, i);
        gchar *vcd = g_build_filename(work, name, NULL);
        guint64 share = 800 / n, base = 20 + share * i;
        Stage st = {cb, ud, base, share * 4 / 10, FALSE};
        GPtrArray *one = g_ptr_array_new();
        gboolean good;

        good = ps1_make_vcd(d->cue_path, vcd, 0, stage_cb, &st, error);
        if (good) {
            report(cb, ud, base + share * 4 / 10, _("Copying to the PS2 disk (this takes a few minutes)"));
            g_ptr_array_add(one, vcd);
            good = put_files(c, "__.POPS", "", one, error);
        }
        g_ptr_array_free(one, TRUE);
        g_unlink(vcd);
        g_free(vcd);
        if (!good)
            goto out;
    }

    if (n > 1) {
        /*
         * POPStarter swaps discs from DISCS.TXT, which has to be in the folder of every disc.
         * VMCDIR.TXT (one line, the name of a folder in POPS, at most 103 bytes) makes a disc use the memory
         * card of that folder instead of its own: all discs use the one of disc 1, so the saves are shared.
         */
        gchar *discfile = g_build_filename(work, "DISCS.TXT", NULL);
        gchar *vmcfile = g_build_filename(work, "VMCDIR.TXT", NULL);
        gchar *first_dir = g_strndup(g_ptr_array_index(names, 0), strlen(g_ptr_array_index(names, 0)) - 4);
        GString *text = g_string_new(NULL);

        for (i = 0; i < n; i++)
            g_string_append_printf(text, "%s\n", (char *)g_ptr_array_index(names, i));
        if (!g_file_set_contents(discfile, text->str, text->len, error) ||
            !g_file_set_contents(vmcfile, first_dir, strlen(first_dir), error)) {
            g_string_free(text, TRUE);
            g_free(discfile);
            g_free(vmcfile);
            g_free(first_dir);
            goto out;
        }
        for (i = 0; i < n; i++) {
            const char *name = g_ptr_array_index(names, i);
            gchar *base = g_strndup(name, strlen(name) - 4), *dir = g_strdup_printf("POPS/%s", base);
            GPtrArray *one = g_ptr_array_new();
            gboolean good;

            g_ptr_array_add(one, discfile);
            if (strlen(first_dir) <= 103)
                g_ptr_array_add(one, vmcfile);
            good = put_files(c, "__common", dir, one, error);
            g_ptr_array_free(one, TRUE);
            g_free(base);
            g_free(dir);
            if (!good) {
                g_string_free(text, TRUE);
                g_free(discfile);
                g_free(vmcfile);
                g_free(first_dir);
                goto out;
            }
        }
        g_string_free(text, TRUE);
        g_free(discfile);
        g_free(vmcfile);
        g_free(first_dir);
    }

    if (with_art) {
        library_free(lib);
        lib = backend_library(c->backend, error);
        if (lib == NULL)
            goto out;
        for (i = 0; i < n; i++) {
            Ps1Disc *d = g_ptr_array_index(discs, i);
            Stage st = {cb, ud, 850 + 150 * i / n, 150 / n, FALSE};
            gboolean dup = FALSE;

            for (k = 0; k < i; k++)
                if (!g_strcmp0(((Ps1Disc *)g_ptr_array_index(discs, k))->id, d->id))
                    dup = TRUE;
            report(cb, ud, st.base, _("Downloading the art"));
            if (!dup)
                put_art_and_cfg(c, lib, work, d->id, PS2GM_PS1, title, FALSE, notes, &st);
        }
    }
    report(cb, ud, 1000, _("Done"));
    ok = TRUE;
out:
    if (work) {
        remove_tree(work);
        g_free(work);
    }
    g_ptr_array_free(names, TRUE);
    library_free(lib);
    g_free(title);
    return ok;
}

/* ---- removal ----------------------------------------------------------------------------------- */

gboolean ops_remove_ps2(OpsContext *c, const char *id, const char *title, GError **error)
{
    char *cmd[] = {"REMOVE_PS2", (char *)id, (char *)title, NULL};

    return backend_run(c->backend, cmd, NULL, NULL, error);
}

gboolean ops_remove_ps1(OpsContext *c, const char *vcd_name, GError **error)
{
    char *cmd[] = {"REMOVE_FILE", "__.POPS", "", (char *)vcd_name, NULL};

    return backend_run(c->backend, cmd, NULL, NULL, error);
}
