/*
 * mcfs - reads and edits PS2 memory card images (raw, 512 byte pages, no ECC) as
 * written by Neutrino/OPL virtual memory cards. Portable C: no PS2 headers, so
 * the same code is tested on the PC (tests/) and linked into the console app.
 *
 * Only the root level is managed: a "save" is a directory in the card root.
 * Saves are exchanged as EMS .psu files.
 */
#ifndef MCFS_H
#define MCFS_H

#include <stdint.h>

#define MC_CLUSTER 1024
#define MC_NAME    33

enum {
    MC_OK = 0,
    MC_E_IO = -1,       /* read/write failed */
    MC_E_FORMAT = -2,   /* not a (valid) raw memory card image */
    MC_E_ECC = -3,      /* image has ECC data, only raw images are supported */
    MC_E_NOTFOUND = -4,
    MC_E_EXISTS = -5,
    MC_E_FULL = -6,
    MC_E_BADPSU = -7,
    MC_E_RDONLY = -8
};

typedef struct mc {
    int fd;
    int writable;
    uint32_t clusters, alloc_offset, alloc_end, root;
    uint32_t ifc[32];
    uint32_t fat_idx, fat[256];   /* cached FAT cluster */
    int fat_valid, fat_dirty;
    uint32_t ifc_idx, ifc_buf[256]; /* cached indirect FAT cluster */
    int ifc_valid;
    uint32_t cursor;              /* where the next free cluster search starts */
} mc_t;

struct mc_save {
    char name[MC_NAME];
    uint32_t size_kb;   /* clusters used by the files */
    uint32_t entries;   /* files in the save */
    uint16_t mode;
};

int mc_open(mc_t *mc, const char *path, int writable);
int mc_close(mc_t *mc); /* flushes; always call, even after errors */

/* fills out[] with the saves in the root, sorted by name; returns the count or <0 */
int mc_list(mc_t *mc, struct mc_save *out, int max);
uint32_t mc_free_kb(mc_t *mc);

/* readable title from icon.sys (full-width Shift-JIS letters become ASCII), "" if none */
int mc_title(mc_t *mc, const char *name, char *out, int len);

int mc_delete(mc_t *mc, const char *name);
int mc_export_psu(mc_t *mc, const char *name, const char *path);
int mc_import_psu(mc_t *mc, const char *path, char *name_out); /* name_out: MC_NAME bytes or NULL */

#endif
