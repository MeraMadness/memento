/* Shared with ../savemgr (the console app): the same file is tested on the PC and runs on the PS2. If you fix one, fix both. */
/*
 * mcfs - PS2 memory card image access. Layout reference: the mymc tool by Ross Ridge.
 *
 * Cluster = 1024 bytes (2 pages of 512). Cluster numbers inside the FAT and
 * directory entries are relative to alloc_offset, the first allocatable cluster.
 * FAT entry: bit 31 set = cluster in use, low 31 bits = next cluster; 0x7fffffff
 * as "next" ends the chain. A free cluster has bit 31 clear.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include "mcfs.h"

#define MC_MAGIC     "Sony PS2 Memory Card Format "
#define MC_MAGIC_LEN 28
#define ENT_LEN      512
#define FAT_ALLOC    0x80000000u
#define FAT_MASK     0x7fffffffu
#define FAT_END      0xffffffffu /* used cluster, last of its chain */
#define CH_END       0x7fffffffu /* "next" value meaning no next cluster */
#define MAX_DEPTH    4

#define DF_RWX       0x0007
#define DF_FILE      0x0010
#define DF_DIR       0x0020
#define DF_0400      0x0400
#define DF_EXISTS    0x8000

/* ---- little endian helpers ---- */

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

/* ---- raw cluster IO ---- */

static int rd_abs(mc_t *mc, uint32_t abs, void *buf)
{
    if (abs >= mc->clusters)
        return MC_E_FORMAT;
    if (lseek(mc->fd, (off_t)abs * MC_CLUSTER, SEEK_SET) < 0 || read(mc->fd, buf, MC_CLUSTER) != MC_CLUSTER)
        return MC_E_IO;
    return MC_OK;
}

static int wr_abs(mc_t *mc, uint32_t abs, const void *buf)
{
    if (!mc->writable)
        return MC_E_RDONLY;
    if (abs >= mc->clusters)
        return MC_E_FORMAT;
    if (lseek(mc->fd, (off_t)abs * MC_CLUSTER, SEEK_SET) < 0 || write(mc->fd, buf, MC_CLUSTER) != MC_CLUSTER)
        return MC_E_IO;
    return MC_OK;
}

static int rd_alloc(mc_t *mc, uint32_t n, void *buf)
{
    return rd_abs(mc, mc->alloc_offset + n, buf);
}

static int wr_alloc(mc_t *mc, uint32_t n, const void *buf)
{
    return wr_abs(mc, mc->alloc_offset + n, buf);
}

/* ---- FAT ---- */

/* absolute cluster of FAT cluster number f, through the indirect FAT */
static int fat_location(mc_t *mc, uint32_t f, uint32_t *abs)
{
    uint8_t buf[MC_CLUSTER];
    uint32_t ifc_i = f / 256, i;
    int rc;

    if (ifc_i >= 32 || mc->ifc[ifc_i] == 0 || mc->ifc[ifc_i] == FAT_END)
        return MC_E_FORMAT;
    if (!mc->ifc_valid || mc->ifc_idx != ifc_i) {
        rc = rd_abs(mc, mc->ifc[ifc_i], buf);
        if (rc != MC_OK)
            return rc;
        for (i = 0; i < 256; i++)
            mc->ifc_buf[i] = get32(buf + i * 4);
        mc->ifc_idx = ifc_i;
        mc->ifc_valid = 1;
    }
    *abs = mc->ifc_buf[f % 256];
    return MC_OK;
}

static int fat_flush(mc_t *mc)
{
    uint8_t buf[MC_CLUSTER];
    uint32_t abs, i;
    int rc;

    if (!mc->fat_valid || !mc->fat_dirty)
        return MC_OK;
    rc = fat_location(mc, mc->fat_idx, &abs);
    if (rc != MC_OK)
        return rc;
    for (i = 0; i < 256; i++)
        put32(buf + i * 4, mc->fat[i]);
    rc = wr_abs(mc, abs, buf);
    if (rc == MC_OK)
        mc->fat_dirty = 0;
    return rc;
}

