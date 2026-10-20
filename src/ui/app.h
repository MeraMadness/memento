/*
 * Shared declarations of the Memento window code (src/ui).
 *
 * One file per page, see docs/LAYOUT.md:
 *   main.c          window, header bar, application start
 *   common.c        small helpers shared by every page (toast, error dialog, sizes)
 *   jobs.c          long running work in a thread behind a progress dialog
 *   i18n.c          translations and the language setting
 *   sidebar.c       the navigation sidebar and the free space meter
 *   style.c         the look: colors, cards, banner
 *   connect_page.c  choose the PS2 drive and connect
 *   library_page.c  the launcher: banner, search, cover grid, remove, art
 *   apps_page.c     the programs (homebrew) on the APPS partition: add, update, rename, remove
 *   add_page.c      install PS2 ISOs and PS1 BIN/CUE games
 *   art_dialog.c    choose or download the pictures of one game
 *   cover_dialog.c  the cover carousel: choose a front cover from several, or add your own
 *   settings_dialog.c  OPL settings of a PS2 game: compatibility modes, disk speed, video mode
 *   cheats_dialog.c edit the cheat list of a PS2 game
 *   script.c        scripted actions for tests (PS2GM_SCRIPT)
 */
#ifndef PS2GM_APP_H
#define PS2GM_APP_H

#include <adwaita.h>
#include <string.h>

#include "ops.h"
#include "tools.h"

#define APP_ID "io.github.memento"
#define APP_NAME "Memento" /* shown in the window; the one place to rename the program */

/* the cover chosen in the carousel for the game about to be installed, and its row on the Add games page */
typedef struct {
    char *path;
    gboolean whole;
    GtkWidget *row, *thumb, *choose_btn, *reset_btn;
} CoverSlot;

typedef struct {
    GtkApplication *app;
    AdwApplicationWindow *win;
    AdwToastOverlay *toasts;
    AdwViewStack *stack;
    GtkWidget *refresh_btn, *disconnect_btn;

    /* connect page */
    AdwComboRow *disk_row;
    GtkStringList *disk_model;
    GPtrArray *disk_paths;
    GPtrArray *disk_names; /* the model of each listed disk, in the same order */
    char *image_path; /* a disk image chosen by hand */
    char *device_name;     /* what to call the connected disk in the top bar */
    GtkWidget *conn_status, *conn_connect_box, *conn_connected_box;

    /* session */
    Ps2gmTools tools;
    Backend *backend;
    OpsContext *ops;
    Library *lib;
    char *device;

    /* library page */
    char *art_cache;        /* this disk's pictures on this computer */
    GPtrArray *entries;     /* Entry: the games shown as cards */
    char *selected;         /* key of the selected game, see entry_key() in library_page.c */
    int filter;             /* 0 all, 1 PS2, 2 PS1 */
    int filter2;            /* 0 all games, 1 with cheats, 2 without a cover */
    int sort;               /* 0 name A-Z, 1 name Z-A, 2 largest first, 3 smallest first */
    gboolean list_view;
    GtkWidget *search, *flow, *list, *view_stack, *count_label, *empty_label;
    /* the details panel of the selected game */
    GtkWidget *det_content, *det_empty, *det_empty_title, *det_empty_sub, *det_cover_box, *det_tags, *det_title, *det_id;
    GtkWidget *det_backdrop, *win_backdrop, *win_scrim, *det_vals[4], *det_settings, *det_art, *det_cheats, *det_share, *det_rm;

    /* add page: PS2 */
    char *iso_path;
    gboolean iso_is_zso;  /* iso_path is a compressed .zso */
    guint64 iso_zso_kb;   /* its size on this computer (what goes on the disk when it stays compressed) */
    GtkWidget *iso_keep_row; /* switch: keep a .zso compressed on the disk */
    GtkWidget *iso_pack_row; /* switch: compress an .iso to ZSO while installing it */
    HdlIsoInfo iso_info;
    gboolean iso_ok;
    GtkWidget *iso_file_row, *iso_id_row, *iso_title_row, *iso_art_row, *iso_go;
    CoverSlot iso_cover, ps1_cover;
    /* add page: PS1 */
    GPtrArray *discs; /* Ps1Disc */
    GtkWidget *iso_verify, *ps1_verify;
    GtkWidget *ps1_files_row, *ps1_title_row, *ps1_art_row, *ps1_go;
    AdwPreferencesGroup *ps1_disc_group;
    GPtrArray *ps1_disc_rows;
} App;

