/*
 * Redump lists: a zip made here (deflated and stored), a small DAT, the index and the lookups, hashing a file.
 */
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <glib/gstdio.h>

#include "redump.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void w16(GByteArray *a, guint v) { guint8 b[2] = {v, v >> 8}; g_byte_array_append(a, b, 2); }
static void w32(GByteArray *a, guint32 v) { guint8 b[4] = {v, v >> 8, v >> 16, v >> 24}; g_byte_array_append(a, b, 4); }

/* a one-file zip; method 8 = deflate, 0 = stored */
static GBytes *make_zip(const char *name, const char *text, int method)
{
    GByteArray *z = g_byte_array_new();
    gsize n = strlen(text), clen = n;
    guint8 *comp = g_malloc(n + 1024);
    guint32 crc = crc32(0, (const Bytef *)text, n), cd;

    if (method == 8) {
        z_stream s = {0};

        deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        s.next_in = (Bytef *)text;
        s.avail_in = n;
        s.next_out = comp;
        s.avail_out = n + 1024;
        deflate(&s, Z_FINISH);
        clen = s.total_out;
        deflateEnd(&s);
    } else
        memcpy(comp, text, n);
    w32(z, 0x04034b50); w16(z, 20); w16(z, 0); w16(z, method); w16(z, 0); w16(z, 0);
    w32(z, crc); w32(z, clen); w32(z, n); w16(z, strlen(name)); w16(z, 0);
    g_byte_array_append(z, (const guint8 *)name, strlen(name));
    g_byte_array_append(z, comp, clen);
    cd = z->len;
    w32(z, 0x02014b50); w16(z, 20); w16(z, 20); w16(z, 0); w16(z, method); w16(z, 0); w16(z, 0);
    w32(z, crc); w32(z, clen); w32(z, n); w16(z, strlen(name)); w16(z, 0); w16(z, 0); w16(z, 0); w16(z, 0); w32(z, 0); w32(z, 0);
    g_byte_array_append(z, (const guint8 *)name, strlen(name));
    w32(z, 0x06054b50); w16(z, 0); w16(z, 0); w16(z, 1); w16(z, 1); w32(z, z->len - cd); w32(z, cd); w16(z, 0);
    g_free(comp);
    return g_byte_array_free_to_bytes(z);
}

static const char *DAT =
    "<?xml version=\"1.0\"?>\n<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" \"http://www.logiqx.com/Dats/datafile.dtd\">\n"
    "<datafile><header><name>Sony - PlayStation 2</name></header>\n"
    "<game name=\"Test Game &amp; Friends (Europe)\"><category>Games</category><rom name=\"Test Game.iso\" size=\"3\" crc=\"352441c2\" "
    "md5=\"900150983CD24FB0D6963F7D28E17F72\" sha1=\"a9993e364706816aba3e25717850c26c9cd0d89d\"/></game>\n"
    "<game name=\"Two Tracks\"><rom name=\"t (Track 1).bin\" size=\"10\" md5=\"11111111111111111111111111111111\"/>"
    "<rom name=\"t (Track 2).bin\" size=\"20\" md5=\"22222222222222222222222222222222\"/></game>\n</datafile>\n";

int main(void)
{
    char *tmp = g_dir_make_tmp("redump-XXXXXX", NULL), *file, *md5 = NULL, *name = NULL, *info;
    guint64 size = 0;
    GError *err = NULL;
    GBytes *zip;
    int method;

    g_setenv("XDG_CACHE_HOME", tmp, TRUE);
    printf("== no list yet\n");
    CHECK(redump_info(PS2GM_PS2) == NULL);
    CHECK(redump_lookup(PS2GM_PS2, "900150983cd24fb0d6963f7d28e17f72", 3, NULL) == REDUMP_NO_LIST);

    for (method = 0; method <= 8; method += 8) {
        printf("== a list in a zip (method %d)\n", method);
        zip = make_zip("Sony - PlayStation 2 - Datfile.dat", DAT, method);
        CHECK(redump_import_zip(PS2GM_PS2, zip, &err));
        g_clear_error(&err);
        g_bytes_unref(zip);
        info = redump_info(PS2GM_PS2);
        CHECK(info != NULL && strstr(info, "3 images") != NULL);
        g_free(info);
        CHECK(redump_lookup(PS2GM_PS2, "900150983CD24FB0D6963F7D28E17F72", 3, &name) == REDUMP_MATCH); /* upper case is fine */
        CHECK(name != NULL && !strcmp(name, "Test Game & Friends (Europe)"));
        g_free(name);
        CHECK(redump_lookup(PS2GM_PS2, "900150983cd24fb0d6963f7d28e17f72", 4, NULL) == REDUMP_UNKNOWN); /* same hash, other size */
        CHECK(redump_lookup(PS2GM_PS2, "00000000000000000000000000000000", 3, NULL) == REDUMP_UNKNOWN);
        CHECK(redump_lookup(PS2GM_PS2, "22222222222222222222222222222222", 20, &name) == REDUMP_MATCH && !strcmp(name, "Two Tracks"));
        g_free(name);
        CHECK(redump_lookup(PS2GM_PS1, "22222222222222222222222222222222", 20, NULL) == REDUMP_NO_LIST); /* the lists are per platform */
    }

    printf("== things that are not lists\n");
    zip = g_bytes_new("not a zip at all, but long enough to look at", 44);
    CHECK(!redump_import_zip(PS2GM_PS2, zip, &err) && err != NULL);
    g_clear_error(&err);
    g_bytes_unref(zip);
    zip = make_zip("x.dat", "<datafile><game name=\"a\"></game></datafile>", 8);
    CHECK(!redump_import_zip(PS2GM_PS2, zip, &err)); /* no images in it */
    g_clear_error(&err);
    g_bytes_unref(zip);
    CHECK(redump_lookup(PS2GM_PS2, "900150983cd24fb0d6963f7d28e17f72", 3, NULL) == REDUMP_MATCH); /* the good list survived the bad ones */

    printf("== hashing a file\n");
    file = g_build_filename(tmp, "abc.iso", NULL);
    g_file_set_contents(file, "abc", 3, NULL);
    CHECK(redump_hash_file(file, &md5, &size, NULL, NULL, &err));
    CHECK(md5 != NULL && !strcmp(md5, "900150983cd24fb0d6963f7d28e17f72") && size == 3);
    CHECK(redump_lookup(PS2GM_PS2, md5, size, &name) == REDUMP_MATCH);
    g_free(md5);
    g_free(name);
    CHECK(!redump_hash_file("/nonexistent/file.iso", &md5, &size, NULL, NULL, &err) && err != NULL);
    g_clear_error(&err);

    printf(failures ? "\n%d FAILED\n" : "\nall ok\n", failures);
    return failures != 0;
}
