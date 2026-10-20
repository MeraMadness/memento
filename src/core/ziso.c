#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <glib/gstdio.h>

#include <lz4hc.h>

#include "ziso.h"

#define HEADER_SIZE 24
#define MAX_TOTAL ((guint64)16 << 30) /* a dual layer DVD is about 8.5 GB */
#define PLAIN_BIT 0x80000000u
#define SPACE_MARGIN ((guint64)64 << 20)

static guint32 rd32(const guint8 *p)
{
    return (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 | (guint32)p[3] << 24;
}

static guint64 rd64(const guint8 *p)
{
    return (guint64)rd32(p) | (guint64)rd32(p + 4) << 32;
}

/* ---- LZ4 -------------------------------------------------------------------------------------- */

gboolean ziso_lz4_decode(const guint8 *src, gsize src_len, guint8 *dst, gsize target)
{
    const guint8 *ip = src, *end = src + src_len;
    gsize op = 0;

    while (op < target) {
        gsize literals, match;
        unsigned token, offset;

        if (ip >= end)
            return FALSE;
        token = *ip++;
        literals = token >> 4;
        if (literals == 15) {
            unsigned b;

            do {
                if (ip >= end)
                    return FALSE;
                b = *ip++;
                literals += b;
                if (literals > target) /* also stops a run of 255s from overflowing */
                    return FALSE;
            } while (b == 255);
        }
        if (literals > (gsize)(end - ip) || literals > target - op)
            return FALSE;
        memcpy(dst + op, ip, literals);
        ip += literals;
        op += literals;
        if (op == target)
            return TRUE; /* the last sequence has literals only */

        if (end - ip < 2)
            return FALSE;
        offset = ip[0] | (unsigned)ip[1] << 8;
        ip += 2;
        if (offset == 0 || offset > op)
            return FALSE;
        match = token & 15;
        if (match == 15) {
            unsigned b;

            do {
                if (ip >= end)
                    return FALSE;
                b = *ip++;
                match += b;
                if (match > target)
                    return FALSE;
            } while (b == 255);
        }
        match += 4;
        if (match > target - op)
            return FALSE;
        for (gsize i = 0; i < match; i++, op++) /* may overlap: byte by byte */
            dst[op] = dst[op - offset];
    }
    return TRUE;
}

/* ---- the file --------------------------------------------------------------------------------- */

gboolean ziso_is_zso(const char *path)
{
    guint8 magic[4];
    FILE *f = g_fopen(path, "rb");
    gboolean ok;

    if (f == NULL)
        return FALSE;
    ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "ZISO", 4);
    fclose(f);
    return ok;
}

static gboolean read_header(FILE *f, guint64 file_size, ZisoInfo *info, GError **error)
{
    guint8 h[HEADER_SIZE];
    guint32 header_size;

    if (fread(h, 1, HEADER_SIZE, f) != HEADER_SIZE || memcmp(h, "ZISO", 4) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Not a ZSO file"));
        return FALSE;
    }
    header_size = rd32(h + 4);
    info->total_bytes = rd64(h + 8);
    info->block_size = rd32(h + 16);
    info->shift = h[21];
    if (header_size != HEADER_SIZE || h[20] != 1 || info->total_bytes == 0 || info->total_bytes > MAX_TOTAL ||
        info->block_size < 512 || info->block_size > (1u << 20) || (info->block_size & (info->block_size - 1)) != 0 ||
        info->shift > 12) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("This ZSO file has a header I do not understand"));
        return FALSE;
    }
    /* the index has to fit in the file */
    if (HEADER_SIZE + ((info->total_bytes + info->block_size - 1) / info->block_size + 1) * 4 > file_size) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("This ZSO file is incomplete or damaged"));
        return FALSE;
    }
    return TRUE;
}

static guint64 file_size_of(const char *path)
{
    GStatBuf st;

    return g_stat(path, &st) == 0 ? (guint64)st.st_size : 0;
}

gboolean ziso_info(const char *path, ZisoInfo *info, GError **error)
{
    FILE *f = g_fopen(path, "rb");
    gboolean ok;

    if (f == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), path, g_strerror(errno));
        return FALSE;
    }
    ok = read_header(f, file_size_of(path), info, error);
    fclose(f);
    return ok;
}

