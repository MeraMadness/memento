/*
 * Per-game OPL settings in +OPL/CFG/<ID>.cfg ("key=value" lines). Only the keys listed here are read and written;
 * everything else in the file (title, cheat keys, ...) is kept as it is. Names and meanings are OPL's own
 * (include/config.h, src/guigame.c): $Compatibility is a bit mask (bit 0 = mode 1 ... bit 5 = mode 6) and is
 * removed when no mode is set; $DMA is a mode number where 7 (UDMA 4) is the default and the key is removed then.
 */
#ifndef PS2GM_GAMECFG_H
#define PS2GM_GAMECFG_H

#include "ps2gm.h"

#define GAME_COMPAT_MODES 6
#define GAME_DMA_DEFAULT 7
#define GAME_GSM_MODES 29

typedef struct {
    guint compat;        /* bit i = compatibility mode i+1 */
    int dma;             /* 0 MDMA 0, 1 MDMA 1, 2 MDMA 2, 3 UDMA 0 ... 7 UDMA 4 (the default) */
    char *alt_startup;   /* "" = none */
    gboolean gsm_on;     /* force a video mode (GS Mode Selector) for this game */
    int gsm_vmode;       /* index into the list of video modes, 0 = NTSC */
} GameSettings;

void game_settings_init(GameSettings *s);
void game_settings_clear(GameSettings *s);
/* reads the settings out of the text of a .cfg file (NULL = no file: the defaults) */
void game_settings_parse(const char *cfg_text, GameSettings *s);
/*
 * The .cfg text with these settings applied: keys at their default are removed, the others set, $ConfigSource=1
 * ("customized") added like OPL does. old_text may be NULL. Fails only for an alternative startup file with
 * characters that would break the file.
 */
char *game_settings_apply(const char *old_text, const GameSettings *s, GError **error);
/* the text with all the keys managed here removed (back to what OPL would do by itself) */
char *game_settings_reset(const char *old_text);

const char *game_dma_name(int mode);      /* "UDMA 4" */
const char *game_gsm_mode_name(int mode); /* "PAL Non Interlaced" */

#endif
