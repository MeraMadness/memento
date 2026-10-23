/*
 * Virtual memory cards: the card code (list, import, export, delete on a freshly formatted card) and the trip through the
 * disk: put a card in +OPL/VMC, fetch it, change it, store it back, and the helper's refusals.
 * Needs PS2GM_TEST_DISK and PS2GM_TEST_HELPER (set by run_tests.sh).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "mcfs.h"
#include "ops.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void p16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* a blank card, laid out the way openVMC makes them (the layout openVMC uses) */
static void dirent(uint8_t *e, uint16_t mode, uint32_t length, const char *name)
{
    static const uint8_t stamp[8] = {0, 0, 0, 12, 1, 1, 0xe9, 0x07};

    memset(e, 0, 512);
    p16(e, mode);
    p32(e + 4, length);
    memcpy(e + 8, stamp, 8);
    memcpy(e + 0x18, stamp, 8);
    strcpy((char *)e + 0x40, name);
}

static void format_card(const char *path, int mb)
{
    uint8_t sb[1024], cl[1024], zero[1024];
    uint32_t size = mb * 1024 * 1024, clusters = size / 1024, fat_count = 1, alloc_offset, alloc_end, i, j;
    int fd;

    while (fat_count * 256 < clusters - 9 - fat_count)
        fat_count++;
    alloc_offset = 9 + fat_count;
    alloc_end = clusters - alloc_offset - 16;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    memset(zero, 0, sizeof(zero));
    for (i = 0; i < clusters; i++)
        if (write(fd, zero, sizeof(zero)) != sizeof(zero))
            abort();
    memset(sb, 0, sizeof(sb));
    memcpy(sb, "Sony PS2 Memory Card Format ", 28);
    memcpy(sb + 0x1c, "1.2.0.0", 7);
    p16(sb + 0x28, 512);
    sb[0x2a] = 2;
    sb[0x2c] = 0x10;
    sb[0x2e] = 0x00;
    sb[0x2f] = 0xff;
    p32(sb + 0x30, clusters);
    p32(sb + 0x34, alloc_offset);
    p32(sb + 0x38, alloc_end);
    p32(sb + 0x3c, 0);
    p32(sb + 0x40, size / 8192 - 1);
    p32(sb + 0x44, size / 8192 - 2);
    p32(sb + 0x50, 8);
    memset(sb + 0xd0, 0xff, 0x80);
    sb[0x150] = 2;
    sb[0x151] = 0x2b;
    lseek(fd, 0, SEEK_SET);
    if (write(fd, sb, 1024) != 1024)
        abort();
    for (i = 0; i < 256; i++)
        p32(cl + i * 4, i < fat_count ? 9 + i : 0xffffffff);
    lseek(fd, 8 * 1024, SEEK_SET);
    if (write(fd, cl, 1024) != 1024)
        abort();
    for (i = 0; i < fat_count; i++) {
        for (j = 0; j < 256; j++) {
            uint32_t idx = i * 256 + j;

            p32(cl + j * 4, (idx == 0 || idx >= alloc_end) ? 0xffffffff : 0x7fffffff);
        }
        if (write(fd, cl, 1024) != 1024)
            abort();
    }
    memset(cl, 0, sizeof(cl));
    dirent(cl, 0x8427, 2, ".");
    dirent(cl + 512, 0xa426, 0, "..");
    lseek(fd, (off_t)alloc_offset * 1024, SEEK_SET);
    if (write(fd, cl, 1024) != 1024)
        abort();
    close(fd);
}

/* a .psu with an icon.sys (title "Test 1" in full-width Shift-JIS) and one data file */
static void psu_put(FILE *f, uint16_t mode, uint32_t len, const char *name)
{
    uint8_t b[512] = {0};

    p16(b, mode);
    p32(b + 4, len);
    b[9] = 30; b[10] = 12; b[11] = 1; b[12] = 1; p16(b + 13, 2005);
    memcpy(b + 0x18, b + 8, 8);
    strcpy((char *)b + 0x40, name);
    fwrite(b, 1, 512, f);
}

static void make_psu(const char *path, const char *dir, uint32_t datalen)
{
    FILE *f = fopen(path, "wb");
    uint8_t icon[1024] = {0}, *data = g_malloc0((datalen + 1023) / 1024 * 1024);
    uint32_t k;

    psu_put(f, 0x8427, 4, dir);
    psu_put(f, 0x8427, 0, ".");
    psu_put(f, 0x8427, 0, "..");
    memcpy(icon, "PS2D", 4);
    memcpy(icon + 0xc0, "\x82\x73\x82\x85\x82\x93\x82\x94\x81\x40\x82\x50", 12);
    psu_put(f, 0x8497, 964, "icon.sys");
    fwrite(icon, 1, 1024, f);
    for (k = 0; k < datalen; k++)
        data[k] = (uint8_t)(k * 7 + dir[0]);
    psu_put(f, 0x8497, datalen, "data.bin");
    fwrite(data, 1, (datalen + 1023) / 1024 * 1024, f);
    fclose(f);
    g_free(data);
}