static void damaged(GError **error)
{
    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("This ZSO file is damaged"));
}

/* ---- reading blocks --------------------------------------------------------------------------- */

typedef struct {
    FILE *f;
    ZisoInfo info;
    guint64 fsize, blocks;
    guint32 *index;
    guint8 *raw, *plain;
} Reader;

static void reader_close(Reader *r)
{
    if (r->f != NULL)
        fclose(r->f);
    g_free(r->index);
    g_free(r->raw);
    g_free(r->plain);
    memset(r, 0, sizeof(*r));
}

static gboolean reader_open(Reader *r, const char *path, GError **error)
{
    guint8 *index_bytes;
    guint64 i;

    memset(r, 0, sizeof(*r));
    r->f = g_fopen(path, "rb");
    if (r->f == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), path, g_strerror(errno));
        return FALSE;
    }
    r->fsize = file_size_of(path);
    if (!read_header(r->f, r->fsize, &r->info, error))
        goto fail;
    r->blocks = (r->info.total_bytes + r->info.block_size - 1) / r->info.block_size;
    index_bytes = g_try_malloc((r->blocks + 1) * 4);
    r->index = g_try_malloc((r->blocks + 1) * sizeof(guint32));
    /* a block, stored, can be a little longer than block_size when the index shift pads it */
    r->raw = g_try_malloc(r->info.block_size + ((gsize)1 << r->info.shift) + 16);
    r->plain = g_try_malloc(r->info.block_size);
    if (!index_bytes || !r->index || !r->raw || !r->plain) {
        g_free(index_bytes);
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Not enough memory"));
        goto fail;
    }
    if (fread(index_bytes, 4, r->blocks + 1, r->f) != r->blocks + 1) {
        g_free(index_bytes);
        damaged(error);
        goto fail;
    }
    for (i = 0; i <= r->blocks; i++)
        r->index[i] = rd32(index_bytes + i * 4);
    g_free(index_bytes);
    return TRUE;
fail:
    reader_close(r);
    return FALSE;
}

/* decodes block i into r->plain; *len = how many bytes of it belong to the image (the last block can be short) */
static gboolean reader_block(Reader *r, guint64 i, gsize *len, GError **error)
{
    guint64 start = (guint64)(r->index[i] & ~PLAIN_BIT) << r->info.shift, stop = (guint64)(r->index[i + 1] & ~PLAIN_BIT) << r->info.shift;
    gsize want = (gsize)MIN((guint64)r->info.block_size, r->info.total_bytes - i * r->info.block_size), have;

    if (stop < start || stop > r->fsize || stop - start > r->info.block_size + ((guint64)1 << r->info.shift) + 16 || start < HEADER_SIZE) {
        damaged(error);
        return FALSE;
    }
    have = (gsize)(stop - start);
    if (fseeko(r->f, (off_t)start, SEEK_SET) != 0 || fread(r->raw, 1, have, r->f) != have) {
        damaged(error);
        return FALSE;
    }
    if (r->index[i] & PLAIN_BIT) {
        if (have < want) {
            damaged(error);
            return FALSE;
        }
        memcpy(r->plain, r->raw, want);
    } else if (!ziso_lz4_decode(r->raw, have, r->plain, want)) {
        damaged(error);
        return FALSE;
    }
    *len = want;
    return TRUE;
}

/* reads len bytes of the image starting at byte offset off (a small read: the blocks are decoded one by one) */
static gboolean reader_read(Reader *r, guint64 off, guint8 *buf, gsize len, GError **error)
{
    while (len > 0) {
        guint64 b = off / r->info.block_size;
        gsize inside = (gsize)(off % r->info.block_size), have, take;

        if (b >= r->blocks || !reader_block(r, b, &have, error)) {
            if (b >= r->blocks)
                damaged(error);
            return FALSE;
        }
        if (inside >= have) {
            damaged(error);
            return FALSE;
        }
        take = MIN(len, have - inside);
        memcpy(buf, r->plain + inside, take);
        buf += take;
        off += take;
        len -= take;
    }
    return TRUE;
}

