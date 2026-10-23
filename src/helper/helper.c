/*
 * ps2gm-helper: the only part of Memento that touches the disk.
 *
 * Started once per session (normally through pkexec) as   ps2gm-helper <device>
 * and then driven with one tab-separated command per line on stdin. Every command answers with
 * zero or more data lines and ends with "END" or "ERR<TAB>message". It never runs a command that is
 * not in the list below and only writes to the folders OPL and POPStarter use:
 *
 *   LIBRARY                                      games, PS1 images, art and config files, free space
 *   INSTALL_PS2 <CD|DVD> <title> <iso> <id>      install an ISO as an HDL game
 *   REMOVE_PS2 <id> <title>                      delete the game whose ID and title both match
 *   PUT <partition> <dir> <file>...              copy local files into a folder of the disk
 *   REMOVE_FILE <partition> <dir> <name>         delete a file from such a folder
 *   RENAME_FILE <partition> <dir> <old> <new>    rename a program of APPS
 *   GET <ART|CFG|CHT> <name>...                  copy files of +OPL/ART (.png), CFG (.cfg) or CHT (.cht) into a
 *                                                temporary folder; answers "GOT <name> <local path>" per file (the
 *                                                folder is the caller's, emptied by the next GET and removed when the
 *                                                helper quits)
 *   QUIT
 *
 * Allowed partitions and folders:  +OPL: ART, CFG, CHT, VMC   __.POPS: (root)   __common: POPS/<one folder>
 *                                   APPS: (root), only .elf files that really are PS2 programs
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib/gstdio.h>

#include "folderfs.h"
#include "tools.h"
#include "ps1fix.h"

static Ps2gmTools tools;
static const char *device;

static void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void fail(const char *msg)
{
    gchar *flat = g_strdup(msg);

    g_strdelimit(flat, "\t\r\n", ' ');
    say("ERR\t%s", flat);
    g_free(flat);
}

/* ---- what may be written where ----------------------------------------------------------------- */

static gboolean location_allowed(const char *partition, const char *dir)
{
    if (!g_strcmp0(partition, "+OPL"))
        return !g_strcmp0(dir, "ART") || !g_strcmp0(dir, "CFG") || !g_strcmp0(dir, "CHT") || !g_strcmp0(dir, "VMC");
    if (!g_strcmp0(partition, "__.POPS") || !g_strcmp0(partition, "APPS"))
        return dir == NULL || *dir == '\0';
    if (!g_strcmp0(partition, "__common")) {
        gchar **parts;
        gboolean ok;

        if (dir == NULL || !g_str_has_prefix(dir, "POPS/"))
            return FALSE;
        parts = g_strsplit(dir, "/", -1);
        ok = g_strv_length(parts) == 2 && ps2gm_arg_ok(parts[1]);
        g_strfreev(parts);
        return ok;
    }
    return FALSE;
}

/* the only files that go into a PS1 game's folder in __common/POPS: what Memento writes for multi-disc games and the
 * settings POPStarter reads (CHEATS.TXT and the TROJAN_#.BIN / PATCH_#.BIN fix files) */
static gboolean pops_file_ok(const char *name)
{
    return ps2gm_arg_ok(name) && !strchr(name, '/') &&
           (!strcmp(name, "VMCDIR.TXT") || !strcmp(name, "DISCS.TXT") || !strcmp(name, "CHEATS.TXT") || ps1fix_is_fix_file_name(name));
}

/* a virtual memory card: a plain .bin name, no folders */
static gboolean is_vmc_name(const char *name)
{
    return ps2gm_arg_ok(name) && !strchr(name, '/') && strlen(name) > 4 && g_str_has_suffix(name, ".bin");
}

/* ... and it really starts like a memory card image, so nothing else can be stored in the VMC folder */
static gboolean looks_like_vmc(const char *path)
{
    char head[28];
    FILE *f = g_fopen(path, "rb");
    gboolean ok = f != NULL && fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "Sony PS2 Memory Card Format ", 28) == 0;

    if (f != NULL)
        fclose(f);
    return ok;
}

