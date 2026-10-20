/*
 * The top bar (the name, and a chip with the connected disk and how full it is) and the tabs at the bottom
 * (Library, Homebrew, Devices, Settings). The pages themselves are in the view stack of the window.
 */
#include "app.h"

static const struct {
    const char *page, *label, *icon;
} tabs[] = {
    {"library", N_("Library"), "view-grid-symbolic"},
    {"transfers", N_("Transfers"), "folder-download-symbolic"},
    {"saves", N_("Saves"), "media-flash-symbolic"},
    {"apps", N_("Homebrew"), "application-x-executable-symbolic"},
    {"connect", N_("Devices"), "drive-harddisk-symbolic"},
    {"settings", N_("Settings"), "emblem-system-symbolic"},
};
#define N_TABS G_N_ELEMENTS(tabs)

static GtkWidget *tab_button[N_TABS], *tab_label[N_TABS];
static GtkWidget *status_dot, *status_state, *status_scan, *status_counts; /* the status bar */
static char *last_scan;
static GtkWidget *chip_dot, *chip_name, *chip_usage, *chip_bar;

/* ---- the disk chip ---------------------------------------------------------------------------- */

void update_topbar(void)
{
    if (A.backend != NULL && A.lib != NULL && A.lib->total_mb > 0) {
        char *used = mb_text(A.lib->used_mb), *total = mb_text(A.lib->total_mb), *usage = g_strdup_printf("%s / %s", used, total);

        gtk_widget_remove_css_class(chip_dot, "off");
        gtk_label_set_text(GTK_LABEL(chip_name), A.device_name && *A.device_name ? A.device_name : _("PS2 disk"));
        gtk_label_set_text(GTK_LABEL(chip_usage), usage);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(chip_bar), (double)A.lib->used_mb / A.lib->total_mb);
        gtk_widget_set_visible(chip_usage, TRUE);
        gtk_widget_set_visible(chip_bar, TRUE);
        g_free(used);
        g_free(total);
        g_free(usage);
    } else {
        gtk_widget_add_css_class(chip_dot, "off");
        gtk_label_set_text(GTK_LABEL(chip_name), _("No disk connected"));
        gtk_widget_set_visible(chip_usage, FALSE);
        gtk_widget_set_visible(chip_bar, FALSE);
    }
}

GtkWidget *build_topbar(void)
{
    GtkWidget *header = adw_header_bar_new(), *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10), *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *name = gtk_label_new(NULL), *sub, *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    char *lower = g_utf8_strdown(APP_NAME, -1), *upper = g_utf8_strup(_("PS2 library manager"), -1);

    gtk_label_set_text(GTK_LABEL(name), lower);
    gtk_widget_add_css_class(name, "brand-name");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    sub = gtk_label_new(upper);
    gtk_widget_add_css_class(sub, "brand-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_box_append(GTK_BOX(names), name);
    gtk_box_append(GTK_BOX(names), sub);
    gtk_widget_set_margin_start(brand, 12);
    gtk_box_append(GTK_BOX(brand), names);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), brand);

    chip_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(chip_dot, "chip-dot");
    gtk_widget_set_valign(chip_dot, GTK_ALIGN_CENTER);
    chip_name = gtk_label_new("");
    gtk_widget_add_css_class(chip_name, "chip-name");
    chip_usage = gtk_label_new("");
    gtk_widget_add_css_class(chip_usage, "chip-usage");
    chip_bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(chip_bar, "chip-bar");
    gtk_widget_set_valign(chip_bar, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(chip), chip_dot);
    gtk_box_append(GTK_BOX(chip), chip_name);
    gtk_box_append(GTK_BOX(chip), chip_usage);
    gtk_box_append(GTK_BOX(chip), chip_bar);
    gtk_widget_add_css_class(chip, "device-chip");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), chip);

    A.refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(A.refresh_btn, _("Read the disk again"));
    gtk_widget_add_css_class(A.refresh_btn, "flat");
    g_signal_connect(A.refresh_btn, "clicked", G_CALLBACK(on_refresh), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), A.refresh_btn);

    gtk_widget_add_css_class(header, "topbar");
    update_topbar();
    g_free(lower);
    g_free(upper);
    return header;
}

/* ---- the tabs ---------------------------------------------------------------------------------- */

static void on_tab_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    adw_view_stack_set_visible_child_name(A.stack, tabs[GPOINTER_TO_INT(data)].page);
}

/* follow the page (also when the script mode or a button changes it); the Add games page belongs to the Library tab */
static void on_page_changed(GObject *stack, GParamSpec *pspec, gpointer data)
{
    const char *page = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(stack));
    guint i;

    (void)pspec;
    (void)data;
    if (page != NULL && !strcmp(page, "add"))
        page = "library";
    for (i = 0; i < N_TABS; i++) {
        if (page != NULL && !strcmp(tabs[i].page, page))
            gtk_widget_add_css_class(tab_button[i], "active");
        else
            gtk_widget_remove_css_class(tab_button[i], "active");
    }
}

