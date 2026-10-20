/* Command line front end for the VCD converter (used by the tests and handy on its own). */
#include <stdio.h>
#include <stdlib.h>
#include "ps2gm.h"

static gboolean progress(guint64 done, guint64 total, const char *what, gpointer ud)
{
    (void)ud;
    fprintf(stderr, "\r%s: %3d%%", what, total ? (int)(done * 100 / total) : 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    GError *err = NULL;
    int gap = argc > 3 ? atoi(argv[3]) : 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s game.cue out.vcd [gap seconds: 2 or -2]\n"
                        "       %s --id game.bin\n", argv[0], argv[0]);
        return 2;
    }
    if (!g_strcmp0(argv[1], "--id")) {
        char *id = ps1_detect_game_id(argv[2]);

        printf("%s\n", id ? id : "(none)");
        g_free(id);
        return 0;
    }
    if (!ps1_make_vcd(argv[1], argv[2], gap, progress, NULL, &err)) {
        fprintf(stderr, "\nerror: %s\n", err->message);
        return 1;
    }
    fprintf(stderr, "\n");
    return 0;
}
