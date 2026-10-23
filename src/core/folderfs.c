#include "folderfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <glib/gstdio.h>

#define MSDOS_FS_MAGIC 0x4d44 /* FAT: a file cannot be 4 GiB or more */
#define CHUNK (4 * 1024 * 1024)

gboolean folder_is(const char *device)
{
    return device != NULL && g_file_test(device, G_FILE_TEST_IS_DIR);
}

/* ---- where things are ---------------------------------------------------------------------------- */

/* the folder on the drive for a partition name and folder of the PFS layout; NULL if the app has no such place here */
static char *map_dir(const char *root, const char *partition, const char *dir)
{
    const char *sub = dir != NULL ? dir : "";

    if (!ps2gm_remote_path_ok(sub))
        return NULL;
    if (!g_strcmp0(partition, "+OPL")) /* ART, CFG, CHT, VMC, and POPS (shared with the PS1 folder) */
        return *sub ? g_build_filename(root, sub, NULL) : g_strdup(root);
    if (!g_strcmp0(partition, "APPS"))
        return *sub ? g_build_filename(root, "APPS", sub, NULL) : g_build_filename(root, "APPS", NULL);
    if (!g_strcmp0(partition, "__.POPS"))
        return *sub ? g_build_filename(root, "POPS", sub, NULL) : g_build_filename(root, "POPS", NULL);
    if (!g_strcmp0(partition, "__common")) /* __common/POPS/<game>: the same folder as POPS/<game> of the drive */
        return *sub ? g_build_filename(root, sub, NULL) : g_strdup(root);
    return NULL;
}

static void bad_place(GError **error)
{
    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("That place does not exist on a drive of this kind"));
}

/* ---- free space ---------------------------------------------------------------------------------- */

gboolean folder_space(const char *root, guint64 *total_mb, guint64 *used_mb, guint64 *free_mb, GError **error)
{
    struct statvfs s;
    guint64 total, avail;

    if (statvfs(root, &s) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read the size of %s: %s"), root, g_strerror(errno));
        return FALSE;
    }
    total = (guint64)s.f_blocks * s.f_frsize / (1024 * 1024);
    avail = (guint64)s.f_bavail * s.f_frsize / (1024 * 1024);
    *total_mb = total;
    *free_mb = avail;
    *used_mb = total > avail ? total - avail : 0;
    return TRUE;
}

/* ---- the games ----------------------------------------------------------------------------------- */

static GRegex *game_name_re(void)
{
    static GRegex *re;

    if (re == NULL)
        re = g_regex_new("^([A-Z]{4}_[0-9]{3}\\.[0-9]{2})\\.(.+)\\.(iso|zso)$", G_REGEX_CASELESS, 0, NULL);
    return re;
}

/* "SLES_539.08.Title.iso" -> id and title; FALSE for any other name (the app only manages files named the way OPL expects) */
static gboolean parse_game_name(const char *name, char **id, char **title, char **ext)
{
    GMatchInfo *mi = NULL;
    gboolean ok = g_regex_match(game_name_re(), name, 0, &mi);

    if (ok) {
        char *i = g_match_info_fetch(mi, 1);

        for (char *p = i; *p; p++)
            *p = g_ascii_toupper(*p);
        *id = i;
        *title = g_match_info_fetch(mi, 2);
        {
            char *e = g_match_info_fetch(mi, 3);

            *ext = g_ascii_strdown(e, -1);
            g_free(e);
        }
    }
    g_match_info_free(mi);
    return ok;
}

GPtrArray *folder_list_games(const char *root, GError **error)
{
    GPtrArray *games = g_ptr_array_new_with_free_func(hdl_game_free);
    static const char *media[] = {"DVD", "CD"};
    guint m;

    (void)error;
    for (m = 0; m < G_N_ELEMENTS(media); m++) {
        char *dir = g_build_filename(root, media[m], NULL);
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *name;

        while (d && (name = g_dir_read_name(d)) != NULL) {
            char *id, *title, *ext, *path;
            GStatBuf st;

            if (!parse_game_name(name, &id, &title, &ext))
                continue;
            path = g_build_filename(dir, name, NULL);
            if (g_stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                HdlGame *g = g_new0(HdlGame, 1);

                g->media = g_strdup(media[m]);
                g->size_kb = (guint64)st.st_size / 1024;
                g->flags = g_strdup("");
                g->id = id;
                g->title = title;
                g_ptr_array_add(games, g);
            } else {
                g_free(id);
                g_free(title);
            }
            g_free(ext);
            g_free(path);
        }
        if (d)
            g_dir_close(d);
        g_free(dir);
    }
    return games;
}