gboolean ziso_unpack(const char *zso_path, const char *iso_path, Ps2gmProgress progress, gpointer user_data, GError **error)
{
    Reader r;
    FILE *out = NULL;
    guint64 i, done = 0, last_report = 0;
    gchar *dir;
    struct statvfs sv;
    gboolean ok = FALSE;

    if (!reader_open(&r, zso_path, error))
        return FALSE;

    dir = g_path_get_dirname(iso_path);
    g_mkdir_with_parents(dir, 0755);
    if (statvfs(dir, &sv) == 0 && (guint64)sv.f_bavail * sv.f_frsize < r.info.total_bytes + SPACE_MARGIN) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Not enough free space on this computer to unpack the game (%lu MB needed, %lu MB free)"),
                    (gulong)((r.info.total_bytes + SPACE_MARGIN) >> 20), (gulong)(((guint64)sv.f_bavail * sv.f_frsize) >> 20));
        g_free(dir);
        goto out;
    }
    g_free(dir);

    out = g_fopen(iso_path, "wb");
    if (out == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create %s: %s"), iso_path, g_strerror(errno));
        goto out;
    }
    for (i = 0; i < r.blocks; i++) {
        gsize want;

        if (!reader_block(&r, i, &want, error))
            goto out;
        if (fwrite(r.plain, 1, want, out) != want) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
            goto out;
        }
        done += want;
        if (progress && done - last_report >= ((guint64)16 << 20)) {
            last_report = done;
            if (!progress(done, r.info.total_bytes, _("Unpacking"), user_data)) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
                goto out;
            }
        }
    }
    if (fflush(out) != 0 || ferror(out)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        goto out;
    }
    ok = TRUE;
out:
    if (out != NULL && fclose(out) != 0 && ok) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        ok = FALSE;
    }
    if (!ok && out != NULL)
        g_unlink(iso_path); /* nothing half written is left behind */
    reader_close(&r);
    return ok;
}

/* ---- what game is in there (ISO9660 read through the blocks) ----------------------------------- */

void ziso_game_clear(ZisoGame *g)
{
    g_free(g->id);
    g_free(g->label);
    memset(g, 0, sizeof(*g));
}

static void not_ps2(const char *why, GError **error)
{
    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_TOOL, _("Not a PlayStation 2 disc image: %s"), why);
}

gboolean ziso_game_info(const char *path, ZisoGame *g, GError **error)
{
    Reader r;
    guint8 pvd[2048], *dir = NULL, *cnf = NULL;
    guint32 root_lba, root_len, cnf_lba = 0, cnf_len = 0;
    gsize pos = 0;
    gboolean ok = FALSE, found = FALSE;
    char *text = NULL;
    GRegex *re;
    GMatchInfo *mi = NULL;
    int i;

    memset(g, 0, sizeof(*g));
    if (!reader_open(&r, path, error))
        return FALSE;
    if (!reader_read(&r, 16 * 2048, pvd, sizeof(pvd), error))
        goto out;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        not_ps2(_("no ISO 9660 disc inside"), error);
        goto out;
    }
    /* the same rule hdl_dump uses: "CD-XA001" at 1024 = CD, eight zero bytes = DVD, anything else: by size */
    if (!memcmp(pvd + 1024, "CD-XA001", 8))
        strcpy(g->media, "CD");
    else if (!memcmp(pvd + 1024, "\0\0\0\0\0\0\0\0", 8))
        strcpy(g->media, "DVD");
    else
        strcpy(g->media, r.info.total_bytes > ((guint64)750 << 20) ? "DVD" : "CD");
    g->size_kb = r.info.total_bytes / 1024;
    g->label = g_strndup((const char *)pvd + 40, 32);
    g_strchomp(g->label);

    root_lba = rd32(pvd + 156 + 2);
    root_len = rd32(pvd + 156 + 10);
    if (root_len == 0 || root_len > 256 * 1024) {
        not_ps2(_("no root folder"), error);
        goto out;
    }
    dir = g_try_malloc(root_len);
    if (dir == NULL || !reader_read(&r, (guint64)root_lba * 2048, dir, root_len, error)) {
        if (dir == NULL)
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Not enough memory"));
        goto out;
    }
    while (pos + 34 <= root_len) {
        guint8 rec_len = dir[pos], name_len;

        if (rec_len == 0) { /* records do not cross a sector: skip the padding */
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        name_len = dir[pos + 32];
        if (pos + rec_len > root_len || 33u + name_len > rec_len)
            break;
        if (name_len == 12 && !g_ascii_strncasecmp((const char *)dir + pos + 33, "SYSTEM.CNF;1", 12)) {
            cnf_lba = rd32(dir + pos + 2);
            cnf_len = rd32(dir + pos + 10);
            found = TRUE;
            break;
        }
        pos += rec_len;
    }
    if (!found || cnf_len == 0 || cnf_len > 4096) {
        not_ps2(_("no SYSTEM.CNF"), error);
        goto out;
    }
    cnf = g_malloc0(cnf_len + 1);
    if (!reader_read(&r, (guint64)cnf_lba * 2048, cnf, cnf_len, error))
        goto out;
    text = (char *)cnf;
    for (i = 0; text[i]; i++)
        if (text[i] == '\r')
            text[i] = '\n';
    re = g_regex_new("BOOT2\\s*=\\s*cdrom0:\\\\?(?:[^;\\n]*\\\\)?([A-Za-z]{4}_[0-9]{3}\\.[0-9]{2})", G_REGEX_CASELESS, 0, NULL);
    if (g_regex_match(re, text, 0, &mi)) {
        char *m = g_match_info_fetch(mi, 1);

        g->id = g_ascii_strup(m, -1);
        g_free(m);
        ok = TRUE;
    } else
        not_ps2(_("no BOOT2 line in SYSTEM.CNF"), error);
    g_match_info_free(mi);
    g_regex_unref(re);
out:
    g_free(dir);
    g_free(cnf);
    reader_close(&r);
    if (!ok)
        ziso_game_clear(g);
    return ok;
}

