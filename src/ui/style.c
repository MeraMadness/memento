/*
 * The look of Memento: the Catppuccin Mocha palette with the Mauve accent, mapped onto libadwaita's named colors so
 * every standard widget (buttons, switches, dialogs, entries) follows it, plus the styles of Memento's own widgets.
 * Headings use Cinzel (SIL Open Font License, data/fonts/OFL.txt), loaded from the program's folder.
 */
#include <fontconfig/fontconfig.h>

#include "app.h"

/* palette: https://catppuccin.com/palette (Mocha) */
#define MOCHA_COLORS \
    "@define-color ctp_rosewater #f5e0dc;\n@define-color ctp_flamingo #f2cdcd;\n@define-color ctp_pink #f5c2e7;\n" \
    "@define-color ctp_mauve #cba6f7;\n@define-color ctp_red #f38ba8;\n@define-color ctp_maroon #eba0ac;\n" \
    "@define-color ctp_peach #fab387;\n@define-color ctp_yellow #f9e2af;\n@define-color ctp_green #a6e3a1;\n" \
    "@define-color ctp_teal #94e2d5;\n@define-color ctp_sky #89dceb;\n@define-color ctp_sapphire #74c7ec;\n" \
    "@define-color ctp_blue #89b4fa;\n@define-color ctp_lavender #b4befe;\n@define-color ctp_text #cdd6f4;\n" \
    "@define-color ctp_subtext1 #bac2de;\n@define-color ctp_subtext0 #a6adc8;\n@define-color ctp_overlay2 #9399b2;\n" \
    "@define-color ctp_overlay1 #7f849c;\n@define-color ctp_overlay0 #6c7086;\n@define-color ctp_surface2 #585b70;\n" \
    "@define-color ctp_surface1 #45475a;\n@define-color ctp_surface0 #313244;\n@define-color ctp_base #1e1e2e;\n" \
    "@define-color ctp_mantle #181825;\n@define-color ctp_crust #11111b;\n"

/* libadwaita's own names, pointed at the palette */
#define ADW_COLORS \
    "@define-color window_bg_color @ctp_crust;\n@define-color window_fg_color @ctp_text;\n" \
    "@define-color view_bg_color @ctp_mantle;\n@define-color view_fg_color @ctp_text;\n" \
    "@define-color headerbar_bg_color @ctp_crust;\n@define-color headerbar_fg_color @ctp_text;\n" \
    "@define-color headerbar_border_color @ctp_surface0;\n@define-color headerbar_backdrop_color @ctp_crust;\n" \
    "@define-color headerbar_shade_color alpha(black, 0.3);\n" \
    "@define-color card_bg_color @ctp_mantle;\n@define-color card_fg_color @ctp_text;\n" \
    "@define-color card_shade_color alpha(black, 0.3);\n" \
    "@define-color dialog_bg_color @ctp_base;\n@define-color dialog_fg_color @ctp_text;\n" \
    "@define-color popover_bg_color @ctp_base;\n@define-color popover_fg_color @ctp_text;\n" \
    "@define-color sidebar_bg_color @ctp_mantle;\n@define-color sidebar_fg_color @ctp_text;\n" \
    "@define-color accent_bg_color @ctp_mauve;\n@define-color accent_fg_color @ctp_crust;\n@define-color accent_color @ctp_mauve;\n" \
    "@define-color destructive_bg_color @ctp_red;\n@define-color destructive_fg_color @ctp_crust;\n@define-color destructive_color @ctp_red;\n" \
    "@define-color success_bg_color @ctp_green;\n@define-color success_fg_color @ctp_crust;\n@define-color success_color @ctp_green;\n" \
    "@define-color warning_bg_color @ctp_peach;\n@define-color warning_fg_color @ctp_crust;\n@define-color warning_color @ctp_peach;\n" \
    "@define-color error_bg_color @ctp_red;\n@define-color error_fg_color @ctp_crust;\n@define-color error_color @ctp_red;\n" \
    "@define-color shade_color alpha(black, 0.36);\n@define-color scrollbar_outline_color alpha(black, 0.5);\n"

