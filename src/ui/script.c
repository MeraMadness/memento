#include <stdlib.h>

#include "app.h"

/* ---- scripted actions for tests (PS2GM_SCRIPT) -------------------------------------------------- */
/*
 * Runs the real buttons' code paths without a person: steps separated by ";"
 *   connect=<device or image>   iso=<file>   install_ps2   cue=<a.cue>,<b.cue>   install_ps1
 *   remove_ps2=<id>|<title>   remove_ps1=<vcd name>   art=<ps1|ps2>|<id>|<title>   refresh
 *   cover=<ps1|ps2>|<picture>[|whole] (the carousel's choice for the next install)   keepzso=<on|off>   packzso=<on|off>   appadd=<file.elf>[|keep]   apprename=<old>|<new>   appremove=<name>   ok (closes the open dialog)   select=<title>   setart=<ps1|ps2>|<id>|<key>|<file or address, - to download>[|whole]   artdialog=<ps1|ps2>|<id>|<title>
 *   page=<connect|library|add>   shot=<png>   wait=<ms>   quit
 * The next step runs when no job is active. Errors are printed to stderr.
 */

static char **script_steps;
static guint script_pos;
static int script_delay_ms;

static void take_shot(const char *path)
{
    GtkWidget *w = GTK_WIDGET(A.win);
    GdkPaintable *p = gtk_widget_paintable_new(w);
    GtkSnapshot *snap = gtk_snapshot_new();
    GskRenderNode *node;
    GdkTexture *tex;
    GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(A.win));

    gdk_paintable_snapshot(p, snap, gtk_widget_get_width(w), gtk_widget_get_height(w));
    node = gtk_snapshot_free_to_node(snap);
    tex = gsk_renderer_render_texture(r, node, NULL);
    gdk_texture_save_to_png(tex, path);
    g_object_unref(tex);
    gsk_render_node_unref(node);
    g_object_unref(p);
}