/* ---- disk check -------------------------------------------------------------------------------- */

/* A PS2 (APA) disk has "APA" at byte 4 of its first sector. Anything else is refused. */
static gboolean device_is_ps2_disk(const char *path, GError **error)
{
    if (folder_is(path)) { /* a mounted drive (SD card, USB stick): it must be writable, nothing else is checked */
        if (access(path, R_OK | W_OK) != 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED, "%s cannot be read and written by this user", path);
            return FALSE;
        }
        return TRUE;
    }
    guint8 sector[512];
    int fd = g_open(path, O_RDONLY, 0);
    gssize n;

    if (fd < 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, "Cannot open %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    n = read(fd, sector, sizeof(sector));
    close(fd);
    if (n != (gssize)sizeof(sector) || memcmp(sector + 4, "APA", 3) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED,
                    "%s is not a PS2 hard disk (no APA partition table); nothing was touched", path);
        return FALSE;
    }
    return TRUE;
}

/* ---- commands ---------------------------------------------------------------------------------- */

/* prints the entries; returns the total size of the files (not folders) listed */
static guint64 list_dir(const char *kind, const char *partition, const char *dir, gboolean dirs_only)
{
    GPtrArray *l = pfs_list(&tools, device, partition, dir, NULL);
    guint i;
    guint64 total = 0;

    for (i = 0; l && i < l->len; i++) {
        PfsEntry *e = g_ptr_array_index(l, i);

        if (e->is_dir == dirs_only)
            say("%s\t%s\t%" G_GUINT64_FORMAT, kind, e->name, e->size);
        if (!e->is_dir)
            total += e->size;
    }
    if (l)
        g_ptr_array_free(l, TRUE);
    return total;
}

/* the .VCD files of POPS (a drive's POPS folder also holds POPStarter's own files); returns their total size */
static guint64 list_vcds(void)
{
    GPtrArray *l = pfs_list(&tools, device, "__.POPS", "", NULL);
    guint64 total = 0;
    guint i;

    for (i = 0; l && i < l->len; i++) {
        PfsEntry *e = g_ptr_array_index(l, i);

        if (!e->is_dir && g_str_has_suffix(e->name, ".VCD")) {
            say("VCD\t%s\t%" G_GUINT64_FORMAT, e->name, e->size);
            total += e->size;
        }
    }
    if (l)
        g_ptr_array_free(l, TRUE);
    return total;
}