/* the file of an installed game, by exact ID and title; NULL if there is none */
static char *find_game_file(const char *root, const char *id, const char *title)
{
    static const char *media[] = {"DVD", "CD"};
    guint m;

    for (m = 0; m < G_N_ELEMENTS(media); m++) {
        char *dir = g_build_filename(root, media[m], NULL);
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *name;

        while (d && (name = g_dir_read_name(d)) != NULL) {
            char *gid, *gtitle, *ext;

            if (parse_game_name(name, &gid, &gtitle, &ext)) {
                gboolean same = !g_strcmp0(gid, id) && !g_strcmp0(gtitle, title);

                g_free(gid);
                g_free(gtitle);
                g_free(ext);
                if (same) {
                    char *path = g_build_filename(dir, name, NULL);

                    g_dir_close(d);
                    g_free(dir);
                    return path;
                }
            }
        }
        if (d)
            g_dir_close(d);
        g_free(dir);
    }
    return NULL;
}

/* copies a file to dest through a temporary name, so a half written file never has the final name; progress 0..100 */
static gboolean copy_file(const char *from, const char *dest, gboolean fat_check, Ps2gmProgress progress, gpointer ud, const char *what, GError **error)
{
    FILE *in = g_fopen(from, "rb");
    char *part = g_strdup_printf("%s.part", dest);
    int fd = -1;
    guint8 *buf = g_malloc(CHUNK);
    GStatBuf st;
    guint64 total = 0, done = 0;
    gboolean ok = FALSE;
    gssize n;
    int last = -1;

    if (in == NULL || g_stat(from, &st) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), from);
        goto out;
    }
    total = (guint64)st.st_size;
    {
        char *dir = g_path_get_dirname(dest);
        struct statfs fs;

        g_mkdir_with_parents(dir, 0755);
        if (fat_check && statfs(dir, &fs) == 0 && (unsigned long)fs.f_type == MSDOS_FS_MAGIC && total >= 4294967295ull) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED,
                        _("This drive uses FAT32, which cannot hold a file of 4 GiB or more. Use a compressed .zso, or format the card as exFAT."));
            g_free(dir);
            goto out;
        }
        g_free(dir);
    }
    fd = g_open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot write %s: %s"), part, g_strerror(errno));
        goto out;
    }
    while ((n = (gssize)fread(buf, 1, CHUNK, in)) > 0) {
        gssize off = 0;

        while (off < n) {
            gssize w = write(fd, buf + off, (size_t)(n - off));

            if (w < 0) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Writing to the drive failed: %s"), g_strerror(errno));
                goto out;
            }
            off += w;
        }
        done += (guint64)n;
        if (progress != NULL && total > 0) {
            int pct = (int)(done * 100 / total);

            if (pct != last) {
                last = pct;
                if (!progress((guint64)pct, 100, what, ud)) {
                    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
                    goto out;
                }
            }
        }
    }
    if (ferror(in)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), from);
        goto out;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        fd = -1;
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Writing to the drive failed: %s"), g_strerror(errno));
        goto out;
    }
    fd = -1;
    if (g_rename(part, dest) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot write %s: %s"), dest, g_strerror(errno));
        goto out;
    }
    ok = TRUE;
out:
    if (fd >= 0)
        close(fd);
    if (!ok)
        g_unlink(part);
    if (in)
        fclose(in);
    g_free(part);
    g_free(buf);
    return ok;
}

gboolean folder_install(const char *root, const char *media, const char *title, const char *iso, const char *id, Ps2gmProgress progress,
                        gpointer user_data, GError **error)
{
    const char *ext = ps2gm_is_zso_name(iso) ? "zso" : "iso";
    char *name, *dest, *dir;
    gboolean ok;

    if (!ps2gm_arg_ok(title) || !ps2gm_arg_ok(id) || strchr(title, '/') || strchr(id, '/') || (g_strcmp0(media, "CD") && g_strcmp0(media, "DVD"))) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("Bad game name"));
        return FALSE;
    }
    if (find_game_file(root, id, title) != NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("%s (%s) is already installed"), title, id);
        return FALSE;
    }
    name = g_strdup_printf("%s.%s.%s", id, title, ext);
    dir = g_build_filename(root, media, NULL);
    dest = g_build_filename(dir, name, NULL);
    ok = copy_file(iso, dest, TRUE, progress, user_data, _("Installing to the PS2 disk"), error);
    g_free(name);
    g_free(dir);
    g_free(dest);
    return ok;
}

gboolean folder_remove_game(const char *root, const char *id, const char *title, GError **error)
{
    char *path = find_game_file(root, id, title);
    gboolean ok;

    if (path == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("No installed game matches %s (%s)"), title, id);
        return FALSE;
    }
    ok = g_unlink(path) == 0;
    if (!ok)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not remove %s: %s"), path, g_strerror(errno));
    g_free(path);
    return ok;
}

