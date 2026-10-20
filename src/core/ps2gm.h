/*
 * Memento - core library (no GUI code in here).
 *
 * Everything the GTK front end and the privileged helper share: PS1 BIN/CUE -> VCD conversion,
 * game ID detection, art download, OPL config files, and small string helpers.
 */
#ifndef PS2GM_H
#define PS2GM_H

#include <glib.h>
#include <glib/gi18n-lib.h> /* _() and N_(): texts shown to the user are translated, see po/ */
#include <stdint.h>

#define PS2GM_ERROR (ps2gm_error_quark())
GQuark ps2gm_error_quark(void);

typedef enum {
    PS2GM_ERROR_FAILED,
    PS2GM_ERROR_BAD_CUE,
    PS2GM_ERROR_IO,
    PS2GM_ERROR_NET,
    PS2GM_ERROR_TOOL,
    PS2GM_ERROR_REFUSED,
    PS2GM_ERROR_BAD_ARG,
} Ps2gmError;

/* progress: done/total in bytes (or steps); return FALSE to cancel */
typedef gboolean (*Ps2gmProgress)(guint64 done, guint64 total, const char *what, gpointer user_data);

/* ---- vcd.c : PS1 BIN/CUE -> POPStarter VCD ----------------------------------------------------- */

#define VCD_HEADER_SIZE 0x100000 /* 1 MiB header in front of the raw 2352-byte sectors */
#define CD_SECTOR_SIZE  2352

typedef struct {
    int number;
    gboolean audio;
    gboolean has_index00;
    guint32 index00; /* sectors */
    guint32 index01; /* sectors */
    int sector_size;
} Ps1Track;

typedef struct {
    char *path;     /* path of the BIN (resolved against the CUE's folder) */
    guint64 size;
    GArray *tracks; /* of Ps1Track */
} Ps1File;

typedef struct {
    GPtrArray *files; /* of Ps1File */
} Ps1Cue;

Ps1Cue *ps1cue_parse(const char *cue_path, GError **error);
void ps1cue_free(Ps1Cue *cue);
guint ps1cue_track_count(const Ps1Cue *cue);

/*
 * Writes <vcd_path>: 1 MiB header + the disc data. gap_adjust_seconds is the optional +2/-2 s tweak
 * some problem discs need (0 = none). The VCD is written to a temporary name and renamed on success.
 */
gboolean ps1_make_vcd(const char *cue_path, const char *vcd_path, int gap_adjust_seconds,
                      Ps2gmProgress progress, gpointer user_data, GError **error);

/* Game ID found in the first sectors of a BIN ("SLES_026.98"), or NULL. Free with g_free(). */
char *ps1_detect_game_id(const char *bin_path);

#endif