static void cmd_library(void)
{
    GError *err = NULL;
    GPtrArray *games = hdl_list_games(&tools, device, &err);
    guint64 total, used, freem;
    guint i;

    if (games == NULL) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    if (hdl_disk_space(&tools, device, &total, &used, &freem, NULL))
        say("SPACE\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT, total, used, freem);
    for (i = 0; i < games->len; i++) {
        HdlGame *g = g_ptr_array_index(games, i);

        say("GAME\t%s\t%" G_GUINT64_FORMAT "\t%s\t%s", g->media, g->size_kb, g->id, g->title);
    }
    g_ptr_array_free(games, TRUE);
    {
        guint64 size, fr, vcd_bytes;
        guint parts;

        if (pfs_space(&tools, device, "+OPL", &size, &fr, NULL))
            say("OPLSPACE\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT, size, fr);

        /*
         * __.POPS usually has sub-partitions, and pfsshell's df is wrong for those (it showed "1 GiB free"
         * on an empty 10 GB partition). The estimate: nominal size from the partition table minus the
         * files in it, minus a 3% margin for the filesystem. Real shortages are still caught when copying.
         */
        vcd_bytes = list_vcds();
        if (folder_is(device)) { /* a drive has one pool of space for everything */
            guint64 used_mb;

            if (folder_space(device, &size, &used_mb, &fr, NULL))
                say("POPSSPACE\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT, size, fr);
        } else if (hdl_partition_size(&tools, device, "__.POPS", &parts, &size, NULL)) {
            guint64 used = (vcd_bytes + 1024 * 1024 - 1) / (1024 * 1024), margin = size / 33;

            fr = size > used + margin ? size - used - margin : 0;
            say("POPSSPACE\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT, size, fr);
        }
    }
    list_dir("APP", "APPS", "", FALSE);
    list_dir("ART", "+OPL", "ART", FALSE);
    list_dir("CFG", "+OPL", "CFG", FALSE);
    list_dir("CHT", "+OPL", "CHT", FALSE);
    list_dir("VMC", "+OPL", "VMC", FALSE);
    list_dir("POPSDIR", "__common", "POPS", TRUE);
    say("END");
}

static gboolean progress_cb(guint64 done, guint64 total, const char *what, gpointer ud)
{
    (void)ud;
    say("PROGRESS\t%d\t%s", total ? (int)(done * 100 / total) : 0, what);
    return TRUE;
}

static void cmd_install_ps2(char **f, guint n)
{
    GError *err = NULL;

    if (n != 5 || (g_strcmp0(f[1], "CD") && g_strcmp0(f[1], "DVD"))) {
        fail("INSTALL_PS2: bad arguments");
        return;
    }
    if (!g_file_test(f[3], G_FILE_TEST_IS_REGULAR)) {
        fail("INSTALL_PS2: the ISO file does not exist");
        return;
    }
    if (!hdl_install(&tools, device, f[1], f[2], f[3], f[4], progress_cb, NULL, &err)) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    say("END");
}

static void cmd_remove_ps2(char **f, guint n)
{
    GError *err = NULL;

    if (n != 3) {
        fail("REMOVE_PS2: bad arguments");
        return;
    }
    if (!hdl_remove_game(&tools, device, f[1], f[2], &err)) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    say("END");
}

static void cmd_put(char **f, guint n)
{
    GError *err = NULL;

    guint i;

    if (n < 4 || !location_allowed(f[1], f[2])) {
        fail("PUT: that folder is not allowed");
        return;
    }
    if (!g_strcmp0(f[1], "+OPL") && !g_strcmp0(f[2], "VMC")) /* memory cards only: .bin by name and by content */
        for (i = 3; i < n; i++) {
            gchar *base = g_path_get_basename(f[i]);
            gboolean good = is_vmc_name(base) && looks_like_vmc(f[i]);

            g_free(base);
            if (!good) {
                fail("PUT: VMC takes memory card images (.bin) only");
                return;
            }
        }
    if (!g_strcmp0(f[1], "__common")) /* the game folders of POPStarter take settings files only */
        for (i = 3; i < n; i++) {
            gchar *base = g_path_get_basename(f[i]);
            gboolean good = pops_file_ok(base);

            g_free(base);
            if (!good) {
                fail("PUT: a PS1 game folder takes only VMCDIR.TXT, DISCS.TXT, CHEATS.TXT and TROJAN_#.BIN / PATCH_#.BIN files");
                return;
            }
        }
    if (!g_strcmp0(f[1], "APPS")) /* programs only: .elf by name and by content */
        for (i = 3; i < n; i++) {
            gchar *base = g_path_get_basename(f[i]);
            gboolean good = ps2gm_is_elf_name(base) && ps2gm_elf_check(f[i], NULL);

            g_free(base);
            if (!good) {
                fail("PUT: APPS takes PS2 programs (.elf) only");
                return;
            }
        }
    if (!pfs_put(&tools, device, f[1], f[2], &f[3], &err)) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    say("END");
}

static char *art_tmp; /* folder for GET, created on first use */

static void forget_art_tmp(void)
{
    GDir *d;
    const char *name;

    if (art_tmp == NULL)
        return;
    d = g_dir_open(art_tmp, 0, NULL);
    while (d && (name = g_dir_read_name(d)) != NULL) {
        gchar *p = g_build_filename(art_tmp, name, NULL);

        g_unlink(p);
        g_free(p);
    }
    if (d)
        g_dir_close(d);
}

/* hands a file or folder to the user who started pkexec, so the application can read and delete it */
static void give_to_caller(const char *path)
{
    const char *uid = g_getenv("PKEXEC_UID");

    if (geteuid() == 0 && uid != NULL && *uid != '\0') {
        if (chown(path, (uid_t)g_ascii_strtoull(uid, NULL, 10), (gid_t)-1) != 0) {
            /* best effort: the files are readable anyway */
        }
    }
}

/* copies the named files of one folder into the helper's temporary folder and says where they are (GOT lines) */
static void get_into_tmp(const char *partition, const char *dir, char **names, guint count, gboolean missing_folder_ok)
{
    GError *err = NULL;
    guint i;

    if (art_tmp == NULL) {
        art_tmp = g_build_filename(g_get_tmp_dir(), "ps2gm-art-XXXXXX", NULL);
        if (g_mkdtemp_full(art_tmp, 0700) == NULL) {
            fail("GET: cannot create a temporary folder");
            g_clear_pointer(&art_tmp, g_free);
            return;
        }
        give_to_caller(art_tmp);
    }
    forget_art_tmp();
    if (!pfs_get(&tools, device, partition, dir, names, art_tmp, &err)) {
        if (missing_folder_ok && g_error_matches(err, PS2GM_ERROR, PS2GM_ERROR_IO)) { /* a game that has no folder yet has no file */
            g_clear_error(&err);
            say("END");
            return;
        }
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    for (i = 0; i < count; i++) {
        gchar *p = g_build_filename(art_tmp, names[i], NULL);

        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
            give_to_caller(p);
            say("GOT\t%s\t%s", names[i], p);
        }
        g_free(p);
    }
    say("END");
}

/* LIST_POPS <game folder>: the fix files (TROJAN_#.BIN, PATCH_#.BIN) in the game's folder, as FIXFILE lines */
static void cmd_list_pops(char **f, guint n)
{
    char *dir;
    GPtrArray *l;
    guint i;

    if (n != 2 || !ps2gm_arg_ok(f[1]) || strchr(f[1], '/')) {
        fail("LIST_POPS: one game folder");
        return;
    }
    dir = g_strdup_printf("POPS/%s", f[1]);
    l = pfs_list(&tools, device, "__common", dir, NULL);
    for (i = 0; l && i < l->len; i++) {
        PfsEntry *e = g_ptr_array_index(l, i);

        if (!e->is_dir && ps1fix_is_fix_file_name(e->name))
            say("FIXFILE\t%s\t%" G_GUINT64_FORMAT, e->name, e->size);
    }
    if (l)
        g_ptr_array_free(l, TRUE);
    g_free(dir);
    say("END");
}

/* REMOVE_POPS_DIR <game folder>: removes the folder of a PS1 game in __common/POPS, but only if it holds nothing but what Memento and
 * POPStarter put there (the memory cards and the settings files); anything else and it is left alone */
static void cmd_remove_pops_dir(char **f, guint n)
{
    GError *err = NULL;
    char *dir;
    GPtrArray *l;
    guint i;

    if (n != 2 || !ps2gm_arg_ok(f[1]) || strchr(f[1], '/')) {
        fail("REMOVE_POPS_DIR: one game folder");
        return;
    }
    dir = g_strdup_printf("POPS/%s", f[1]);
    l = pfs_list(&tools, device, "__common", dir, NULL);
    if (l == NULL) { /* there is no such folder: nothing to do */
        g_free(dir);
        say("END");
        return;
    }
    for (i = 0; i < l->len; i++) {
        PfsEntry *e = g_ptr_array_index(l, i);

        if (e->is_dir || !(pops_file_ok(e->name) || !strcmp(e->name, "SLOT0.VMC") || !strcmp(e->name, "SLOT1.VMC"))) {
            fail("REMOVE_POPS_DIR: the folder holds other files, so it was left alone");
            goto out;
        }
    }
    for (i = 0; i < l->len; i++)
        if (!pfs_remove(&tools, device, "__common", dir, ((PfsEntry *)g_ptr_array_index(l, i))->name, &err)) {
            fail(err->message);
            g_clear_error(&err);
            goto out;
        }
    if (!pfs_rmdir(&tools, device, "__common", "POPS", f[1], &err)) {
        fail(err->message);
        g_clear_error(&err);
        goto out;
    }
    say("END");
out:
    g_ptr_array_free(l, TRUE);
    g_free(dir);
}

/* REMOVE_LAUNCHER <VCD name without .VCD>: the launcher file POPStarter made for a game, +OPL/POPS/<name>.ELF */
static void cmd_remove_launcher(char **f, guint n)
{
    GError *err = NULL;
    char *file;

    if (n != 2 || !ps2gm_arg_ok(f[1]) || strchr(f[1], '/') || !g_ascii_strcasecmp(f[1], "POPSTARTER") || !g_ascii_strcasecmp(f[1], "POPS")) {
        fail("REMOVE_LAUNCHER: the name of one game");
        return;
    }
    file = g_strdup_printf("%s.ELF", f[1]);
    {
        /* a game that was never started has no launcher file (and maybe no folder for it): nothing to do */
        GPtrArray *l = pfs_list(&tools, device, "+OPL", "POPS", NULL);
        gboolean there = FALSE;
        guint i;

        for (i = 0; l && i < l->len; i++)
            if (!strcmp(((PfsEntry *)g_ptr_array_index(l, i))->name, file))
                there = TRUE;
        if (l)
            g_ptr_array_free(l, TRUE);
        if (!there)
            say("END");
        else if (!pfs_remove(&tools, device, "+OPL", "POPS", file, &err)) {
            fail(err->message);
            g_clear_error(&err);
        } else
            say("END");
    }
    g_free(file);
}

/* GET_POPS <game folder> CHEATS.TXT: the fixes file of a PS1 game, in __common/POPS/<folder> */
static void cmd_get_pops(char **f, guint n)
{
    char *dir;
    char *names[2];

    if (n != 3 || !ps2gm_arg_ok(f[1]) || strchr(f[1], '/') ||
        (g_strcmp0(f[2], "CHEATS.TXT") != 0 && g_strcmp0(f[2], "VMCDIR.TXT") != 0 && g_strcmp0(f[2], "DISCS.TXT") != 0)) {
        fail("GET_POPS: only CHEATS.TXT, VMCDIR.TXT or DISCS.TXT of one game folder");
        return;
    }
    dir = g_strdup_printf("POPS/%s", f[1]);
    names[0] = f[2];
    names[1] = NULL;
    get_into_tmp("__common", dir, names, 1, TRUE);
    g_free(dir);
}

static void cmd_get(char **f, guint n)
{
    guint i;
    const char *suffix = NULL;

    if (n < 3) {
        fail("GET: no files named");
        return;
    }
    if (!g_strcmp0(f[1], "ART"))
        suffix = ".png";
    else if (!g_strcmp0(f[1], "CFG"))
        suffix = ".cfg";
    else if (!g_strcmp0(f[1], "CHT"))
        suffix = ".cht";
    else if (!g_strcmp0(f[1], "VMC"))
        suffix = ".bin";
    if (suffix == NULL) {
        fail("GET: the folder must be ART, CFG, CHT or VMC");
        return;
    }
    for (i = 2; i < n; i++)
        if (!ps2gm_arg_ok(f[i]) || strchr(f[i], '/') || !g_str_has_suffix(f[i], suffix)) {
            fail("GET: only files of that folder with the matching ending");
            return;
        }
    get_into_tmp("+OPL", f[1], &f[2], n - 2, FALSE);
}

static void cmd_rename_file(char **f, guint n)
{
    GError *err = NULL;

    gboolean apps = n == 5 && !g_strcmp0(f[1], "APPS") && ps2gm_is_elf_name(f[3]) && ps2gm_is_elf_name(f[4]);
    gboolean vmc = n == 5 && !g_strcmp0(f[1], "+OPL") && !g_strcmp0(f[2], "VMC") && is_vmc_name(f[3]) && is_vmc_name(f[4]);

    if (!apps && !vmc) {
        fail("RENAME_FILE: only .elf programs of APPS, or memory cards (.bin) of VMC, to another name of the same kind");
        return;
    }
    if (!pfs_rename(&tools, device, f[1], f[2], f[3], f[4], &err)) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    say("END");
}

static void cmd_remove_file(char **f, guint n)
{
    GError *err = NULL;

    if (n != 4 || !location_allowed(f[1], f[2])) {
        fail("REMOVE_FILE: that folder is not allowed");
        return;
    }
    if (!g_strcmp0(f[1], "APPS") && !ps2gm_is_elf_name(f[3])) {
        fail("REMOVE_FILE: APPS: only .elf programs can be removed");
        return;
    }
    if (!g_strcmp0(f[1], "__common") && !pops_file_ok(f[3])) {
        fail("REMOVE_FILE: a PS1 game folder: only its settings files can be removed");
        return;
    }
    if (!g_strcmp0(f[2], "VMC") && !is_vmc_name(f[3])) {
        fail("REMOVE_FILE: VMC: only memory cards (.bin) can be removed");
        return;
    }
    if (!pfs_remove(&tools, device, f[1], f[2], f[3], &err)) {
        fail(err->message);
        g_clear_error(&err);
        return;
    }
    say("END");
}

/*
 * As root, run only files that nobody else can replace: this program and the two disk tools next to it. A copy installed in
 * somebody's home folder would let anything running as that user become root through the password window.
 */
static gboolean root_trusts_install(GError **error)
{
    const char *files[3];
    char *self = g_file_read_link("/proc/self/exe", NULL), *why = NULL;
    guint i;

    if (geteuid() != 0)
        return TRUE; /* started by the user with access to the disk: nothing is gained by tampering */
    files[0] = self;
    files[1] = tools.hdl_dump;
    files[2] = tools.pfsshell;
    for (i = 0; i < 3; i++)
        if (files[i] == NULL || !ps2gm_path_root_safe(files[i], &why)) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_REFUSED,
                        _("For your safety the disk helper will not run as administrator from files that a normal user can change (%s). "
                        "Install Memento for all users (your distribution's package, or scripts/install.sh --system), or give your user "
                        "access to the disk with setfacl instead."),
                        why ? why : "?");
            g_free(why);
            g_free(self);
            return FALSE;
        }
    g_free(self);
    return TRUE;
}

int main(int argc, char **argv)
{
    GError *err = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <device>   (commands on stdin)\n", argv[0]);
        return 2;
    }
    device = argv[1];
    if (!ps2gm_tools_init(&tools, NULL, &err) || !root_trusts_install(&err) || !device_is_ps2_disk(device, &err)) {
        say("ERR\t%s", err->message);
        return 1;
    }
    say("READY");

    while ((len = getline(&line, &cap, stdin)) >= 0) {
        gchar **f;
        guint n;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        f = g_strsplit(line, "\t", -1);
        n = g_strv_length(f);
        if (!strcmp(f[0], "QUIT")) {
            g_strfreev(f);
            say("END");
            break;
        } else if (!strcmp(f[0], "LIBRARY"))
            cmd_library();
        else if (!strcmp(f[0], "INSTALL_PS2"))
            cmd_install_ps2(f, n);
        else if (!strcmp(f[0], "REMOVE_PS2"))
            cmd_remove_ps2(f, n);
        else if (!strcmp(f[0], "PUT"))
            cmd_put(f, n);
        else if (!strcmp(f[0], "REMOVE_FILE"))
            cmd_remove_file(f, n);
        else if (!strcmp(f[0], "RENAME_FILE"))
            cmd_rename_file(f, n);
        else if (!strcmp(f[0], "GET"))
            cmd_get(f, n);
        else if (!strcmp(f[0], "GET_POPS"))
            cmd_get_pops(f, n);
        else if (!strcmp(f[0], "LIST_POPS"))
            cmd_list_pops(f, n);
        else if (!strcmp(f[0], "REMOVE_POPS_DIR"))
            cmd_remove_pops_dir(f, n);
        else if (!strcmp(f[0], "REMOVE_LAUNCHER"))
            cmd_remove_launcher(f, n);
        else
            fail("unknown command");
        g_strfreev(f);
    }
    free(line);
    forget_art_tmp();
    if (art_tmp != NULL)
        g_rmdir(art_tmp);
    return 0;
}
