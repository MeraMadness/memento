/*
 * Exercises the hdl_dump / pfsshell wrappers against a throw-away PS2 disk image (a plain file).
 *   PS2GM_TEST_DISK   image created by tests/make_disk.sh
 *   PS2GM_TEST_TOOLS  folder with hdl_dump (built with DEBUG so files count as disks) and pfsshell
 *   PS2GM_TEST_ISO    a small PS2 ISO (SYSTEM.CNF with BOOT2) and PS2GM_TEST_ISO2 a second one
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "art.h"
#include "cheats.h"
#include "gamecfg.h"
#include "tools.h"

static int failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                       \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static gboolean has_entry(GPtrArray *l, const char *name, gboolean dir)
{
    guint i;

    for (i = 0; l && i < l->len; i++) {
        PfsEntry *e = g_ptr_array_index(l, i);

        if (!strcmp(e->name, name) && e->is_dir == dir)
            return TRUE;
    }
    return FALSE;
}

int main(void)
{
    const char *disk = g_getenv("PS2GM_TEST_DISK"), *tools_dir = g_getenv("PS2GM_TEST_TOOLS");
    const char *iso = g_getenv("PS2GM_TEST_ISO"), *iso2 = g_getenv("PS2GM_TEST_ISO2");
    Ps2gmTools t;
    GError *err = NULL;
    GPtrArray *games, *list;
    HdlIsoInfo info;
    guint64 total, used, freem;
    gchar *tmp = g_dir_make_tmp("ps2gm-test-XXXXXX", NULL);

    if (!disk || !tools_dir || !iso || !iso2) {
        printf("set PS2GM_TEST_DISK / _TOOLS / _ISO / _ISO2\n");
        return 2;
    }
    {
        /* what root may trust: system files yes; anything a user can change no */
        char *why = NULL, *mine = g_build_filename(tmp, "tool", NULL);

        g_file_set_contents(mine, "x", 1, NULL);
        CHECK(ps2gm_path_root_safe("/usr/bin/env", &why));
        g_free(why);
        CHECK(!ps2gm_path_root_safe(mine, &why) && why != NULL && strstr(why, tmp) != NULL); /* my own file and folder */
        g_free(why);
        CHECK(!ps2gm_path_root_safe("/tmp", &why)); /* world writable */
        g_free(why);
        CHECK(!ps2gm_path_root_safe("/nonexistent/file", &why) && why != NULL);
        g_free(why);
        g_free(mine);
    }
    CHECK(ps2gm_tools_init(&t, tools_dir, &err));

    printf("== arguments are checked\n");
    CHECK(!ps2gm_arg_ok("a\"b"));
    CHECK(!ps2gm_arg_ok("line\nbreak"));
    CHECK(!ps2gm_arg_ok(".."));
    CHECK(ps2gm_arg_ok("SLES_026.98.Resident Evil 3.VCD"));
    CHECK(!ps2gm_remote_path_ok("../etc"));
    CHECK(!ps2gm_remote_path_ok("/abs"));
    CHECK(ps2gm_remote_path_ok("POPS/SLES_026.98.Resident Evil 3"));

    printf("== ISO info\n");
    CHECK(hdl_iso_info(&t, iso, &info, &err));
    CHECK(!g_strcmp0(info.id, "SLES_999.91"));
    CHECK(info.size_kb > 1000);
    hdl_iso_info_clear(&info);
    CHECK(!hdl_iso_info(&t, "/etc/hostname", &info, NULL));

    printf("== disk space and games\n");
    CHECK(hdl_disk_space(&t, disk, &total, &used, &freem, &err));
    printf("   total %lu MB, used %lu MB, free %lu MB\n", (gulong)total, (gulong)used, (gulong)freem);
    CHECK(total > 1000 && freem > 100 && used + freem <= total + 1);
    games = hdl_list_games(&t, disk, &err);
    CHECK(games != NULL);
    guint before = games ? games->len : 0;

    printf("== install a game\n");
    CHECK(hdl_iso_info(&t, iso2, &info, &err));
    CHECK(hdl_install(&t, disk, "CD", "Second Test Game", iso2, info.id, NULL, NULL, &err));
    if (err)
        printf("   %s\n", err->message);
    g_clear_error(&err);
    hdl_iso_info_clear(&info);
    g_ptr_array_free(games, TRUE);
    games = hdl_list_games(&t, disk, &err);
    CHECK(games && games->len == before + 1);
    {
        gboolean found = FALSE;
        guint i;

        for (i = 0; games && i < games->len; i++)
            if (!g_strcmp0(((HdlGame *)g_ptr_array_index(games, i))->title, "Second Test Game"))
                found = TRUE;
        CHECK(found);
    }
    CHECK(!hdl_install(&t, disk, "CD", "Bad \"title\"", iso2, "SLES_000.00", NULL, NULL, NULL));

    printf("== files with spaces in their names\n");
    {
        gchar *f1 = g_build_filename(tmp, "SLES_026.98.Resident Evil 3.VCD", NULL);
        gchar *f2 = g_build_filename(tmp, "DISCS.TXT", NULL);
        char *files[] = {f1, f2, NULL};
        char *just1[] = {f1, NULL};
        gchar *data = g_malloc0(3 * 1024 * 1024 + 17);

        g_file_set_contents(f1, data, 3 * 1024 * 1024 + 17, NULL);
        g_file_set_contents(f2, "disc1.vcd\ndisc2.vcd\n", -1, NULL);
        g_free(data);

        CHECK(pfs_put(&t, disk, "__.POPS", "", just1, &err));
        g_clear_error(&err);
        list = pfs_list(&t, disk, "__.POPS", "", &err);
        CHECK(has_entry(list, "SLES_026.98.Resident Evil 3.VCD", FALSE));
        if (list)
            g_ptr_array_free(list, TRUE);

        CHECK(pfs_put(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", files, &err));
        if (err)
            printf("   %s\n", err->message);
        g_clear_error(&err);
        list = pfs_list(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", &err);
        CHECK(has_entry(list, "DISCS.TXT", FALSE));
        CHECK(has_entry(list, "SLES_026.98.Resident Evil 3.VCD", FALSE));
        if (list)
            g_ptr_array_free(list, TRUE);

        printf("== replacing an existing file\n");
        g_file_set_contents(f2, "disc1.vcd\ndisc2.vcd\ndisc3.vcd\n", -1, NULL);
        CHECK(pfs_put(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", files, &err));
        g_clear_error(&err);
        list = pfs_list(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", &err);
        {
            guint i, n = 0;

            for (i = 0; list && i < list->len; i++)
                if (!strcmp(((PfsEntry *)g_ptr_array_index(list, i))->name, "DISCS.TXT"))
                    n++;
            CHECK(n == 1);
        }
        if (list)
            g_ptr_array_free(list, TRUE);

        printf("== a file that does not fit is reported\n");
        {
            gchar *big = g_build_filename(tmp, "BIG.BIN", NULL);
            char *bigl[] = {big, NULL};
            FILE *fp = fopen(big, "wb");

            fseek(fp, 700L * 1024 * 1024, SEEK_SET);
            fputc(0, fp);
            fclose(fp);
            /* +OPL is a 128 MB partition in the test disk */
            CHECK(!pfs_put(&t, disk, "+OPL", "ART", bigl, &err));
            printf("   expected error: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
            /* nothing of the failed copy may be left behind */
            list = pfs_list(&t, disk, "+OPL", "ART", &err);
            CHECK(list != NULL && !has_entry(list, "BIG.BIN", FALSE));
            if (list)
                g_ptr_array_free(list, TRUE);
            g_clear_error(&err);
            g_unlink(big);
            g_free(big);
        }

        printf("== remove\n");
        CHECK(pfs_remove(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", "DISCS.TXT", &err));
        g_clear_error(&err);
        list = pfs_list(&t, disk, "__common", "POPS/SLES_026.98.Resident Evil 3", &err);
        CHECK(!has_entry(list, "DISCS.TXT", FALSE));
        CHECK(has_entry(list, "SLES_026.98.Resident Evil 3.VCD", FALSE));
        if (list)
            g_ptr_array_free(list, TRUE);
        g_free(f1);
        g_free(f2);
    }

    printf("== a folder with hundreds of files (long listings must not hide entries)\n");
    {
        int batch, n, bad = 0;

        for (batch = 0; batch < 12; batch++) {
            char *files[26];
            gchar *names[25];

            for (n = 0; n < 25; n++) {
                gchar *name = g_strdup_printf("BATCH%02d_%03d.png", batch, n);
                gchar *data = g_malloc0(300 + n);

                names[n] = g_build_filename(tmp, name, NULL);
                g_file_set_contents(names[n], data, 300 + n, NULL);
                files[n] = names[n];
                g_free(data);
                g_free(name);
            }
            files[25] = NULL;
            if (!pfs_put(&t, disk, "+OPL", "ART", files, &err)) {
                bad++;
                printf("   batch %d: %s\n", batch, err->message);
                g_clear_error(&err);
            }
            for (n = 0; n < 25; n++)
                g_free(names[n]);
        }
        CHECK(bad == 0);
        list = pfs_list(&t, disk, "+OPL", "ART", &err);
        CHECK(list != NULL && list->len == 300);
        printf("   listed %u entries\n", list ? list->len : 0);
        if (list)
            g_ptr_array_free(list, TRUE);
    }

    printf("== your own pictures (no network)\n");
    {
        struct { int w, h; const char *key; Ps2gmPlatform pf; int want_w, want_h; } cases[] = {
            {1000, 1500, "COV", PS2GM_PS2, 140, 200}, /* cropped to 7:10, scaled */
            {3000, 1000, "COV", PS2GM_PS2, 140, 200},
            {800, 600, "COV", PS2GM_PS1, 200, 150},   /* PS1: fits in 200x200, not stretched */
            {2000, 1000, "BG", PS2GM_PS2, 640, 320},
            {50, 50, "BG", PS2GM_PS2, 50, 50},        /* never enlarged */
        };
        gchar *outdir = g_build_filename(tmp, "artout", NULL);
        guint k;

        for (k = 0; k < G_N_ELEMENTS(cases); k++) {
            GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, cases[k].w, cases[k].h);
            gchar *src = g_build_filename(tmp, "mine.png", NULL), *name = NULL, *dst;
            int w = 0, h = 0;

            gdk_pixbuf_fill(pb, 0x336699ff);
            gdk_pixbuf_save(pb, src, "png", NULL, NULL);
            g_object_unref(pb);
            CHECK(art_import_file(src, "SLES_123.45", cases[k].key, cases[k].pf, FALSE, outdir, &name, &err));
            dst = g_build_filename(outdir, name ? name : "none", NULL);
            CHECK(gdk_pixbuf_get_file_info(dst, &w, &h) != NULL && w == cases[k].want_w && h == cases[k].want_h);
            if (w != cases[k].want_w || h != cases[k].want_h)
                printf("   case %u: got %dx%d, wanted %dx%d\n", k, w, h, cases[k].want_w, cases[k].want_h);
            CHECK(name != NULL && g_str_has_prefix(name, "SLES_123.45_") && g_str_has_suffix(name, ".png"));
            g_clear_error(&err);
            g_free(name);
            g_free(dst);
            g_free(src);
        }
        {
            /* "whole": a tall picture is fitted inside the cover frame instead of cropped to its shape */
            GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 3000, 1000);
            gchar *src = g_build_filename(tmp, "wide.png", NULL), *name = NULL, *dst;
            int w = 0, h = 0;

            gdk_pixbuf_fill(pb, 0x336699ff);
            gdk_pixbuf_save(pb, src, "png", NULL, NULL);
            g_object_unref(pb);
            CHECK(art_import_file(src, "SLES_123.45", "COV", PS2GM_PS2, TRUE, outdir, &name, &err));
            dst = g_build_filename(outdir, name ? name : "none", NULL);
            CHECK(gdk_pixbuf_get_file_info(dst, &w, &h) != NULL && w == 140 && h == 46);
            if (w != 140 || h != 46)
                printf("   whole: got %dx%d, wanted 140x46\n", w, h);
            g_free(name);
            g_free(dst);
            g_free(src);
        }
        {
            gchar *txt = g_build_filename(tmp, "notapicture.png", NULL);

            g_file_set_contents(txt, "this is text", -1, NULL);
            CHECK(!art_import_file(txt, "SLES_123.45", "COV", PS2GM_PS2, FALSE, outdir, NULL, &err));
            printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
            CHECK(!art_import_file(txt, "SLES_123.45", "NOPE", PS2GM_PS2, FALSE, outdir, NULL, &err));
            g_clear_error(&err);
            CHECK(!art_import_url("file:///etc/passwd", "SLES_123.45", "COV", PS2GM_PS2, FALSE, outdir, NULL, &err));
            printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
            g_free(txt);
        }
        g_free(outdir);
    }

    printf("== cheat lists as OPL reads them\n");
    {
        char *out = NULL;
        CheatInfo ci;
        const char *good = "\xEF\xBB\xBF// a comment\r\nInfinite Health #1\r\n10B8DAFA 00003F00\r\n20B8DAFB 00000001 // note\r\n\r\n\r\nMaster Code\r\n"
                           "90000000 00000000\r\n";
        const char *bad[] = {"", "just some words\nand more words\n", "10B8DAFA 00003F00\n", "Name only\n", "Name\n1234\n", NULL};
        int k;

        CHECK(cheats_normalize(good, &out, &ci, &err));
        CHECK(ci.cheats == 2 && ci.codes == 3);
        CHECK(out && strchr(out, '\r') == NULL && strstr(out, "\xEF\xBB") == NULL && g_str_has_suffix(out, "00000000\n"));
        CHECK(out && !g_str_has_suffix(out, "\n\n"));
        g_free(out);
        for (k = 0; bad[k]; k++) {
            CHECK(!cheats_normalize(bad[k], &out, NULL, &err));
            if (k == 2) printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
        }
        {
            gchar *longline = g_strnfill(300, 'x');

            CHECK(!cheats_normalize(longline, &out, NULL, &err));
            g_clear_error(&err);
            g_free(longline);
        }

        {
            const char *pairs[] = {"$CheatsSource", "1", "$EnableCheat", "1", "$CheatMode", "0", NULL};
            char *cfg = cfg_set_keys("Title=My Game\r\n$EnableCheat=0\r\nDescription=Something\r\n", pairs);

            CHECK(!strcmp(cfg, "Title=My Game\n$EnableCheat=1\nDescription=Something\n$CheatsSource=1\n$CheatMode=0\n"));
            g_free(cfg);
            cfg = cfg_set_keys(NULL, pairs);
            CHECK(!strcmp(cfg, "$CheatsSource=1\n$EnableCheat=1\n$CheatMode=0\n"));
            g_free(cfg);
        }
    }

    printf("== hdl_dump's description of a disc (including dual-layer)\n");
    {
        HdlIsoInfo hi;
        guint k;
        struct { const char *text; gboolean good, dual; const char *media, *label, *id; guint64 kb; } cases[] = {
            {"dual-layer DVD 7830688KB \"GODOFWAR\" \"SCES_531.33\" \n", TRUE, TRUE, "DVD", "GODOFWAR", "SCES_531.33", 7830688},
            {"DVD 4500000KB \"SOME GAME\" \"SLUS_200.62\" \n", TRUE, FALSE, "DVD", "SOME GAME", "SLUS_200.62", 4500000},
            {"CD 700000KB \"A CD\" \"SLES_539.08\" \n", TRUE, FALSE, "CD", "A CD", "SLES_539.08", 700000},
            {"? 3000KB \"UNKNOWN\" \"SLPS_250.01\" \n", TRUE, FALSE, "?", "UNKNOWN", "SLPS_250.01", 3000},
            {"some warning\nDVD 100KB \"X\" \"SLES_000.01\"\n", TRUE, FALSE, "DVD", "X", "SLES_000.01", 100},
            {"Not a PS2 disc\n", FALSE, FALSE, NULL, NULL, NULL, 0},
            {"", FALSE, FALSE, NULL, NULL, NULL, 0},
        };

        for (k = 0; k < G_N_ELEMENTS(cases); k++) {
            gboolean got = hdl_parse_cdvd_info(cases[k].text, &hi);

            CHECK(got == cases[k].good);
            if (got && cases[k].good) {
                CHECK(hi.dual_layer == cases[k].dual && !g_strcmp0(hi.media, cases[k].media) && !g_strcmp0(hi.label, cases[k].label) &&
                      !g_strcmp0(hi.id, cases[k].id) && hi.size_kb == cases[k].kb);
                if (hi.size_kb != cases[k].kb || g_strcmp0(hi.id, cases[k].id))
                    printf("   case %lu: got %s %lu\n", (gulong)k, hi.id, (gulong)hi.size_kb);
            }
            hdl_iso_info_clear(&hi);
        }
    }

    printf("== per-game OPL settings\n");
    {
        GameSettings g;
        const char *file = "Title=My Game\r\n$Compatibility=5\r\n$DMA=3\r\n$AltStartup=FOO.ELF\r\n$GSMSource=1\r\n$EnableGSM=1\r\n"
                           "$GSMVMode=2\r\n$EnableCheat=1\r\n$CheatsSource=1\r\n";
        char *out;

        game_settings_init(&g);
        game_settings_parse(NULL, &g);
        CHECK(g.compat == 0 && g.dma == GAME_DMA_DEFAULT && !strcmp(g.alt_startup, "") && !g.gsm_on);
        game_settings_parse(file, &g);
        CHECK(g.compat == 5 && g.dma == 3 && !strcmp(g.alt_startup, "FOO.ELF") && g.gsm_on && g.gsm_vmode == 2);
        game_settings_parse("$Compatibility=255\n$DMA=99\n$GSMVMode=500\n$GSMSource=0\n$EnableGSM=1\n", &g);
        CHECK(g.compat == 63 && g.dma == GAME_DMA_DEFAULT && g.gsm_vmode == 0 && !g.gsm_on); /* out of range / global GSM: ignored */

        /* change some settings: everything else in the file stays */
        game_settings_parse(file, &g);
        g.compat = 2;
        g.dma = GAME_DMA_DEFAULT;
        g.gsm_on = FALSE;
        out = game_settings_apply(file, &g, &err);
        CHECK(out != NULL);
        CHECK(out && strstr(out, "Title=My Game\n") && strstr(out, "$EnableCheat=1\n") && strstr(out, "$CheatsSource=1\n") && strstr(out, "$AltStartup=FOO.ELF\n"));
        CHECK(out && strstr(out, "$Compatibility=2\n") && !strstr(out, "$DMA") && !strstr(out, "$EnableGSM") && !strstr(out, "$GSMVMode") && strstr(out, "$ConfigSource=1\n"));
        CHECK(out && !strchr(out, '\r'));
        {
            GameSettings back;

            game_settings_init(&back);
            game_settings_parse(out, &back);
            CHECK(back.compat == 2 && back.dma == GAME_DMA_DEFAULT && !back.gsm_on && !strcmp(back.alt_startup, "FOO.ELF"));
            game_settings_clear(&back);
        }
        g_free(out);

        /* all defaults on a file without settings: nothing is added */
        game_settings_init(&g);
        out = game_settings_apply("Title=Plain\n", &g, &err);
        CHECK(out && !strcmp(out, "Title=Plain\n"));
        g_free(out);
        out = game_settings_apply(NULL, &g, &err);
        CHECK(out && !strcmp(out, ""));
        g_free(out);

        /* a new setting makes a new file */
        g.dma = 3;
        g.gsm_on = TRUE;
        g.gsm_vmode = 8;
        out = game_settings_apply(NULL, &g, &err);
        CHECK(out && strstr(out, "$DMA=3\n") && strstr(out, "$GSMSource=1\n") && strstr(out, "$EnableGSM=1\n") && strstr(out, "$GSMVMode=8\n") && strstr(out, "$ConfigSource=1\n"));
        g_free(out);

        /* repeated keys collapse into one */
        game_settings_init(&g);
        g.dma = 4;
        out = game_settings_apply("$DMA=1\n$DMA=2\n", &g, &err);
        CHECK(out && !strcmp(out, "$DMA=4\n$ConfigSource=1\n"));
        g_free(out);

        /* a startup file with characters that could break the file is refused */
        g_free(g.alt_startup);
        g.alt_startup = g_strdup("evil\"name\nbreak");
        CHECK(game_settings_apply(NULL, &g, &err) == NULL);
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);

        out = game_settings_reset(file);
        CHECK(out && !strstr(out, "$Compatibility") && !strstr(out, "$DMA") && !strstr(out, "$AltStartup") && !strstr(out, "$GSMVMode") &&
              strstr(out, "Title=My Game\n") && strstr(out, "$EnableCheat=1\n"));
        g_free(out);
        CHECK(!strcmp(game_dma_name(7), "UDMA 4") && !strcmp(game_dma_name(0), "MDMA 0") && !strcmp(game_gsm_mode_name(2), "PAL"));
        game_settings_clear(&g);
    }

    printf("== partition size from the partition table\n");
    {
        guint parts = 0;
        guint64 mb = 0;

        CHECK(hdl_partition_size(&t, disk, "__.POPS", &parts, &mb, &err));
        CHECK(parts >= 1 && mb == 1024);
        CHECK(!hdl_partition_size(&t, disk, "no.such.partition", &parts, &mb, NULL));
        g_clear_error(&err);
    }

    printf("== missing folder and bad names\n");
    list = pfs_list(&t, disk, "__common", "NOPE/NOPE", &err);
    CHECK(list == NULL);
    g_clear_error(&err);
    CHECK(pfs_list(&t, disk, "__common", "../x", NULL) == NULL);

    printf("== remove the installed game\n");
    CHECK(!hdl_remove_game(&t, disk, "SLES_999.92", "Wrong Title", NULL)); /* both must match */
    CHECK(hdl_remove_game(&t, disk, "SLES_999.92", "Second Test Game", &err));
    if (err)
        printf("   %s\n", err->message);
    g_clear_error(&err);
    g_ptr_array_free(games, TRUE);
    games = hdl_list_games(&t, disk, &err);
    CHECK(games && games->len == before);
    g_ptr_array_free(games, TRUE);

    ps2gm_tools_clear(&t);
    printf(failures ? "\n%d FAILURES\n" : "\nall ok\n", failures);
    return failures != 0;
}
