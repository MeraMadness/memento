/*
 * Wrappers around the two command line tools that do the actual disk work:
 *   hdl_dump  - installs/removes PS2 games (HDL partitions) and lists them
 *   pfsshell  - reads/writes files inside the PFS partitions (+OPL, __.POPS, __common)
 *
 * The wrappers never decide who may touch a disk; that is the job of the privileged helper that
 * calls them (src/helper). They do refuse any argument that could break the tools' own parsing.
 */
#ifndef PS2GM_TOOLS_H
#define PS2GM_TOOLS_H

#include "ps2gm.h"

typedef struct {
    char *dir; /* folder with the hdl_dump and pfsshell binaries */
    char *hdl_dump;
    char *pfsshell;
} Ps2gmTools;

/* dir NULL = the folder this executable lives in */
/*
 * Can a program running as root trust this file? Only if it and every folder above it belong to root and nobody else can
 * write to them (a user who could replace the file could run anything as root). Symbolic links are followed first.
 * On FALSE, *why (free with g_free) says which path is the problem.
 */
gboolean ps2gm_path_root_safe(const char *path, char **why);

gboolean ps2gm_tools_init(Ps2gmTools *t, const char *dir, GError **error);
void ps2gm_tools_clear(Ps2gmTools *t);

/* ---- PS2 games (hdl_dump) ---------------------------------------------------------------------- */

typedef struct {
    char *media;    /* "CD" or "DVD" */
    guint64 size_kb;
    char *flags;    /* compatibility flags as hdl_dump prints them, may be "" */
    char *id;       /* SLES_539.08 */
    char *title;
} HdlGame;

typedef struct {
    char *media; /* "CD", "DVD" or "?" when hdl_dump cannot tell */
    guint64 size_kb;
    char *label;
    char *id;
    gboolean dual_layer; /* hdl_dump said "dual-layer" */
} HdlIsoInfo;

void hdl_game_free(gpointer game);
void hdl_iso_info_clear(HdlIsoInfo *info);

GPtrArray *hdl_list_games(const Ps2gmTools *t, const char *device, GError **error); /* of HdlGame */
gboolean hdl_disk_space(const Ps2gmTools *t, const char *device, guint64 *total_mb, guint64 *used_mb,
                        guint64 *free_mb, GError **error);
/* reads hdl_dump's cdvd_info2 line:  [dual-layer ]CD|DVD|? <size>KB "<label>" "<ID>"  (exposed for tests) */
gboolean hdl_parse_cdvd_info(const char *text, HdlIsoInfo *info);
gboolean hdl_iso_info(const Ps2gmTools *t, const char *iso, HdlIsoInfo *info, GError **error);
gboolean hdl_install(const Ps2gmTools *t, const char *device, const char *media, const char *title,
                     const char *iso, const char *id, Ps2gmProgress progress, gpointer user_data,
                     GError **error);
/*
 * Removes an installed game: finds the HDL partition whose ID and title both match (exactly one must),
 * then deletes it. hdl_dump has no delete command, the partition is removed with pfsshell.
 */
gboolean hdl_remove_game(const Ps2gmTools *t, const char *device, const char *id, const char *title,
                         GError **error);

/* ---- files in PFS partitions (pfsshell) -------------------------------------------------------- */

typedef struct {
    char *name;
    guint64 size;
    gboolean is_dir;
} PfsEntry;

void pfs_entry_free(gpointer entry);

/* partition must be one of the names the app uses; remote paths are relative to the partition root */
GPtrArray *pfs_list(const Ps2gmTools *t, const char *device, const char *partition, const char *dir,
                    GError **error); /* of PfsEntry; NULL with an error if the directory is missing */
gboolean pfs_put(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                 char *const *local_files, GError **error); /* NULL-terminated list; creates remote_dir */
/* copies files from a folder of a partition into the local folder local_dir (NULL-terminated names) */
gboolean pfs_get(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                 char *const *names, const char *local_dir, GError **error);
/* nominal size and number of parts of a partition, from the disk's partition table (hdl_dump toc) */
gboolean hdl_partition_size(const Ps2gmTools *t, const char *device, const char *name, guint *parts,
                            guint64 *size_mb, GError **error);

/* size and free space of a mounted partition, in MiB. Only reliable for partitions of a single part:
   pfsshell's df is wrong for partitions with sub-partitions such as __.POPS (see ops.c / helper.c) */
gboolean pfs_space(const Ps2gmTools *t, const char *device, const char *partition, guint64 *size_mb,
                   guint64 *free_mb, GError **error);
/* renames a file inside a folder of a partition; fails if the new name is already taken */
gboolean pfs_rename(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir,
                    const char *from, const char *to, GError **error);
/* TRUE if the file is a PS2 program (a 32-bit little-endian MIPS ELF); else error says why */
gboolean ps2gm_elf_check(const char *path, GError **error);
/* TRUE if name ends in .elf in any letter case */
gboolean ps2gm_is_elf_name(const char *name);
gboolean ps2gm_is_zso_name(const char *name);
/* removes an empty folder <name> inside remote_dir */
gboolean pfs_rmdir(const Ps2gmTools *t, const char *device, const char *partition, const char *remote_dir, const char *name, GError **error);
gboolean pfs_remove(const Ps2gmTools *t, const char *device, const char *partition,
                    const char *remote_dir, const char *name, GError **error);

/* ---- shared checks ----------------------------------------------------------------------------- */

/* TRUE if the string is safe to hand to the tools as a (quoted) argument */
gboolean ps2gm_arg_ok(const char *s);
/* TRUE if path has no ".." components and is not absolute */
gboolean ps2gm_remote_path_ok(const char *path);

#endif