static int count_saves(const char *card, char *first_title, int tlen)
{
    mc_t mc;
    struct mc_save l[64];
    int n;

    if (mc_open(&mc, card, 0) != MC_OK)
        return -1;
    n = mc_list(&mc, l, 64);
    if (n > 0 && first_title != NULL)
        mc_title(&mc, l[0].name, first_title, tlen);
    mc_close(&mc);
    return n;
}

int main(void)
{
    char *tmp = g_dir_make_tmp("vmc-XXXXXX", NULL), *card = g_build_filename(tmp, "card.bin", NULL), *psu = g_build_filename(tmp, "BESLES-50001TEST.psu", NULL);
    char *out = g_build_filename(tmp, "out.psu", NULL), *back = g_build_filename(tmp, "back.bin", NULL), *junk = g_build_filename(tmp, "junk.bin", NULL);
    char title[64] = "", name[MC_NAME];
    GError *err = NULL;
    mc_t mc;
    Backend *b;
    OpsContext *ops;
    Library *lib;
    guint64 size = 0;
    guint i;

    printf("== the card code on a blank card\n");
    format_card(card, 8);
    make_psu(psu, "BESLES-50001TEST", 3000);
    CHECK(count_saves(card, NULL, 0) == 0);
    CHECK(mc_open(&mc, card, 1) == MC_OK);
    CHECK(mc_import_psu(&mc, psu, name) == MC_OK && !strcmp(name, "BESLES-50001TEST"));
    CHECK(mc_close(&mc) == MC_OK);
    CHECK(count_saves(card, title, sizeof(title)) == 1 && !strcmp(title, "Test 1"));
    CHECK(mc_open(&mc, card, 0) == MC_OK);
    CHECK(mc_export_psu(&mc, "BESLES-50001TEST", out) == MC_OK);
    mc_close(&mc);
    CHECK(g_file_test(out, G_FILE_TEST_IS_REGULAR));
    CHECK(mc_open(&mc, card, 1) == MC_OK);
    CHECK(mc_delete(&mc, "BESLES-50001TEST") == MC_OK);
    mc_close(&mc);
    CHECK(count_saves(card, NULL, 0) == 0);

    printf("== the card on the disk\n");
    b = backend_start(g_getenv("PS2GM_TEST_DISK"), FALSE, g_getenv("PS2GM_TEST_HELPER"), &err);
    if (b == NULL) {
        printf("%s\n", err->message);
        return 1;
    }
    ops = ops_new(b, NULL);
    CHECK(backend_run(b, (char *[]){"PUT", "+OPL", "VMC", card, NULL}, NULL, NULL, &err));
    g_clear_error(&err);
    lib = backend_library(b, &err);
    CHECK(lib != NULL && lib->vmcs->len == 1 && !g_strcmp0(((LibFile *)g_ptr_array_index(lib->vmcs, 0))->name, "card.bin"));
    CHECK(lib != NULL && ((LibFile *)g_ptr_array_index(lib->vmcs, 0))->size == 8 * 1024 * 1024);
    library_free(lib);

    CHECK(ops_vmc_fetch(ops, "card.bin", back, &err));
    g_clear_error(&err);
    CHECK(count_saves(back, NULL, 0) == 0);
    CHECK(mc_open(&mc, back, 1) == MC_OK && mc_import_psu(&mc, out, NULL) == MC_OK);
    mc_close(&mc);
    CHECK(ops_vmc_store(ops, "card.bin", back, &err));
    if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
    lib = backend_library(b, &err);
    CHECK(lib != NULL && lib->vmcs->len == 1); /* the new card took the place of the old one, no leftovers */
    for (i = 0; lib && i < lib->vmcs->len; i++) {
        LibFile *f = g_ptr_array_index(lib->vmcs, i);

        size = f->size;
        CHECK(!strstr(f->name, "ps2gm-new"));
    }
    CHECK(size == 8 * 1024 * 1024);
    library_free(lib);
    g_remove(back);
    CHECK(ops_vmc_fetch(ops, "card.bin", back, &err));
    g_clear_error(&err);
    CHECK(count_saves(back, title, sizeof(title)) == 1 && !strcmp(title, "Test 1")); /* the change is really on the disk */

    printf("== refusals\n");
    CHECK(!ops_vmc_store(ops, "missing.bin", back, &err)); /* a card that is not on the disk */
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    CHECK(!ops_vmc_fetch(ops, "../card.bin", back, &err));
    g_clear_error(&err);
    CHECK(!ops_vmc_fetch(ops, "card.txt", back, &err));
    g_clear_error(&err);
    g_file_set_contents(junk, "this is not a memory card, only a file with the right ending", -1, NULL);
    CHECK(!backend_run(b, (char *[]){"PUT", "+OPL", "VMC", junk, NULL}, NULL, NULL, &err)); /* the VMC folder takes cards only */
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"REMOVE_FILE", "+OPL", "VMC", "notes.txt", NULL}, NULL, NULL, &err));
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"RENAME_FILE", "+OPL", "VMC", "card.bin", "card.txt", NULL}, NULL, NULL, &err));
    g_clear_error(&err);

    printf(failures ? "\n%d FAILED\n" : "\nall ok\n", failures);
    return failures != 0;
}
