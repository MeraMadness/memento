/*
 * Cover art, logos, backgrounds... for OPL (all PNG: OPL no longer reads JPG), and the game title
 * lookup. Network access goes through libcurl; images are converted with GdkPixbuf.
 */
#ifndef PS2GM_ART_H
#define PS2GM_ART_H

#include "ps2gm.h"

typedef enum { PS2GM_PS2, PS2GM_PS1 } Ps2gmPlatform;

/* "SLES_539.08" -> "SLES-53908" (the form the cover project and PCSX2 use) */
char *ps2gm_serial(const char *id);

/*
 * Downloads the art of one game into out_dir as <ID>_COV.png, _COV2, _ICO, _LGO, _LAB, _BG, _SCR, _SCR2.
 * Files named in skip (NULL-terminated, e.g. art that already exists on the disk) are not fetched.
 * Returns the names of the files written (g_strfreev), or NULL with an error. Missing art is not an error.
 */
char **art_fetch(const char *id, Ps2gmPlatform platform, const char *out_dir, char *const *skip,
                 Ps2gmProgress progress, gpointer user_data, GError **error);

/* The kinds of picture OPL shows for a game, with the largest size worth putting on the console */
typedef struct {
    const char *key;   /* file suffix: <ID>_<key>.png */
    const char *label; /* what to call it in the window */
    int max_w, max_h;
} ArtType;

const ArtType *art_types(guint *count);
const ArtType *art_type_find(const char *key); /* NULL if unknown */

/*
 * Your own pictures (like OPL Manager): turns an image (png, jpg, bmp, ...) into the PNG OPL wants and writes
 * <out_dir>/<ID>_<key>.png. Pictures are only ever made smaller, never stretched; a PS2 front cover is cropped
 * to the 7:10 shape of the cover frame, unless whole is set (then it is only fitted inside the frame, like the
 * other pictures: for 3D box art or covers whose edges matter). The name of the file written is in *out_name.
 */
gboolean art_import_bytes(GBytes *data, const char *id, const char *key, Ps2gmPlatform platform,
                          gboolean whole, const char *out_dir, char **out_name, GError **error);
gboolean art_import_file(const char *path, const char *id, const char *key, Ps2gmPlatform platform,
                         gboolean whole, const char *out_dir, char **out_name, GError **error);
/* http:// or https:// address of an image */
gboolean art_import_url(const char *url, const char *id, const char *key, Ps2gmPlatform platform,
                        gboolean whole, const char *out_dir, char **out_name, GError **error);
/* downloads the usual source of one picture again; error if there is none for this game */
gboolean art_fetch_one(const char *id, Ps2gmPlatform platform, const char *key, const char *out_dir,
                       char **out_name, GError **error);

/* ---- cover candidates (for the cover carousel) ---- */

typedef struct {
    char *label;    /* where it comes from, e.g. "3D box art" */
    char *path;     /* the downloaded file (png or jpg) */
    gboolean whole; /* better kept whole than cropped to the cover frame (3D box art) */
} CoverCandidate;

void cover_candidate_free(gpointer candidate);
/*
 * Downloads every front cover that can be found online for a game into out_dir (flat cover, 3D box art, the OPL
 * Manager archive's). Returns CoverCandidate entries (possibly none; sources that fail are skipped). Slow: network.
 */
GPtrArray *art_cover_candidates(const char *id, Ps2gmPlatform platform, const char *out_dir);

/* Title from the PCSX2 game index (PS2 only), cached in ~/.cache/ps2gm. NULL if unknown. */
char *ps2_title_lookup(const char *id);

/* OPL game config ("Title=..." and optional description) */
gboolean cfg_write(const char *path, const char *title, const char *description, GError **error);

/* low level: download url into memory (follows redirects, retries rate limits) */
GBytes *ps2gm_http_get(const char *url, GError **error);

#endif
