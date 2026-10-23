#include "ps1fix.h"

#include <string.h>

#define MAX_LINE 200
#define MAX_TOTAL 16384

Ps1Fixes *ps1fix_parse(const char *text)
{
    Ps1Fixes *f = g_new0(Ps1Fixes, 1);
    GString *other = g_string_new(NULL);
    char **lines;
    guint i;

    f->crlf = text == NULL || strstr(text, "\r\n") != NULL || strchr(text, '\n') == NULL;
    lines = g_strsplit(text ? text : "", "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]); /* also drops the \r of Windows line ends */

        if (*line == '\0')
            continue;
        if (!strcmp(line, "$CACHE1"))
            f->cache1 = TRUE;
        else if (!strcmp(line, "$SUBCDSTATUS"))
            f->subcdstatus = TRUE;
        else if (!strcmp(line, "$CODECACHE_ADDON_0"))
            f->codecache0 = TRUE;
        else if (!strcmp(line, "$UNDO_GAME_FIXES"))
            f->undo_fixes = TRUE;
        else {
            if (other->len > 0)
                g_string_append_c(other, '\n');
            g_string_append(other, line);
        }
    }
    g_strfreev(lines);
    f->other = g_string_free(other, FALSE);
    return f;
}

void ps1fix_free(Ps1Fixes *f)
{
    if (f == NULL)
        return;
    g_free(f->other);
    g_free(f);
}

gboolean ps1fix_compose(const Ps1Fixes *f, char **text, GError **error)
{
    GString *out = g_string_new(NULL);
    const char *eol = f->crlf ? "\r\n" : "\n";
    char **lines;
    guint i;

    *text = NULL;
    if (f->cache1)
        g_string_append_printf(out, "$CACHE1%s", eol);
    if (f->subcdstatus)
        g_string_append_printf(out, "$SUBCDSTATUS%s", eol);
    if (f->codecache0)
        g_string_append_printf(out, "$CODECACHE_ADDON_0%s", eol);
    if (f->undo_fixes)
        g_string_append_printf(out, "$UNDO_GAME_FIXES%s", eol);
    lines = g_strsplit(f->other ? f->other : "", "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        const char *p;

        if (*line == '\0')
            continue;
        if (strlen(line) > MAX_LINE) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("A line is too long (at most %d characters)"), MAX_LINE);
            goto bad;
        }
        for (p = line; *p; p++)
            if ((guchar)*p < 0x20 || (guchar)*p > 0x7e) { /* plain ASCII only: the file is read by a program that knows nothing else */
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("Only plain letters, digits and symbols are allowed: %s"), line);
                goto bad;
            }
        g_string_append_printf(out, "%s%s", line, eol);
    }
    g_strfreev(lines);
    if (out->len > MAX_TOTAL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_ARG, _("The list is too long"));
        g_string_free(out, TRUE);
        return FALSE;
    }
    *text = out->len > 0 ? g_string_free(out, FALSE) : (g_string_free(out, TRUE), NULL);
    return TRUE;
bad:
    g_strfreev(lines);
    g_string_free(out, TRUE);
    return FALSE;
}

gboolean ps1fix_is_fix_file_name(const char *name)
{
    const char *p;
    gsize digits = 0;

    if (g_str_has_prefix(name, "TROJAN_"))
        p = name + 7;
    else if (g_str_has_prefix(name, "PATCH_"))
        p = name + 6;
    else
        return FALSE;
    while (g_ascii_isdigit(p[digits]))
        digits++;
    return (digits == 1 || digits == 2) && !strcmp(p + digits, ".BIN");
}
