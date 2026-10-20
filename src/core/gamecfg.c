#include <stdlib.h>
#include <string.h>

#include "gamecfg.h"
#include "tools.h"

static const char *const dma_names[] = {"MDMA 0", "MDMA 1", "MDMA 2", "UDMA 0", "UDMA 1", "UDMA 2", "UDMA 3", "UDMA 4"};

static const char *const gsm_names[GAME_GSM_MODES] = {
    "NTSC", "NTSC Non Interlaced", "PAL", "PAL Non Interlaced", "PAL @60Hz", "PAL @60Hz Non Interlaced",
    "PS1 NTSC (HDTV 480p @60Hz)", "PS1 PAL (HDTV 576p @50Hz)", "HDTV 480p @60Hz", "HDTV 576p @50Hz",
    "HDTV 720p @60Hz", "HDTV 1080i @60Hz", "HDTV 1080i @60Hz Non Interlaced", "VGA 640x480p @60Hz",
    "VGA 640x480p @72Hz", "VGA 640x480p @75Hz", "VGA 640x480p @85Hz", "VGA 640x960i @60Hz", "VGA 800x600p @56Hz",
    "VGA 800x600p @60Hz", "VGA 800x600p @72Hz", "VGA 800x600p @75Hz", "VGA 800x600p @85Hz", "VGA 1024x768p @60Hz",
    "VGA 1024x768p @70Hz", "VGA 1024x768p @75Hz", "VGA 1024x768p @85Hz", "VGA 1280x1024p @60Hz", "VGA 1280x1024p @75Hz",
};

const char *game_dma_name(int mode)
{
    return mode >= 0 && mode < 8 ? dma_names[mode] : "?";
}

const char *game_gsm_mode_name(int mode)
{
    return mode >= 0 && mode < GAME_GSM_MODES ? gsm_names[mode] : "?";
}

void game_settings_init(GameSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->dma = GAME_DMA_DEFAULT;
    s->alt_startup = g_strdup("");
}

void game_settings_clear(GameSettings *s)
{
    g_free(s->alt_startup);
    memset(s, 0, sizeof(*s));
}

/* the value of "key=value" (a copy), or NULL; the first line that has that key wins, like in OPL */
static char *cfg_get(const char *text, const char *key)
{
    gchar **lines;
    char *value = NULL, *prefix = g_strdup_printf("%s=", key);
    int i;

    if (text == NULL) {
        g_free(prefix);
        return NULL;
    }
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i] != NULL && value == NULL; i++)
        if (g_str_has_prefix(lines[i], prefix)) {
            value = g_strdup(lines[i] + strlen(prefix));
            g_strstrip(value);
        }
    g_strfreev(lines);
    g_free(prefix);
    return value;
}

static int cfg_int(const char *text, const char *key, int fallback)
{
    char *v = cfg_get(text, key), *end;
    long n = fallback;

    if (v != NULL && *v != '\0') {
        n = strtol(v, &end, 10);
        if (*end != '\0')
            n = fallback;
    }
    g_free(v);
    return (int)n;
}

void game_settings_parse(const char *cfg_text, GameSettings *s)
{
    char *alt;
    int dma, vmode;

    game_settings_clear(s);
    game_settings_init(s);
    s->compat = (guint)cfg_int(cfg_text, "$Compatibility", 0) & ((1u << GAME_COMPAT_MODES) - 1);
    dma = cfg_int(cfg_text, "$DMA", GAME_DMA_DEFAULT);
    s->dma = dma >= 0 && dma <= 7 ? dma : GAME_DMA_DEFAULT;
    alt = cfg_get(cfg_text, "$AltStartup");
    if (alt != NULL) {
        g_free(s->alt_startup);
        s->alt_startup = alt;
    }
    /* GSM counts only when this game has its own GSM settings (source 1) and has it switched on */
    s->gsm_on = cfg_int(cfg_text, "$GSMSource", 0) == 1 && cfg_int(cfg_text, "$EnableGSM", 0) != 0;
    vmode = cfg_int(cfg_text, "$GSMVMode", 0);
    s->gsm_vmode = vmode >= 0 && vmode < GAME_GSM_MODES ? vmode : 0;
}