/* ---- compressing ------------------------------------------------------------------------------- */

#define PACK_BLOCK 2048
#define PACK_LEVEL 9                 /* LZ4 HC level: a good ratio at a speed of tens of MB/s per core */
#define PACK_CHUNK_BLOCKS 512        /* one job of the thread pool: 1 MiB of the ISO */
#define PACK_MIN_SAVING 32           /* a block that does not shrink by this much is stored as it is */

int ziso_pick_shift(guint64 total_bytes)
{
    guint64 blocks = total_bytes / PACK_BLOCK;
    int shift;

    for (shift = 0; shift <= 3; shift++) {
        guint64 align = (guint64)1 << shift, worst = HEADER_SIZE + (blocks + 1) * 4 + align + blocks * (PACK_BLOCK + align - 1);

        if (worst < (guint64)PLAIN_BIT << shift) /* the offset, shifted right, has to fit in 31 bits */
            return shift;
    }
    return -1;
}

typedef struct {
    guint8 *in, *out;   /* the ISO's bytes of this chunk; room for the result (one block-sized slot per block) */
    guint16 *len;       /* result: bytes used in each slot */
    guint8 *plain;      /* result: 1 = the slot holds the block as it is */
    guint blocks;
    gboolean done;
} PackChunk;

typedef struct {
    GMutex lock;
    GCond cond;
} PackSync;

static void pack_worker(gpointer data, gpointer user)
{
    PackChunk *c = data;
    PackSync *sync = user;
    guint i;

    for (i = 0; i < c->blocks; i++) {
        int n = LZ4_compress_HC((const char *)c->in + (gsize)i * PACK_BLOCK, (char *)c->out + (gsize)i * PACK_BLOCK, PACK_BLOCK,
                                PACK_BLOCK - PACK_MIN_SAVING, PACK_LEVEL); /* 0 when it does not fit: not worth it */

        if (n > 0) {
            c->len[i] = (guint16)n;
            c->plain[i] = 0;
        } else {
            memcpy(c->out + (gsize)i * PACK_BLOCK, c->in + (gsize)i * PACK_BLOCK, PACK_BLOCK);
            c->len[i] = PACK_BLOCK;
            c->plain[i] = 1;
        }
    }
    g_mutex_lock(&sync->lock);
    c->done = TRUE;
    g_cond_broadcast(&sync->cond);
    g_mutex_unlock(&sync->lock);
}

static void pack_chunk_free(PackChunk *c)
{
    g_free(c->in);
    g_free(c->out);
    g_free(c->len);
    g_free(c->plain);
    g_free(c);
}

