/*
 * Checking a disc image against the Redump lists (redump.org): a database of the hashes of good dumps, free to
 * download. The lists are downloaded once (about a megabyte each), reduced to a small index in the user's cache folder
 * and used offline from then on.
 */
#ifndef PS2GM_REDUMP_H
#define PS2GM_REDUMP_H

#include "ps2gm.h"
#include "art.h" /* Ps2gmPlatform */

/* downloads the list of this platform (PS2GM_PS2 or PS2GM_PS1) and rebuilds the index */
gboolean redump_update(Ps2gmPlatform platform, GError **error);

/* the same from a zip or from the DAT (XML) text already in memory; the platform's old index is replaced */
gboolean redump_import_zip(Ps2gmPlatform platform, GBytes *zip, GError **error);
gboolean redump_import_dat(Ps2gmPlatform platform, const char *xml, gsize len, GError **error);

/* "11774 images, downloaded 2026-10-20", or NULL when the list was never downloaded. Free with g_free(). */
char *redump_info(Ps2gmPlatform platform);

/* MD5 (lowercase hex) and size of a file; progress may be NULL */
gboolean redump_hash_file(const char *path, char **md5, guint64 *size, Ps2gmProgress progress, gpointer user_data, GError **error);

typedef enum {
    REDUMP_NO_LIST,   /* the list of this platform was never downloaded */
    REDUMP_MATCH,     /* a known good dump: name is its title in Redump */
    REDUMP_UNKNOWN,   /* not in the list (another dump, a modified or damaged image, or a game Redump does not have) */
} RedumpResult;

/* name (free with g_free) is set for REDUMP_MATCH */
RedumpResult redump_lookup(Ps2gmPlatform platform, const char *md5, guint64 size, char **name);

#endif
