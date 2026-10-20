/* Command line front end for art download and title lookup (tests, and handy on its own). */
#include <stdio.h>
#include "art.h"

static gboolean progress(guint64 done, guint64 total, const char *what, gpointer ud)
{
    (void)ud;
    fprintf(stderr, "  [%u/%u] %s\n", (unsigned)done + 1, (unsigned)total, what);
    return TRUE;
}

int main(int argc, char **argv)
{
    GError *err = NULL;

    if (argc >= 3 && !g_strcmp0(argv[1], "--title")) {
        char *t = ps2_title_lookup(argv[2]);

        printf("%s\n", t ? t : "(unknown)");
        g_free(t);
        return 0;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: %s ID ps2|ps1 outdir\n       %s --title ID\n", argv[0], argv[0]);
        return 2;
    }
    {
        char **files = art_fetch(argv[1], !g_strcmp0(argv[2], "ps1") ? PS2GM_PS1 : PS2GM_PS2, argv[3], NULL,
                                 progress, NULL, &err);
        int i;

        if (files == NULL) {
            fprintf(stderr, "error: %s\n", err->message);
            return 1;
        }
        for (i = 0; files[i]; i++)
            printf("%s\n", files[i]);
        g_strfreev(files);
    }
    return 0;
}