static const char *css =
    MOCHA_COLORS ADW_COLORS
    /* ---- Cinzel for headings ---- */
    ".display { font-family: 'Cinzel', serif; font-weight: 700; }\n"

    /* ---- top bar ---- */
    ".topbar { background: alpha(@ctp_crust, 0.55); border-bottom: 1px solid @ctp_surface0; }\n"
    ".brand-name { font-family: 'Cinzel', serif; font-weight: 800; font-size: 17px; letter-spacing: 3px; }\n"
    ".brand-sub { font-size: 9px; letter-spacing: 1.5px; color: @ctp_overlay1; }\n"
    ".device-chip { background: alpha(@ctp_surface0, 0.55); border-radius: 999px; padding: 5px 14px; }\n"
    ".chip-dot { min-width: 8px; min-height: 8px; border-radius: 999px; background: @ctp_green; }\n"
    ".chip-dot.off { background: @ctp_overlay0; }\n"
    ".chip-name { font-weight: 700; font-size: 13px; }\n"
    ".chip-usage { font-size: 11px; color: @ctp_overlay2; }\n"
    ".chip-bar trough { min-height: 4px; min-width: 70px; border-radius: 99px; background: @ctp_surface1; }\n"
    ".chip-bar progress { min-height: 4px; border-radius: 99px; background: @ctp_mauve; }\n"

    /* ---- bottom tabs ---- */
    ".tabbar { background: alpha(@ctp_mantle, 0.62); border-top: 1px solid @ctp_surface0; padding: 0 12px; }\n"
    ".tabbar button.tab { background: transparent; border-radius: 0; padding: 10px 38px; min-width: 64px; color: @ctp_overlay1; box-shadow: none; }\n"
    ".tabbar button.tab:hover { background: alpha(@ctp_surface0, 0.5); color: @ctp_text; }\n"
    ".tabbar button.tab:disabled { opacity: 0.35; }\n"
    ".tabbar button.tab.active { color: @ctp_mauve; background: alpha(@ctp_mauve, 0.10); box-shadow: inset 0 2px 0 @ctp_mauve; }\n"
    ".tab-label { font-size: 11px; font-weight: 600; }\n"

    /* ---- page headers ---- */
    ".eyebrow { font-size: 10px; font-weight: 800; letter-spacing: 2px; color: @ctp_mauve; }\n"
    ".page-title { font-family: 'Cinzel', serif; font-size: 30px; font-weight: 700; }\n"
    ".page-sub { font-size: 12px; color: @ctp_overlay1; }\n"
    ".section-title { font-weight: 800; font-size: 15px; }\n"

    /* ---- filter row ---- */
    ".filterbar { background: @ctp_mantle; border: 1px solid @ctp_surface0; border-radius: 12px; padding: 8px; }\n"
    ".filterbar entry, .filterbar text { background: transparent; }\n"
    ".filterbar .search-field { background: @ctp_crust; border: 1px solid @ctp_surface0; border-radius: 8px; padding: 2px 10px; }\n"
    ".filterbar dropdown > button { background: @ctp_crust; border: 1px solid @ctp_surface0; border-radius: 8px; box-shadow: none; }\n"
    ".filterbar .view-toggle button { background: transparent; border-radius: 8px; box-shadow: none; }\n"
    ".filterbar .view-toggle button:checked { background: @ctp_surface0; color: @ctp_mauve; }\n"

    /* ---- the game cards ---- */
    ".game-card { padding: 6px; border-radius: 10px; background: transparent; box-shadow: none; }\n"
    ".game-card:hover { background: alpha(@ctp_surface0, 0.45); }\n"
    ".game-card.selected-card { background: alpha(@ctp_mauve, 0.12); box-shadow: inset 0 0 0 2px alpha(@ctp_mauve, 0.85); }\n"
    ".cover-frame { border-radius: 7px; background: @ctp_mantle; border: 1px solid @ctp_surface0; }\n"
    ".cover-strip { font-size: 9px; font-weight: 800; letter-spacing: 1.5px; padding: 3px 8px; color: @ctp_text; }\n"
    ".cover-strip.ps2 { background: alpha(@ctp_blue, 0.38); }\n"
    ".cover-strip.ps1 { background: alpha(@ctp_peach, 0.38); }\n"
    ".cover { border-radius: 0; }\n"
    ".cover-missing { color: @ctp_surface2; background: @ctp_mantle; }\n"
    ".card-title { font-weight: 700; font-size: 13px; }\n"
    ".card-sub { font-size: 11px; color: @ctp_overlay1; }\n"
    ".game-row { padding: 8px 12px; border-radius: 10px; }\n"
    ".game-row:hover { background: alpha(@ctp_surface0, 0.45); }\n"
    "row.selected-row { background: alpha(@ctp_mauve, 0.12); box-shadow: inset 0 0 0 2px alpha(@ctp_mauve, 0.85); }\n"

    ".transparent-list { background: transparent; }\n"
    ".transparent-list row { background: transparent; border-radius: 10px; }\n"

    /* ---- the details panel ---- */
    ".details { background: @ctp_mantle; border-left: 1px solid @ctp_surface0; }\n"
    /* frosted glass: the selected game's cover, enlarged and blurred, under a dark veil that keeps the text readable */
    ".win-backdrop { filter: blur(44px) saturate(1.4); opacity: 0.5; }\n"
    ".win-scrim { background: alpha(@ctp_crust, 0.74); }\n"
    ".details-backdrop { filter: blur(26px) saturate(1.5); opacity: 0.95; }\n"
    ".details-scrim { background: linear-gradient(to bottom, alpha(@ctp_crust, 0.50), alpha(@ctp_mantle, 0.86) 70%); }\n"
    ".details-title { font-family: 'Cinzel', serif; font-size: 20px; font-weight: 700; }\n"
    ".details-id { font-size: 12px; color: @ctp_overlay1; }\n"
    ".hr { background: @ctp_surface0; min-height: 1px; }\n"
    ".detail-key { font-size: 12px; color: @ctp_overlay1; }\n"
    ".detail-val { font-size: 12px; font-weight: 600; }\n"
    ".tag { font-size: 11px; font-weight: 700; border-radius: 999px; padding: 3px 12px; }\n"
    ".tag-ok { background: alpha(@ctp_green, 0.15); color: @ctp_green; }\n"
    ".tag-warn { background: alpha(@ctp_peach, 0.15); color: @ctp_peach; }\n"
    ".tag-info { background: alpha(@ctp_mauve, 0.15); color: @ctp_mauve; }\n"
    ".btn-secondary { background: @ctp_crust; border: 1px solid @ctp_surface0; box-shadow: none; }\n"
    ".btn-secondary:hover { background: @ctp_surface0; }\n"
    ".btn-danger { background: transparent; border: 1px solid alpha(@ctp_red, 0.45); color: @ctp_red; box-shadow: none; }\n"
    ".btn-danger:hover { background: alpha(@ctp_red, 0.14); }\n"

    /* ---- the other pages ---- */
    ".add-card { padding: 20px; border-radius: 14px; background: @ctp_mantle; border: 1px solid @ctp_surface0; }\n"
    ".add-card.drop-hover { box-shadow: inset 0 0 0 2px @ctp_mauve; background: alpha(@ctp_mauve, 0.10); }\n"
    ".badge { font-size: 15px; font-weight: 800; padding: 8px 14px; border-radius: 10px; color: @ctp_crust; }\n"
    ".badge-ps2 { background: @ctp_blue; }\n"
    ".badge-ps1 { background: @ctp_peach; }\n"
    ".statusbar { background: alpha(@ctp_crust, 0.7); border-top: 1px solid @ctp_surface0; padding: 4px 16px; }\n"
    ".statusbar label { font-size: 11px; color: @ctp_overlay1; }\n"
    ".statusbar .ok { color: @ctp_green; }\n"
    ".space-text { font-size: 11px; color: @ctp_overlay1; }\n";