static int fat_load(mc_t *mc, uint32_t f)
{
    uint8_t buf[MC_CLUSTER];
    uint32_t abs, i;
    int rc;

    if (mc->fat_valid && mc->fat_idx == f)
        return MC_OK;
    rc = fat_flush(mc);
    if (rc == MC_OK)
        rc = fat_location(mc, f, &abs);
    if (rc == MC_OK)
        rc = rd_abs(mc, abs, buf);
    if (rc != MC_OK)
        return rc;
    for (i = 0; i < 256; i++)
        mc->fat[i] = get32(buf + i * 4);
    mc->fat_idx = f;
    mc->fat_valid = 1;
    mc->fat_dirty = 0;
    return MC_OK;
}

static int fat_get(mc_t *mc, uint32_t n, uint32_t *v)
{
    int rc;

    if (n >= mc->alloc_end)
        return MC_E_FORMAT;
    rc = fat_load(mc, n / 256);
    if (rc != MC_OK)
        return rc;
    *v = mc->fat[n % 256];
    return MC_OK;
}

static int fat_set(mc_t *mc, uint32_t n, uint32_t v)
{
    int rc;

    if (!mc->writable)
        return MC_E_RDONLY;
    if (n >= mc->alloc_end)
        return MC_E_FORMAT;
    rc = fat_load(mc, n / 256);
    if (rc != MC_OK)
        return rc;
    mc->fat[n % 256] = v;
    mc->fat_dirty = 1;
    return MC_OK;
}

/* next cluster of a chain, CH_END at its end; negative on error */
static int64_t chain_next(mc_t *mc, uint32_t c)
{
    uint32_t v;
    int rc = fat_get(mc, c, &v);

    if (rc != MC_OK)
        return rc;
    if (!(v & FAT_ALLOC))
        return CH_END; /* damaged chain: stop here */
    return v & FAT_MASK;
}

static int alloc_cluster(mc_t *mc, uint32_t *out)
{
    uint32_t n, v, pass;
    int rc;

    for (pass = 0; pass < 2; pass++) {
        for (n = pass == 0 ? mc->cursor : 0; n < mc->alloc_end; n++) {
            rc = fat_get(mc, n, &v);
            if (rc != MC_OK)
                return rc;
            if (!(v & FAT_ALLOC)) {
                rc = fat_set(mc, n, FAT_END);
                if (rc != MC_OK)
                    return rc;
                mc->cursor = n + 1;
                *out = n;
                return MC_OK;
            }
        }
    }
    return MC_E_FULL;
}

static int free_chain(mc_t *mc, uint32_t c)
{
    uint32_t guard = 0, v;
    int rc;

    while (c != CH_END && c != FAT_END && c < mc->alloc_end && guard++ < mc->alloc_end) {
        rc = fat_get(mc, c, &v);
        if (rc != MC_OK)
            return rc;
        if (!(v & FAT_ALLOC))
            break;
        rc = fat_set(mc, c, v & FAT_MASK);
        if (rc != MC_OK)
            return rc;
        if (c < mc->cursor)
            mc->cursor = c;
        c = v & FAT_MASK;
    }
    return MC_OK;
}

uint32_t mc_free_kb(mc_t *mc)
{
    uint32_t n, v, count = 0;

    for (n = 0; n < mc->alloc_end; n++)
        if (fat_get(mc, n, &v) == MC_OK && !(v & FAT_ALLOC))
            count++;
    return count;
}

/* ---- directories ---- */

struct ent {
    uint16_t mode;
    uint32_t length, cluster, parent;
    uint8_t created[8], modified[8];
    char name[MC_NAME];
};

static void ent_unpack(const uint8_t *b, struct ent *e)
{
    e->mode = get16(b);
    e->length = get32(b + 4);
    memcpy(e->created, b + 8, 8);
    e->cluster = get32(b + 0x10);
    e->parent = get32(b + 0x14);
    memcpy(e->modified, b + 0x18, 8);
    memcpy(e->name, b + 0x40, 32);
    e->name[32] = '\0';
}

static void ent_pack(const struct ent *e, uint8_t *b)
{
    memset(b, 0, ENT_LEN);
    put16(b, e->mode);
    put32(b + 4, e->length);
    memcpy(b + 8, e->created, 8);
    put32(b + 0x10, e->cluster);
    put32(b + 0x14, e->parent);
    memcpy(b + 0x18, e->modified, 8);
    memcpy(b + 0x40, e->name, strnlen(e->name, 32));
}

