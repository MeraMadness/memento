/*
 * PS1 fixes (CHEATS.TXT of a game folder): reading and writing the file text, and the trip through the disk.
 * Needs PS2GM_TEST_DISK and PS2GM_TEST_HELPER (set by run_tests.sh).
 */
#include <stdio.h>
#include <string.h>

#include "ops.h"
#include "ps1fix.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    GError *err = NULL;
    Ps1Fixes *f;
    char *text = NULL, *back = NULL;
    Backend *b;
    OpsContext *ops;

    printf("== the file text\n");
    f = ps1fix_parse(NULL);
    CHECK(!f->cache1 && !f->subcdstatus && !f->codecache0 && !f->undo_fixes && f->crlf && !strcmp(f->other, ""));
    CHECK(ps1fix_compose(f, &text, &err) && text == NULL); /* nothing set: no file */
    ps1fix_free(f);

    f = ps1fix_parse("$CACHE1\r\n$SUBCDSTATUS\r\n\r\n$80123456 00FF\r\n$C0 8001abcd\r\n");
    CHECK(f->cache1 && f->subcdstatus && !f->codecache0 && !f->undo_fixes && f->crlf);
    CHECK(!strcmp(f->other, "$80123456 00FF\n$C0 8001abcd")); /* codes the program does not know stay as they are */
    f->codecache0 = TRUE;
    CHECK(ps1fix_compose(f, &text, &err));
    CHECK(!g_strcmp0(text, "$CACHE1\r\n$SUBCDSTATUS\r\n$CODECACHE_ADDON_0\r\n$80123456 00FF\r\n$C0 8001abcd\r\n"));
    g_free(text);
    ps1fix_free(f);

    f = ps1fix_parse("$UNDO_GAME_FIXES\n"); /* a file with Unix line ends keeps them */
    CHECK(f->undo_fixes && !f->crlf);
    CHECK(ps1fix_compose(f, &text, &err) && !g_strcmp0(text, "$UNDO_GAME_FIXES\n"));
    g_free(text);
    ps1fix_free(f);

    f = ps1fix_parse(NULL);
    g_free(f->other);
    f->other = g_strdup("$OK 1\n$caf\xc3\xa9"); /* not plain ASCII */
    CHECK(!ps1fix_compose(f, &text, &err) && err != NULL);
    g_clear_error(&err);
    g_free(f->other);
    f->other = g_strnfill(300, 'x');
    CHECK(!ps1fix_compose(f, &text, &err) && err != NULL); /* a line that is far too long */
    g_clear_error(&err);
    ps1fix_free(f);

    printf("== the file on the disk\n");
    b = backend_start(g_getenv("PS2GM_TEST_DISK"), FALSE, g_getenv("PS2GM_TEST_HELPER"), &err);
    if (b == NULL) {
        printf("%s\n", err->message);
        return 1;
    }
    ops = ops_new(b, NULL);
    CHECK(ops_get_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", &back, &err) && back == NULL); /* nothing yet, not an error */
    g_clear_error(&err);
    CHECK(ops_set_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", "$CACHE1\r\n$SUBCDSTATUS\r\n", &err));
    if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
    CHECK(ops_get_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", &back, &err) && !g_strcmp0(back, "$CACHE1\r\n$SUBCDSTATUS\r\n"));
    g_free(back);
    back = NULL;
    CHECK(ops_set_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", "$UNDO_GAME_FIXES\r\n", &err)); /* replaces it */
    CHECK(ops_get_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", &back, &err) && !g_strcmp0(back, "$UNDO_GAME_FIXES\r\n"));
    g_free(back);
    back = NULL;
    CHECK(ops_set_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", NULL, &err)); /* remove */
    CHECK(ops_get_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", &back, &err) && back == NULL);
    CHECK(ops_set_ps1_fixes(ops, "SLPS_020.38.Racing Lagoon.VCD", NULL, &err)); /* removing what is not there is fine */

    printf("== fix files (TROJAN_#.BIN / PATCH_#.BIN)\n");
    {
        GPtrArray *files = NULL;
        char *tmp = g_dir_make_tmp("fix-XXXXXX", NULL), *good = g_build_filename(tmp, "TROJAN_2.BIN", NULL), *bad = g_build_filename(tmp, "evil.exe", NULL);
        char *lower = g_build_filename(tmp, "trojan_2.bin", NULL);

        CHECK(ps1fix_is_fix_file_name("TROJAN_2.BIN") && ps1fix_is_fix_file_name("PATCH_12.BIN") && ps1fix_is_fix_file_name("TROJAN_0.BIN"));
        CHECK(!ps1fix_is_fix_file_name("TROJAN_.BIN") && !ps1fix_is_fix_file_name("TROJAN_123.BIN") && !ps1fix_is_fix_file_name("trojan_2.bin") &&
              !ps1fix_is_fix_file_name("TROJAN_2.BINX") && !ps1fix_is_fix_file_name("../TROJAN_2.BIN") && !ps1fix_is_fix_file_name("SLOT0.VMC"));
        g_file_set_contents(good, "0123456789abcdef", -1, NULL);
        g_file_set_contents(bad, "MZ", -1, NULL);
        g_file_set_contents(lower, "x", -1, NULL);
        CHECK(ops_list_fix_files(ops, "SLPS_020.38.Racing Lagoon.VCD", &files, &err) && files != NULL && files->len == 0);
        g_ptr_array_free(files, TRUE);
        CHECK(ops_add_fix_file(ops, "SLPS_020.38.Racing Lagoon.VCD", good, &err));
        if (err) { printf("   %s\n", err->message); g_clear_error(&err); }
        CHECK(ops_list_fix_files(ops, "SLPS_020.38.Racing Lagoon.VCD", &files, &err) && files->len == 1 &&
              !g_strcmp0(((LibFile *)g_ptr_array_index(files, 0))->name, "TROJAN_2.BIN") && ((LibFile *)g_ptr_array_index(files, 0))->size == 16);
        g_ptr_array_free(files, TRUE);
        CHECK(!ops_add_fix_file(ops, "SLPS_020.38.Racing Lagoon.VCD", bad, &err)); /* not a fix file name */
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!ops_add_fix_file(ops, "SLPS_020.38.Racing Lagoon.VCD", lower, &err)); /* the name must be in capitals, as POPStarter looks for it */
        g_clear_error(&err);
        CHECK(!backend_run(b, (char *[]){"PUT", "__common", "POPS/SLPS_020.38.Racing Lagoon", bad, NULL}, NULL, NULL, &err)); /* the helper checks too */
        printf("   expected: %s\n", err ? err->message : "(none)");
        g_clear_error(&err);
        CHECK(!backend_run(b, (char *[]){"REMOVE_FILE", "__common", "POPS/SLPS_020.38.Racing Lagoon", "SLOT0.VMC", NULL}, NULL, NULL, &err)); /* a game's card is not removable here */
        g_clear_error(&err);
        CHECK(!ops_remove_fix_file(ops, "SLPS_020.38.Racing Lagoon.VCD", "SLOT0.VMC", &err));
        g_clear_error(&err);
        CHECK(ops_remove_fix_file(ops, "SLPS_020.38.Racing Lagoon.VCD", "TROJAN_2.BIN", &err));
        CHECK(ops_list_fix_files(ops, "SLPS_020.38.Racing Lagoon.VCD", &files, &err) && files->len == 0);
        g_ptr_array_free(files, TRUE);
        g_free(good);
        g_free(bad);
        g_free(lower);
        g_free(tmp);
    }

    printf("== refusals\n");
    CHECK(!ops_get_ps1_fixes(ops, "../evil.VCD", &back, &err));
    g_clear_error(&err);
    CHECK(!ops_set_ps1_fixes(ops, "not-a-vcd.txt", "$X\r\n", &err));
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"GET_POPS", "x", "SLOT0.VMC", NULL}, NULL, NULL, &err)); /* only CHEATS.TXT */
    printf("   expected: %s\n", err ? err->message : "(none)");
    g_clear_error(&err);
    CHECK(!backend_run(b, (char *[]){"GET_POPS", "a/b", "CHEATS.TXT", NULL}, NULL, NULL, &err));
    g_clear_error(&err);

    printf(failures ? "\n%d FAILED\n" : "\nall ok\n", failures);
    return failures != 0;
}
