/* Backend + library against the test disk: PS2GM_TEST_DISK, PS2GM_TEST_HELPER (a copy of ps2gm-helper next to the tools) */
#include <stdio.h>
#include <glib/gstdio.h>
#include "backend.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    GError *err = NULL;
    Backend *b;
    Library *lib;
    char *id = NULL, *title = NULL;

    CHECK(vcd_name_parse("SLES_026.98.Resident Evil 3.VCD", &id, &title));
    CHECK(!g_strcmp0(id, "SLES_026.98") && !g_strcmp0(title, "Resident Evil 3"));
    g_free(id); g_free(title);
    CHECK(!vcd_name_parse("Crash Bandicoot.VCD", NULL, NULL));
    CHECK(!vcd_name_parse("SLES_026.98.Game.vcdx", NULL, NULL));

    b = backend_start(g_getenv("PS2GM_TEST_DISK"), FALSE, g_getenv("PS2GM_TEST_HELPER"), &err);
    CHECK(b != NULL);
    if (!b) { printf("%s\n", err->message); return 1; }
    lib = backend_library(b, &err);
    CHECK(lib != NULL && lib->total_mb > 1000);
    CHECK(lib && lib->games->len == 0);
    printf("   +OPL %lu/%lu MB, __.POPS %lu/%lu MB free\n", (gulong)lib->opl_free_mb, (gulong)lib->opl_size_mb, (gulong)lib->pops_free_mb, (gulong)lib->pops_size_mb);
    CHECK(lib && lib->opl_size_mb > 100 && lib->opl_free_mb > 50 && lib->pops_size_mb >= 1000 && lib->pops_free_mb > 900);
    library_free(lib);

    printf("== art cache\n");
    {
        gchar *png = g_build_filename(g_get_tmp_dir(), "ps2gm-backend-art.png", NULL);
        gchar *cache = g_build_filename(g_get_tmp_dir(), "ps2gm-backend-cache", NULL), *cached;
        char *put[] = {"PUT", "+OPL", "ART", png, NULL};
        char *names[] = {"ps2gm-backend-art.png", "MISSING.png", NULL};
        gchar *data = g_malloc0(5000);
        guint copied;
        GStatBuf st;

        g_file_set_contents(png, data, 5000, NULL);
        CHECK(backend_run(b, put, NULL, NULL, &err));
        lib = backend_library(b, &err);
        CHECK(lib && library_art_size(lib, "ps2gm-backend-art.png") == 5000);
        CHECK(lib && library_art_size(lib, "MISSING.png") == 0);
        copied = backend_cache_art(b, lib, names, cache, &err);
        cached = g_build_filename(cache, "ps2gm-backend-art.png", NULL);
        CHECK(copied == 1 && g_stat(cached, &st) == 0 && st.st_size == 5000);
        CHECK(backend_cache_art(b, lib, names, cache, &err) == 0); /* up to date: nothing is read again */
        g_unlink(cached);
        CHECK(backend_cache_art(b, lib, names, cache, &err) == 1); /* gone from the cache: read again */
        {
            char *rm[] = {"REMOVE_FILE", "+OPL", "ART", "ps2gm-backend-art.png", NULL};
            CHECK(backend_run(b, rm, NULL, NULL, &err));
        }
        g_unlink(cached);
        g_rmdir(cache);
        g_unlink(png);
        library_free(lib);
        g_free(cached);
        g_free(cache);
        g_free(png);
        g_free(data);
    }
    {
        char *bad[] = {"NOPE", NULL};
        CHECK(!backend_run(b, bad, NULL, NULL, &err));
        printf("   expected: %s\n", err->message);
        g_clear_error(&err);
    }
    backend_stop(b);
    CHECK(backend_start("/etc/hostname", FALSE, g_getenv("PS2GM_TEST_HELPER"), &err) == NULL);
    printf("   expected: %s\n", err->message);
    printf(failures ? "\n%d FAILURES\n" : "\nall ok\n", failures);
    return failures != 0;
}