static void put32(guint8 *p, guint32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

gboolean ziso_pack(const char *iso_path, const char *zso_path, Ps2gmProgress progress, gpointer user_data, GError **error)
{
    return ziso_pack_shift(iso_path, zso_path, -1, progress, user_data, error);
}

gboolean ziso_pack_shift(const char *iso_path, const char *zso_path, int force_shift, Ps2gmProgress progress, gpointer user_data, GError **error)
{
    FILE *in = g_fopen(iso_path, "rb"), *out = NULL;
    guint64 total = file_size_of(iso_path), blocks, next_block = 0, read_blocks = 0, written = 0, last_report = 0, pos;
    guint32 *index = NULL;
    int shift;
    GThreadPool *pool = NULL;
    PackSync sync;
    GQueue queue = G_QUEUE_INIT;
    gboolean ok = FALSE, failed = FALSE;
    guint workers = CLAMP(g_get_num_processors(), 1, 8), window = workers * 3;
    gchar *dir;
    struct statvfs sv;
    guint8 header[HEADER_SIZE];

    if (in == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), iso_path, g_strerror(errno));
        return FALSE;
    }
    if (total == 0 || total % PACK_BLOCK != 0 || total > MAX_TOTAL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That is not a disc image made of whole 2048-byte sectors"));
        goto out;
    }
    shift = force_shift >= 0 ? force_shift : ziso_pick_shift(total);
    if (shift < 0 || shift > 3) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That disc image is too big for the ZSO format"));
        goto out;
    }
    dir = g_path_get_dirname(zso_path);
    g_mkdir_with_parents(dir, 0755);
    if (statvfs(dir, &sv) == 0 && (guint64)sv.f_bavail * sv.f_frsize < total + SPACE_MARGIN) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Not enough free space on this computer to compress the game (%lu MB needed, %lu MB free)"),
                    (gulong)((total + SPACE_MARGIN) >> 20), (gulong)(((guint64)sv.f_bavail * sv.f_frsize) >> 20));
        g_free(dir);
        goto out;
    }
    g_free(dir);

    blocks = total / PACK_BLOCK;
    index = g_try_malloc((blocks + 1) * sizeof(guint32));
    out = g_fopen(zso_path, "wb");
    if (index == NULL || out == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, index == NULL ? _("Not enough memory") : _("Cannot create %s: %s"), zso_path, g_strerror(errno));
        goto out;
    }
    {
        guint64 align = (guint64)1 << shift;

        pos = ((HEADER_SIZE + (blocks + 1) * 4) + align - 1) / align * align; /* where the first block goes */
        if (fseeko(out, (off_t)pos, SEEK_SET) != 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
            goto out;
        }
    }
    g_mutex_init(&sync.lock);
    g_cond_init(&sync.cond);
    pool = g_thread_pool_new(pack_worker, &sync, (gint)workers, FALSE, NULL);

    while (!failed && (read_blocks < blocks || !g_queue_is_empty(&queue))) {
        PackChunk *head;
        guint i;

        /* keep the workers busy: read ahead up to the window */
        while (read_blocks < blocks && g_queue_get_length(&queue) < window) {
            guint n = (guint)MIN((guint64)PACK_CHUNK_BLOCKS, blocks - read_blocks);
            PackChunk *c = g_new0(PackChunk, 1);

            c->blocks = n;
            c->in = g_malloc((gsize)n * PACK_BLOCK);
            c->out = g_malloc((gsize)n * PACK_BLOCK);
            c->len = g_new0(guint16, n);
            c->plain = g_new0(guint8, n);
            if (fread(c->in, PACK_BLOCK, n, in) != n) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), iso_path);
                pack_chunk_free(c);
                failed = TRUE;
                break;
            }
            read_blocks += n;
            g_queue_push_tail(&queue, c);
            g_thread_pool_push(pool, c, NULL);
        }
        head = g_queue_peek_head(&queue);
        if (head == NULL)
            break;
        g_mutex_lock(&sync.lock);
        while (!head->done)
            g_cond_wait(&sync.cond, &sync.lock);
        g_mutex_unlock(&sync.lock);
        g_queue_pop_head(&queue);

        for (i = 0; i < head->blocks && !failed; i++) {
            guint64 align = (guint64)1 << shift, padded = (pos + align - 1) / align * align;
            gsize len = head->len[i];

            if (padded != pos) { /* pad up to the alignment */
                guint8 zeros[16] = {0};

                if (fwrite(zeros, 1, (gsize)(padded - pos), out) != padded - pos)
                    failed = TRUE;
                pos = padded;
            }
            index[next_block++] = (guint32)(pos >> shift) | (head->plain[i] ? PLAIN_BIT : 0);
            if (!failed && fwrite(head->out + (gsize)i * PACK_BLOCK, 1, len, out) != len)
                failed = TRUE;
            pos += len;
            if (failed)
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        }
        written += (guint64)head->blocks * PACK_BLOCK;
        pack_chunk_free(head);
        if (!failed && progress && written - last_report >= ((guint64)16 << 20)) {
            last_report = written;
            if (!progress(written, total, _("Compressing"), user_data)) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
                failed = TRUE;
            }
        }
    }
    /* let the workers that are still running finish before the chunks they use are freed */
    g_thread_pool_free(pool, FALSE, TRUE);
    pool = NULL;
    while (!g_queue_is_empty(&queue))
        pack_chunk_free(g_queue_pop_head(&queue));
    g_mutex_clear(&sync.lock);
    g_cond_clear(&sync.cond);
    if (failed)
        goto out;

    {
        guint64 i;
        guint8 *index_bytes = g_malloc((blocks + 1) * 4);

        /*
         * The end of the last block is the last index entry, and offsets are counted in units of 1 << shift: the end has
         * to be rounded UP to such a unit (rounding down cut the last block short whenever shift > 0), with zero padding.
         */
        {
            guint64 align = (guint64)1 << shift, end = (pos + align - 1) / align * align;

            if (end != pos) {
                guint8 zeros[16] = {0};

                if (fseeko(out, (off_t)pos, SEEK_SET) != 0 || fwrite(zeros, 1, (gsize)(end - pos), out) != end - pos)
                    failed = TRUE;
                pos = end;
            }
        }
        index[blocks] = (guint32)(pos >> shift);
        /*
         * hdl_dump loses the end of a file that is not a whole number of sectors long (see ziso.h): pad the file
         * (readers stop at the end of the last block, so the padding is never read).
         */
        if (pos % PACK_BLOCK != 0) {
            guint8 zeros[PACK_BLOCK] = {0};
            gsize pad = PACK_BLOCK - (gsize)(pos % PACK_BLOCK);

            if (fseeko(out, (off_t)pos, SEEK_SET) != 0 || fwrite(zeros, 1, pad, out) != pad)
                failed = TRUE;
        }
        for (i = 0; i <= blocks; i++)
            put32(index_bytes + i * 4, index[i]);
        memcpy(header, "ZISO", 4);
        put32(header + 4, HEADER_SIZE);
        put32(header + 8, (guint32)total);
        put32(header + 12, (guint32)(total >> 32));
        put32(header + 16, PACK_BLOCK);
        header[20] = 1;
        header[21] = (guint8)shift;
        header[22] = header[23] = 0;
        ok = !failed && fseeko(out, 0, SEEK_SET) == 0 && fwrite(header, 1, HEADER_SIZE, out) == HEADER_SIZE &&
             fwrite(index_bytes, 4, blocks + 1, out) == blocks + 1 && fflush(out) == 0 && !ferror(out);
        g_free(index_bytes);
        if (!ok)
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
    }
