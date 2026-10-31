/*
 * Opening a disk through UDisks (the system service behind "Disks" and the file managers' drive menus).
 * The service opens /dev/sdX for us after a polkit password window and hands back the open descriptor, so the program
 * needs neither root nor access to /dev. This is how a Flatpak reaches a PS2 hard disk.
 */
#ifndef PS2GM_UDISKS_H
#define PS2GM_UDISKS_H

#include "ps2gm.h"

/* The descriptor of dev_path ("/dev/sdb") opened read/write, or -1 with *error set. May show the password window. */
int udisks_open_device(const char *dev_path, GError **error);

#endif