/* the one application state; the widgets and the connection to the disk live here */
extern App A;
extern int active_jobs; /* running background jobs (also used by the test script) */

/* ---- common.c ---------------------------------------------------------------------------------- */
void toast(const char *msg);
char *size_text(guint64 kb);
char *mb_text(guint64 mb);
void show_error(const char *heading, const char *body);
/* a row with a title, an optional subtitle and one button; the button is returned in *out */
GtkWidget *button_row(const char *title, const char *subtitle, const char *label, GCallback cb, GtkWidget **out);

/* ---- jobs.c ------------------------------------------------------------------------------------ */
typedef gboolean (*JobFn)(gpointer data, Ps2gmProgress progress, gpointer pdata, GPtrArray *notes, GError **error);
typedef void (*JobDone)(gboolean ok, gpointer data);
/* runs fn(data) in a worker thread behind a progress dialog, then done(ok, data) and free_data(data) */
/* the install queue (transfers.c): runs in the background, one at a time; kind is "PS2" or "PS1" */
void queue_transfer(const char *kind, const char *title, const char *id, JobFn fn, gpointer data, GDestroyNotify free_data, JobDone done);
guint transfers_count(void);   /* how many are queued or running */
gboolean transfers_busy(void); /* something is queued or running: nothing else may touch the disk */
char *transfers_summary(void);
void transfers_show_covers(void); /* after the disk was read: covers of the finished installs */
char *library_cover_path(const char *id); /* a cached cover of the library, or NULL; free with g_free */ /* "Installing X: 43%" for the status bar, or NULL */
GtkWidget *build_transfers_page(void);
/* saves_page.c: the virtual memory cards */
GtkWidget *build_saves_page(void);
void rebuild_saves(void); /* from A.lib */
void saves_open_remote(const char *name); /* a card of the disk */
void saves_open_file(const char *path);   /* a card file on this computer */
void saves_import_psus(char **paths);
void saves_delete(const char *name);
/* verify.c: check an image against the Redump lists */
GtkWidget *build_verify_row(Ps2gmPlatform platform);
void verify_start(GtkWidget *row);
void verify_set_files(GtkWidget *row, char **paths, const char *reason);
GtkWidget *build_redump_group(void);
void tabs_set_badge(guint active); /* the number on the Transfers tab */
void run_job(const char *title, JobFn fn, gpointer data, GDestroyNotify free_data, JobDone done);

/* ---- i18n.c ------------------------------------------------------------------------------------ */
typedef struct {
    const char *code; /* "" = follow the system; else a language code such as "it" or "pt_BR" */
    const char *name; /* in its own language, except the first one (translated) */
} LangOption;
extern const LangOption languages[];
void i18n_init(void);                 /* call first thing in main */
char *i18n_saved_language(void);      /* g_free; "" if none */
void i18n_save_language(const char *code);
GtkWidget *build_language_row(void); /* a combo row to choose the language (Settings page) */

/* ---- topbar.c / style.c ------------------------------------------------------------------------ */
GtkWidget *build_topbar(void);  /* logo, name, the chip with the connected disk */
GtkWidget *build_tabbar(void);  /* Library, Homebrew, Devices, Settings at the bottom */
void update_topbar(void);       /* the chip, from A.lib */
void tabs_set_connected(gboolean on);
void load_style(void);          /* palette, fonts, styles; call before the window is made */