out:
    if (pool != NULL) {
        g_thread_pool_free(pool, FALSE, TRUE);
        while (!g_queue_is_empty(&queue))
            pack_chunk_free(g_queue_pop_head(&queue));
    }
    if (out != NULL && fclose(out) != 0 && ok) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        ok = FALSE;
    }
    if (in != NULL)
        fclose(in);
    if (!ok && out != NULL)
        g_unlink(zso_path); /* nothing half written is left behind */
    g_free(index);
    return ok;
}

/* ---- padding a ZSO to whole sectors ----------------------------------------------------------- */

gboolean ziso_needs_padding(const char *zso_path)
{
    return file_size_of(zso_path) % PACK_BLOCK != 0;
}

gboolean ziso_pad_copy(const char *zso_path, const char *copy_path, Ps2gmProgress progress, gpointer user_data, GError **error)
{
    FILE *in = g_fopen(zso_path, "rb"), *out = NULL;
    guint64 size = file_size_of(zso_path), done = 0, last = 0;
    guint8 *buf = g_malloc(1 << 20);
    gchar *dir;
    struct statvfs sv;
    gboolean ok = FALSE;
    gsize n;

    if (in == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), zso_path, g_strerror(errno));
        g_free(buf);
        return FALSE;
    }
    dir = g_path_get_dirname(copy_path);
    g_mkdir_with_parents(dir, 0755);
    if (statvfs(dir, &sv) == 0 && (guint64)sv.f_bavail * sv.f_frsize < size + SPACE_MARGIN) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Not enough free space on this computer to prepare the game (%lu MB needed, %lu MB free)"),
                    (gulong)((size + SPACE_MARGIN) >> 20), (gulong)(((guint64)sv.f_bavail * sv.f_frsize) >> 20));
        g_free(dir);
        goto out;
    }
    g_free(dir);
    out = g_fopen(copy_path, "wb");
    if (out == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create %s: %s"), copy_path, g_strerror(errno));
        goto out;
    }
    while ((n = fread(buf, 1, 1 << 20, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
            goto out;
        }
        done += n;
        if (progress && done - last >= ((guint64)64 << 20)) {
            last = done;
            if (!progress(done, size, _("Preparing"), user_data)) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
                goto out;
            }
        }
    }
    if (done % PACK_BLOCK != 0) {
        guint8 zeros[PACK_BLOCK] = {0};
        gsize pad = PACK_BLOCK - (gsize)(done % PACK_BLOCK);

        if (fwrite(zeros, 1, pad, out) != pad) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
            goto out;
        }
    }
    ok = fflush(out) == 0 && !ferror(out);
    if (!ok)
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
out:
    if (out != NULL && fclose(out) != 0 && ok) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        ok = FALSE;
    }
    fclose(in);
    if (!ok && out != NULL)
        g_unlink(copy_path);
    g_free(buf);
    return ok;
}

