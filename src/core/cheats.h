/*
 * Cheats for OPL: +OPL/CHT/<Game ID>.cht, read by OPL's PS2RD cheat engine. The format (from OPL's cheatman.c):
 * a line with the name of a cheat, followed by its code lines "AAAAAAAA VVVVVVVV" (16 hex digits), repeated.
 * "//" and "#" start a comment. At most 250 cheats; lines up to 255 characters; names up to 128.
 * Turning the cheats on for a game is done in the game's +OPL/CFG/<ID>.cfg ($EnableCheat=1 ...).
 */
#ifndef PS2GM_CHEATS_H
#define PS2GM_CHEATS_H

#include "ps2gm.h"

typedef struct {
    guint cheats; /* names that have at least one code */
    guint codes;
} CheatInfo;

/*
 * Checks a cheat list the way OPL reads it and returns it cleaned up (line endings, BOM, trailing newline) in
 * *out (g_free). Fails if there is not a single usable cheat, or if the text is something OPL would choke on.
 * info may be NULL.
 */
gboolean cheats_normalize(const char *text, char **out, CheatInfo *info, GError **error);

/*
 * Sets "key=value" lines in the text of an OPL config file: an existing line of that key is replaced, else the
 * line is added. pairs: key, value, key, value ... NULL. old_text may be NULL (new file). Returns g_free text.
 */
char *cfg_set_keys(const char *old_text, const char *const *pairs);

#endif