/* ---- settings_page.c --------------------------------------------------------------------------- */
GtkWidget *build_settings_page(void);

/* ---- connect_page.c ---------------------------------------------------------------------------- */
GtkWidget *build_connect_page(void);
void set_connected(gboolean on);
GdkPaintable *pixel_art(const char *name, int height); /* a picture of data/images, scaled with sharp pixels */
GtkWidget *build_statusbar(void);
void update_statusbar(gboolean scanned); /* scanned: the disk was just read, note the time */
void update_devices_page(void); /* connect controls, or the connected disk */
gboolean job_connect(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error);
void after_connect(gboolean ok, gpointer data);
void on_disconnect(GtkButton *b, gpointer d);

/* ---- library_page.c ---------------------------------------------------------------------------- */
GtkWidget *build_library_page(void);
/* reads the disk and the pictures of every game (runs in a job; used when connecting and refreshing) */
gboolean load_library(Ps2gmProgress cb, gpointer pd, GError **error);
void refresh_library(void);
void rebuild_library(void);
void after_change(gboolean ok, gpointer data); /* job finished: toast and read the disk again */
void do_remove_ps2(const char *id, const char *title);
void do_remove_ps1(const char *name);
void do_update_art(const char *id, Ps2gmPlatform platform, const char *title, gboolean replace);
void on_refresh(GtkButton *b, gpointer d);
gboolean select_game_by_title(const char *title);
void library_set_list_view(gboolean list); /* the grid of covers or the list */

/* ---- apps_page.c ------------------------------------------------------------------------------- */
GtkWidget *build_apps_page(void);
void rebuild_apps(void); /* from A.lib */
/* ask: when a program of that name is on the disk, ask whether to replace it (else keep_old decides) */
void apps_add_file(const char *path, gboolean ask, gboolean keep_old);
void apps_remove_file(const char *name);
void apps_rename_file(const char *from, const char *to);

/* ---- add_page.c -------------------------------------------------------------------------------- */
GtkWidget *build_add_page(void);
void update_iso_widgets(void);
void rebuild_disc_rows(void);
void load_iso(const char *path); /* an .iso, or a .zso (read as it is: nothing is unpacked until the install) */
void load_isos(char **paths);    /* one file: as load_iso; several: a list to install one after the other */
void load_cues(char **paths, guint n);
void on_install_ps2(GtkButton *b, gpointer d);
void on_install_ps1(GtkButton *b, gpointer d);
void set_install_cover(Ps2gmPlatform platform, const char *path, gboolean whole); /* script mode */

/* ---- art_dialog.c ------------------------------------------------------------------------------ */
void show_art_dialog(Ps2gmPlatform platform, const char *id, const char *title);
/* replaces one picture (key: COV, COV2, BG, LGO, ICO, SCR, SCR2); source NULL = download, else a file or http(s) address */
void do_set_art(Ps2gmPlatform platform, const char *id, const char *key, const char *source, gboolean whole);

/* ---- cover_dialog.c ---------------------------------------------------------------------------- */
/* path: a copy of the chosen picture that stays until the next choice for this game; whole: do not crop it */
typedef void (*CoverChosen)(const char *path, gboolean whole, gpointer data);
/* free_data (may be NULL) is called on data when the picker is gone, whether or not a cover was chosen */
void show_cover_picker(Ps2gmPlatform platform, const char *id, const char *title, CoverChosen cb, gpointer data,
                       GDestroyNotify free_data);

/* ---- settings_dialog.c ------------------------------------------------------------------------- */
void show_settings_dialog(const char *id, const char *title); /* PS2 games: compatibility modes, DMA, video mode */

/* ---- cheats_dialog.c --------------------------------------------------------------------------- */
void show_cheats_dialog(const char *id, const char *title); /* PS2 games: edit the OPL cheat list */

/* ---- script.c ---------------------------------------------------------------------------------- */
void debug_start(void); /* starts the PS2GM_SCRIPT steps if that variable is set */

#endif
