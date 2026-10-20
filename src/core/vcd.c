/*
 * PS1 BIN/CUE -> POPStarter VCD.
 *
 * A VCD is a 1 MiB header (disc table of contents) followed by the raw 2352-byte sectors of the
 * disc. The header layout and the track position rules below follow cue2pops v2.0 as implemented
 * by psx-vcd (https://github.com/leji-a/psx-vcd, MIT OR Apache-2.0); they were cross-checked
 * byte for byte against that tool in tests/test_vcd.sh.
 */

#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>

#include "ps2gm.h"

GQuark ps2gm_error_quark(void)
{
    return g_quark_from_static_string("ps2gm-error");
}

/* ---- MSF helpers (minutes:seconds:frames, 75 frames per second) -------------------------------- */

typedef struct {
    guint m, s, f;
} Msf;

static Msf msf_from_sectors(guint32 sectors)
{
    Msf r;

    r.f = sectors % 75;
    r.s = (sectors / 75) % 60;
    r.m = sectors / 75 / 60;
    return r;
}

static guint8 bcd(guint v)
{
    return (guint8)(((v / 10) << 4) | (v % 10));
}

static void put_msf_bcd(guint8 *dst, guint32 sectors)
{
    Msf m = msf_from_sectors(sectors);

    dst[0] = bcd(m.m);
    dst[1] = bcd(m.s);
    dst[2] = bcd(m.f);
}

static guint32 add_seconds(guint32 sectors, int seconds)
{
    gint64 total = (gint64)sectors + (gint64)seconds * 75;

    return total < 0 ? 0 : (guint32)total;
}

/* ---- CUE parsing ------------------------------------------------------------------------------- */

static int sector_size_for(const char *type, gboolean *audio)
{
    *audio = FALSE;
    if (!strcmp(type, "AUDIO")) {
        *audio = TRUE;
        return 2352;
    }
    if (!strcmp(type, "MODE1/2048"))
        return 2048;
    if (!strcmp(type, "MODE1/2352") || !strcmp(type, "MODE2/2352"))
        return 2352;
    if (!strcmp(type, "MODE2/2336"))
        return 2336;
    return 0;
}

static gboolean parse_msf(const char *s, guint32 *sectors)
{
    guint m, sec, f;

    if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3)
        return FALSE;
    *sectors = ((m * 60 + sec) * 75) + f;
    return TRUE;
}

static void ps1file_free(gpointer p)
{
    Ps1File *f = p;

    g_free(f->path);
    g_array_free(f->tracks, TRUE);
    g_free(f);
}

void ps1cue_free(Ps1Cue *cue)
{
    if (cue == NULL)
        return;
    g_ptr_array_free(cue->files, TRUE);
    g_free(cue);
}

guint ps1cue_track_count(const Ps1Cue *cue)
{
    guint n = 0, i;

    for (i = 0; i < cue->files->len; i++)
        n += ((Ps1File *)g_ptr_array_index(cue->files, i))->tracks->len;
    return n;
}

