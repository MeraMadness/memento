/*
 * Talks to ps2gm-helper (started once per session, normally through pkexec) and keeps the
 * parsed contents of the PS2 disk (the "library").
 */
#ifndef PS2GM_BACKEND_H
#define PS2GM_BACKEND_H

#include "ps2gm.h"

typedef struct _Backend Backend;

typedef struct {
    char *media; /* CD / DVD */
    guint64 size_kb;
    char *id;
    char *title;
} LibGame;

typedef struct {
    char *name;
    guint64 size;
} LibFile;

typedef struct {
    guint64 total_mb, used_mb, free_mb;        /* the whole disk */
    guint64 opl_size_mb, opl_free_mb;          /* +OPL */
    guint64 pops_size_mb, pops_free_mb;        /* __.POPS */
    GPtrArray *games;      /* LibGame: PS2 games */
    GPtrArray *vcds;       /* LibFile: PS1 images in __.POPS */
    GPtrArray *apps;       /* LibFile: the files in the root of APPS (programs are the .elf ones) */
    GHashTable *art;       /* art file names in +OPL/ART -> size in bytes (guint64 *) */
    GHashTable *cfgs;      /* config file names in +OPL/CFG */
    GHashTable *cheats;    /* cheat file names in +OPL/CHT */
    GPtrArray *vmcs;       /* LibFile: the virtual memory cards in +OPL/VMC */
    GHashTable *popsdirs;  /* folders in __common/POPS */
} Library;

typedef void (*BackendLineFn)(const char *line, gpointer user_data);

/* helper_path: NULL = the ps2gm-helper next to this executable. use_pkexec: ask for the admin password. */
Backend *backend_start(const char *device, gboolean use_pkexec, const char *helper_path, GError **error);
void backend_stop(Backend *b);

/* One command (fields are joined with tabs). Blocks until the helper answers END/ERR; data lines go to cb. */
gboolean backend_run(Backend *b, char *const *fields, BackendLineFn cb, gpointer user_data, GError **error);

Library *backend_library(Backend *b, GError **error);
void library_free(Library *lib);
gboolean library_has_art(const Library *lib, const char *id); /* the front cover exists */
/* size in bytes of a file of +OPL/ART, 0 if it is not there */
guint64 library_art_size(const Library *lib, const char *name);

/*
 * Pictures of the disk on this computer: makes sure cache_dir holds the PNGs named in names (NULL-terminated, from
 * +OPL/ART) as they are now on the disk. Files already there with the same size are not read again.
 * Returns how many were copied from the disk.
 */
guint backend_cache_art(Backend *b, const Library *lib, char *const *names, const char *cache_dir, GError **error);
/* ~/.cache/ps2gm/art/<short code of the device>: the pictures of one disk */
/* GET one file of +OPL: *path is a temporary copy made for this user (valid until the next GET), free with g_free */
/* CHEATS.TXT of a PS1 game folder (__common/POPS/<folder>): *text is NULL when the game has no such file */
/* the TROJAN_#.BIN / PATCH_#.BIN files of a PS1 game folder: *files is a new array of LibFile (name, size), possibly empty */
gboolean backend_list_fix_files(Backend *b, const char *folder, GPtrArray **files, GError **error);
gboolean backend_read_pops_text(Backend *b, const char *folder, char **text, GError **error);
/* the same for CHEATS.TXT, VMCDIR.TXT or DISCS.TXT */
gboolean backend_read_pops_file(Backend *b, const char *folder, const char *name, char **text, GError **error);
gboolean backend_get_file(Backend *b, const char *folder, const char *name, char **path, GError **error);
char *backend_art_cache_dir(const char *device);

/* reads one text file of +OPL/CFG or +OPL/CHT (folder "CFG"/"CHT", name with its ending) into *text (g_free) */
gboolean backend_read_text(Backend *b, const char *folder, const char *name, char **text, GError **error);

/* "SLES_026.98.Resident Evil 3.VCD" -> id "SLES_026.98" (g_free), title "Resident Evil 3"; FALSE if no ID */
gboolean vcd_name_parse(const char *name, char **id, char **title);

#endif