/* ---- files and folders ---------------------------------------------------------------------------- */

GPtrArray *folder_list(const char *root, const char *partition, const char *dir, GError **error)
{
    char *path = map_dir(root, partition, dir);
    GDir *d;
    const char *name;
    GPtrArray *list;

    if (path == NULL) {
        bad_place(error);
        return NULL;
    }
    d = g_dir_open(path, 0, NULL);
    if (d == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, dir ? dir : "");
        g_free(path);
        return NULL;
    }
    list = g_ptr_array_new_with_free_func(pfs_entry_free);
    while ((name = g_dir_read_name(d)) != NULL) {
        char *full = g_build_filename(path, name, NULL);
        GStatBuf st;

        if (g_stat(full, &st) == 0) {
            PfsEntry *e = g_new0(PfsEntry, 1);

            e->name = g_strdup(name);
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = e->is_dir ? 0 : (guint64)st.st_size;
            g_ptr_array_add(list, e);
        }
        g_free(full);
    }
    g_dir_close(d);
    g_free(path);
    return list;
}

gboolean folder_put(const char *root, const char *partition, const char *remote_dir, char *const *local_files, GError **error)
{
    char *path = map_dir(root, partition, remote_dir);
    guint i;
    gboolean ok = TRUE;

    if (path == NULL) {
        bad_place(error);
        return FALSE;
    }
    if (g_mkdir_with_parents(path, 0755) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create the folder %s: %s"), path, g_strerror(errno));
        g_free(path);
        return FALSE;
    }
    for (i = 0; ok && local_files[i] != NULL; i++) {
        char *base = g_path_get_basename(local_files[i]), *dest = g_build_filename(path, base, NULL);

        ok = copy_file(local_files[i], dest, TRUE, NULL, NULL, NULL, error);
        g_free(base);
        g_free(dest);
    }
    g_free(path);
    return ok;
}

gboolean folder_get(const char *root, const char *partition, const char *remote_dir, char *const *names, const char *local_dir, GError **error)
{
    char *path = map_dir(root, partition, remote_dir);
    guint i;
    gboolean ok = TRUE;

    if (path == NULL) {
        bad_place(error);
        return FALSE;
    }
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Folder %s/%s does not exist"), partition, remote_dir ? remote_dir : "");
        g_free(path);
        return FALSE;
    }
    for (i = 0; ok && names[i] != NULL; i++) {
        char *from = g_build_filename(path, names[i], NULL), *to = g_build_filename(local_dir, names[i], NULL);

        if (g_file_test(from, G_FILE_TEST_IS_REGULAR)) /* files that are not there are simply missing in local_dir */
            ok = copy_file(from, to, FALSE, NULL, NULL, NULL, error);
        g_free(from);
        g_free(to);
    }
    g_free(path);
    return ok;
}

gboolean folder_remove(const char *root, const char *partition, const char *remote_dir, const char *name, GError **error)
{
    char *path = map_dir(root, partition, remote_dir), *full;
    gboolean ok = TRUE;

    if (path == NULL) {
        bad_place(error);
        return FALSE;
    }
    full = g_build_filename(path, name, NULL);
    if (g_unlink(full) != 0 && errno != ENOENT) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not remove %s"), name);
        ok = FALSE;
    }
    g_free(full);
    g_free(path);
    return ok;
}

gboolean folder_rmdir(const char *root, const char *partition, const char *remote_dir, const char *name, GError **error)
{
    char *path = map_dir(root, partition, remote_dir), *full;
    gboolean ok = TRUE;

    if (path == NULL) {
        bad_place(error);
        return FALSE;
    }
    full = g_build_filename(path, name, NULL);
    if (g_rmdir(full) != 0 && errno != ENOENT) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not remove the folder %s"), name);
        ok = FALSE;
    }
    g_free(full);
    g_free(path);
    return ok;
}

gboolean folder_rename(const char *root, const char *partition, const char *remote_dir, const char *from, const char *to, GError **error)
{
    char *path = map_dir(root, partition, remote_dir), *a, *b;
    gboolean ok = FALSE;

    if (path == NULL) {
        bad_place(error);
        return FALSE;
    }
    a = g_build_filename(path, from, NULL);
    b = g_build_filename(path, to, NULL);
    if (g_file_test(b, G_FILE_TEST_EXISTS))
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, _("%s already exists"), to);
    else if (g_rename(a, b) != 0)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Could not rename %s"), from);
    else
        ok = TRUE;
    g_free(a);
    g_free(b);
    g_free(path);
    return ok;
}