Ps1Cue *ps1cue_parse(const char *cue_path, GError **error)
{
    gchar *text = NULL, *dir;
    gchar **lines;
    Ps1Cue *cue;
    Ps1File *file = NULL;
    Ps1Track *track = NULL;
    int i;

    if (!g_file_get_contents(cue_path, &text, NULL, error))
        return NULL;
    dir = g_path_get_dirname(cue_path);
    cue = g_new0(Ps1Cue, 1);
    cue->files = g_ptr_array_new_with_free_func(ps1file_free);

    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]); /* also drops the \r of CRLF files */

        if (*line == '\0' || g_str_has_prefix(line, "REM"))
            continue;

        if (g_str_has_prefix(line, "FILE ")) {
            const char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            gchar *name;

            if (q1 == NULL || q2 == NULL) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("Invalid FILE line: %s"), line);
                goto fail;
            }
            name = g_strndup(q1 + 1, q2 - q1 - 1);
            file = g_new0(Ps1File, 1);
            file->path = g_path_is_absolute(name) ? g_strdup(name) : g_build_filename(dir, name, NULL);
            file->tracks = g_array_new(FALSE, TRUE, sizeof(Ps1Track));
            g_free(name);
            g_ptr_array_add(cue->files, file);
            track = NULL;
        } else if (g_str_has_prefix(line, "TRACK ")) {
            int number;
            char type[32];
            Ps1Track t = {0};

            if (file == NULL || sscanf(line, "TRACK %d %31s", &number, type) != 2) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("Invalid TRACK line: %s"), line);
                goto fail;
            }
            t.number = number;
            t.sector_size = sector_size_for(type, &t.audio);
            if (t.sector_size == 0) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("Unknown track type: %s"), type);
                goto fail;
            }
            g_array_append_val(file->tracks, t);
            track = &g_array_index(file->tracks, Ps1Track, file->tracks->len - 1);
        } else if (g_str_has_prefix(line, "INDEX ")) {
            int index;
            char msf[32];
            guint32 sectors;

            if (track == NULL || sscanf(line, "INDEX %d %31s", &index, msf) != 2 || !parse_msf(msf, &sectors)) {
                g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("Invalid INDEX line: %s"), line);
                goto fail;
            }
            if (index == 0) {
                track->has_index00 = TRUE;
                track->index00 = sectors;
            } else if (index == 1)
                track->index01 = sectors;
        }
    }

    if (cue->files->len == 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("The CUE file has no FILE entries"));
        goto fail;
    }
    {
        Ps1File *first = g_ptr_array_index(cue->files, 0);
        Ps1Track *t0 = first->tracks->len ? &g_array_index(first->tracks, Ps1Track, 0) : NULL;

        if (t0 != NULL && t0->index01 != 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("First track INDEX 01 must be 00:00:00"));
            goto fail;
        }
        if (t0 != NULL && (t0->audio || t0->sector_size != 2352)) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE,
                        _("The first track must be MODE2/2352 (a raw PS1 disc image)"));
            goto fail;
        }
    }
    for (i = 0; i < (int)cue->files->len; i++) {
        Ps1File *f = g_ptr_array_index(cue->files, i);
        GStatBuf st;

        if (f->tracks->len == 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_BAD_CUE, _("FILE %s has no TRACK"), f->path);
            goto fail;
        }
        if (g_stat(f->path, &st) != 0) {
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot find %s"), f->path);
            goto fail;
        }
        f->size = st.st_size;
    }

    g_strfreev(lines);
    g_free(text);
    g_free(dir);
    return cue;

fail:
    g_strfreev(lines);
    g_free(text);
    g_free(dir);
    ps1cue_free(cue);
    return NULL;
}

/* ---- header ------------------------------------------------------------------------------------ */

/*
 * Track start positions as they go into the header, after the cue2pops adjustments. For a CUE with
 * several FILE entries (one track each) the files are concatenated and every track after the first
 * is moved by the pregaps; see psx-vcd's recalculate_msf_for_combined().
 */
static void recalc_multifile(Ps1Cue *cue)
{
    guint64 accumulated = 0;
    guint fi, ti;

    for (fi = 0; fi < cue->files->len; fi++) {
        Ps1File *file = g_ptr_array_index(cue->files, fi);
        guint32 physical = (guint32)(file->size / ((Ps1Track *)file->tracks->data)->sector_size);

        for (ti = 0; ti < file->tracks->len; ti++) {
            Ps1Track *t = &g_array_index(file->tracks, Ps1Track, ti);

            if (t->number == 1) {
                t->has_index00 = TRUE;
                t->index00 = 0;
                t->index01 = 150;
            } else if (t->has_index00) {
                t->index00 = (guint32)accumulated + 150 + 150;
                t->index01 = t->index00 + 150;
            } else {
                t->index00 = (guint32)accumulated + 150;
                t->index01 = t->index00;
                t->has_index00 = TRUE;
            }
            accumulated += physical;
        }
    }
}

static void build_header(guint8 *h, Ps1Cue *cue, guint32 total_sectors, int gap_adjust)
{
    guint32 leadout = total_sectors + 150;
    guint tracks = ps1cue_track_count(cue);
    Ps1File *last_file = g_ptr_array_index(cue->files, cue->files->len - 1);
    Ps1Track *last = &g_array_index(last_file->tracks, Ps1Track, last_file->tracks->len - 1);
    guint8 content = last->audio ? 0x01 : 0x41;
    guint off = 30, fi, ti;

    memset(h, 0, VCD_HEADER_SIZE);

    /* descriptor A0: first track / disc type */
    h[0] = 0x41;
    h[2] = 0xA0;
    h[7] = 0x01;
    h[8] = 0x20; /* CD-XA */
    /* descriptor A1: last track / track count */
    h[10] = content;
    h[12] = 0xA1;
    h[17] = bcd(tracks);
    h[20] = content;
    /* descriptor A2: lead-out position */
    h[22] = 0xA2;
    put_msf_bcd(h + 27, leadout);

    /* one 10-byte entry per track */
    for (fi = 0; fi < cue->files->len; fi++) {
        Ps1File *file = g_ptr_array_index(cue->files, fi);

        for (ti = 0; ti < file->tracks->len; ti++) {
            Ps1Track *t = &g_array_index(file->tracks, Ps1Track, ti);
            guint32 i00 = t->has_index00 ? t->index00 : t->index01, i01 = t->index01;

            if (t->number > 1 && gap_adjust != 0) {
                i00 = add_seconds(i00, gap_adjust);
                i01 = add_seconds(i01, gap_adjust);
            }
            h[off] = t->audio ? 0x01 : 0x41;
            h[off + 2] = bcd(t->number);
            put_msf_bcd(h + off + 3, i00);
            put_msf_bcd(h + off + 7, i01);
            off += 10;
        }
    }

    /* signature and sector counts */
    h[0x400] = 0x6B;
    h[0x401] = 0x48;
    h[0x402] = 0x6E;
    h[0x403] = 0x20;
    h[0x408] = total_sectors;
    h[0x409] = total_sectors >> 8;
    h[0x40a] = total_sectors >> 16;
    h[0x40b] = total_sectors >> 24;
    memcpy(h + 0x40c, h + 0x408, 4);
}

