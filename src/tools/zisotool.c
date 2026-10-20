/* ps2gm-ziso: pack, unpack or inspect ZSO files from the command line (the same code Memento uses). */
#include <stdio.h>
#include <string.h>

#include "ziso.h"

static gboolean show(guint64 done, guint64 total, const char *what, gpointer ud)
{
    (void)ud;
    fprintf(stderr, "\r%s %3u%%", what, (unsigned)(done * 100 / total));
    return TRUE;
}

int main(int argc, char **argv)
{
    GError *err = NULL;
    gboolean ok = FALSE;

    if (argc == 4 && !strcmp(argv[1], "pack"))
        ok = ziso_pack(argv[2], argv[3], show, NULL, &err);
    else if (argc == 4 && !strcmp(argv[1], "unpack"))
        ok = ziso_unpack(argv[2], argv[3], show, NULL, &err);
    else if (argc == 3 && !strcmp(argv[1], "estimate")) {
        double r = 0;

        if ((ok = ziso_estimate(argv[2], &r, &err)))
            printf("estimated size after compressing: %.1f%% of the original (saves about %.1f%%)\n", r * 100, (1 - r) * 100);
    } else if (argc == 3 && !strcmp(argv[1], "info")) {
        ZisoGame g;
        ZisoInfo i;

        if ((ok = ziso_info(argv[2], &i, &err))) {
            printf("ISO size %lu bytes, block %u, index shift %u\n", (gulong)i.total_bytes, i.block_size, i.shift);
            if (ziso_game_info(argv[2], &g, NULL)) {
                printf("game %s, %s, label \"%s\"\n", g.id, g.media, g.label);
                ziso_game_clear(&g);
            }
        }
    } else {
        fprintf(stderr, "usage: %s pack <in.iso> <out.zso> | estimate <in.iso> | unpack <in.zso> <out.iso> | info <file.zso>\n", argv[0]);
        return 2;
    }
    fprintf(stderr, "\n");
    if (!ok) {
        fprintf(stderr, "%s\n", err ? err->message : "failed");
        return 1;
    }
    return 0;
}
