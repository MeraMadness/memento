/*
 * End-to-end workflows against the test disk (uses the network for art):
 * PS2GM_TEST_DISK, PS2GM_TEST_HELPER, PS2GM_TEST_ISO (SLES_999.91), PS2GM_TEST_PS1 (folder with disc1.cue/disc2.cue)
 */
#include <stdio.h>
#include <string.h>
#include "ops.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include "tools.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static gboolean progress(guint64 done, guint64 total, const char *what, gpointer ud)
{
    guint *last = ud;
    if (done / 250 != *last) {
        *last = done / 250;
        printf("   [%3lu%%] %s\n", (gulong)(done * 100 / total), what);
    }
    return TRUE;
}

static void print_notes(GPtrArray *n)
{
    guint i;
    for (i = 0; i < n->len; i++)
        printf("   note: %s\n", (char *)g_ptr_array_index(n, i));
}

static Ps1Disc *disc(const char *dir, const char *cue, const char *id)
{
    Ps1Disc *d = g_new0(Ps1Disc, 1);
    d->cue_path = g_build_filename(dir, cue, NULL);
    d->id = g_strdup(id);
    return d;
}

int main(void)
{
    GError *err = NULL;
    Backend *b = backend_start(g_getenv("PS2GM_TEST_DISK"), FALSE, g_getenv("PS2GM_TEST_HELPER"), &err);
    OpsContext *ops;
    GPtrArray *notes = g_ptr_array_new_with_free_func(g_free);
    Library *lib;
    const char *ps1 = g_getenv("PS2GM_TEST_PS1");
    guint last = 99;
    gchar *clean;

    if (!b) { printf("%s\n", err->message); return 1; }
    ops = ops_new(b, NULL);

    clean = ops_clean_title("  Resident Evil: 3 / Nemesis?  ");
    CHECK(!g_strcmp0(clean, "Resident Evil 3 Nemesis"));
    g_free(clean);
    clean = ops_clean_title("Ōkami \"special\" edition");
    CHECK(!g_strcmp0(clean, "Ōkami special edition"));
    g_free(clean);

    printf("== PS2 install\n");
    CHECK(ops_install_ps2(ops, g_getenv("PS2GM_TEST_ISO"), "?", 3298, "SLES_999.91", "Ops Test: PS2 Game", TRUE, notes, progress, &last, &err));
    if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
    print_notes(notes);
    lib = backend_library(b, &err);
    CHECK(lib && lib->games->len == 1 && !g_strcmp0(((LibGame *)g_ptr_array_index(lib->games, 0))->title, "Ops Test PS2 Game"));
    CHECK(lib && g_hash_table_contains(lib->cfgs, "SLES_999.91.cfg"));
    library_free(lib);
    CHECK(!ops_install_ps2(ops, g_getenv("PS2GM_TEST_ISO"), "CD", 3298, "SLES_999.91", "Again", FALSE, NULL, NULL, NULL, &err));
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);

    printf("== PS1 install (one disc, real game ID so the art is downloaded)\n");
    {
        GPtrArray *d = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(d, disc(ps1, "disc1.cue", "SLES_026.98"));
        g_ptr_array_set_free_func(d, NULL);
        g_ptr_array_set_size(d, 1);
        CHECK(ops_install_ps1(ops, d, "Resident Evil 3: Nemesis", TRUE, notes, progress, &last, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        print_notes(notes);
        lib = backend_library(b, &err);
        CHECK(lib && lib->vcds->len == 1 && !g_strcmp0(((LibFile *)g_ptr_array_index(lib->vcds, 0))->name, "SLES_026.98.Resident Evil 3 Nemesis.VCD"));
        CHECK(lib && library_has_art(lib, "SLES_026.98"));
        CHECK(lib && g_hash_table_contains(lib->art, "SLES_026.98_BG.png"));
        printf("   __.POPS free after: %lu MB, +OPL free: %lu MB\n", (gulong)lib->pops_free_mb, (gulong)lib->opl_free_mb);
        library_free(lib);
        CHECK(!ops_install_ps1(ops, d, "Resident Evil 3: Nemesis", FALSE, NULL, NULL, NULL, &err));
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
    }

    printf("== PS1 install (two discs -> DISCS.TXT and a shared memory card)\n");
    {
        GPtrArray *d = g_ptr_array_new();
        g_ptr_array_add(d, disc(ps1, "disc1.cue", "SLES_999.93"));
        g_ptr_array_add(d, disc(ps1, "disc2.cue", "SLES_999.94"));
        CHECK(ops_install_ps1(ops, d, "Two Disc Game", FALSE, notes, progress, &last, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        lib = backend_library(b, &err);
        CHECK(lib && lib->vcds->len == 3);
        CHECK(lib && g_hash_table_contains(lib->popsdirs, "SLES_999.93.Two Disc Game (Disc 1)"));
        CHECK(lib && g_hash_table_contains(lib->popsdirs, "SLES_999.94.Two Disc Game (Disc 2)"));
        library_free(lib);

        /* every disc folder: DISCS.TXT, and VMCDIR.TXT naming the folder of disc 1 (one shared memory card) */
        {
            Ps2gmTools t;
            const char *folders[] = {"POPS/SLES_999.93.Two Disc Game (Disc 1)", "POPS/SLES_999.94.Two Disc Game (Disc 2)"};
            int f;

            CHECK(ps2gm_tools_init(&t, g_getenv("PS2GM_TEST_TOOLS"), &err));
            for (f = 0; f < 2; f++) {
                GPtrArray *l = pfs_list(&t, g_getenv("PS2GM_TEST_DISK"), "__common", folders[f], &err);
                guint k;
                gboolean has_discs = FALSE, vmc_ok = FALSE;

                for (k = 0; l && k < l->len; k++) {
                    PfsEntry *e = g_ptr_array_index(l, k);

                    if (!strcmp(e->name, "DISCS.TXT"))
                        has_discs = TRUE;
                    if (!strcmp(e->name, "VMCDIR.TXT"))
                        vmc_ok = e->size == strlen("SLES_999.93.Two Disc Game (Disc 1)");
                }
                CHECK(has_discs);
                CHECK(vmc_ok);
                if (l)
                    g_ptr_array_free(l, TRUE);
            }
        }
    }

    printf("== share the memory card of discs that were installed without it\n");
    {
        Ps2gmTools t;
        char *discs[] = {"SLES_999.93.Two Disc Game (Disc 1).VCD", "SLES_999.94.Two Disc Game (Disc 2).VCD", NULL};
        char *one[] = {"SLES_999.93.Two Disc Game (Disc 1).VCD", NULL};
        GPtrArray *l;
        guint k;
        gboolean has = FALSE;

        CHECK(ps2gm_tools_init(&t, g_getenv("PS2GM_TEST_TOOLS"), &err));
        {
            char *rm[] = {"REMOVE_FILE", "__common", "POPS/SLES_999.94.Two Disc Game (Disc 2)", "VMCDIR.TXT", NULL};
            CHECK(backend_run(b, rm, NULL, NULL, &err));
        }
        CHECK(ops_share_memcard(ops, discs, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        l = pfs_list(&t, g_getenv("PS2GM_TEST_DISK"), "__common", "POPS/SLES_999.94.Two Disc Game (Disc 2)", &err);
        for (k = 0; l && k < l->len; k++)
            if (!strcmp(((PfsEntry *)g_ptr_array_index(l, k))->name, "VMCDIR.TXT") &&
                ((PfsEntry *)g_ptr_array_index(l, k))->size == strlen("SLES_999.93.Two Disc Game (Disc 1)"))
                has = TRUE;
        CHECK(has);
        if (l)
            g_ptr_array_free(l, TRUE);
        CHECK(!ops_share_memcard(ops, one, &err)); /* one disc has nothing to share */
        g_clear_error(&err);
    }

    printf("== a disc that does not fit in __.POPS is refused up front\n");
    {
        GPtrArray *d = g_ptr_array_new();
        g_ptr_array_add(d, disc(ps1, "huge.cue", "SLES_999.95"));
        CHECK(!ops_install_ps1(ops, d, "Huge", FALSE, NULL, NULL, NULL, &err));
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        lib = backend_library(b, &err);
        CHECK(lib && lib->vcds->len == 3); /* nothing was left behind */
        library_free(lib);
    }

    printf("== update art\n");
    CHECK(ops_update_art(ops, "SLES_026.98", PS2GM_PS1, "Resident Evil 3", FALSE, notes, NULL, NULL, &err));
    g_clear_error(&err);

    printf("== choose a picture yourself, and download a single one again\n");
    {
        GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 900, 1200);
        gchar *mine = g_build_filename(g_get_tmp_dir(), "ps2gm-test-cover.png", NULL);

        gdk_pixbuf_fill(pb, 0xaa3355ff);
        gdk_pixbuf_save(pb, mine, "png", NULL, NULL);
        g_object_unref(pb);
        CHECK(ops_set_art(ops, "SLES_999.91", PS2GM_PS2, "COV", mine, FALSE, NULL, NULL, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        lib = backend_library(b, &err);
        CHECK(lib && library_has_art(lib, "SLES_999.91"));
        library_free(lib);
        CHECK(!ops_set_art(ops, "SLES_999.91", PS2GM_PS2, "COV", "/no/such/file.png", FALSE, NULL, NULL, &err));
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!ops_set_art(ops, "SLES_999.91", PS2GM_PS2, "../../x", mine, FALSE, NULL, NULL, &err));
        g_clear_error(&err);
        g_unlink(mine);
        g_free(mine);
        CHECK(ops_set_art(ops, "SLES_026.98", PS2GM_PS1, "COV2", NULL, FALSE, NULL, NULL, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
    }

    printf("== cover candidates for the carousel (network)\n");
    {
        gchar *dir = g_build_filename(g_get_tmp_dir(), "ps2gm-test-candidates", NULL);
        GPtrArray *c = art_cover_candidates("SLES_539.08", PS2GM_PS2, dir);
        guint k;
        gboolean has3d = FALSE;

        printf("   %u candidates\n", c->len);
        CHECK(c->len >= 2);
        for (k = 0; k < c->len; k++) {
            CoverCandidate *cc = g_ptr_array_index(c, k);
            gchar *data = NULL;
            gsize n = 0;

            printf("   %s -> %s\n", cc->label, cc->path);
            CHECK(g_file_get_contents(cc->path, &data, &n, NULL) && n > 1000);
            g_free(data);
            if (cc->whole)
                has3d = TRUE;
            g_unlink(cc->path);
        }
        CHECK(has3d);
        g_ptr_array_free(c, TRUE);
        c = art_cover_candidates("XXXX_000.00", PS2GM_PS1, dir); /* a game nobody has art for: no candidates, no error */
        CHECK(c->len == 0);
        g_ptr_array_free(c, TRUE);
        g_rmdir(dir);
        g_free(dir);
    }

    printf("== per-game OPL settings: save, read back, other keys kept, reset\n");
    {
        GameSettings g, back;
        char *cfg = NULL;

        /* the cheat part of the file must survive: switch cheats on first */
        CHECK(ops_set_cheats(ops, "SLES_999.91", "Infinite Health\n10B8DAFA 00003F00\n", TRUE, NULL, &err));
        g_clear_error(&err);
        CHECK(ops_get_settings(ops, "SLES_999.91", &g, &err));
        CHECK(g.compat == 0 && g.dma == GAME_DMA_DEFAULT);
        g.compat = 0x05;
        g.dma = 2;
        g.gsm_on = TRUE;
        g.gsm_vmode = 4;
        g_free(g.alt_startup);
        g.alt_startup = g_strdup("ALT.ELF");
        CHECK(ops_set_settings(ops, "SLES_999.91", &g, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(ops_get_settings(ops, "SLES_999.91", &back, &err));
        CHECK(back.compat == 5 && back.dma == 2 && back.gsm_on && back.gsm_vmode == 4 && !strcmp(back.alt_startup, "ALT.ELF"));
        CHECK(backend_read_text(b, "CFG", "SLES_999.91.cfg", &cfg, &err));
        CHECK(cfg && strstr(cfg, "Title=") && strstr(cfg, "$EnableCheat=1") && strstr(cfg, "$CheatsSource=1") && strstr(cfg, "$Compatibility=5"));
        g_free(cfg);
        game_settings_clear(&back);
        CHECK(ops_reset_settings(ops, "SLES_999.91", &err));
        CHECK(ops_get_settings(ops, "SLES_999.91", &back, &err));
        CHECK(back.compat == 0 && back.dma == GAME_DMA_DEFAULT && !back.gsm_on && !strcmp(back.alt_startup, ""));
        CHECK(backend_read_text(b, "CFG", "SLES_999.91.cfg", &cfg, &err));
        CHECK(cfg && strstr(cfg, "Title=") && strstr(cfg, "$EnableCheat=1") && !strstr(cfg, "$Compatibility") && !strstr(cfg, "$ConfigSource"));
        g_free(cfg);
        CHECK(!ops_get_settings(ops, "../evil", &back, &err));
        g_clear_error(&err);
        CHECK(ops_set_cheats(ops, "SLES_999.91", NULL, FALSE, NULL, &err)); /* leave the cheats as they were before this test */
        g_clear_error(&err);
        game_settings_clear(&g);
        game_settings_clear(&back);
    }

    printf("== programs on APPS: add, replace keeping the old one, rename, remove\n");
    {
        gchar *dir = g_dir_make_tmp("apps-test-XXXXXX", NULL), *e1 = g_build_filename(dir, "TOOL.ELF", NULL), *e2dir = g_build_filename(dir, "v3", NULL), *e2 = g_build_filename(e2dir, "TOOL.ELF", NULL);
        gchar *bad = g_build_filename(dir, "BAD.ELF", NULL), *e3 = g_build_filename(dir, "Tool2.elf", NULL);
        guint8 hdr[64] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 8, 0};
        Library *l;
        GPtrArray *nt = g_ptr_array_new_with_free_func(g_free);

        g_file_set_contents(e1, (char *)hdr, sizeof hdr, NULL);
        hdr[40] = 0x55; /* a different second version */
        g_mkdir_with_parents(e2dir, 0755);
        hdr[41] = 0x66; /* the third version */
        g_file_set_contents(e2, (char *)hdr, sizeof hdr, NULL);
        g_file_set_contents(bad, "this is not an ELF", -1, NULL);
        g_file_set_contents(e3, (char *)hdr, sizeof hdr, NULL);

        CHECK(!ops_apps_add(ops, bad, TRUE, nt, &err)); /* refused before anything is sent */
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(ops_apps_add(ops, e1, TRUE, nt, &err));
        l = backend_library(b, &err);
        CHECK(l && l->apps->len == 1 && !strcmp(((LibFile *)g_ptr_array_index(l->apps, 0))->name, "TOOL.ELF"));
        library_free(l);

        /* a new version with the same name: the old one stays as TOOL-OLD.ELF */
        {
            gchar *e1b = g_build_filename(dir, "sub", NULL), *newpath;

            g_mkdir_with_parents(e1b, 0755);
            newpath = g_build_filename(e1b, "TOOL.ELF", NULL);
            g_file_set_contents(newpath, (char *)hdr, sizeof hdr, NULL);
            CHECK(ops_apps_add(ops, newpath, TRUE, nt, &err));
            if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
            CHECK(nt->len >= 1 && strstr(g_ptr_array_index(nt, 0), "TOOL-OLD.ELF"));
            g_unlink(newpath);
            g_rmdir(e1b);
            g_free(newpath);
            g_free(e1b);
        }
        l = backend_library(b, &err);
        CHECK(l && l->apps->len == 2);
        library_free(l);
        /* applying it again replaces the old backup instead of failing */
        CHECK(ops_apps_add(ops, e2, TRUE, nt, &err));
        g_clear_error(&err);
        l = backend_library(b, &err);
        CHECK(l && l->apps->len == 2);
        library_free(l);

        CHECK(ops_apps_rename(ops, "TOOL-OLD.ELF", "Tool2.elf", &err));
        CHECK(!ops_apps_rename(ops, "TOOL.ELF", "Tool2.elf", &err)); /* the name is taken */
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!ops_apps_rename(ops, "TOOL.ELF", "notaprogram.txt", &err));
        g_clear_error(&err);
        CHECK(!ops_apps_remove(ops, "lang", &err));
        g_clear_error(&err);
        CHECK(ops_apps_remove(ops, "TOOL.ELF", &err));
        CHECK(ops_apps_remove(ops, "Tool2.elf", &err));
        l = backend_library(b, &err);
        CHECK(l && l->apps->len == 0);
        library_free(l);
        {
            gchar *bk = ops_apps_backup_name("OPL-PS1.ELF"), *bk2 = ops_apps_backup_name("tool.elf");

            CHECK(!strcmp(bk, "OPL-PS1-OLD.ELF") && !strcmp(bk2, "tool-OLD.elf"));
            g_free(bk);
            g_free(bk2);
        }
        /* a copy that does not fit: the old program must be back under its own name, nothing half written */
        {
            gchar *fat_dir = g_build_filename(dir, "fat", NULL), *a1 = g_build_filename(fat_dir, "A.ELF", NULL), *b1 = g_build_filename(dir, "B.ELF", NULL);
            gchar *a2 = g_build_filename(dir, "fat2", NULL), *a2f;
            guint8 *big = g_malloc0(55 * 1024 * 1024);
            gboolean fits;
            guint k;
            gboolean a_back = FALSE, no_old = TRUE;

            memcpy(big, hdr, 20);
            g_mkdir_with_parents(fat_dir, 0755);
            g_mkdir_with_parents(a2, 0755);
            a2f = g_build_filename(a2, "A.ELF", NULL);
            g_file_set_contents(a1, (char *)big, 55 * 1024 * 1024, NULL);
            g_file_set_contents(b1, (char *)big, 55 * 1024 * 1024, NULL);
            g_file_set_contents(a2f, (char *)big, 55 * 1024 * 1024, NULL);
            CHECK(ops_apps_add(ops, a1, TRUE, nt, &err));
            g_clear_error(&err);
            fits = ops_apps_add(ops, b1, TRUE, nt, &err);
            g_clear_error(&err);
            printf("   second 55 MB program %s\n", fits ? "fit" : "did not fit");
            g_ptr_array_set_size(nt, 0);
            if (fits) {
                CHECK(!ops_apps_add(ops, a2f, TRUE, nt, &err)); /* A.ELF again, while the disk is nearly full */
                printf("   expected: %s\n", err ? err->message : "(none)");
                g_clear_error(&err);
            }
            l = backend_library(b, &err);
            for (k = 0; l && k < l->apps->len; k++) {
                const char *nm = ((LibFile *)g_ptr_array_index(l->apps, k))->name;

                if (!strcmp(nm, "A.ELF"))
                    a_back = TRUE;
                if (!strcmp(nm, "A-OLD.ELF"))
                    no_old = FALSE;
            }
            CHECK(a_back && no_old); /* put back under its own name, and no stray backup left over */
            library_free(l);
            ops_apps_remove(ops, "A.ELF", NULL);
            ops_apps_remove(ops, "B.ELF", NULL);
            ops_apps_remove(ops, "A-OLD.ELF", NULL);
            g_unlink(a1); g_unlink(b1); g_unlink(a2f); g_rmdir(fat_dir); g_rmdir(a2);
            g_free(big); g_free(fat_dir); g_free(a1); g_free(b1); g_free(a2); g_free(a2f);
        }
        g_ptr_array_free(nt, TRUE);
        g_unlink(e1); g_unlink(e2); g_unlink(e3); g_unlink(bad); g_rmdir(e2dir); g_rmdir(dir);
        g_free(e1); g_free(e2); g_free(e2dir); g_free(e3); g_free(bad); g_free(dir);
    }

    printf("== cheats: save, read back, switch on, remove\n");
    {
        const char *cheats = "Infinite Health\n10B8DAFA 00003F00\nMaster Code\n90000000 00000000\n";
        char *text = NULL, *cfg = NULL;
        CheatInfo ci = {0, 0};

        CHECK(ops_get_cheats(ops, "SLES_999.91", &text, &err) && text == NULL); /* none yet */
        g_clear_error(&err);
        CHECK(!ops_set_cheats(ops, "SLES_999.91", "this is not a cheat list", TRUE, NULL, &err));
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(ops_set_cheats(ops, "SLES_999.91", cheats, TRUE, &ci, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(ci.cheats == 2 && ci.codes == 2);
        CHECK(ops_get_cheats(ops, "SLES_999.91", &text, &err) && text && !strcmp(text, cheats));
        g_free(text);
        CHECK(backend_read_text(b, "CFG", "SLES_999.91.cfg", &cfg, &err));
        CHECK(cfg && strstr(cfg, "$EnableCheat=1") && strstr(cfg, "$CheatsSource=1") && strstr(cfg, "Title=")); /* the Title line from the install is kept */
        g_free(cfg);
        CHECK(ops_set_cheats(ops, "SLES_999.91", NULL, FALSE, NULL, &err)); /* remove */
        text = NULL;
        CHECK(ops_get_cheats(ops, "SLES_999.91", &text, &err) && text == NULL);
        CHECK(backend_read_text(b, "CFG", "SLES_999.91.cfg", &cfg, &err) && cfg && strstr(cfg, "$EnableCheat=0") && strstr(cfg, "Title="));
        g_free(cfg);
        g_clear_error(&err);
    }

    printf("== remove\n");
    {
        GPtrArray *rn = g_ptr_array_new_with_free_func(g_free);
        char *t1 = NULL, *t2 = NULL;

        CHECK(ops_remove_ps1(ops, "SLES_026.98.Resident Evil 3 Nemesis.VCD", FALSE, rn, &err));
        {
            char *group[] = {"SLES_999.93.Two Disc Game (Disc 1).VCD", "SLES_999.94.Two Disc Game (Disc 2).VCD", NULL};

            CHECK(ops_share_memcard(ops, group, &err)); /* disc 2 now takes its card from disc 1's folder */
            g_clear_error(&err);
        }
        /* disc 1 with its folder: disc 2 takes its card from it (VMCDIR.TXT), so the folder must stay, with a note saying why */
        CHECK(backend_read_pops_file(b, "SLES_999.93.Two Disc Game (Disc 1)", "DISCS.TXT", &t1, &err) && t1 != NULL);
        g_free(t1);
        CHECK(ops_remove_ps1(ops, "SLES_999.93.Two Disc Game (Disc 1).VCD", TRUE, rn, &err));
        CHECK(rn->len == 1 && strstr(g_ptr_array_index(rn, 0), "another disc") != NULL);
        t1 = NULL;
        CHECK(backend_read_pops_file(b, "SLES_999.93.Two Disc Game (Disc 1)", "DISCS.TXT", &t1, &err) && t1 != NULL); /* still there */
        g_free(t1);
        /* disc 2 with its folder: nothing uses its folder, so it goes away */
        CHECK(ops_remove_ps1(ops, "SLES_999.94.Two Disc Game (Disc 2).VCD", TRUE, rn, &err));
        CHECK(backend_read_pops_file(b, "SLES_999.94.Two Disc Game (Disc 2)", "DISCS.TXT", &t2, &err) && t2 == NULL);
        /* now disc 1's folder is free: cleaning it up works (the VCD is already gone, which is fine) */
        CHECK(ops_remove_ps1(ops, "SLES_999.93.Two Disc Game (Disc 1).VCD", TRUE, rn, &err));
        t1 = NULL;
        CHECK(backend_read_pops_file(b, "SLES_999.93.Two Disc Game (Disc 1)", "DISCS.TXT", &t1, &err) && t1 == NULL);
        g_free(t1);
        CHECK(rn->len == 1); /* no further notes */
        /* the helper refuses folders with other files and the master launcher */
        CHECK(!backend_run(b, (char *[]){"REMOVE_LAUNCHER", "POPSTARTER", NULL}, NULL, NULL, &err));
        g_clear_error(&err);
        g_ptr_array_free(rn, TRUE);
    }
    CHECK(ops_remove_ps2(ops, "SLES_999.91", "Ops Test PS2 Game", &err));
    g_clear_error(&err);
    lib = backend_library(b, &err);
    CHECK(lib && lib->games->len == 0 && lib->vcds->len == 0);
    library_free(lib);

    backend_stop(b);
    ops_free(ops);
    printf(failures ? "\n%d FAILURES\n" : "\nall ok\n", failures);
    return failures != 0;
}