/* cluster holding entry idx of the directory that starts at first */
static int dir_locate(mc_t *mc, uint32_t first, uint32_t idx, uint32_t *cluster)
{
    uint32_t c = first, k;
    int64_t nx;

    for (k = 0; k < idx / 2; k++) {
        nx = chain_next(mc, c);
        if (nx < 0)
            return (int)nx;
        if (nx == CH_END)
            return MC_E_FORMAT;
        c = (uint32_t)nx;
    }
    *cluster = c;
    return MC_OK;
}

static int dir_read(mc_t *mc, uint32_t first, uint32_t idx, struct ent *e)
{
    uint8_t buf[MC_CLUSTER];
    uint32_t c = 0;
    int rc = dir_locate(mc, first, idx, &c);

    if (rc == MC_OK)
        rc = rd_alloc(mc, c, buf);
    if (rc == MC_OK)
        ent_unpack(buf + (idx % 2) * ENT_LEN, e);
    return rc;
}

static int dir_write(mc_t *mc, uint32_t first, uint32_t idx, const struct ent *e)
{
    uint8_t buf[MC_CLUSTER];
    uint32_t c = 0;
    int rc = dir_locate(mc, first, idx, &c);

    if (rc == MC_OK)
        rc = rd_alloc(mc, c, buf);
    if (rc == MC_OK) {
        ent_pack(e, buf + (idx % 2) * ENT_LEN);
        rc = wr_alloc(mc, c, buf);
    }
    return rc;
}

/* makes sure the directory has a cluster for entry idx (two entries per cluster) */
static int dir_grow(mc_t *mc, uint32_t first, uint32_t idx)
{
    uint8_t zero[MC_CLUSTER];
    uint32_t need = idx / 2, c = first, k = 0, nc;
    int64_t nx;
    int rc;

    while (k < need) {
        nx = chain_next(mc, c);
        if (nx < 0)
            return (int)nx;
        if (nx == CH_END)
            break;
        c = (uint32_t)nx;
        k++;
    }
    memset(zero, 0, sizeof(zero));
    while (k < need) {
        rc = alloc_cluster(mc, &nc);
        if (rc == MC_OK)
            rc = wr_alloc(mc, nc, zero);
        if (rc == MC_OK)
            rc = fat_set(mc, c, nc | FAT_ALLOC);
        if (rc != MC_OK)
            return rc;
        c = nc;
        k++;
    }
    return MC_OK;
}

static int dir_set_length(mc_t *mc, uint32_t first, uint32_t idx, uint32_t length)
{
    struct ent e;
    int rc = dir_read(mc, first, idx, &e);

    if (rc != MC_OK)
        return rc;
    e.length = length;
    return dir_write(mc, first, idx, &e);
}

/* root directory: its length lives in its own "." entry */
static int root_length(mc_t *mc, uint32_t *len)
{
    struct ent e;
    int rc = dir_read(mc, mc->root, 0, &e);

    *len = e.length;
    return rc;
}

static int find_save(mc_t *mc, const char *name, uint32_t *idx, struct ent *e)
{
    uint32_t len, i;
    int rc = root_length(mc, &len);

    if (rc != MC_OK)
        return rc;
    for (i = 2; i < len; i++) {
        rc = dir_read(mc, mc->root, i, e);
        if (rc != MC_OK)
            return rc;
        if ((e->mode & DF_EXISTS) && (e->mode & DF_DIR) && !strcmp(e->name, name)) {
            *idx = i;
            return MC_OK;
        }
    }
    return MC_E_NOTFOUND;
}

/* ---- open / close ---- */

int mc_open(mc_t *mc, const char *path, int writable)
{
    uint8_t sb[MC_CLUSTER];
    off_t size;
    uint32_t i;

    memset(mc, 0, sizeof(*mc));
    mc->fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (mc->fd < 0)
        return MC_E_IO;
    mc->writable = writable;
    size = lseek(mc->fd, 0, SEEK_END);
    lseek(mc->fd, 0, SEEK_SET);
    if (size < 0 || read(mc->fd, sb, sizeof(sb)) != sizeof(sb)) {
        close(mc->fd);
        return MC_E_IO;
    }
    if (memcmp(sb, MC_MAGIC, MC_MAGIC_LEN) != 0 || get16(sb + 0x28) != 512 || get16(sb + 0x2a) != 2) {
        close(mc->fd);
        return MC_E_FORMAT;
    }
    mc->clusters = get32(sb + 0x30);
    mc->alloc_offset = get32(sb + 0x34);
    mc->alloc_end = get32(sb + 0x38);
    mc->root = get32(sb + 0x3c);
    for (i = 0; i < 32; i++)
        mc->ifc[i] = get32(sb + 0x50 + i * 4);
    if (size == (off_t)mc->clusters * 2 * 528) {
        close(mc->fd);
        return MC_E_ECC;
    }
    if (size != (off_t)mc->clusters * MC_CLUSTER || mc->alloc_offset >= mc->clusters ||
        mc->alloc_end > mc->clusters - mc->alloc_offset) {
        close(mc->fd);
        return MC_E_FORMAT;
    }
    return MC_OK;
}