static gboolean script_tick(gpointer data)
{
    char *step, *arg;

    (void)data;
    if (active_jobs > (int)transfers_count()) /* the transfers run on their own: the step waittransfers waits for them */
        return G_SOURCE_CONTINUE;
    if (script_delay_ms > 0) {
        script_delay_ms -= 250;
        return G_SOURCE_CONTINUE;
    }
    step = script_steps[script_pos];
    if (step == NULL) {
        g_application_quit(G_APPLICATION(A.app));
        return G_SOURCE_REMOVE;
    }
    if (!strcmp(step, "waittransfers") && transfers_busy()) /* look again at the next tick, without logging each time */
        return G_SOURCE_CONTINUE;
    script_pos++;
    arg = strchr(step, '=');
    if (arg)
        *arg++ = '\0';
    g_printerr("[script] %s%s%s\n", step, arg ? " " : "", arg ? arg : "");

    if (!strcmp(step, "waittransfers")) {
    } else if (!strcmp(step, "connect"))
        run_job("Connecting to the PS2 disk", job_connect, g_strdup(arg), g_free, after_connect);
    else if (!strcmp(step, "iso"))
        load_iso(arg);
    else if (!strcmp(step, "isos")) { /* isos=<path>,<path>...: several games at once */
        char **paths = g_strsplit(arg, ",", -1);

        load_isos(paths);
        g_strfreev(paths);
    }
    else if (!strcmp(step, "install_ps2"))
        on_install_ps2(NULL, NULL);
    else if (!strcmp(step, "cue")) {
        char **paths = g_strsplit(arg, ",", -1);

        load_cues(paths, g_strv_length(paths));
        g_strfreev(paths);
    } else if (!strcmp(step, "install_ps1"))
        on_install_ps1(NULL, NULL);
    else if (!strcmp(step, "remove_ps2")) {
        char **v = g_strsplit(arg, "|", 2);

        do_remove_ps2(v[0], v[1]);
        g_strfreev(v);
    } else if (!strcmp(step, "remove_ps1"))
        do_remove_ps1(arg, FALSE);
    else if (!strcmp(step, "art")) {
        char **v = g_strsplit(arg, "|", 3);

        do_update_art(v[1], !strcmp(v[0], "ps1") ? PS2GM_PS1 : PS2GM_PS2, v[2], FALSE);
        g_strfreev(v);
    } else if (!strcmp(step, "setart")) { /* setart=<ps1|ps2>|<id>|<key>|<file, address, or - to download> */
        char **v = g_strsplit(arg, "|", 5);

        do_set_art(!strcmp(v[0], "ps1") ? PS2GM_PS1 : PS2GM_PS2, v[1], v[2], !strcmp(v[3], "-") ? NULL : v[3],
                   v[4] != NULL && !strcmp(v[4], "whole"));
        g_strfreev(v);
    } else if (!strcmp(step, "artdialog")) { /* artdialog=<ps1|ps2>|<id>|<title>: only to look at it */
        char **v = g_strsplit(arg, "|", 3);

        show_art_dialog(!strcmp(v[0], "ps1") ? PS2GM_PS1 : PS2GM_PS2, v[1], v[2]);
        g_strfreev(v);
    } else if (!strcmp(step, "cheats")) { /* cheats=<id>|<title>: opens the cheats dialog */
        char **v = g_strsplit(arg, "|", 2);

        show_cheats_dialog(v[0], v[1] ? v[1] : v[0]);
        g_strfreev(v);
    } else if (!strcmp(step, "coverpicker")) { /* coverpicker=<ps1|ps2>|<id>|<title>: opens the carousel (nothing is saved) */
        char **v = g_strsplit(arg, "|", 3);

        show_cover_picker(!strcmp(v[0], "ps1") ? PS2GM_PS1 : PS2GM_PS2, v[1], v[2] ? v[2] : v[1], NULL, NULL, NULL);
        g_strfreev(v);
    } else if (!strcmp(step, "cover")) { /* cover=<ps1|ps2>|<picture>[|whole]: the carousel's choice for the next install */
        char **v = g_strsplit(arg, "|", 3);

        set_install_cover(!strcmp(v[0], "ps1") ? PS2GM_PS1 : PS2GM_PS2, v[1], v[2] != NULL && !strcmp(v[2], "whole"));
        g_strfreev(v);
    } else if (!strcmp(step, "keepzso")) { /* keepzso=<on|off>: the "keep it compressed" switch of a .zso */
        adw_switch_row_set_active(ADW_SWITCH_ROW(A.iso_keep_row), !strcmp(arg, "on"));
    } else if (!strcmp(step, "appadd")) { /* appadd=<file.elf>[|keep]: add a program (keep: keep an old one of that name) */
        char **v = g_strsplit(arg, "|", 2);

        apps_add_file(v[0], FALSE, v[1] != NULL && !strcmp(v[1], "keep"));
        g_strfreev(v);
    } else if (!strcmp(step, "apprename")) { /* apprename=<old>|<new> */
        char **v = g_strsplit(arg, "|", 2);

        apps_rename_file(v[0], v[1]);
        g_strfreev(v);
    } else if (!strcmp(step, "appremove")) {
        apps_remove_file(arg);
    } else if (!strcmp(step, "settings")) { /* settings=<id>|<title>: opens the OPL settings of a game */
        char **v = g_strsplit(arg, "|", 2);

        show_settings_dialog(v[0], v[1] ? v[1] : v[0]);
        g_strfreev(v);
    } else if (!strcmp(step, "packzso")) { /* packzso=<on|off>: the "compress to ZSO while installing" switch of an .iso */
        adw_switch_row_set_active(ADW_SWITCH_ROW(A.iso_pack_row), !strcmp(arg, "on"));
    } else if (!strcmp(step, "listview")) { /* listview=<on|off> */
        library_set_list_view(!strcmp(arg, "on"));
    } else if (!strcmp(step, "ps1fix")) { /* ps1fix=<VCD name>: opens the PS1 fixes dialog */
        show_ps1fix_dialog(arg, arg);
    } else if (!strcmp(step, "card")) { /* card=<name of a card on the disk> */
        saves_open_remote(arg);
    } else if (!strcmp(step, "cardfile")) { /* cardfile=<path of a card file> */
        saves_open_file(arg);
    } else if (!strcmp(step, "importpsu")) { /* importpsu=<path>,<path>... into the open card */
        char **paths = g_strsplit(arg, ",", -1);

        saves_import_psus(paths);
        g_strfreev(paths);
    } else if (!strcmp(step, "deletesave")) { /* deletesave=<save name>: asks first, "confirm" answers yes */
        saves_delete(arg);
    } else if (!strcmp(step, "verify")) { /* verify=<iso|ps1>: press Check on that card */
        verify_start(!strcmp(arg, "ps1") ? A.ps1_verify : A.iso_verify);
    } else if (!strcmp(step, "confirm")) { /* answers the destructive button of the dialog that is showing (not the cancel one) */
        AdwDialog *dlg = adw_application_window_get_visible_dialog(A.win);

        if (dlg != NULL && ADW_IS_ALERT_DIALOG(dlg)) {
            g_signal_emit_by_name(dlg, "response::delete", "delete");
            adw_dialog_force_close(dlg);
        }
    } else if (!strcmp(step, "ok")) { /* closes the dialog that is showing, as a click on OK would */
        AdwDialog *dlg = adw_application_window_get_visible_dialog(A.win);

        if (dlg != NULL)
            adw_dialog_force_close(dlg);
    } else if (!strcmp(step, "select")) { /* select=<exact title of a game in the library> */
        if (!select_game_by_title(arg))
            g_printerr("[script] no game titled %s\n", arg);
    } else if (!strcmp(step, "refresh"))
        refresh_library();
    else if (!strcmp(step, "page"))
        adw_view_stack_set_visible_child_name(A.stack, arg);
    else if (!strcmp(step, "shot"))
        take_shot(arg);
    else if (!strcmp(step, "wait"))
        script_delay_ms = atoi(arg);
    else if (!strcmp(step, "quit")) {
        g_application_quit(G_APPLICATION(A.app));
        return G_SOURCE_REMOVE;
    } else
        g_printerr("[script] unknown step %s\n", step);
    return G_SOURCE_CONTINUE;
}

void debug_start(void)
{
    const char *sc = g_getenv("PS2GM_SCRIPT");

    if (sc == NULL)
        return;
    script_steps = g_strsplit(sc, ";", -1);
    g_timeout_add(500, script_tick, NULL);
}
