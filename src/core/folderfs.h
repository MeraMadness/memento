/*
 * "Folder mode": the PS2 drive is not a hard disk with PFS partitions but a mounted drive with a normal file system (an SD card in an
 * MX4SIO adapter, a USB stick, a share), laid out the way OPL reads it:
 *
 *   DVD/ID.Title.iso   CD/ID.Title.iso      the games (.zso too)
 *   ART/  CFG/  CHT/  VMC/                  covers, game settings, cheats, memory cards
 *   POPS/ID.Title.VCD, POPS/<game>/         PS1 games (POPStarter) and the folder of each (memory cards, settings)
 *   APPS/                                   programs
 *
 * These functions are what tools.c calls instead of hdl_dump and pfsshell when the "device" is a folder; nothing here needs
 * administrator rights. The names of partitions and folders are the ones the app already uses for the PFS disk, mapped to the folders above.
 */
#ifndef PS2GM_FOLDERFS_H
#define PS2GM_FOLDERFS_H

#include "tools.h"

gboolean folder_is(const char *device);

GPtrArray *folder_list_games(const char *root, GError **error);
gboolean folder_space(const char *root, guint64 *total_mb, guint64 *used_mb, guint64 *free_mb, GError **error);
gboolean folder_install(const char *root, const char *media, const char *title, const char *iso, const char *id, Ps2gmProgress progress,
                        gpointer user_data, GError **error);
gboolean folder_remove_game(const char *root, const char *id, const char *title, GError **error);

GPtrArray *folder_list(const char *root, const char *partition, const char *dir, GError **error);
gboolean folder_put(const char *root, const char *partition, const char *remote_dir, char *const *local_files, GError **error);
gboolean folder_get(const char *root, const char *partition, const char *remote_dir, char *const *names, const char *local_dir, GError **error);
gboolean folder_remove(const char *root, const char *partition, const char *remote_dir, const char *name, GError **error);
gboolean folder_rmdir(const char *root, const char *partition, const char *remote_dir, const char *name, GError **error);
gboolean folder_rename(const char *root, const char *partition, const char *remote_dir, const char *from, const char *to, GError **error);

#endif