/* loads the .ttf files of data/fonts: next to the program when installed, or from the project's data folder when run from build/ */
static void load_fonts(void)
{
    char *self = g_file_read_link("/proc/self/exe", NULL), *dir = self ? g_path_get_dirname(self) : NULL;
    const char *where[2] = {"fonts", "../data/fonts"};
    int i;

    for (i = 0; dir != NULL && i < 2; i++) {
        char *folder = g_build_filename(dir, where[i], NULL);
        GDir *d = g_dir_open(folder, 0, NULL);
        const char *name;

        while (d && (name = g_dir_read_name(d)) != NULL)
            if (g_str_has_suffix(name, ".ttf")) {
                char *path = g_build_filename(folder, name, NULL);

                FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
                g_free(path);
            }
        if (d)
            g_dir_close(d);
        g_free(folder);
    }
    g_free(dir);
    g_free(self);
}

/* a pixel-art picture of data/images (installed next to the program, or the project's data folder when run from build/),
 * scaled with nearest neighbor to this height so the pixels stay sharp; NULL if the file is missing */
GdkPaintable *pixel_art(const char *name, int height)
{
    char *self = g_file_read_link("/proc/self/exe", NULL), *dir = self ? g_path_get_dirname(self) : NULL;
    const char *where[2] = {"images", "../data/images"};
    GdkPaintable *result = NULL;
    int i;

    for (i = 0; dir != NULL && result == NULL && i < 2; i++) {
        char *path = g_build_filename(dir, where[i], name, NULL);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);

        if (pb != NULL) {
            int w = gdk_pixbuf_get_width(pb) * height / gdk_pixbuf_get_height(pb);
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, w, height, GDK_INTERP_NEAREST);

            GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(scaled), (gsize)gdk_pixbuf_get_rowstride(scaled) * (gsize)height);

            result = GDK_PAINTABLE(gdk_memory_texture_new(w, height, gdk_pixbuf_get_has_alpha(scaled) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                          bytes, (gsize)gdk_pixbuf_get_rowstride(scaled)));
            g_bytes_unref(bytes);
            g_object_unref(scaled);
            g_object_unref(pb);
        }
        g_free(path);
    }
    g_free(dir);
    g_free(self);
    return result;
}

void load_style(void)
{
    GtkCssProvider *p = gtk_css_provider_new();

    load_fonts();
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK); /* Mocha is a dark palette */
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}