/* the number of running and waiting transfers on the Transfers tab */
void tabs_set_badge(guint active)
{
    guint i;

    for (i = 0; i < N_TABS; i++)
        if (!strcmp(tabs[i].page, "transfers") && tab_label[i] != NULL) {
            char *text = active ? g_strdup_printf("%s · %u", _(tabs[i].label), active) : g_strdup(_(tabs[i].label));

            gtk_label_set_text(GTK_LABEL(tab_label[i]), text);
            g_free(text);
        }
}

void tabs_set_connected(gboolean on)
{
    /* without a disk only Devices (to connect) and Settings make sense */
    guint i;

    for (i = 0; i < N_TABS; i++)
        if (!strcmp(tabs[i].page, "library") || !strcmp(tabs[i].page, "apps"))
            gtk_widget_set_sensitive(tab_button[i], on);
    if (status_state != NULL)
        update_statusbar(FALSE);
}

/* ---- the status bar under the tabs: is a disk ready, and when was it last read ---- */


void update_statusbar(gboolean scanned)
{
    gboolean on = A.backend != NULL && A.lib != NULL;
    char *text;

    if (scanned) {
        GDateTime *now = g_date_time_new_now_local();

        g_free(last_scan);
        last_scan = g_date_time_format(now, "%H:%M");
        g_date_time_unref(now);
    }
    if (!on) {
        g_clear_pointer(&last_scan, g_free);
        gtk_widget_remove_css_class(status_dot, "ok");
        gtk_label_set_text(GTK_LABEL(status_state), _("No disk connected"));
        gtk_label_set_text(GTK_LABEL(status_counts), "");
        gtk_label_set_text(GTK_LABEL(status_scan), "");
        return;
    }
    gtk_widget_add_css_class(status_dot, "ok");
    gtk_label_set_text(GTK_LABEL(status_state), _("Disk ready"));
    text = transfers_summary();
    if (text == NULL)
        text = g_strdup_printf(_("%u PS2 · %u PS1 · %u programs"), A.lib->games->len, A.lib->vcds->len, A.lib->apps ? A.lib->apps->len : 0);
    gtk_label_set_text(GTK_LABEL(status_counts), text);
    g_free(text);
    text = last_scan ? g_strdup_printf(_("Last read: %s"), last_scan) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(status_scan), text);
    g_free(text);
}

GtkWidget *build_statusbar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    status_dot = gtk_label_new("●");
    status_state = gtk_label_new("");
    status_counts = gtk_label_new("");
    status_scan = gtk_label_new("");
    gtk_widget_set_hexpand(status_counts, TRUE);
    gtk_label_set_xalign(GTK_LABEL(status_counts), 0.5);
    gtk_box_append(GTK_BOX(bar), status_dot);
    gtk_box_append(GTK_BOX(bar), status_state);
    gtk_box_append(GTK_BOX(bar), status_counts);
    gtk_box_append(GTK_BOX(bar), status_scan);
    gtk_widget_add_css_class(bar, "statusbar");
    update_statusbar(FALSE);
    return bar;
}

GtkWidget *build_tabbar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    guint i;

    for (i = 0; i < N_TABS; i++) {
        GtkWidget *b = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3), *label = gtk_label_new(_(tabs[i].label));
        GtkWidget *icon = gtk_image_new_from_icon_name(tabs[i].icon);

        gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "tab-label");
        tab_label[i] = label;
        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), label);
        gtk_button_set_child(GTK_BUTTON(b), box);
        gtk_widget_add_css_class(b, "tab");
        gtk_widget_set_hexpand(b, FALSE);
        gtk_widget_set_size_request(b, 124, 58); /* wide tabs, like the design (a stylesheet padding is overridden by libadwaita) */
        g_signal_connect(b, "clicked", G_CALLBACK(on_tab_clicked), GINT_TO_POINTER((int)i));
        gtk_box_append(GTK_BOX(bar), b);
        tab_button[i] = b;
    }
    gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
    g_signal_connect(A.stack, "notify::visible-child-name", G_CALLBACK(on_page_changed), NULL);
    {
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); /* full width background, tabs in the middle */

        gtk_widget_add_css_class(outer, "tabbar");
        gtk_widget_set_hexpand(bar, TRUE);
        gtk_box_append(GTK_BOX(outer), bar);
        on_page_changed(G_OBJECT(A.stack), NULL, NULL); /* highlight the tab of the page that is showing */
        return outer;
    }
}
