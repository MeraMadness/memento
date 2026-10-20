/*
 * ZSO unpacking. Reference files are made with the real liblz4 (only the tests use it; the program has its own
 * decoder), then unpacked and compared byte for byte; damaged files must fail cleanly, never crash.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lz4.h>
#include <glib/gstdio.h>

#include "ziso.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void put32(guint8 *p, guint32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(guint8 *p, guint64 v) { put32(p, (guint32)v); put32(p + 4, (guint32)(v >> 32)); }

/* some data of every kind: text-like (compresses), random (stored), zeros (compresses a lot) */
static guint8 *make_data(gsize size)
{
    guint8 *d = g_malloc(size);
    gsize i;
    guint32 seed = 12345;

    for (i = 0; i < size; i++) {
        gsize section = (i / 8192) % 3;

        if (section == 0)
            d[i] = "SYSTEM.CNF;1 BOOT2 = cdrom0:\\SLES_999.99;1 VER = 1.00 "[i % 52];
        else if (section == 1) {
            seed = seed * 1103515245u + 12345u;
            d[i] = seed >> 16;
        } else
            d[i] = 0;
    }
    return d;
}

static guint8 *make_zso(const guint8 *data, gsize size, guint32 block_size, guint8 shift, gsize *out_len)
{
    gsize blocks = (size + block_size - 1) / block_size, cap = 24 + (blocks + 1) * 4 + blocks * (block_size + 64) + 64, pos, i;
    guint8 *z = g_malloc0(cap), *tmp = g_malloc(LZ4_compressBound(block_size));
    gsize align = (gsize)1 << shift;

    memcpy(z, "ZISO", 4);
    put32(z + 4, 24);
    put64(z + 8, size);
    put32(z + 16, block_size);
    z[20] = 1;
    z[21] = shift;
    pos = 24 + (blocks + 1) * 4;
    pos = (pos + align - 1) / align * align;
    for (i = 0; i < blocks; i++) {
        gsize want = MIN((gsize)block_size, size - i * block_size);
        int clen = LZ4_compress_default((const char *)data + i * block_size, (char *)tmp, (int)want, LZ4_compressBound(block_size));
        gboolean plain = clen <= 0 || (gsize)clen >= want;
        gsize len = plain ? want : (gsize)clen;

        put32(z + 24 + i * 4, (guint32)(pos >> shift) | (plain ? 0x80000000u : 0));
        memcpy(z + pos, plain ? data + i * block_size : tmp, len);
        pos += len;
        pos = (pos + align - 1) / align * align;
    }
    put32(z + 24 + blocks * 4, (guint32)(pos >> shift));
    g_free(tmp);
    *out_len = pos;
    return z;
}

static gboolean same_file(const char *path, const guint8 *data, gsize len)
{
    gchar *c = NULL;
    gsize n = 0;
    gboolean ok = g_file_get_contents(path, &c, &n, NULL) && n == len && !memcmp(c, data, len);

    g_free(c);
    return ok;
}

static gboolean cancel_at_once(guint64 d, guint64 t, const char *w, gpointer u)
{
    (void)d; (void)t; (void)w; (void)u;
    return FALSE;
}

/* a tiny ISO 9660 disc: PVD at sector 16, root folder at 18 holding SYSTEM.CNF;1 at 20 */
static guint8 *make_iso(const char *system_cnf, int kind /* 0 CD-XA, 1 DVD (zeros), 2 neither */, gsize *len)
{
    gsize sectors = 64;
    guint8 *iso = g_malloc0(sectors * 2048), *pvd = iso + 16 * 2048, *dir = iso + 18 * 2048;
    gsize cnf_len = strlen(system_cnf), pos = 0;

    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 40, ' ', 32);
    memcpy(pvd + 40, "TEST_GAME", 9);
    if (kind == 0)
        memcpy(pvd + 1024, "CD-XA001", 8);
    else if (kind == 2)
        memcpy(pvd + 1024, "XXXXXXXX", 8);
    put32(pvd + 156 + 2, 18);
    put32(pvd + 156 + 10, 2048);
    pvd[156] = 34;
    pvd[156 + 25] = 2;
    pvd[156 + 32] = 1;
    memset(iso + 17 * 2048, 0, 2048);
    iso[17 * 2048] = 0xFF;
    memcpy(iso + 17 * 2048 + 1, "CD001", 5);
    /* "." and ".." */
    for (int k = 0; k < 2; k++) {
        dir[pos] = 34;
        put32(dir + pos + 2, 18);
        put32(dir + pos + 10, 2048);
        dir[pos + 25] = 2;
        dir[pos + 32] = 1;
        dir[pos + 33] = k;
        pos += 34;
    }
    dir[pos] = 46; /* 33 + 12, made even */
    put32(dir + pos + 2, 20);
    put32(dir + pos + 10, (guint32)cnf_len);
    dir[pos + 32] = 12;
    memcpy(dir + pos + 33, "SYSTEM.CNF;1", 12);
    memcpy(iso + 20 * 2048, system_cnf, cnf_len);
    *len = sectors * 2048;
    return iso;
}

