/*
 * Folder mode: the "drive" is a mounted folder laid out like an OPL drive (an SD card in an MX4SIO adapter, a USB stick): games in
 * DVD/ and CD/, ART/ CFG/ CHT/ VMC/, PS1 images in POPS/. The same workflows as on the PFS disk, on plain files, without any
 * administrator rights. Needs PS2GM_TEST_HELPER, PS2GM_TEST_ISO (SLES_999.91), PS2GM_TEST_PS1 (disc1.cue ... ), PS2GM_TEST_TOOLS.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "mcfs.h"
#include "ops.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static gboolean has_file(const char *root, const char *a, const char *b)
{
    char *p = g_build_filename(root, a, b, NULL);
    gboolean yes = g_file_test(p, G_FILE_TEST_IS_REGULAR);

    g_free(p);
    return yes;
}

int main(void)
{
    char *root = g_dir_make_tmp("folder-drive-XXXXXX", NULL), *text = NULL;
    GError *err = NULL;
    Backend *b;
    OpsContext *ops;
    Library *lib;
    GPtrArray *notes = g_ptr_array_new_with_free_func(g_free);
    const char *ps1 = g_getenv("PS2GM_TEST_PS1");

    printf("== connecting to a folder\n");
    b = backend_start(root, FALSE, g_getenv("PS2GM_TEST_HELPER"), &err); /* no administrator rights: it is a folder */
    if (b == NULL) {
        printf("%s\n", err->message);
        return 1;
    }
    ops = ops_new(b, NULL);
    lib = backend_library(b, &err);
    CHECK(lib != NULL && lib->games->len == 0 && lib->vcds->len == 0 && lib->total_mb > 0 && lib->free_mb > 0);
    library_free(lib);

    printf("== PS2 game\n");
    CHECK(ops_install_ps2(ops, g_getenv("PS2GM_TEST_ISO"), "CD", 3298, "SLES_999.91", "Folder Test Game", FALSE, notes, NULL, NULL, &err));
    if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
    CHECK(has_file(root, "CD", "SLES_999.91.Folder Test Game.iso")); /* the name OPL expects */
    lib = backend_library(b, &err);
    CHECK(lib && lib->games->len == 1 && !g_strcmp0(((LibGame *)g_ptr_array_index(lib->games, 0))->id, "SLES_999.91") &&
          !g_strcmp0(((LibGame *)g_ptr_array_index(lib->games, 0))->title, "Folder Test Game") &&
          !g_strcmp0(((LibGame *)g_ptr_array_index(lib->games, 0))->media, "CD"));
    library_free(lib);
    CHECK(!ops_install_ps2(ops, g_getenv("PS2GM_TEST_ISO"), "CD", 3298, "SLES_999.91", "Folder Test Game", FALSE, NULL, NULL, NULL, &err));
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    {
        char *part = g_build_filename(root, "CD", "SLES_999.91.Folder Test Game.iso.part", NULL);

        CHECK(!g_file_test(part, G_FILE_TEST_EXISTS)); /* no half written file is left behind */
        g_free(part);
    }

    printf("== settings and cheats (CFG, CHT)\n");
    {
        char *cheats = g_strdup("Infinite Health\n10B8DAFA 00003F00\n");
        CheatInfo ci;
        char *cfg = NULL;

        CHECK(ops_set_cheats(ops, "SLES_999.91", cheats, TRUE, &ci, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(has_file(root, "CHT", "SLES_999.91.cht") && has_file(root, "CFG", "SLES_999.91.cfg"));
        CHECK(ops_get_cheats(ops, "SLES_999.91", &text, &err) && text && strstr(text, "Infinite Health"));
        g_free(text);
        text = NULL;
        CHECK(backend_read_text(b, "CFG", "SLES_999.91.cfg", &cfg, &err) && cfg && strstr(cfg, "$EnableCheat=1"));
        g_free(cfg);
        g_free(cheats);
    }

    printf("== memory card (VMC)\n");
    {
        char *card = g_build_filename(root, "blank.bin", NULL), *back = g_build_filename(root, "back.bin", NULL);
        char *make[] = {"python3", "tests/make_card.py", "card", card, "8", NULL};
        gint st = 0;

        if (g_spawn_sync(NULL, make, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, NULL) && st == 0) {
            CHECK(backend_run(b, (char *[]){"PUT", "+OPL", "VMC", card, NULL}, NULL, NULL, &err));
            g_clear_error(&err);
            CHECK(has_file(root, "VMC", "blank.bin"));
            lib = backend_library(b, &err);
            CHECK(lib && lib->vmcs->len == 1);
            library_free(lib);
            CHECK(ops_vmc_fetch(ops, "blank.bin", back, &err));
            g_clear_error(&err);
            CHECK(ops_vmc_store(ops, "blank.bin", back, &err)); /* the safe replace works on a folder too */
            if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
            CHECK(has_file(root, "VMC", "blank.bin") && !has_file(root, "VMC", "blank.ps2gm-new.bin"));
        } else
            printf("   (skipped: no python3 or tests/make_card.py here)\n");
        g_free(card);
        g_free(back);
    }

    printf("== PS1 game (POPS/)\n");
    {
        GPtrArray *d = g_ptr_array_new();
        Ps1Disc *disc = g_new0(Ps1Disc, 1);
        char *fake = g_build_filename(root, "POPS", "POPS.ELF", NULL);

        disc->cue_path = g_build_filename(ps1, "disc1.cue", NULL);
        disc->id = g_strdup("SLES_026.98");
        g_ptr_array_add(d, disc);
        g_mkdir_with_parents(g_path_get_dirname(fake), 0755);
        g_file_set_contents(fake, "not a game", -1, NULL); /* POPStarter's own files share the folder */
        CHECK(ops_install_ps1(ops, d, "Folder PS1 Game", FALSE, notes, NULL, NULL, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(has_file(root, "POPS", "SLES_026.98.Folder PS1 Game.VCD"));
        lib = backend_library(b, &err);
        CHECK(lib && lib->vcds->len == 1); /* POPS.ELF is not a game */
        library_free(lib);
        CHECK(ops_set_ps1_fixes(ops, "SLES_026.98.Folder PS1 Game.VCD", "$CACHE1\r\n", &err)); /* the game's own folder in POPS/ */
        CHECK(has_file(root, "POPS/SLES_026.98.Folder PS1 Game", "CHEATS.TXT"));
        CHECK(ops_remove_ps1(ops, "SLES_026.98.Folder PS1 Game.VCD", TRUE, notes, &err));
        CHECK(!has_file(root, "POPS", "SLES_026.98.Folder PS1 Game.VCD"));
        CHECK(!g_file_test(g_build_filename(root, "POPS", "SLES_026.98.Folder PS1 Game", NULL), G_FILE_TEST_EXISTS)); /* the folder is gone too */
        CHECK(has_file(root, "POPS", "POPS.ELF")); /* and POPStarter's files are untouched */
    }

    printf("== remove and refusals\n");
    CHECK(ops_remove_ps2(ops, "SLES_999.91", "Folder Test Game", &err));
    CHECK(!has_file(root, "CD", "SLES_999.91.Folder Test Game.iso"));
    CHECK(!ops_remove_ps2(ops, "SLES_999.91", "Folder Test Game", &err)); /* not there any more */
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"PUT", "+OPL", "../evil", "/etc/hostname", NULL}, NULL, NULL, &err)); /* paths cannot leave the drive */
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"PUT", "+OPL", "DVD", "/etc/hostname", NULL}, NULL, NULL, &err)); /* the helper allows the same places as on the disk */
    g_clear_error(&err);
    backend_stop(b);
    {
        char *ro = g_dir_make_tmp("folder-ro-XXXXXX", NULL);

        g_chmod(ro, 0555);
        if (access(ro, W_OK) != 0) { /* not root: a folder that cannot be written is refused */
            Backend *b2 = backend_start(ro, FALSE, g_getenv("PS2GM_TEST_HELPER"), &err);

            CHECK(b2 == NULL && err != NULL);
            printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
        }
        g_chmod(ro, 0755);
    }
    ops_free(ops);
    printf(failures ? "\n%d FAILURES\n" : "\nall ok\n", failures);
    return failures != 0;
}
