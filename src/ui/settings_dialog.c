/*
 * The OPL settings of a PS2 game: compatibility modes, disk speed (DMA), a forced video mode (GSM) and an alternative
 * startup file. They are saved in the game's config file on the disk, where OPL reads them; the controller menu of
 * OPL is no longer needed for this. Opening the dialog first reads the current settings from the disk.
 */
#include "app.h"

static const struct {
    const char *title, *hint;
} modes[GAME_COMPAT_MODES] = {
    {N_("Accurate reads"), N_("For games that stutter, freeze or show errors while loading data.")},
    {N_("Alternative data read method"), N_("A slower, synchronous way of reading. Fixes some games that fail to load.")},
    {N_("Unhook syscalls"), N_("For games that hang right at the start.")},
    {N_("Skip videos"), N_("Skips intro and in-game videos. For games that freeze while a video plays.")},
    {N_("Emulate DVD-DL"), N_("For dual-layer games that fail when they read their second layer.")},
    {N_("Disable in-game reset"), N_("Turns off the button combination that returns to OPL, for games that use it themselves."),},
};

/* the order shown (default first), as OPL's DMA numbers */
static const int dma_order[8] = {7, 6, 5, 4, 3, 2, 1, 0};

typedef struct {
    char *id, *title;
    GameSettings g;
    AdwDialog *dialog;
    GtkWidget *mode_row[GAME_COMPAT_MODES], *dma_row, *gsm_on_row, *gsm_row, *alt_row;
} Settings;

static void settings_free(gpointer p)
{
    Settings *s = p;

    g_free(s->id);
    g_free(s->title);
    game_settings_clear(&s->g);
    g_free(s);
}

/* ---- saving and resetting ---------------------------------------------------------------------- */

typedef struct {
    char *id;
    gboolean reset;
    GameSettings g;
} SaveJob;

static void savejob_free(gpointer p)
{
    SaveJob *j = p;

    g_free(j->id);
    game_settings_clear(&j->g);
    g_free(j);
}

static gboolean job_save(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    SaveJob *j = data;

    (void)notes;
    cb(0, 1, _("Saving the settings"), pd);
    return j->reset ? ops_reset_settings(A.ops, j->id, error) : ops_set_settings(A.ops, j->id, &j->g, error);
}

static void collect(Settings *s, GameSettings *out)
{
    int i;

    game_settings_init(out);
    for (i = 0; i < GAME_COMPAT_MODES; i++)
        if (adw_switch_row_get_active(ADW_SWITCH_ROW(s->mode_row[i])))
            out->compat |= 1u << i;
    out->dma = dma_order[adw_combo_row_get_selected(ADW_COMBO_ROW(s->dma_row)) % 8];
    out->gsm_on = adw_switch_row_get_active(ADW_SWITCH_ROW(s->gsm_on_row));
    out->gsm_vmode = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(s->gsm_row));
    g_free(out->alt_startup);
    out->alt_startup = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(s->alt_row))));
}

static void on_save(GtkButton *b, gpointer data)
{
    Settings *s = data;
    SaveJob *j = g_new0(SaveJob, 1);

    (void)b;
    j->id = g_strdup(s->id);
    collect(s, &j->g);
    adw_dialog_close(s->dialog); /* frees s */
    run_job(_("Saving the settings"), job_save, j, savejob_free, after_change);
}

static void on_reset(GtkButton *b, gpointer data)
{
    Settings *s = data;
    SaveJob *j = g_new0(SaveJob, 1);

    (void)b;
    j->id = g_strdup(s->id);
    j->reset = TRUE;
    game_settings_init(&j->g);
    adw_dialog_close(s->dialog);
    run_job(_("Resetting the settings"), job_save, j, savejob_free, after_change);
}

