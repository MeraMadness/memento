/* ps2gm-redump update|info <ps2|ps1>, ps2gm-redump check <ps2|ps1> <file>: the Redump check without the window. */
#include <stdio.h>
#include <string.h>

#include "redump.h"

static gboolean show(guint64 done, guint64 total, const char *what, gpointer p)
{
    (void)p;
    fprintf(stderr, "\r%s %3d%%", what, total ? (int)(done * 100 / total) : 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    Ps2gmPlatform pl;
    GError *err = NULL;

    if (argc < 3 || (strcmp(argv[2], "ps2") && strcmp(argv[2], "ps1"))) {
        fprintf(stderr, "usage: %s update|info ps2|ps1\n       %s check ps2|ps1 <file>\n", argv[0], argv[0]);
        return 2;
    }
    pl = !strcmp(argv[2], "ps1") ? PS2GM_PS1 : PS2GM_PS2;
    if (!strcmp(argv[1], "update")) {
        if (!redump_update(pl, &err)) {
            fprintf(stderr, "%s\n", err->message);
            return 1;
        }
    } else if (!strcmp(argv[1], "info")) {
        char *i = redump_info(pl);

        printf("%s\n", i ? i : "(no list)");
        g_free(i);
        return 0;
    } else if (!strcmp(argv[1], "check") && argc >= 4) {
        char *md5, *name = NULL;
        guint64 size;
        RedumpResult r;

        if (!redump_hash_file(argv[3], &md5, &size, show, NULL, &err)) {
            fprintf(stderr, "\n%s\n", err->message);
            return 1;
        }
        r = redump_lookup(pl, md5, size, &name);
        fprintf(stderr, "\n");
        printf("%s  %llu bytes: %s%s\n", md5, (unsigned long long)size, r == REDUMP_MATCH ? "MATCH " : r == REDUMP_UNKNOWN ? "not in the list" : "no list",
               name ? name : "");
        return r == REDUMP_MATCH ? 0 : 3;
    }
    return 0;
}