/* ---- conversion -------------------------------------------------------------------------------- */

typedef struct {
    FILE *out;
    guint64 written, total;
    Ps2gmProgress cb;
    gpointer ud;
    gboolean cancelled;
} Sink;

static gboolean sink_copy(Sink *s, const char *path, guint64 from, guint64 len, GError **error)
{
    static guint8 buf[1 << 20];
    FILE *in = fopen(path, "rb");

    if (in == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot open %s: %s"), path, g_strerror(errno));
        return FALSE;
    }
    if (from > 0 && fseeko(in, from, SEEK_SET) != 0) {
        fclose(in);
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot seek in %s"), path);
        return FALSE;
    }
    while (len > 0) {
        size_t want = len < sizeof(buf) ? (size_t)len : sizeof(buf), got = fread(buf, 1, want, in);

        if (got == 0)
            break;
        if (fwrite(buf, 1, got, s->out) != got) {
            fclose(in);
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
            return FALSE;
        }
        len -= got;
        s->written += got;
        if (s->cb && !s->cb(s->written, s->total, _("Writing VCD"), s->ud)) {
            fclose(in);
            s->cancelled = TRUE;
            g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_FAILED, _("Cancelled"));
            return FALSE;
        }
    }
    fclose(in);
    return TRUE;
}

gboolean ps1_make_vcd(const char *cue_path, const char *vcd_path, int gap_adjust_seconds,
                      Ps2gmProgress progress, gpointer user_data, GError **error)
{
    Ps1Cue *cue = ps1cue_parse(cue_path, error);
    guint8 *header;
    Sink sink = {0};
    gchar *tmp;
    guint64 data_bytes = 0;
    guint fi, ti;
    gboolean ok = FALSE;
    gboolean single_track;

    if (cue == NULL)
        return FALSE;

    single_track = cue->files->len == 1 && ps1cue_track_count(cue) == 1;

    if (single_track) {
        /* the common case: one BIN, one data track. Track 1 starts at 00:00:00 with its INDEX 01 after the 2 s pregap */
        Ps1File *f = g_ptr_array_index(cue->files, 0);
        Ps1Track *t = &g_array_index(f->tracks, Ps1Track, 0);

        t->has_index00 = TRUE;
        t->index00 = 0;
        t->index01 = 150;
        data_bytes = f->size;
    } else if (cue->files->len > 1) {
        for (fi = 0; fi < cue->files->len; fi++)
            data_bytes += ((Ps1File *)g_ptr_array_index(cue->files, fi))->size;
        recalc_multifile(cue);
    } else {
        /* one BIN holding several tracks: the data is copied track by track from each INDEX 01 */
        Ps1File *f = g_ptr_array_index(cue->files, 0);

        for (ti = 0; ti < f->tracks->len; ti++) {
            Ps1Track *t = &g_array_index(f->tracks, Ps1Track, ti);
            guint64 start = (guint64)t->index01 * t->sector_size, end;

            end = ti + 1 < f->tracks->len ?
                      (guint64)g_array_index(f->tracks, Ps1Track, ti + 1).index01 *
                          g_array_index(f->tracks, Ps1Track, ti + 1).sector_size :
                      f->size;
            if (end > start)
                data_bytes += end - start;
        }
    }

    header = g_malloc(VCD_HEADER_SIZE);
    build_header(header, cue, (guint32)(data_bytes / CD_SECTOR_SIZE), gap_adjust_seconds);

    tmp = g_strconcat(vcd_path, ".part", NULL);
    sink.out = fopen(tmp, "wb");
    if (sink.out == NULL) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot create %s: %s"), tmp, g_strerror(errno));
        goto done;
    }
    sink.cb = progress;
    sink.ud = user_data;
    sink.total = VCD_HEADER_SIZE + data_bytes;
    if (fwrite(header, 1, VCD_HEADER_SIZE, sink.out) != VCD_HEADER_SIZE) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        goto done;
    }
    sink.written = VCD_HEADER_SIZE;

    if (single_track || cue->files->len > 1) {
        for (fi = 0; fi < cue->files->len; fi++) {
            Ps1File *f = g_ptr_array_index(cue->files, fi);

            if (!sink_copy(&sink, f->path, 0, f->size, error))
                goto done;
        }
    } else {
        Ps1File *f = g_ptr_array_index(cue->files, 0);

        for (ti = 0; ti < f->tracks->len; ti++) {
            Ps1Track *t = &g_array_index(f->tracks, Ps1Track, ti);
            guint64 start = (guint64)t->index01 * t->sector_size, end;

            end = ti + 1 < f->tracks->len ?
                      (guint64)g_array_index(f->tracks, Ps1Track, ti + 1).index01 *
                          g_array_index(f->tracks, Ps1Track, ti + 1).sector_size :
                      f->size;
            if (end > start && !sink_copy(&sink, f->path, start, end - start, error))
                goto done;
        }
    }

    if (fclose(sink.out) != 0) {
        sink.out = NULL;
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Write failed (disk full?)"));
        goto done;
    }
    sink.out = NULL;
    if (g_rename(tmp, vcd_path) != 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, _("Cannot rename %s: %s"), tmp, g_strerror(errno));
        goto done;
    }
    ok = TRUE;