static void on_gsm_toggled(GObject *row, GParamSpec *ps, gpointer data)
{
    Settings *s = data;

    (void)ps;
    gtk_widget_set_sensitive(s->gsm_row, adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
}

static void on_closed(AdwDialog *d, gpointer data)
{
    (void)d;
    settings_free(data);
}

/* ---- the dialog ------------------------------------------------------------------------------- */

static void present_settings(gboolean ok, gpointer data)
{
    Settings *job = data, *s;
    GtkWidget *view = adw_toolbar_view_new(), *header = adw_header_bar_new();
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *gm = ADW_PREFERENCES_GROUP(adw_preferences_group_new()), *gd = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *gv = ADW_PREFERENCES_GROUP(adw_preferences_group_new()), *ga = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *gb = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkStringList *dma = gtk_string_list_new(NULL), *gsm = gtk_string_list_new(NULL);
    GtkWidget *row, *btn;
    char *title;
    int i;

    if (!ok)
        return;
    s = g_new0(Settings, 1); /* the dialog's own copy: the job's data is freed after this call */
    s->id = g_strdup(job->id);
    s->title = g_strdup(job->title);
    game_settings_init(&s->g);
    g_free(s->g.alt_startup);
    s->g.compat = job->g.compat;
    s->g.dma = job->g.dma;
    s->g.alt_startup = g_strdup(job->g.alt_startup);
    s->g.gsm_on = job->g.gsm_on;
    s->g.gsm_vmode = job->g.gsm_vmode;

    adw_preferences_group_set_title(gm, _("Compatibility"));
    adw_preferences_group_set_description(gm, _("Try these one at a time when a game does not start or misbehaves."));
    for (i = 0; i < GAME_COMPAT_MODES; i++) {
        s->mode_row[i] = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->mode_row[i]), _(modes[i].title));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(s->mode_row[i]), _(modes[i].hint));
        adw_switch_row_set_active(ADW_SWITCH_ROW(s->mode_row[i]), (s->g.compat >> i) & 1);
        adw_preferences_group_add(gm, s->mode_row[i]);
    }

    adw_preferences_group_set_title(gd, _("Disk speed"));
    for (i = 0; i < 8; i++) {
        char *label = g_strdup_printf(i == 0 ? _("%s (default)") : "%s", game_dma_name(dma_order[i]));

        gtk_string_list_append(dma, label);
        g_free(label);
    }
    s->dma_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->dma_row), _("Transfer mode (DMA)"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->dma_row), _("A slower mode can fix read errors on some games or disks."));
    adw_combo_row_set_model(ADW_COMBO_ROW(s->dma_row), G_LIST_MODEL(dma));
    for (i = 0; i < 8; i++)
        if (dma_order[i] == s->g.dma)
            adw_combo_row_set_selected(ADW_COMBO_ROW(s->dma_row), i);
    adw_preferences_group_add(gd, s->dma_row);

    adw_preferences_group_set_title(gv, _("Video"));
    for (i = 0; i < GAME_GSM_MODES; i++)
        gtk_string_list_append(gsm, game_gsm_mode_name(i));
    s->gsm_on_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->gsm_on_row), _("Force a video mode"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->gsm_on_row), _("Makes this game output the mode below (for example a progressive 480p picture). Not every game or screen likes it."));
    adw_switch_row_set_active(ADW_SWITCH_ROW(s->gsm_on_row), s->g.gsm_on);
    s->gsm_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->gsm_row), _("Video mode"));
    adw_combo_row_set_model(ADW_COMBO_ROW(s->gsm_row), G_LIST_MODEL(gsm));
    adw_combo_row_set_selected(ADW_COMBO_ROW(s->gsm_row), (guint)s->g.gsm_vmode);
    gtk_widget_set_sensitive(s->gsm_row, s->g.gsm_on);
    g_signal_connect(s->gsm_on_row, "notify::active", G_CALLBACK(on_gsm_toggled), s);
    adw_preferences_group_add(gv, s->gsm_on_row);
    adw_preferences_group_add(gv, s->gsm_row);

    adw_preferences_group_set_title(ga, _("Advanced"));
    s->alt_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->alt_row), _("Alternative startup file"));
    gtk_editable_set_text(GTK_EDITABLE(s->alt_row), s->g.alt_startup);
    adw_preferences_group_add(ga, s->alt_row);
    adw_preferences_group_set_description(ga, _("Only for games that need a different file to start. Leave it empty normally."));

    row = button_row(_("Save"), _("Writes the settings to the game's config file on the disk"), _("Save to the disk"), NULL, &btn);
    gtk_widget_add_css_class(btn, "suggested-action");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_save), s);
    adw_preferences_group_add(gb, row);
    row = button_row(_("Reset"), _("Removes these settings: OPL goes back to its own defaults for this game"), _("Reset to defaults"), NULL, &btn);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_reset), s);
    adw_preferences_group_add(gb, row);

    adw_preferences_page_add(page, gm);
    adw_preferences_page_add(page, gd);
    adw_preferences_page_add(page, gv);
    adw_preferences_page_add(page, ga);
    adw_preferences_page_add(page, gb);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));

    s->dialog = adw_dialog_new();
    title = g_strdup_printf(_("Settings: %s"), s->title && *s->title ? s->title : s->id);
    adw_dialog_set_title(s->dialog, title);
    g_free(title);
    adw_dialog_set_child(s->dialog, view);
    adw_dialog_set_content_width(s->dialog, 560);
    adw_dialog_set_content_height(s->dialog, 780);
    g_signal_connect(s->dialog, "closed", G_CALLBACK(on_closed), s);
    adw_dialog_present(s->dialog, GTK_WIDGET(A.win));
}

static gboolean job_read(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    Settings *s = data;

    (void)notes;
    cb(0, 1, _("Reading the settings from the disk"), pd);
    game_settings_clear(&s->g);
    return ops_get_settings(A.ops, s->id, &s->g, error);
}

void show_settings_dialog(const char *id, const char *title)
{
    Settings *s = g_new0(Settings, 1);

    s->id = g_strdup(id);
    s->title = g_strdup(title);
    game_settings_init(&s->g);
    run_job(_("Reading the settings"), job_read, s, settings_free, present_settings);
}