int main(void)
{
    gchar *tmp = g_dir_make_tmp("ziso-test-XXXXXX", NULL);
    gchar *zpath = g_build_filename(tmp, "game.zso", NULL), *ipath = g_build_filename(tmp, "game.iso", NULL);
    gsize size = 3 * 1024 * 1024 + 700, zlen, k;
    guint8 *data = make_data(size);
    struct { guint32 bs; guint8 shift; } combos[] = {{2048, 0}, {2048, 2}, {4096, 0}, {2048, 3}};
    GError *err = NULL;
    guint8 *z;

    printf("== unpacking files made by liblz4\n");
    for (k = 0; k < G_N_ELEMENTS(combos); k++) {
        ZisoInfo info;

        z = make_zso(data, size, combos[k].bs, combos[k].shift, &zlen);
        g_file_set_contents(zpath, (char *)z, zlen, NULL);
        CHECK(ziso_is_zso(zpath));
        CHECK(ziso_info(zpath, &info, &err) && info.total_bytes == size && info.block_size == combos[k].bs);
        CHECK(ziso_unpack(zpath, ipath, NULL, NULL, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(same_file(ipath, data, size));
        printf("   block %u, shift %u: %lu -> %lu bytes\n", combos[k].bs, combos[k].shift, (gulong)size, (gulong)zlen);
        g_unlink(ipath);
        g_free(z);
    }

    printf("== reading the game out of a ZSO (no unpacking)\n");
    {
        struct { const char *cnf; int kind; const char *id, *media; gboolean good; } cases[] = {
            {"BOOT2 = cdrom0:\\SLES_999.99;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n", 0, "SLES_999.99", "CD", TRUE},
            {"BOOT2 = cdrom0:\\SLUS_200.62;1\nVER = 1.01\n", 1, "SLUS_200.62", "DVD", TRUE},
            {"BOOT2=cdrom0:\\slps_250.01;1", 2, "SLPS_250.01", "CD", TRUE},                 /* lower case, no spaces, unknown kind: small, so CD */
            {"BOOT2 = cdrom0:\\GAME\\SCES_500.00;1\n", 1, "SCES_500.00", "DVD", TRUE},    /* in a folder */
            {"BOOT = cdrom:\\SLES_012.34;1\nVER = 1.00\n", 1, NULL, NULL, FALSE},           /* a PS1 disc */
            {"nothing useful here\n", 1, NULL, NULL, FALSE},
        };

        for (k = 0; k < G_N_ELEMENTS(cases); k++) {
            gsize ilen, zl;
            guint8 *iso = make_iso(cases[k].cnf, cases[k].kind, &ilen), *zz = make_zso(iso, ilen, 2048, 0, &zl);
            ZisoGame g;
            gboolean got;

            g_file_set_contents(zpath, (char *)zz, zl, NULL);
            got = ziso_game_info(zpath, &g, &err);
            CHECK(got == cases[k].good);
            if (cases[k].good && got) {
                CHECK(!g_strcmp0(g.id, cases[k].id) && !strcmp(g.media, cases[k].media));
                CHECK(!g_strcmp0(g.label, "TEST_GAME") && g.size_kb == ilen / 1024);
                if (g_strcmp0(g.id, cases[k].id))
                    printf("   case %lu: got id %s\n", (gulong)k, g.id);
            } else if (!cases[k].good && k == 4)
                printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
            ziso_game_clear(&g);
            g_free(iso);
            g_free(zz);
        }
        /* a ZSO that is not a disc at all, and a file that is not a ZSO */
        {
            ZisoGame g;
            guint8 *rnd = g_malloc(100 * 2048), *zz;
            gsize zl;

            for (k = 0; k < 100 * 2048; k++)
                rnd[k] = (guint8)(k * 2654435761u >> 13);
            zz = make_zso(rnd, 100 * 2048, 2048, 0, &zl);
            g_file_set_contents(zpath, (char *)zz, zl, NULL);
            CHECK(!ziso_game_info(zpath, &g, &err));
            g_clear_error(&err);
            g_file_set_contents(zpath, "plain text", -1, NULL);
            CHECK(!ziso_game_info(zpath, &g, &err));
            g_clear_error(&err);
            g_free(rnd);
            g_free(zz);
        }
        /* damage never crashes the reader */
        {
            gsize ilen, zl, round;
            guint8 *iso = make_iso("BOOT2 = cdrom0:\\SLES_999.99;1\n", 1, &ilen), *zz = make_zso(iso, ilen, 2048, 0, &zl), *dmg = g_malloc(zl);
            GRand *rnd = g_rand_new_with_seed(11);

            for (round = 0; round < 300; round++) {
                ZisoGame g;

                memcpy(dmg, zz, zl);
                for (k = 0; k < 1 + (gsize)g_rand_int_range(rnd, 0, 6); k++)
                    dmg[g_rand_int_range(rnd, 0, zl)] ^= (guint8)(1 + g_rand_int_range(rnd, 0, 255));
                g_file_set_contents(zpath, (char *)dmg, zl, NULL);
                if (ziso_game_info(zpath, &g, &err))
                    ziso_game_clear(&g);
                g_clear_error(&err);
            }
            g_free(iso);
            g_free(zz);
            g_free(dmg);
            g_rand_free(rnd);
        }
    }

    printf("== compressing (ziso_pack) and unpacking again\n");
    {
        gsize sizes[] = {2048, 2048 * 511, 2048 * 512, 2048 * 513, 2048 * 1537, 40 * 1024 * 1024};
        gchar *packed = g_build_filename(tmp, "packed.zso", NULL), *plain = g_build_filename(tmp, "plain.iso", NULL);

        for (k = 0; k < G_N_ELEMENTS(sizes); k++) {
            guint8 *d = make_data(sizes[k]);
            ZisoInfo info;

            g_file_set_contents(plain, (char *)d, sizes[k], NULL);
            CHECK(ziso_pack(plain, packed, NULL, NULL, &err));
            if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
            CHECK(ziso_info(packed, &info, &err) && info.total_bytes == sizes[k] && info.block_size == 2048 && info.shift == 0);
            {
                GStatBuf pst;

                CHECK(g_stat(packed, &pst) == 0 && pst.st_size % 2048 == 0); /* hdl_dump copies only whole sectors */
            }
            g_clear_error(&err);
            g_unlink(plain);
            CHECK(ziso_unpack(packed, plain, NULL, NULL, &err));
            g_clear_error(&err);
            CHECK(same_file(plain, d, sizes[k]));
            if (sizes[k] >= 1024 * 1024) {
                GStatBuf st;

                g_stat(packed, &st);
                printf("   %lu KB -> %lu KB\n", (gulong)(sizes[k] / 1024), (gulong)(st.st_size / 1024));
                CHECK((gsize)st.st_size < sizes[k] * 8 / 10); /* a third of the test data is incompressible, the rest shrinks */
            }
            g_unlink(plain);
            g_free(d);
        }

        /* the packed file is a good ZSO for the game reader too */
        {
            gsize ilen;
            guint8 *iso = make_iso("BOOT2 = cdrom0:\\SLES_999.99;1\r\nVER = 1.00\r\n", 1, &ilen);
            ZisoGame g;

            g_file_set_contents(plain, (char *)iso, ilen, NULL);
            CHECK(ziso_pack(plain, packed, NULL, NULL, &err));
            CHECK(ziso_game_info(packed, &g, &err) && !g_strcmp0(g.id, "SLES_999.99") && !strcmp(g.media, "DVD") && g.size_kb == ilen / 1024);
            g_clear_error(&err);
            ziso_game_clear(&g);
            g_unlink(plain);
            g_free(iso);
        }

        /* bad input */
        g_unlink(packed);
        g_file_set_contents(plain, "not a multiple of 2048 bytes", -1, NULL);
        CHECK(!ziso_pack(plain, packed, NULL, NULL, &err) && !g_file_test(packed, G_FILE_TEST_EXISTS));
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!ziso_pack("/no/such.iso", packed, NULL, NULL, &err));
        g_clear_error(&err);

        /* cancelling leaves nothing behind */
        {
            gsize big = 40 * 1024 * 1024;
            guint8 *d = make_data(big);
            guint64 unused = 0;

            g_file_set_contents(plain, (char *)d, big, NULL);
            CHECK(!ziso_pack(plain, packed, cancel_at_once, &unused, &err));
            printf("   expected: %s\n", err ? err->message : "(none)");
            g_clear_error(&err);
            CHECK(!g_file_test(packed, G_FILE_TEST_EXISTS));
            g_unlink(plain);
            g_free(d);
        }
        g_free(packed);
        g_free(plain);
    }

    printf("== a ZSO from another tool that is not a whole number of sectors long\n");
    {
        gsize isz = 2048 * 700, zl;
        guint8 *d = make_data(isz), *zz = make_zso(d, isz, 2048, 0, &zl);
        gchar *odd = g_build_filename(tmp, "odd.zso", NULL), *padded = g_build_filename(tmp, "padded.zso", NULL), *back = g_build_filename(tmp, "back.iso", NULL);
        gchar *content = NULL;
        gsize clen = 0;

        CHECK(zl % 2048 != 0); /* the test data was chosen so that the file is not a whole number of sectors */
        g_file_set_contents(odd, (char *)zz, zl, NULL);
        CHECK(ziso_needs_padding(odd));
        CHECK(ziso_pad_copy(odd, padded, NULL, NULL, &err));
        g_clear_error(&err);
        CHECK(!ziso_needs_padding(padded));
        CHECK(g_file_get_contents(padded, &content, &clen, NULL) && clen % 2048 == 0 && clen >= zl && !memcmp(content, zz, zl));
        CHECK(ziso_unpack(padded, back, NULL, NULL, &err)); /* the copy is still a good ZSO */
        g_clear_error(&err);
        CHECK(same_file(back, d, isz));
        CHECK(!ziso_pad_copy("/no/such.zso", padded, NULL, NULL, &err));
        g_clear_error(&err);
        g_unlink(odd); g_unlink(padded); g_unlink(back);
        g_free(odd); g_free(padded); g_free(back); g_free(content); g_free(d); g_free(zz);
    }

    printf("== every index shift, and tails of every length\n");
    {
        gchar *plain = g_build_filename(tmp, "s.iso", NULL), *packed = g_build_filename(tmp, "s.zso", NULL), *back = g_build_filename(tmp, "s_back.iso", NULL);
        int shift, bad = 0, runs = 0;
        gsize nblocks;

        for (shift = 0; shift <= 3; shift++)
            for (nblocks = 1; nblocks <= 40; nblocks++) {
                gsize size = nblocks * 2048, i;
                guint8 *d = g_malloc(size);
                GRand *r = g_rand_new_with_seed((guint32)(nblocks * 31 + shift));

                /* blocks that compress to many different lengths: runs of repeated bytes of varying size, some noise */
                for (i = 0; i < size; i++)
                    d[i] = (i / (1 + (nblocks + i / 2048) % 29)) % 7 == 0 ? (guint8)g_rand_int(r) : (guint8)(i / 64);
                g_file_set_contents(plain, (char *)d, size, NULL);
                if (!ziso_pack_shift(plain, packed, shift, NULL, NULL, &err) || !ziso_unpack(packed, back, NULL, NULL, &err) || !same_file(back, d, size)) {
                    bad++;
                    if (bad <= 3)
                        printf("   shift %d, %lu blocks: %s\n", shift, (gulong)nblocks, err ? err->message : "differs");
                }
                runs++;
                g_clear_error(&err);
                g_unlink(plain); g_unlink(packed); g_unlink(back);
                g_rand_free(r);
                g_free(d);
            }
        CHECK(bad == 0);
        printf("   %d packs and unpacks (shifts 0-3, 1-40 blocks), %d wrong\n", runs, bad);
        CHECK(!ziso_pack_shift(plain, packed, 4, NULL, NULL, &err));
        g_clear_error(&err);
        g_free(plain); g_free(packed); g_free(back);
    }

    printf("== estimating how much compressing saves\n");
    {
        gchar *plain = g_build_filename(tmp, "e.iso", NULL), *packed = g_build_filename(tmp, "e.zso", NULL);
        gsize sizes[] = {2048, 2048 * 100, 6 * 1024 * 1024, 120 * 1024 * 1024};
        gsize kinds;

        for (kinds = 0; kinds < 3; kinds++) /* 0 mixed (a third random, the rest shrinks), 1 all random, 2 all zeros */
            for (k = 0; k < G_N_ELEMENTS(sizes); k++) {
                gsize size = sizes[k], i;
                guint8 *d = make_data(size);
                double est = -1, actual;
                GStatBuf st;

                if (kinds == 1) {
                    GRand *r = g_rand_new_with_seed(5);
                    for (i = 0; i < size; i++) d[i] = (guint8)g_rand_int(r);
                    g_rand_free(r);
                } else if (kinds == 2)
                    memset(d, 0, size);
                g_file_set_contents(plain, (char *)d, size, NULL);
                CHECK(ziso_estimate(plain, &est, &err));
                g_clear_error(&err);
                CHECK(ziso_pack(plain, packed, NULL, NULL, &err));
                g_clear_error(&err);
                g_stat(packed, &st);
                actual = (double)st.st_size / size;
                if (size >= 6 * 1024 * 1024) {
                    CHECK(est > 0 && fabs(est - actual) < 0.06); /* within a few percent */
                    if (fabs(est - actual) >= 0.06)
                        printf("   kind %lu, %lu KB: estimate %.3f, real %.3f\n", (gulong)kinds, (gulong)(size / 1024), est, actual);
                }
                if (kinds == 1) CHECK(est > 0.97);
                if (kinds == 2 && size >= 6 * 1024 * 1024) CHECK(est < 0.05);
                g_free(d);
                g_unlink(plain);
                g_unlink(packed);
            }
        {
            double est;

            CHECK(!ziso_estimate("/no/such.iso", &est, &err));
            g_clear_error(&err);
        }
        g_free(plain);
        g_free(packed);
    }

    printf("== which index shift a big image needs\n");
    {
        struct { guint64 bytes; int shift; } cases[] = {
            {3u << 20, 0}, {1900000000ull / 2048 * 2048, 0}, {2200000000ull / 2048 * 2048, 1}, {4700000000ull / 2048 * 2048, 2},
            {8543666176ull, 2}, /* the largest dual layer DVD */ {12ull << 30, 3}, {17ull << 30, -1},
        };

        for (k = 0; k < G_N_ELEMENTS(cases); k++) {
            CHECK(ziso_pick_shift(cases[k].bytes) == cases[k].shift);
            if (ziso_pick_shift(cases[k].bytes) != cases[k].shift)
                printf("   %lu bytes: got %d, wanted %d\n", (gulong)cases[k].bytes, ziso_pick_shift(cases[k].bytes), cases[k].shift);
        }
    }

    printf("== things that are not ZSO files\n");
    g_file_set_contents(zpath, "this is just text, not a game", -1, NULL);
    CHECK(!ziso_is_zso(zpath));
    CHECK(!ziso_info(zpath, &(ZisoInfo){0}, &err));
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    CHECK(!ziso_unpack(zpath, ipath, NULL, NULL, &err) && !g_file_test(ipath, G_FILE_TEST_EXISTS));
    g_clear_error(&err);
    CHECK(!ziso_unpack("/no/such/file.zso", ipath, NULL, NULL, &err));
    g_clear_error(&err);

    printf("== a truncated file leaves nothing behind\n");
    z = make_zso(data, size, 2048, 0, &zlen);
    g_file_set_contents(zpath, (char *)z, zlen / 2, NULL);
    CHECK(!ziso_unpack(zpath, ipath, NULL, NULL, &err));
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    CHECK(!g_file_test(ipath, G_FILE_TEST_EXISTS));
    g_file_set_contents(zpath, (char *)z, 100, NULL); /* the index does not even fit */
    CHECK(!ziso_unpack(zpath, ipath, NULL, NULL, &err));
    g_clear_error(&err);

    printf("== cancelling halfway leaves nothing behind\n");
    {
        gsize big = 40 * 1024 * 1024, biglen;
        guint8 *bigdata = g_malloc(big), *bigz;
        guint64 unused = 0;

        for (k = 0; k < big; k++)
            bigdata[k] = "abcdefgh"[k % 8];
        bigz = make_zso(bigdata, big, 2048, 0, &biglen);
        g_file_set_contents(zpath, (char *)bigz, biglen, NULL);
        CHECK(!ziso_unpack(zpath, ipath, cancel_at_once, &unused, &err)); /* the first progress report says stop */
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!g_file_test(ipath, G_FILE_TEST_EXISTS));
        CHECK(ziso_unpack(zpath, ipath, NULL, NULL, &err)); /* and without cancelling it works */
        g_clear_error(&err);
        CHECK(same_file(ipath, bigdata, big));
        g_unlink(ipath);
        g_free(bigdata);
        g_free(bigz);
    }

    printf("== the LZ4 decoder refuses bad streams\n");
    {
        guint8 out[64];
        const guint8 ok_lit[] = {0x30, 'a', 'b', 'c'};                       /* 3 literals */
        const guint8 match_before_data[] = {0x10, 'a', 0x05, 0x00, 0x00};   /* offset 5 > produced 1 */
        const guint8 zero_offset[] = {0x10, 'a', 0x00, 0x00, 0x00};
        const guint8 cut_literals[] = {0x50, 'a', 'b'};                      /* says 5, has 2 */
        const guint8 no_offset[] = {0x10, 'a'};
        const guint8 long_run[] = {0xF0, 255, 255, 255, 255, 255, 255, 0, 'x'}; /* far more literals than the target */
        const guint8 overlap[] = {0x14, 'a', 0x01, 0x00, 0x00};              /* a + (match 8, offset 1) = "aaaaaaaaa" */

        CHECK(ziso_lz4_decode(ok_lit, sizeof ok_lit, out, 3) && !memcmp(out, "abc", 3));
        CHECK(ziso_lz4_decode(ok_lit, sizeof ok_lit + 0, out, 3));
        CHECK(!ziso_lz4_decode(match_before_data, sizeof match_before_data, out, 10));
        CHECK(!ziso_lz4_decode(zero_offset, sizeof zero_offset, out, 10));
        CHECK(!ziso_lz4_decode(cut_literals, sizeof cut_literals, out, 5));
        CHECK(!ziso_lz4_decode(no_offset, sizeof no_offset, out, 10));
        CHECK(!ziso_lz4_decode(long_run, sizeof long_run, out, 16));
        CHECK(ziso_lz4_decode(overlap, sizeof overlap, out, 9) && !memcmp(out, "aaaaaaaaa", 9));
        CHECK(!ziso_lz4_decode(overlap, sizeof overlap, out, 10)); /* wants one more byte than the stream gives */
        CHECK(!ziso_lz4_decode(ok_lit, 0, out, 3));
    }

    printf("== random damage never crashes and never leaves a partial ISO\n");
    {
        gsize round, ok_count = 0;
        guint8 *dmg = g_malloc(zlen);
        GRand *rnd = g_rand_new_with_seed(7);

        for (round = 0; round < 400; round++) {
            gsize flips = 1 + g_rand_int_range(rnd, 0, 8), f;
            gboolean res;

            memcpy(dmg, z, zlen);
            for (f = 0; f < flips; f++) {
                /* half the time hit the header and index, where it matters most */
                gsize where = g_rand_boolean(rnd) ? g_rand_int_range(rnd, 0, 24 + 4 * 1600) : g_rand_int_range(rnd, 0, zlen);

                dmg[where] ^= (guint8)(1 + g_rand_int_range(rnd, 0, 255));
            }
            g_file_set_contents(zpath, (char *)dmg, zlen, NULL);
            res = ziso_unpack(zpath, ipath, NULL, NULL, &err);
            g_clear_error(&err);
            if (res) {
                ok_count++; /* damage in the data of a stored block is invisible to the format: fine */
                g_unlink(ipath);
            } else
                CHECK(!g_file_test(ipath, G_FILE_TEST_EXISTS));
        }
        printf("   400 damaged files: %lu still unpacked (damage in stored data), the rest failed cleanly\n", (gulong)ok_count);
        g_free(dmg);
        g_rand_free(rnd);
    }

    g_free(z);
    g_free(data);
    g_unlink(zpath);
    g_rmdir(tmp);
    printf(failures ? "\n%d FAILURES\n" : "\nall ok\n", failures);
    return failures != 0;
}
