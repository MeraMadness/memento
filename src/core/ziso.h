/*
 * ZSO ("ZISO"): a PS2 disc image compressed block by block with LZ4, as OPL reads it from USB and network shares.
 * A PS2 hard disk holds games in another format, so Memento unpacks a .zso into a temporary ISO before installing.
 *
 * Layout: 24-byte header ("ZISO", header size, total bytes u64, block size u32, version u8, index shift u8, 2 spare),
 * then (blocks + 1) u32 offsets (shifted left by the index shift; bit 31 set = the block is stored as it is), then the
 * blocks. A compressed block is a raw LZ4 block that may be followed by padding.
 */
#ifndef PS2GM_ZISO_H
#define PS2GM_ZISO_H

#include "ps2gm.h"

typedef struct {
    guint64 total_bytes; /* size of the ISO inside */
    guint32 block_size;
    guint8 shift;
} ZisoInfo;

/* TRUE if the file starts with the ZSO signature (the extension is not trusted) */
gboolean ziso_is_zso(const char *path);
/* reads and checks the header; the file may still be damaged further in */
gboolean ziso_info(const char *path, ZisoInfo *info, GError **error);
/*
 * Writes the ISO inside to iso_path. Refuses up front when the folder of iso_path has too little free space.
 * progress: done/total in bytes, return FALSE to cancel (the partial ISO is deleted). On any failure nothing is left behind.
 */
gboolean ziso_unpack(const char *zso_path, const char *iso_path, Ps2gmProgress progress, gpointer user_data, GError **error);

typedef struct {
    char *id;       /* SLES_539.08 */
    char *label;    /* the disc's volume label */
    char media[4];  /* "CD" or "DVD", decided the way hdl_dump does */
    guint64 size_kb;/* size of the ISO inside (what an unpacked ISO would be) */
} ZisoGame;
void ziso_game_clear(ZisoGame *game);
/* reads the game ID, label and kind straight from the compressed file, without unpacking it */
gboolean ziso_game_info(const char *path, ZisoGame *game, GError **error);

/*
 * Compresses an ISO into a ZSO (2048-byte blocks, LZ4 high compression, all CPU cores). The ISO must be made of whole
 * 2048-byte sectors. progress: done/total bytes of the ISO, return FALSE to cancel. Nothing is left behind on failure
 * or cancel. The file is padded to a whole number of 2048-byte sectors, see ziso_needs_padding.
 * Needs about the size of the ISO free in the folder of zso_path (the result is usually much smaller).
 */
gboolean ziso_pack(const char *iso_path, const char *zso_path, Ps2gmProgress progress, gpointer user_data, GError **error);
/*
 * A quick estimate of how much compressing an ISO would save: samples about 48 MB spread over the whole image (a second or
 * two) and returns compressed size / original size (1.0 = no saving). Real results can differ by a few percent.
 */
gboolean ziso_estimate(const char *iso_path, double *ratio, GError **error);

/* the same with a fixed index shift 0..3 (what ziso_pack picks by itself; this is for tests of every shift) */
gboolean ziso_pack_shift(const char *iso_path, const char *zso_path, int shift, Ps2gmProgress progress, gpointer user_data, GError **error);
/* the index shift a ZSO of that many bytes needs so that every block offset fits (-1: too big for the format) */
int ziso_pick_shift(guint64 total_bytes);

/*
 * Measured: hdl_dump stores a ZSO whose size is a whole number of 2048-byte sectors completely, but loses the last full
 * sector plus the remainder of one that is not. A ZSO from another tool may not be padded: ziso_needs_padding says so,
 * and ziso_pad_copy writes a copy that is (the original is not touched).
 */
gboolean ziso_needs_padding(const char *zso_path);
gboolean ziso_pad_copy(const char *zso_path, const char *copy_path, Ps2gmProgress progress, gpointer user_data, GError **error);

/* the LZ4 block decoder, exposed for tests: decodes exactly `target` bytes; trailing bytes of src are ignored */
gboolean ziso_lz4_decode(const guint8 *src, gsize src_len, guint8 *dst, gsize target);

#endif
