/*
 * Translations (gettext) and the language setting.
 *
 * The language follows the system unless one is chosen in the app; the choice is the one line of
 * ~/.config/ps2gm/language ("it", "es", ... or empty for automatic). It is applied at the next start.
 * The translation files (.mo) are looked for in, in this order: $PS2GM_LOCALEDIR, the "locale" folder next to
 * the program (a build folder), the system folder given at build time.
 */
#include <locale.h>

#include "app.h"

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

const LangOption languages[] = {
    {"", N_("Automatic (like the system)")},
    {"en", "English"},
    {"it", "Italiano"},
    {"es", "Español"},
    {"fr", "Français"},
    {"de", "Deutsch"},
    {"pt_BR", "Português (Brasil)"},
    {NULL, NULL},
};

static char *settings_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "ps2gm", "language", NULL);
}

char *i18n_saved_language(void)
{
    char *file = settings_file(), *text = NULL;

    if (g_file_get_contents(file, &text, NULL, NULL))
        g_strstrip(text);
    else
        text = g_strdup("");
    g_free(file);
    return text;
}

void i18n_save_language(const char *code)
{
    char *file = settings_file(), *dir = g_path_get_dirname(file);

    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(file, code ? code : "", -1, NULL);
    g_free(dir);
    g_free(file);
}

void i18n_init(void)
{
    char *saved = i18n_saved_language(), *self, *dir, *local;
    const char *env = g_getenv("PS2GM_LOCALEDIR");

    if (*saved != '\0')
        g_setenv("LANGUAGE", saved, TRUE); /* what gettext reads first */
    g_free(saved);
    setlocale(LC_ALL, "");

    self = g_file_read_link("/proc/self/exe", NULL);
    dir = self ? g_path_get_dirname(self) : g_strdup(".");
    local = g_build_filename(dir, "locale", NULL);
    bindtextdomain(GETTEXT_PACKAGE, env ? env : (g_file_test(local, G_FILE_TEST_IS_DIR) ? local : LOCALEDIR));
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
    g_free(local);
    g_free(dir);
    g_free(self);
}

/* ---- the language row (on the Settings page) --------------------------------------------------- */

static void on_language_selected(GObject *row, GParamSpec *ps, gpointer data)
{
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(row));

    (void)ps;
    (void)data;
    if (sel < G_N_ELEMENTS(languages) - 1) {
        i18n_save_language(languages[sel].code);
        toast(_("The language will change the next time you start the program."));
    }
}

GtkWidget *build_language_row(void)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    GtkWidget *row = adw_combo_row_new();
    char *saved = i18n_saved_language();
    guint i, current = 0;

    for (i = 0; languages[i].code != NULL; i++) {
        gtk_string_list_append(model, i == 0 ? _(languages[i].name) : languages[i].name);
        if (!strcmp(languages[i].code, saved))
            current = i;
    }
    g_free(saved);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Language"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("Takes effect the next time you start the program."));
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), current);
    g_signal_connect(row, "notify::selected", G_CALLBACK(on_language_selected), NULL);
    return row;
}
