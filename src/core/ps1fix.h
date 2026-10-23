/*
 * The fixes file of a PS1 game: CHEATS.TXT in the game's folder (__common/POPS/<VCD name>/). POPStarter reads "$" lines from it:
 * a few directives that work around problems (see the community POPStarter documentation), and raw cheat codes.
 * The four directives Memento knows by name are switches; every other line is kept as it is, as long as it is plain text.
 */
#ifndef PS2GM_PS1FIX_H
#define PS2GM_PS1FIX_H

#include "ps2gm.h"

typedef struct {
    gboolean cache1;        /* $CACHE1: a read buffer of 1 sector instead of 16 (some video playback hangs) */
    gboolean subcdstatus;   /* $SUBCDSTATUS: CD status workaround for hangs and timeouts */
    gboolean codecache0;    /* $CODECACHE_ADDON_0: recompiler cache workaround for slowdowns and random hangs */
    gboolean undo_fixes;    /* $UNDO_GAME_FIXES: turns off POPStarter's built-in fixes for this game */
    char *other;            /* every other line, one per line, without the line ends */
    gboolean crlf;          /* the file uses Windows line ends (the default for a new file) */
} Ps1Fixes;

/* text: the file, or NULL when there is none yet */
Ps1Fixes *ps1fix_parse(const char *text);
void ps1fix_free(Ps1Fixes *f);

/* the file for these settings; *text is NULL when nothing is set (then the file should not exist). FALSE: a line is not plain text */
gboolean ps1fix_compose(const Ps1Fixes *f, char **text, GError **error);

/* TROJAN_<n>.BIN and PATCH_<n>.BIN: the fix files POPStarter looks for in a game's folder (n: one or two digits, capital letters) */
gboolean ps1fix_is_fix_file_name(const char *name);

#endif
