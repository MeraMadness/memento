/*
 * The things the application does, as whole workflows (used by the GUI and by the tests):
 * install a PS2 ISO, convert and install PS1 discs, fetch art, remove games.
 *
 * Progress callbacks get overall progress in thousandths (done 0..1000 of total 1000) and a message.
 */
#ifndef PS2GM_OPS_H
#define PS2GM_OPS_H

#include "backend.h"
#include "art.h"
#include "cheats.h"
#include "gamecfg.h"

typedef struct {
    Backend *backend;
    char *work_dir;   /* where temporary VCDs and art are made; created on demand */
} OpsContext;

typedef struct {
    char *cue_path;
    char *id;         /* SLES_026.98 */
} Ps1Disc;

OpsContext *ops_new(Backend *backend, const char *work_dir);
void ops_free(OpsContext *ctx);

/* "Resident Evil: 3 / Nemesis" -> "Resident Evil 3 Nemesis": safe for file names and the disk tools */
char *ops_clean_title(const char *title);

/* notes (optional, of char *): non-fatal things the user should know, e.g. art skipped for lack of space */
gboolean ops_install_ps2(OpsContext *ctx, const char *iso, const char *media, guint64 size_kb, const char *id,
                         const char *title, gboolean with_art, GPtrArray *notes, Ps2gmProgress cb,
                         gpointer ud, GError **error);

/* discs: Ps1Disc, in disc order; 1 disc = a normal game, 2-4 discs = multi-disc with DISCS.TXT */
gboolean ops_install_ps1(OpsContext *ctx, GPtrArray *discs, const char *title, gboolean with_art,
                         GPtrArray *notes, Ps2gmProgress cb, gpointer ud, GError **error);

gboolean ops_update_art(OpsContext *ctx, const char *id, Ps2gmPlatform platform, const char *title,
                        gboolean replace, GPtrArray *notes, Ps2gmProgress cb, gpointer ud, GError **error);

/*
 * Makes the discs of an installed multi-disc game share one memory card (the one of the first disc), like a
 * new install does. vcd_names: the .VCD file names in __.POPS, in disc order (2 to 4).
 * Saves already on the other discs' own cards are not merged: they stay where they are, unused.
 */
gboolean ops_share_memcard(OpsContext *ctx, char *const *vcd_names, GError **error);

/*
 * Replaces one picture of a game (key: COV, COV2, BG, LGO, ICO, SCR, SCR2, see art_types).
 * source: NULL = download the usual one again, "http://..."/"https://..." = that address, else a local file.
 */
gboolean ops_set_art(OpsContext *ctx, const char *id, Ps2gmPlatform platform, const char *key, const char *source,
                     gboolean whole, Ps2gmProgress cb, gpointer ud, GError **error); /* whole: see art_import_bytes */

/*
 * Cheats of a PS2 game (+OPL/CHT/<id>.cht, see cheats.h). *text is NULL if the game has none.
 */
/*
 * Virtual memory cards (+OPL/VMC/<name>.bin). ops_vmc_fetch copies one to a local file. ops_vmc_store puts a modified copy back
 * safely: it is written beside the card under another name, checked by size, and only then replaces the card, so an
 * interrupted copy never leaves a half written card.
 */
gboolean ops_vmc_fetch(OpsContext *ctx, const char *name, const char *dest_path, GError **error);
gboolean ops_vmc_store(OpsContext *ctx, const char *name, const char *local_path, GError **error);

gboolean ops_get_cheats(OpsContext *ctx, const char *id, char **text, GError **error);
/*
 * Saves the cheat list (checked and cleaned up first) and, with turn_on, switches the cheat engine on for this
 * game in its config file (all cheats active). text NULL: removes the cheats and switches them off.
 */
gboolean ops_set_cheats(OpsContext *ctx, const char *id, const char *text, gboolean turn_on, CheatInfo *info,
                        GError **error);

/*
 * Programs on the APPS partition (what OPL shows in its Apps tab): .elf files in the root of the partition.
 * ops_apps_add checks that the file is a PS2 program. If a program of that name is already there and keep_old is
 * set, the old one is kept as <name>-OLD.ELF (an older backup of that name is replaced) and put back if the copy fails.
 */
gboolean ops_apps_add(OpsContext *ctx, const char *path, gboolean keep_old, GPtrArray *notes, GError **error);
gboolean ops_apps_remove(OpsContext *ctx, const char *name, GError **error);
gboolean ops_apps_rename(OpsContext *ctx, const char *from, const char *to, GError **error);
/* "OPL-PS1.ELF" -> "OPL-PS1-OLD.ELF" (g_free) */
char *ops_apps_backup_name(const char *name);

/*
 * Per-game OPL settings of a PS2 game (compatibility modes, DMA, video mode, alternative startup file), kept in
 * the game's .cfg next to everything else in it. reset removes them (back to OPL's own defaults).
 */
gboolean ops_get_settings(OpsContext *ctx, const char *id, GameSettings *settings, GError **error);
gboolean ops_set_settings(OpsContext *ctx, const char *id, const GameSettings *settings, GError **error);
gboolean ops_reset_settings(OpsContext *ctx, const char *id, GError **error);

gboolean ops_remove_ps2(OpsContext *ctx, const char *id, const char *title, GError **error);
gboolean ops_remove_ps1(OpsContext *ctx, const char *vcd_name, GError **error);

#endif
