/* Small helpers used by every page. */
#include "app.h"

App A;
int active_jobs;

/* ---- helpers ----------------------------------------------------------------------------------- */

void toast(const char *msg)
{
    AdwToast *t = adw_toast_new(msg);

    adw_toast_set_timeout(t, 6);
    adw_toast_overlay_add_toast(A.toasts, t);
}

char *size_text(guint64 kb)
{
    if (kb < 1024)
        return g_strdup_printf("%lu KB", (gulong)kb);
    if (kb >= 1024 * 1024)
        return g_strdup_printf("%.1f GB", kb / 1024.0 / 1024.0);
    return g_strdup_printf("%.0f MB", kb / 1024.0);
}

char *mb_text(guint64 mb)
{
    return size_text(mb * 1024);
}

void show_error(const char *heading, const char *body)
{
    AdwDialog *d = adw_alert_dialog_new(heading, body);

    if (g_getenv("PS2GM_SCRIPT"))
        g_printerr("[dialog] %s: %s\n", heading, body);

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
    adw_dialog_present(d, GTK_WIDGET(A.win));
}

GtkWidget *button_row(const char *title, const char *subtitle, const char *label, GCallback cb, GtkWidget **out)
{
    GtkWidget *row = adw_action_row_new(), *b = gtk_button_new_with_label(label);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    if (cb != NULL)
        g_signal_connect(b, "clicked", cb, NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
    if (out)
        *out = b;
    return row;
}
