/*
 * Memento - GTK4 / libadwaita front end.
 *
 * Pages: Connect (pick the PS2 drive), Library (what is on it), Add games (PS2 ISO, PS1 BIN/CUE).
 * All disk work goes through ps2gm-helper (one admin password per session); long jobs run in a
 * worker thread behind a progress dialog.
 *
 * This file only builds the window; the pages are in the other files of this folder (see app.h).
 */
#include "app.h"

static gboolean closing_confirmed;

static void on_busy_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    (void)d;
    (void)data;
    if (!strcmp(response, "close")) {
        closing_confirmed = TRUE;
        gtk_window_close(GTK_WINDOW(A.win));
    }
}

/*
 * "close-request": the answer must be returned, TRUE = do not close (it used to return nothing, so the window
 * sometimes refused to close, for example for a window manager's close key). While a job is running, ask first:
 * stopping an installation half way can leave a half written game on the disk.
 */
static gboolean on_close_window(GtkWindow *w, gpointer d)
{
    (void)w;
    (void)d;
    if (active_jobs > 0 && !closing_confirmed) {
        AdwDialog *dlg = adw_alert_dialog_new(_("Still working"),
                                              _("An operation is still running. Closing now could leave a game half installed on the PS2 disk."));

        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "wait", _("Keep waiting"), "close", _("Close anyway"), NULL);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "close", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "wait");
        g_signal_connect(dlg, "response", G_CALLBACK(on_busy_response), NULL);
        adw_dialog_present(dlg, GTK_WIDGET(A.win));
        return TRUE;
    }
    backend_stop(A.backend);
    A.backend = NULL;
    return FALSE;
}

/* the cover behind the window is for the Library page; the other pages stay plain */
static void on_page_backdrop(GObject *stack, GParamSpec *pspec, gpointer data)
{
    const char *page = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(stack));
    gboolean on = page != NULL && !strcmp(page, "library");

    (void)pspec;
    (void)data;
    gtk_widget_set_visible(A.win_backdrop, on);
    gtk_widget_set_visible(A.win_scrim, on);
}

static void activate(GtkApplication *app, gpointer d)
{
    GtkWidget *view = adw_toolbar_view_new();
    GError *err = NULL;

    (void)d;
    if (A.win != NULL) {
        gtk_window_present(GTK_WINDOW(A.win));
        return;
    }
    A.app = app;
    if (!ps2gm_tools_init(&A.tools, NULL, &err)) {
        g_printerr("%s\n", err->message);
        g_clear_error(&err);
    }
    load_style();
    {
        /* the icon: installed in the system icon folders, or (running from the project's build folder) from data/icons */
        char *self = g_file_read_link("/proc/self/exe", NULL), *dir = self ? g_path_get_dirname(self) : NULL;
        char *dev = dir ? g_build_filename(dir, "..", "data", "icons", NULL) : NULL;

        if (dev != NULL && g_file_test(dev, G_FILE_TEST_IS_DIR))
            gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), dev);
        gtk_window_set_default_icon_name(APP_ID);
        g_free(dev);
        g_free(dir);
        g_free(self);
    }
    A.win = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(A.win), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(A.win), 1280, 800);
    A.toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    A.stack = ADW_VIEW_STACK(adw_view_stack_new());
    adw_view_stack_add_named(A.stack, build_connect_page(), "connect"); /* the Devices tab */
    adw_view_stack_add_named(A.stack, build_library_page(), "library");
    adw_view_stack_add_named(A.stack, build_add_page(), "add"); /* reached from the Add game button */
    adw_view_stack_add_named(A.stack, build_transfers_page(), "transfers");
    adw_view_stack_add_named(A.stack, build_saves_page(), "saves");
    adw_view_stack_add_named(A.stack, build_apps_page(), "apps");
    adw_view_stack_add_named(A.stack, build_settings_page(), "settings");

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), build_topbar());
    adw_toast_overlay_set_child(A.toasts, GTK_WIDGET(A.stack));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(A.toasts));
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), build_tabbar());
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), build_statusbar());
    {
        /* behind everything: the selected game's cover, very blurred (library_page.c sets it); only the Library page shows it */
        GtkWidget *glass = gtk_overlay_new(), *scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        A.win_backdrop = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(A.win_backdrop), GTK_CONTENT_FIT_COVER);
        gtk_picture_set_can_shrink(GTK_PICTURE(A.win_backdrop), TRUE);
        gtk_widget_add_css_class(A.win_backdrop, "win-backdrop");
        gtk_widget_add_css_class(scrim, "win-scrim");
        A.win_scrim = scrim;
        gtk_overlay_set_child(GTK_OVERLAY(glass), A.win_backdrop);
        gtk_overlay_add_overlay(GTK_OVERLAY(glass), scrim);
        gtk_overlay_add_overlay(GTK_OVERLAY(glass), view);
        gtk_overlay_set_measure_overlay(GTK_OVERLAY(glass), view, TRUE);
        gtk_widget_set_overflow(glass, GTK_OVERFLOW_HIDDEN);
        g_signal_connect(A.stack, "notify::visible-child-name", G_CALLBACK(on_page_backdrop), NULL);
        adw_application_window_set_content(A.win, glass);
    }
    g_signal_connect(A.win, "close-request", G_CALLBACK(on_close_window), NULL);
    set_connected(FALSE);
    gtk_window_present(GTK_WINDOW(A.win));
    debug_start();
}

int main(int argc, char **argv)
{
    AdwApplication *app;
    int rc;

    i18n_init();
    app = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
