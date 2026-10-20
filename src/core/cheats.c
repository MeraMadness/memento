#include <string.h>

#include "cheats.h"

#define MAX_CHEATS 250
#define MAX_NAME 128
#define MAX_LINE 255
#define MAX_TEXT (256 * 1024)

/* "AAAAAAAA VVVVVVVV": exactly 16 hex digits, spaces allowed anywhere (as OPL's is_cheat_code) */
static gboolean is_code_line(const char *s)
{
    int digits = 0;

    for (; *s; s++) {
        if (g_ascii_isxdigit(*s))
            digits++;
        else if (!g_ascii_isspace(*s))
            return FALSE;
    }
    return digits == 16;
}

gboolean cheats_normalize(const char *text, char **out, CheatInfo *info, GError **error)
{
    GString *clean = g_string_new(NULL);
    gchar **lines;
    CheatInfo ci = {0, 0};
    gboolean have_name = FALSE, current_has_code = FALSE, stray = FALSE;
    int i;
    gsize len = strlen(text);

    if (len > MAX_TEXT) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("That cheat list is too big (%u KB, the limit is %u KB)"),
                    (guint)(len / 1024), (guint)(MAX_TEXT / 1024));
        goto fail;
    }
    if (g_str_has_prefix(text, "\xEF\xBB\xBF")) /* byte order mark */
        text += 3;
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strdup(lines[i]), *cut;

        g_strdelimit(line, "\r", ' ');
        g_strchomp(line);
        if (strlen(line) > MAX_LINE) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Line %d is too long (OPL reads at most %d characters per line)"), i + 1, MAX_LINE);
            g_free(line);
            g_strfreev(lines);
            goto fail;
        }
        g_string_append(clean, line);
        g_string_append_c(clean, '\n');

        /* what OPL sees: the line without its comment, trimmed */
        cut = strstr(line, "//");
        if (cut)
            *cut = '\0';
        cut = strchr(line, '#');
        if (cut)
            *cut = '\0';
        g_strstrip(line);
        if (*line != '\0') {
            if (is_code_line(line)) {
                if (!have_name)
                    stray = TRUE; /* OPL ignores codes that come before the first name */
                else {
                    ci.codes++;
                    if (!current_has_code) {
                        ci.cheats++;
                        current_has_code = TRUE;
                    }
                }
            } else {
                if (strlen(line) > MAX_NAME) {
                    g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("The cheat name on line %d is too long (at most %d characters)"), i + 1, MAX_NAME);
                    g_free(line);
                    g_strfreev(lines);
                    goto fail;
                }
                have_name = TRUE;
                current_has_code = FALSE;
            }
        }
        g_free(line);
    }
    g_strfreev(lines);

    if (ci.cheats == 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG,
                    stray ? _("No cheat found. Each cheat is a line with its name followed by lines of codes like 10B8DAFA 00003F00 (codes before the first name are ignored)")
                          : _("No cheat found. Each cheat is a line with its name followed by lines of codes like 10B8DAFA 00003F00"));
        goto fail;
    }
    if (ci.cheats > MAX_CHEATS) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Too many cheats (%u): OPL reads at most %d per game"), ci.cheats, MAX_CHEATS);
        goto fail;
    }
    /* one newline at the end, none doubled */
    while (clean->len > 1 && clean->str[clean->len - 1] == '\n' && clean->str[clean->len - 2] == '\n')
        g_string_truncate(clean, clean->len - 1);
    if (info)
        *info = ci;
    *out = g_string_free(clean, FALSE);
    return TRUE;
fail:
    g_string_free(clean, TRUE);
    return FALSE;
}

char *cfg_set_keys(const char *old_text, const char *const *pairs)
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
    for (k = 0; pairs[k] != NULL && pairs[k + 1] != NULL; k += 2) {
        gchar *prefix = g_strdup_printf("%s=", pairs[k]), *line = g_strdup_printf("%s=%s", pairs[k], pairs[k + 1]);
        gboolean done = FALSE;

        for (i = 0; i < lines->len; i++)
            if (g_str_has_prefix(g_ptr_array_index(lines, i), prefix)) {
                g_free(g_ptr_array_index(lines, i));
                g_ptr_array_index(lines, i) = g_strdup(line);
                done = TRUE;
                break;
            }
        if (!done)
            g_ptr_array_add(lines, g_strdup(line));
        g_free(prefix);
        g_free(line);
    }
    for (i = 0; i < lines->len; i++)
        g_string_append_printf(out, "%s\n", (char *)g_ptr_array_index(lines, i));
    g_ptr_array_free(lines, TRUE);
    return g_string_free(out, FALSE);
}