int mc_close(mc_t *mc)
{
    int rc = fat_flush(mc);

    if (mc->fd >= 0) {
        if (mc->writable)
            fsync(mc->fd);
        close(mc->fd);
        mc->fd = -1;
    }
    return rc;
}

/* ---- listing ---- */

static int cmp_save(const void *a, const void *b)
{
    return strcasecmp(((const struct mc_save *)a)->name, ((const struct mc_save *)b)->name);
}

static int dir_stats(mc_t *mc, const struct ent *dir, uint32_t *kb, uint32_t *files)
{
    struct ent e;
    uint32_t i;
    int rc;

    *kb = *files = 0;
    for (i = 2; i < dir->length; i++) {
        rc = dir_read(mc, dir->cluster, i, &e);
        if (rc != MC_OK)
            return rc;
        if ((e.mode & DF_EXISTS) && (e.mode & DF_FILE)) {
            *kb += (e.length + MC_CLUSTER - 1) / MC_CLUSTER;
            (*files)++;
        }
    }
    return MC_OK;
}

int mc_list(mc_t *mc, struct mc_save *out, int max)
{
    uint32_t len, i;
    struct ent e;
    int n = 0, rc = root_length(mc, &len);

    if (rc != MC_OK)
        return rc;
    for (i = 2; i < len && n < max; i++) {
        rc = dir_read(mc, mc->root, i, &e);
        if (rc != MC_OK)
            return rc;
        if (!(e.mode & DF_EXISTS) || !(e.mode & DF_DIR))
            continue;
        strcpy(out[n].name, e.name);
        out[n].mode = e.mode;
        if (dir_stats(mc, &e, &out[n].size_kb, &out[n].entries) != MC_OK)
            continue;
        n++;
    }
    qsort(out, n, sizeof(out[0]), cmp_save);
    return n;
}

/* ---- title from icon.sys ---- */

/* full-width Shift-JIS letters, digits and punctuation -> ASCII; 0 if unmapped */
static char sjis_to_ascii(unsigned lead, unsigned trail)
{
    if (lead == 0x82) {
        if (trail >= 0x4f && trail <= 0x58)
            return '0' + (trail - 0x4f);
        if (trail >= 0x60 && trail <= 0x79)
            return 'A' + (trail - 0x60);
        if (trail >= 0x81 && trail <= 0x9a)
            return 'a' + (trail - 0x81);
    } else if (lead == 0x81) {
        switch (trail) {
        case 0x40: return ' ';
        case 0x43: return ',';
        case 0x44: case 0x45: return '.';
        case 0x46: return ':';
        case 0x47: return ';';
        case 0x48: return '?';
        case 0x49: return '!';
        case 0x5b: case 0x5c: case 0x5d: case 0x7c: return '-';
        case 0x5e: return '/';
        case 0x65: case 0x66: return '\'';
        case 0x67: case 0x68: return '"';
        case 0x69: case 0x6d: return '(';
        case 0x6a: case 0x6e: return ')';
        case 0x7b: return '+';
        case 0x93: return '%';
        case 0x94: return '#';
        case 0x95: return '&';
        case 0x96: return '*';
        case 0x97: return '@';
        }
    }
    return 0;
}