/* ---- estimating the saving --------------------------------------------------------------------- */

gboolean ziso_estimate(const char *iso_path, double *ratio, GError **error)
{
    FILE *in = g_fopen(iso_path, "rb");
    guint64 total = file_size_of(iso_path), chunks, i, sampled = 0, packed = 0;
    guint8 *buf = g_malloc((gsize)PACK_CHUNK_BLOCKS * PACK_BLOCK), *out = g_malloc(PACK_BLOCK);
    guint64 chunk_bytes = (guint64)PACK_CHUNK_BLOCKS * PACK_BLOCK;
    gboolean ok = FALSE;

    if (in == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), iso_path, g_strerror(errno));
        goto out;
    }
    if (total < PACK_BLOCK || total % PACK_BLOCK != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That is not a disc image made of whole 2048-byte sectors"));
        goto out;
    }
    chunks = MIN(48, total / chunk_bytes + 1);
    for (i = 0; i < chunks; i++) {
        /* evenly spread: the start, the end and everything in between, whole blocks only */
        guint64 at = chunks > 1 ? (total > chunk_bytes ? (total - chunk_bytes) / (chunks - 1) * i : 0) : 0;
        gsize want = (gsize)MIN(chunk_bytes, total - at), got, b;

        at = at / PACK_BLOCK * PACK_BLOCK;
        want = (gsize)MIN((guint64)want, total - at) / PACK_BLOCK * PACK_BLOCK;
        if (fseeko(in, (off_t)at, SEEK_SET) != 0 || (got = fread(buf, 1, want, in)) != want) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot read %s"), iso_path);
            goto out;
        }
        for (b = 0; b < got / PACK_BLOCK; b++) {
            int n = LZ4_compress_HC((const char *)buf + b * PACK_BLOCK, (char *)out, PACK_BLOCK, PACK_BLOCK - PACK_MIN_SAVING, PACK_LEVEL);

            packed += n > 0 ? (guint64)n : PACK_BLOCK; /* a block that does not shrink is stored as it is */
        }
        sampled += got;
    }
    *ratio = sampled ? (double)packed / sampled : 1.0;
    ok = TRUE;
out:
    if (in)
        fclose(in);
    g_free(buf);
    g_free(out);
    return ok;
}
