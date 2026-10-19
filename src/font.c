/* font.c — load PC Screen Font (PSF) files: the bitmap fonts the Linux console uses.
 *
 * You already have a pile of them from kbd: the .psfu.gz files in /usr/share/consolefonts.
 * PSF1: 4-byte header, 8 pixels wide, 256 or 512 glyphs.
 * PSF2: 32-byte header with width/height/count, optional unicode table.
 * The unicode table says which code points each glyph draws; without it, glyph N = char N. */
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const char *const font_dirs[] = {
    "/usr/share/consolefonts", "/usr/share/kbd/consolefonts", "/usr/lib/kbd/consolefonts",
};
static const char *const font_exts[] = { "", ".psfu.gz", ".psf.gz", ".psfu", ".psf" };

struct cached { char name[128]; struct font *font; bool used; };
static struct cached cache[8];

static uint8_t *slurp(const char *path, size_t *len) {
    gzFile g = gzopen(path, "rb");   /* reads plain files too */
    if (!g) return NULL;
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    int r;
    while ((r = gzread(g, buf + n, cap - n)) > 0) {
        n += r;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    gzclose(g);
    *len = n;
    return buf;
}

static void map_add(struct font *f, uint32_t cp, int glyph, int *cap) {
    if (f->nmap == *cap) {
        *cap = *cap ? *cap * 2 : 512;
        f->map_cp = realloc(f->map_cp, *cap * sizeof *f->map_cp);
        f->map_glyph = realloc(f->map_glyph, *cap * sizeof *f->map_glyph);
    }
    f->map_cp[f->nmap] = cp;
    f->map_glyph[f->nmap] = glyph;
    f->nmap++;
}

static void sort_map(struct font *f) {   /* insertion sort: a few hundred entries, done once */
    for (int i = 1; i < f->nmap; i++) {
        uint32_t cp = f->map_cp[i];
        uint16_t g = f->map_glyph[i];
        int j = i - 1;
        while (j >= 0 && f->map_cp[j] > cp) {
            f->map_cp[j + 1] = f->map_cp[j];
            f->map_glyph[j + 1] = f->map_glyph[j];
            j--;
        }
        f->map_cp[j + 1] = cp;
        f->map_glyph[j + 1] = g;
    }
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static struct font *parse_psf(const uint8_t *d, size_t len) {
    struct font *f = calloc(1, sizeof *f);
    int cap = 0;
    const uint8_t *table = NULL, *end = d + len;
    bool psf1 = false;

    if (len >= 4 && d[0] == 0x36 && d[1] == 0x04) {
        psf1 = true;
        uint8_t mode = d[2];
        f->width = 8;
        f->height = d[3];
        f->row_bytes = 1;
        f->nglyphs = (mode & 1) ? 512 : 256;
        size_t gsize = (size_t)f->nglyphs * f->height;
        if (4 + gsize > len) goto bad;
        f->glyphs = malloc(gsize);
        memcpy(f->glyphs, d + 4, gsize);
        if (mode & 0x06) table = d + 4 + gsize;
    } else if (len >= 32 && rd32(d) == 0x864ab572) {
        uint32_t hdr = rd32(d + 8), flags = rd32(d + 12), count = rd32(d + 16);
        uint32_t charsize = rd32(d + 20), height = rd32(d + 24), width = rd32(d + 28);
        f->width = width;
        f->height = height;
        f->row_bytes = (width + 7) / 8;
        f->nglyphs = count;
        if (charsize != (uint32_t)f->row_bytes * height || hdr > len ||
            (size_t)count * charsize > len - hdr || width == 0 || height == 0) goto bad;
        f->glyphs = malloc((size_t)count * charsize);
        memcpy(f->glyphs, d + hdr, (size_t)count * charsize);
        if (flags & 1) table = d + hdr + (size_t)count * charsize;
    } else {
        goto bad;
    }

    if (table) {
        const uint8_t *p = table;
        for (int g = 0; g < f->nglyphs && p < end; g++) {
            if (psf1) {   /* uint16 code points; 0xFFFE starts a sequence, 0xFFFF ends the glyph */
                bool seq = false;
                while (p + 1 < end) {
                    uint16_t u = p[0] | p[1] << 8;
                    p += 2;
                    if (u == 0xffff) break;
                    if (u == 0xfffe) seq = true;
                    else if (!seq) map_add(f, u, g, &cap);
                }
            } else {      /* UTF-8; 0xFE starts a sequence, 0xFF ends the glyph */
                bool seq = false;
                while (p < end && *p != 0xff) {
                    if (*p == 0xfe) { seq = true; p++; continue; }
                    uint32_t cp;
                    int n;
                    if (*p < 0x80) { cp = *p; n = 1; }
                    else if ((*p & 0xe0) == 0xc0) { cp = *p & 0x1f; n = 2; }
                    else if ((*p & 0xf0) == 0xe0) { cp = *p & 0x0f; n = 3; }
                    else { cp = *p & 0x07; n = 4; }
                    for (int i = 1; i < n && p + i < end; i++) cp = (cp << 6) | (p[i] & 0x3f);
                    p += n;
                    if (!seq) map_add(f, cp, g, &cap);
                }
                p++;
            }
        }
        sort_map(f);
    }
    return f;
bad:
    free(f->glyphs);
    free(f);
    return NULL;
}

int font_glyph(const struct font *f, uint32_t cp) {
    if (!f->nmap) return cp < (uint32_t)f->nglyphs ? (int)cp : -1;
    int lo = 0, hi = f->nmap - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->map_cp[mid] == cp) return f->map_glyph[mid];
        if (f->map_cp[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static struct font *load(const char *family) {
    char path[512];
    for (size_t e = 0; e < ARRAY_LEN(font_exts); e++) {
        if (family[0] == '/') {
            snprintf(path, sizeof path, "%s%s", family, font_exts[e]);
        } else {
            for (size_t d = 0; d < ARRAY_LEN(font_dirs); d++) {
                snprintf(path, sizeof path, "%s/%s%s", font_dirs[d], family, font_exts[e]);
                size_t len;
                uint8_t *data = slurp(path, &len);
                if (!data) continue;
                struct font *f = parse_psf(data, len);
                free(data);
                if (f) return f;
            }
            continue;
        }
        size_t len;
        uint8_t *data = slurp(path, &len);
        if (!data) continue;
        struct font *f = parse_psf(data, len);
        free(data);
        if (f) return f;
    }
    return NULL;
}

const struct font *font_get(const char *family) {
    if (!family || !family[0]) family = "Lat2-Terminus16";
    static size_t next;
    for (size_t i = 0; i < ARRAY_LEN(cache); i++)
        if (cache[i].used && strcmp(cache[i].name, family) == 0) return cache[i].font;
    /* remember misses too, so a typo in the theme doesn't mean a disk read every frame */
    struct font *f = load(family);
    if (!f) fprintf(stderr, "pane: font '%s' not found; text using it won't be drawn\n", family);
    struct cached *slot = &cache[next++ % ARRAY_LEN(cache)];
    snprintf(slot->name, sizeof slot->name, "%s", family);
    slot->font = f;   /* an evicted font is leaked: fonts are small and themes use one or two */
    slot->used = true;
    return f;
}