int mc_title(mc_t *mc, const char *name, char *out, int len)
{
    uint8_t buf[MC_CLUSTER];
    struct ent dir, e;
    uint32_t idx, i, c;
    int rc, n = 0, o, last_space;
    const uint8_t *t;

    out[0] = '\0';
    rc = find_save(mc, name, &idx, &dir);
    if (rc != MC_OK)
        return rc;
    for (i = 2; i < dir.length; i++) {
        rc = dir_read(mc, dir.cluster, i, &e);
        if (rc != MC_OK)
            return rc;
        if ((e.mode & DF_EXISTS) && (e.mode & DF_FILE) && !strcmp(e.name, "icon.sys") && e.length >= 0xc0 + 68 &&
            e.cluster < mc->alloc_end) {
            c = e.cluster;
            if (rd_alloc(mc, c, buf) != MC_OK)
                return MC_E_IO;
            /* icon.sys: magic "PS2D", title at 0xc0 (68 bytes of Shift-JIS) */
            if (memcmp(buf, "PS2D", 4) != 0)
                return MC_OK;
            t = buf + 0xc0;
            last_space = 1;
            for (o = 0; o < 68 && t[o] != 0 && n < len - 1;) {
                char ch = 0;

                if (t[o] < 0x80) {
                    ch = t[o++];
                } else if (o + 1 < 68) {
                    ch = sjis_to_ascii(t[o], t[o + 1]);
                    o += 2;
                } else
                    break;
                if (ch == 0)
                    continue;
                if (ch == ' ' && last_space)
                    continue;
                last_space = ch == ' ';
                out[n++] = ch;
            }
            while (n > 0 && out[n - 1] == ' ')
                n--;
            out[n] = '\0';
            return MC_OK;
        }
    }
    return MC_OK;
}

/* ---- delete ---- */

static int free_dir(mc_t *mc, const struct ent *dir, int depth)
{
    struct ent e;
    uint32_t i;
    int rc;

    if (depth > MAX_DEPTH)
        return MC_E_FORMAT;
    for (i = 2; i < dir->length; i++) {
        rc = dir_read(mc, dir->cluster, i, &e);
        if (rc != MC_OK)
            return rc;
        if (!(e.mode & DF_EXISTS))
            continue;
        rc = (e.mode & DF_DIR) ? free_dir(mc, &e, depth + 1) : free_chain(mc, e.cluster);
        if (rc != MC_OK)
            return rc;
    }
    return free_chain(mc, dir->cluster);
}

static int delete_slot(mc_t *mc, uint32_t idx, struct ent *dir)
{
    int rc;

    dir->mode &= ~DF_EXISTS; /* unlink first: an interrupted delete leaks clusters but never corrupts */
    rc = dir_write(mc, mc->root, idx, dir);
    if (rc == MC_OK)
        rc = free_dir(mc, dir, 0);
    return rc;
}

int mc_delete(mc_t *mc, const char *name)
{
    struct ent dir;
    uint32_t idx;
    int rc;

    if (!mc->writable)
        return MC_E_RDONLY;
    rc = find_save(mc, name, &idx, &dir);
    if (rc != MC_OK)
        return rc;
    return delete_slot(mc, idx, &dir);
}

/* ---- PSU export / import ---- */

static void write_all(int fd, const void *buf, size_t len, int *err)
{
    if (!*err && write(fd, buf, len) != (ssize_t)len)
        *err = MC_E_IO;
}

static void psu_dirent(uint8_t *b, const struct ent *src)
{
    struct ent e = *src;

    e.cluster = 0;
    e.parent = 0;
    ent_pack(&e, b);
}

