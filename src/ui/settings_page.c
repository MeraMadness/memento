/* Settings page: the language, and what Memento is made of (versions and licenses). */
#include "app.h"

#ifndef APP_VERSION
#define APP_VERSION "0.1"
#endif

static GtkWidget *info_row(const char *title, const char *value)
{
    GtkWidget *row = adw_action_row_new();

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
    gtk_widget_set_focusable(row, FALSE);
    return row;
}

GtkWidget *build_settings_page(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new(), *clamp = adw_clamp_new(), *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *eyebrow = gtk_label_new(NULL), *title = gtk_label_new(_("Settings")), *sub = gtk_label_new(_("How Memento looks and speaks."));
    AdwPreferencesGroup *lang = ADW_PREFERENCES_GROUP(adw_preferences_group_new()), *about = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    char *eb = g_utf8_strup(APP_NAME, -1), *version = g_strdup_printf("%s %s", APP_NAME, APP_VERSION);

    gtk_label_set_text(GTK_LABEL(eyebrow), eb);
    gtk_widget_add_css_class(eyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0);
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(sub, "page-sub");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);

    adw_preferences_group_set_title(lang, _("Language"));
    adw_preferences_group_add(lang, build_language_row());

    adw_preferences_group_set_title(about, _("About"));
    adw_preferences_group_add(about, info_row(_("Version"), version));
    adw_preferences_group_add(about, info_row(_("Colors"), "Catppuccin Mocha, Mauve (MIT License)"));
    adw_preferences_group_add(about, info_row(_("Headings font"), "Cinzel, Copyright 2020 The Cinzel Project Authors (SIL Open Font License 1.1)"));
    adw_preferences_group_add(about, info_row(_("Hash lists"), _("Redump (redump.org): only the checksums of good dumps are used, to check images.")));
    adw_preferences_group_add(about, info_row(_("Disk tools"), "hdl_dump and pfsshell (GPL). Game covers: ps2-covers and the OPL Manager art archive."));

    gtk_widget_set_margin_top(page, 22);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    {
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GdkPaintable *art = pixel_art("ps2-controller.png", 70);

        gtk_box_append(GTK_BOX(texts), eyebrow);
        gtk_box_append(GTK_BOX(texts), title);
        gtk_box_append(GTK_BOX(texts), sub);
        gtk_widget_set_hexpand(texts, TRUE);
        gtk_box_append(GTK_BOX(head), texts);
        if (art != NULL) {
            GtkWidget *pic = gtk_picture_new_for_paintable(art);

            gtk_picture_set_can_shrink(GTK_PICTURE(pic), FALSE);
            gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(head), pic);
            g_object_unref(art);
        }
        gtk_box_append(GTK_BOX(page), head);
    }
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(lang));
    gtk_box_append(GTK_BOX(page), build_redump_group());
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(about));
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    adw_clamp_set_child(ADW_CLAMP(clamp), page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    g_free(eb);
    g_free(version);
    return scroll;
}