typedef struct {
    const char *key;
    char *value; /* NULL: remove the key */
} Edit;

/* applies the edits to the text: a key that is there is replaced (or dropped), a new one is appended */
static char *cfg_edit(const char *old_text, Edit *edits, int n)
{
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    GString *out = g_string_new(NULL);
    guint i;
    int k;

    if (old_text != NULL) {
        gchar **v = g_strsplit(old_text, "\n", -1);

        for (i = 0; v[i] != NULL; i++) {
            gchar *l = g_strdup(v[i]);

            g_strdelimit(l, "\r", ' ');
            g_strchomp(l);
            if (*l != '\0')
                g_ptr_array_add(lines, l);
            else
                g_free(l);
        }
        g_strfreev(v);
    }
    for (k = 0; k < n; k++) {
        gchar *prefix = g_strdup_printf("%s=", edits[k].key);
        gboolean done = FALSE;

        for (i = 0; i < lines->len;) {
            if (g_str_has_prefix(g_ptr_array_index(lines, i), prefix)) {
                if (edits[k].value != NULL && !done) {
                    g_free(g_ptr_array_index(lines, i));
                    g_ptr_array_index(lines, i) = g_strdup_printf("%s=%s", edits[k].key, edits[k].value);
                    done = TRUE;
                    i++;
                } else
                    g_ptr_array_remove_index(lines, i); /* removed, or a repeated key */
            } else
                i++;
        }
        if (!done && edits[k].value != NULL)
            g_ptr_array_add(lines, g_strdup_printf("%s=%s", edits[k].key, edits[k].value));
        g_free(prefix);
    }
    for (i = 0; i < lines->len; i++)
        g_string_append_printf(out, "%s\n", (char *)g_ptr_array_index(lines, i));
    g_ptr_array_free(lines, TRUE);
    return g_string_free(out, FALSE);
}

char *game_settings_apply(const char *old_text, const GameSettings *s, GError **error)
{
    Edit e[10];
    int n = 0;
    char *result;
    gboolean custom;

    if (s->alt_startup != NULL && *s->alt_startup != '\0' && (!ps2gm_arg_ok(s->alt_startup) || strlen(s->alt_startup) > 64)) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("The alternative startup file has characters that are not allowed"));
        return NULL;
    }
    e[n].key = "$Compatibility";
    e[n++].value = (s->compat & ((1u << GAME_COMPAT_MODES) - 1)) ? g_strdup_printf("%u", s->compat & ((1u << GAME_COMPAT_MODES) - 1)) : NULL;
    e[n].key = "$DMA";
    e[n++].value = s->dma != GAME_DMA_DEFAULT && s->dma >= 0 && s->dma <= 7 ? g_strdup_printf("%d", s->dma) : NULL;
    e[n].key = "$AltStartup";
    e[n++].value = s->alt_startup && *s->alt_startup ? g_strdup(s->alt_startup) : NULL;
    custom = e[0].value || e[1].value || e[2].value;
    if (s->gsm_on) {
        e[n].key = "$GSMSource";
        e[n++].value = g_strdup("1");
        e[n].key = "$EnableGSM";
        e[n++].value = g_strdup("1");
        e[n].key = "$GSMVMode";
        e[n++].value = s->gsm_vmode > 0 && s->gsm_vmode < GAME_GSM_MODES ? g_strdup_printf("%d", s->gsm_vmode) : NULL;
    } else {
        e[n].key = "$EnableGSM";
        e[n++].value = NULL;
        e[n].key = "$GSMVMode";
        e[n++].value = NULL;
    }
    if (custom) {
        e[n].key = "$ConfigSource"; /* "customized settings": like OPL, so downloaded defaults never overwrite them */
        e[n++].value = g_strdup("1");
    }
    result = cfg_edit(old_text, e, n);
    for (int i = 0; i < n; i++)
        g_free(e[i].value);
    return result;
}

char *game_settings_reset(const char *old_text)
{
    Edit e[] = {{"$Compatibility", NULL}, {"$DMA", NULL}, {"$AltStartup", NULL}, {"$ConfigSource", NULL},
                {"$EnableGSM", NULL}, {"$GSMVMode", NULL}};

    return cfg_edit(old_text, e, G_N_ELEMENTS(e));
}