int mc_export_psu(mc_t *mc, const char *name, const char *path)
{
    static const uint8_t zero[MC_CLUSTER];
    uint8_t buf[MC_CLUSTER], b[ENT_LEN];
    struct ent dir, e, dot;
    uint32_t idx, i, files = 0, c, left, dlen, dcluster;
    int fd, rc, err = 0;
    int64_t nx;

    rc = find_save(mc, name, &idx, &dir);
    if (rc != MC_OK)
        return rc;
    for (i = 2; i < dir.length; i++) {
        rc = dir_read(mc, dir.cluster, i, &e);
        if (rc != MC_OK)
            return rc;
        if ((e.mode & DF_EXISTS) && (e.mode & DF_FILE))
            files++;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return MC_E_IO;

    dlen = dir.length;
    dcluster = dir.cluster;
    dir.length = files + 2;
    psu_dirent(b, &dir);
    write_all(fd, b, ENT_LEN, &err);
    memset(&dot, 0, sizeof(dot));
    dot.mode = DF_RWX | DF_DIR | DF_0400 | DF_EXISTS;
    memcpy(dot.created, dir.created, 8);
    memcpy(dot.modified, dir.created, 8);
    strcpy(dot.name, ".");
    psu_dirent(b, &dot);
    write_all(fd, b, ENT_LEN, &err);
    strcpy(dot.name, "..");
    psu_dirent(b, &dot);
    write_all(fd, b, ENT_LEN, &err);

    rc = MC_OK;
    for (i = 2; rc == MC_OK && !err && i < dlen; i++) {
        rc = dir_read(mc, dcluster, i, &e);
        if (rc != MC_OK)
            break;
        if (!((e.mode & DF_EXISTS) && (e.mode & DF_FILE)))
            continue;
        psu_dirent(b, &e);
        write_all(fd, b, ENT_LEN, &err);
        c = e.cluster;
        for (left = e.length; left > 0 && !err; left -= left < MC_CLUSTER ? left : MC_CLUSTER) {
            if (c == CH_END || c == FAT_END || c >= mc->alloc_end) {
                rc = MC_E_FORMAT;
                break;
            }
            rc = rd_alloc(mc, c, buf);
            if (rc != MC_OK)
                break;
            write_all(fd, buf, left < MC_CLUSTER ? left : MC_CLUSTER, &err);
            if (left < MC_CLUSTER)
                write_all(fd, zero, MC_CLUSTER - left, &err);
            nx = chain_next(mc, c);
            if (nx < 0) {
                rc = (int)nx;
                break;
            }
            c = (uint32_t)nx;
        }
    }
    close(fd);
    if (rc == MC_OK)
        rc = err;
    if (rc != MC_OK)
        unlink(path);
    return rc;
}

static int read_full(int fd, void *buf, size_t len)
{
    return read(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

static int psu_is_dir(const struct ent *e)
{
    return (e->mode & DF_DIR) != 0 && (e->mode & DF_FILE) == 0;
}

int mc_import_psu(mc_t *mc, const char *path, char *name_out)
{
    static uint8_t buf[MC_CLUSTER];
    uint8_t b[ENT_LEN];
    struct ent head, dot, dotdot, f, d, slot;
    uint32_t nfiles, i, need, root_len, idx = 0, dir_c = 0, found, k, c, prev, first, left;
    int fd, rc = MC_OK, created = 0;
    off_t skip;

    if (!mc->writable)
        return MC_E_RDONLY;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return MC_E_IO;
    if (read_full(fd, b, ENT_LEN) || (ent_unpack(b, &head), read_full(fd, b, ENT_LEN)) ||
        (ent_unpack(b, &dot), read_full(fd, b, ENT_LEN))) {
        close(fd);
        return MC_E_BADPSU;
    }
    ent_unpack(b, &dotdot);
    if (!psu_is_dir(&head) || !psu_is_dir(&dot) || !psu_is_dir(&dotdot) || head.length < 2 || head.length > 4096 ||
        head.name[0] == '\0' || strchr(head.name, '/') != NULL) {
        close(fd);
        return MC_E_BADPSU;
    }
    nfiles = head.length - 2;
    if (name_out != NULL)
        strcpy(name_out, head.name);

    rc = find_save(mc, head.name, &found, &d);
    if (rc == MC_OK) {
        close(fd);
        return MC_E_EXISTS;
    }
    if (rc != MC_E_NOTFOUND) {
        close(fd);
        return rc;
    }

    /* pass 1: validate the file and count the clusters it needs */
    need = (nfiles + 2 + 1) / 2 + 1;
    for (i = 0; i < nfiles; i++) {
        if (read_full(fd, b, ENT_LEN)) {
            close(fd);
            return MC_E_BADPSU;
        }
        ent_unpack(b, &f);
        if (!(f.mode & DF_FILE) || f.name[0] == '\0' || strchr(f.name, '/') != NULL) {
            close(fd);
            return MC_E_BADPSU;
        }
        skip = (off_t)((f.length + MC_CLUSTER - 1) / MC_CLUSTER) * MC_CLUSTER;
        need += (f.length + MC_CLUSTER - 1) / MC_CLUSTER;
        if (lseek(fd, skip, SEEK_CUR) < 0) {
            close(fd);
            return MC_E_BADPSU;
        }
    }
    if (need + 1 > mc_free_kb(mc)) {
        close(fd);
        return MC_E_FULL;
    }

    /* pass 2: create the directory, then the files */
    lseek(fd, 3 * ENT_LEN, SEEK_SET);
    rc = root_length(mc, &root_len);
    if (rc != MC_OK) {
        close(fd);
        return rc;
    }
    for (idx = 2; idx < root_len; idx++) {
        rc = dir_read(mc, mc->root, idx, &slot);
        if (rc != MC_OK) {
            close(fd);
            return rc;
        }
        if (!(slot.mode & DF_EXISTS))
            break;
    }
    if (idx == root_len) {
        rc = dir_grow(mc, mc->root, idx);
        if (rc == MC_OK)
            rc = dir_set_length(mc, mc->root, 0, root_len + 1);
        if (rc != MC_OK) {
            close(fd);
            return rc;
        }
    }
    rc = alloc_cluster(mc, &dir_c);
    if (rc != MC_OK) {
        close(fd);
        return rc;
    }

    memset(&d, 0, sizeof(d));
    d.mode = (head.mode & ~DF_FILE) | DF_DIR | DF_EXISTS;
    d.length = 2;
    d.cluster = dir_c;
    memcpy(d.created, head.created, 8);
    memcpy(d.modified, head.modified, 8);
    strcpy(d.name, head.name);

    memset(buf, 0, MC_CLUSTER);
    memset(&dot, 0, sizeof(dot));
    dot.mode = DF_RWX | DF_0400 | DF_DIR | DF_EXISTS;
    dot.cluster = mc->root;
    dot.parent = idx;
    memcpy(dot.created, head.created, 8);
    memcpy(dot.modified, head.created, 8);
    strcpy(dot.name, ".");
    ent_pack(&dot, buf);
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.mode = DF_RWX | DF_0400 | DF_DIR | DF_EXISTS;
    memcpy(dotdot.created, head.created, 8);
    memcpy(dotdot.modified, head.created, 8);
    strcpy(dotdot.name, "..");
    ent_pack(&dotdot, buf + ENT_LEN);
    rc = wr_alloc(mc, dir_c, buf);
    if (rc == MC_OK)
        rc = dir_write(mc, mc->root, idx, &d);
    if (rc != MC_OK) {
        free_chain(mc, dir_c);
        close(fd);
        return rc;
    }
    created = 1;

    for (k = 0; rc == MC_OK && k < nfiles; k++) {
        if (read_full(fd, b, ENT_LEN)) {
            rc = MC_E_BADPSU;
            break;
        }
        ent_unpack(b, &f);
        first = FAT_END;
        prev = 0;
        for (left = f.length, i = 0; left > 0 && rc == MC_OK; i++) {
            uint32_t n = left < MC_CLUSTER ? left : MC_CLUSTER;

            if (read_full(fd, buf, MC_CLUSTER)) {
                rc = MC_E_BADPSU;
                break;
            }
            if (n < MC_CLUSTER)
                memset(buf + n, 0, MC_CLUSTER - n);
            rc = alloc_cluster(mc, &c);
            if (rc != MC_OK)
                break;
            if (i == 0)
                first = c;
            else
                rc = fat_set(mc, prev, c | FAT_ALLOC);
            if (rc == MC_OK)
                rc = wr_alloc(mc, c, buf);
            prev = c;
            left -= n;
        }
        if (rc != MC_OK) {
            if (first != FAT_END)
                free_chain(mc, first);
            break;
        }
        f.mode = (f.mode & ~DF_DIR) | DF_FILE | DF_EXISTS;
        f.cluster = first;
        f.parent = 0;
        /* the file's data is only reachable once its entry is written */
        rc = dir_grow(mc, dir_c, 2 + k);
        if (rc == MC_OK)
            rc = dir_write(mc, dir_c, 2 + k, &f);
        if (rc == MC_OK) {
            d.length = 2 + k + 1;
            rc = dir_set_length(mc, mc->root, idx, d.length);
        } else if (first != FAT_END)
            free_chain(mc, first);
    }
    close(fd);

    if (rc != MC_OK && created) {
        struct ent cur;

        if (dir_read(mc, mc->root, idx, &cur) == MC_OK)
            delete_slot(mc, idx, &cur);
    }
    if (rc == MC_OK)
        rc = fat_flush(mc);
    return rc;
}