done:
    if (sink.out != NULL)
        fclose(sink.out);
    if (!ok)
        g_unlink(tmp);
    g_free(tmp);
    g_free(header);
    ps1cue_free(cue);
    return ok;
}

/* ---- game ID ----------------------------------------------------------------------------------- */

/* Disc ID prefixes of the PS1 era: Europe, USA, Japan (SLPS/SLPM/SCPS/SCPM...), Asia, early Sony discs */
static const char *const id_prefixes[] = {
    "SLES", "SCES", "SLUS", "SCUS", "SLPS", "SLPM", "SCPS", "SCPM", "SIPS", "SLAJ", "SCAJ", "SLKA", "SCKA",
    "SCCS", "SLCS", "SLED", "SCED", "SCPN", "SLPN", "TCES", "TCPS", "PAPX", "PBPX", "PCPX", "PEPX", "PUPX",
    "ESPM", "SIPS", "SPUS", "SCZS", NULL,
};

static gboolean known_prefix(const guint8 *p)
{
    int i;

    for (i = 0; id_prefixes[i] != NULL; i++)
        if (!memcmp(p, id_prefixes[i], 4))
            return TRUE;
    return FALSE;
}

/* "XXXX_123.45", "XXXX-123.45", "XXXX 123.45" or "XXXX_12345": the first one found in the data */
static char *scan_for_id(const guint8 *data, gsize len, gboolean any_letters)
{
    gsize i;

    for (i = 0; i + 11 <= len; i++) {
        const guint8 *p = data + i;
        char sep;
        int k;
        gboolean ok = TRUE;

        for (k = 0; k < 4 && ok; k++)
            ok = g_ascii_isupper(p[k]);
        if (!ok || (!any_letters && !known_prefix(p)))
            continue;
        sep = p[4];
        if (sep != '_' && sep != '-' && sep != ' ')
            continue;
        for (k = 5; k < 8; k++)
            ok = ok && g_ascii_isdigit(p[k]);
        if (!ok)
            continue;
        if (p[8] == '.' && g_ascii_isdigit(p[9]) && g_ascii_isdigit(p[10]))
            return g_strdup_printf("%.4s_%.3s.%.2s", p, p + 5, p + 9);
        if (g_ascii_isdigit(p[8]) && g_ascii_isdigit(p[9]) && sep != ' ') /* SLUS_00707 */
            return g_strdup_printf("%.4s_%.3s.%.2s", p, p + 5, p + 8);
    }
    return NULL;
}

char *ps1_detect_game_id(const char *bin_path)
{
    FILE *f = fopen(bin_path, "rb");
    guint8 *buf;
    gsize got;
    char *id;

    if (f == NULL)
        return NULL;
    buf = g_malloc(150 * 1024);
    got = fread(buf, 1, 150 * 1024, f);
    fclose(f);
    id = scan_for_id(buf, got, FALSE);
    if (id == NULL)
        id = scan_for_id(buf, got, TRUE); /* a prefix not in the list: any four capital letters */
    g_free(buf);
    return id;
}
