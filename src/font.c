/* font.c — text: Linux console fonts (PSF) and, with FreeType, TrueType/OpenType fonts.
 *
 * PSF ("PC Screen Font") is what the Linux console uses: you already have a pile of them
 * from kbd in /usr/share/consolefonts. Each glyph is a little bitmap, so they're crisp
 * but only scale by whole multiples.
 *   PSF1: 4-byte header, 8 pixels wide, 256 or 512 glyphs.
 *   PSF2: 32-byte header with width/height/count, optional unicode table.
 *   The unicode table says which code points each glyph draws; without it, glyph N = char N.
 *
 * FreeType rasterizes outline fonts at any size into 8-bit coverage masks (anti-aliased).
 * We find fonts ourselves by scanning the usual font directories once and reading each
 * file's family name, weight and slant, so there's no fontconfig dependency. */
#include "render.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#ifdef PANE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#endif

/* ===================================================================== PSF */

struct psf {
    int width, height, row_bytes, nglyphs;
    uint8_t *glyphs;
    int nmap;                      /* unicode -> glyph, sorted by code point */
    uint32_t *map_cp;
    uint16_t *map_glyph;
};

static const char *const psf_dirs[] = {
    "/usr/share/consolefonts", "/usr/share/kbd/consolefonts", "/usr/lib/kbd/consolefonts",
};
static const char *const psf_exts[] = { "", ".psfu.gz", ".psf.gz", ".psfu", ".psf" };

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

static void map_add(struct psf *f, uint32_t cp, int glyph, int *cap) {
    if (f->nmap == *cap) {
        *cap = *cap ? *cap * 2 : 512;
        f->map_cp = realloc(f->map_cp, *cap * sizeof *f->map_cp);
        f->map_glyph = realloc(f->map_glyph, *cap * sizeof *f->map_glyph);
    }
    f->map_cp[f->nmap] = cp;
    f->map_glyph[f->nmap] = glyph;
    f->nmap++;
}

static void sort_map(struct psf *f) {   /* insertion sort: a few hundred entries, done once */
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

static struct psf *parse_psf(const uint8_t *d, size_t len) {
    struct psf *f = calloc(1, sizeof *f);
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

static int psf_glyph(const struct psf *f, uint32_t cp) {
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

static struct psf *psf_try(const char *path) {
    size_t len;
    uint8_t *data = slurp(path, &len);
    if (!data) return NULL;
    struct psf *f = parse_psf(data, len);
    free(data);
    return f;
}

/* by console-font name ("Lat2-Terminus16") or path */
static struct psf *psf_find(const char *name) {
    static struct { char name[128]; struct psf *f; } seen[16];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i].name, name)) return seen[i].f;
    struct psf *f = NULL;
    char path[512];
    for (size_t e = 0; e < ARRAY_LEN(psf_exts) && !f; e++) {
        if (strchr(name, '/')) {
            snprintf(path, sizeof path, "%s%s", name, psf_exts[e]);
            f = psf_try(path);
            continue;
        }
        for (size_t d = 0; d < ARRAY_LEN(psf_dirs) && !f; d++) {
            snprintf(path, sizeof path, "%s/%s%s", psf_dirs[d], name, psf_exts[e]);
            f = psf_try(path);
        }
    }
    if (nseen < 16) {   /* remember misses too: no disk access every frame for a typo */
        snprintf(seen[nseen].name, sizeof seen[nseen].name, "%s", name);
        seen[nseen++].f = f;
    }
    return f;
}

/* ================================================================ FreeType */

#ifdef PANE_FREETYPE
static FT_Library ft;

struct face_info {
    char *path;
    int index;
    char family[96];
    int weight;
    bool italic;
    bool mono;
};
static struct face_info *faces;
static int nfaces, capfaces;
static bool scanned;

static void index_file(const char *path) {
    FT_Face face;
    if (FT_New_Face(ft, path, -1, &face)) return;   /* -1: just count the faces */
    int n = face->num_faces;
    FT_Done_Face(face);
    for (int i = 0; i < n; i++) {
        if (FT_New_Face(ft, path, i, &face)) continue;
        if (face->family_name && FT_IS_SCALABLE(face)) {
            if (nfaces == capfaces) {
                capfaces = capfaces ? capfaces * 2 : 256;
                faces = realloc(faces, capfaces * sizeof *faces);
            }
            struct face_info *fi = &faces[nfaces++];
            fi->path = strdup(path);
            fi->index = i;
            snprintf(fi->family, sizeof fi->family, "%s", face->family_name);
            TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
            fi->weight = os2 && os2->usWeightClass ? os2->usWeightClass
                                                   : (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
            fi->italic = face->style_flags & FT_STYLE_FLAG_ITALIC;
            fi->mono = FT_IS_FIXED_WIDTH(face);
        }
        FT_Done_Face(face);
    }
}

static void scan_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d || depth > 6) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) { scan_dir(path, depth + 1); continue; }
        const char *ext = strrchr(e->d_name, '.');
        if (ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf") || !strcasecmp(ext, ".ttc")))
            index_file(path);
    }
    closedir(d);
}

static void scan_fonts(void) {
    if (scanned) return;
    scanned = true;
    if (FT_Init_FreeType(&ft)) { fprintf(stderr, "pane: FreeType failed to start\n"); return; }
    scan_dir("/usr/share/fonts", 0);
    scan_dir("/usr/local/share/fonts", 0);
    const char *home = getenv("HOME");
    if (home) {
        char p[512];
        snprintf(p, sizeof p, "%s/.local/share/fonts", home);
        scan_dir(p, 0);
        snprintf(p, sizeof p, "%s/.fonts", home);
        scan_dir(p, 0);
    }
}

/* the face of `family` closest to the wanted weight and slant */
static struct face_info *ft_match(const char *family, int weight, bool italic) {
    struct face_info *best = NULL;
    int best_score = 1 << 30;
    for (int i = 0; i < nfaces; i++) {
        if (strcasecmp(faces[i].family, family)) continue;
        int score = abs(faces[i].weight - weight) + (faces[i].italic != italic ? 1000 : 0);
        if (score < best_score) { best = &faces[i]; best_score = score; }
    }
    return best;
}

static struct face_info *ft_generic(const char *generic, int weight, bool italic) {
    static const char *const sans[] = { "DejaVu Sans", "Liberation Sans", "Noto Sans", "Cantarell",
                                        "FreeSans", NULL };
    static const char *const serif[] = { "DejaVu Serif", "Liberation Serif", "Noto Serif", "FreeSerif", NULL };
    static const char *const mono[] = { "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono",
                                        "FreeMono", NULL };
    const char *const *list = !strcasecmp(generic, "serif") ? serif :
                              !strcasecmp(generic, "monospace") ? mono : sans;
    for (int i = 0; list[i]; i++) {
        struct face_info *f = ft_match(list[i], weight, italic);
        if (f) return f;
    }
    for (int i = 0; i < nfaces; i++)   /* anything at all, preferring fixed-width for monospace */
        if (faces[i].mono == (list == mono)) return ft_match(faces[i].family, weight, italic);
    return nfaces ? &faces[0] : NULL;
}
#endif

/* ============================================================= font handles */

struct glyph {
    uint32_t cp;
    struct glyph *next;
    unsigned index;                /* FreeType glyph index (for kerning) */
    int w, h, left, top, advance;
    uint8_t *cov;                  /* w * h coverage, 0..255 */
};

struct font {
    char key[192];
    bool is_psf;
    int line_h, ascent;
    /* PSF */
    const struct psf *psf;
    int scale;
#ifdef PANE_FREETYPE
    FT_Face face;
    bool kerning;
#endif
    struct glyph *buckets[128];
};

static struct font *fonts[32];
static int nfonts;

static struct font *new_psf_font(const struct psf *p, int px) {
    struct font *f = calloc(1, sizeof *f);
    f->is_psf = true;
    f->psf = p;
    f->scale = MAX(1, (int)lroundf((float)px / p->height));
    f->line_h = f->ascent = p->height * f->scale;
    return f;
}

#ifdef PANE_FREETYPE
static struct font *new_ft_font(const struct face_info *fi, int px) {
    FT_Face face;
    if (FT_New_Face(ft, fi->path, fi->index, &face)) return NULL;
    FT_Set_Pixel_Sizes(face, 0, MAX(px, 1));
    struct font *f = calloc(1, sizeof *f);
    f->face = face;
    f->kerning = FT_HAS_KERNING(face);
    f->ascent = (int)(face->size->metrics.ascender >> 6);
    f->line_h = (int)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
    return f;
}
#endif

static struct font *resolve_one(const char *name, int weight, bool italic, int px) {
    bool generic = !strcasecmp(name, "sans-serif") || !strcasecmp(name, "serif") ||
                   !strcasecmp(name, "monospace") || !strcasecmp(name, "system-ui");
    const char *ext = strrchr(name, '.');
    bool psf_path = ext && strstr(ext, "psf");
    if (!generic && (psf_path || !strchr(name, ' '))) {   /* console font names have no spaces */
        struct psf *p = psf_find(name);
        if (p) return new_psf_font(p, px);
    }
#ifdef PANE_FREETYPE
    scan_fonts();
    struct face_info *fi = NULL, direct;
    if (strchr(name, '/') && !psf_path) {   /* a path to a .ttf/.otf */
        direct = (struct face_info){ .path = (char *)name, .index = 0 };
        fi = &direct;
    } else {
        fi = generic ? ft_generic(name, weight, italic) : ft_match(name, weight, italic);
    }
    if (fi) return new_ft_font(fi, px);
#else
    (void)weight; (void)italic;
#endif
    return NULL;
}

const struct font *font_get(const char *families, int weight, bool italic, int px) {
    char key[192];
    snprintf(key, sizeof key, "%s|%d|%d|%d", families ? families : "", weight, italic, px);
    for (int i = 0; i < nfonts; i++)
        if (!strcmp(fonts[i]->key, key)) return fonts[i];

    struct font *f = NULL;
    char list[160];
    snprintf(list, sizeof list, "%s", families ? families : "");
    for (char *save, *name = strtok_r(list, ",", &save); name && !f; name = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*name) || *name == '"' || *name == '\'') name++;
        char *end = name + strlen(name);
        while (end > name && (isspace((unsigned char)end[-1]) || end[-1] == '"' || end[-1] == '\'')) *--end = '\0';
        if (*name) f = resolve_one(name, weight, italic, px);
    }
    if (!f) {   /* nothing on the list exists: a sans-serif outline font, else the console font */
#ifdef PANE_FREETYPE
        f = resolve_one("sans-serif", weight, italic, px);
#endif
        if (!f) {
            struct psf *p = psf_find("Lat2-Terminus16");
            if (p) f = new_psf_font(p, px);
        }
        if (!f) {
            static bool warned;
            if (!warned) fprintf(stderr, "pane: no usable font found; text won't be drawn\n");
            warned = true;
            return NULL;
        }
    }
    snprintf(f->key, sizeof f->key, "%s", key);
    if (nfonts == (int)ARRAY_LEN(fonts)) nfonts--;   /* full: replace the newest (fonts are small) */
    fonts[nfonts++] = f;
    return f;
}

int font_line_height(const struct font *f) { return f ? f->line_h : 16; }

/* ------------------------------------------------------------------ glyphs */

static const struct glyph *get_glyph(struct font *f, uint32_t cp) {
    struct glyph **b = &f->buckets[cp % ARRAY_LEN(f->buckets)];
    for (struct glyph *g = *b; g; g = g->next)
        if (g->cp == cp) return g;
    struct glyph *g = calloc(1, sizeof *g);
    g->cp = cp;
    if (f->is_psf) {
        const struct psf *p = f->psf;
        int idx = psf_glyph(p, cp);
        g->advance = p->width * f->scale;
        if (idx >= 0) {   /* expand the bitmap at our scale */
            g->w = p->width * f->scale;
            g->h = p->height * f->scale;
            g->top = f->ascent;
            g->cov = calloc((size_t)g->w * g->h, 1);
            const uint8_t *bits = p->glyphs + (size_t)idx * p->row_bytes * p->height;
            for (int y = 0; y < g->h; y++)
                for (int x = 0; x < g->w; x++) {
                    int gx = x / f->scale, gy = y / f->scale;
                    if (bits[gy * p->row_bytes + gx / 8] & (0x80 >> (gx % 8))) g->cov[y * g->w + x] = 255;
                }
        } else {
            g->advance = 0;   /* missing: the caller substitutes */
        }
    }
#ifdef PANE_FREETYPE
    else {
        g->index = FT_Get_Char_Index(f->face, cp);
        if (g->index && !FT_Load_Glyph(f->face, g->index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
            FT_GlyphSlot s = f->face->glyph;
            g->w = s->bitmap.width;
            g->h = s->bitmap.rows;
            g->left = s->bitmap_left;
            g->top = s->bitmap_top;
            g->advance = (int)((s->advance.x + 32) >> 6);
            g->cov = malloc((size_t)g->w * g->h + 1);
            for (int y = 0; y < g->h; y++)
                memcpy(g->cov + (size_t)y * g->w, s->bitmap.buffer + (ptrdiff_t)y * s->bitmap.pitch, g->w);
        }
    }
#endif
    g->next = *b;
    *b = g;
    return g;
}

static uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t cp;
    int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xe0) == 0xc0) { cp = s[0] & 0x1f; n = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { cp = s[0] & 0x0f; n = 3; }
    else if ((s[0] & 0xf8) == 0xf0) { cp = s[0] & 0x07; n = 4; }
    else { *ps += 1; return 0xfffd; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *ps += i; return 0xfffd; }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *ps += n;
    return cp;
}

/* a glyph for cp, falling back to '?' when the font doesn't have it */
static const struct glyph *glyph_or_fallback(struct font *f, uint32_t cp) {
    const struct glyph *g = get_glyph(f, cp);
    if (!g->advance && !g->cov && cp != ' ') g = get_glyph(f, '?');
    return g;
}

static int kern(struct font *f, unsigned prev, unsigned cur) {
#ifdef PANE_FREETYPE
    if (!f->is_psf && f->kerning && prev && cur) {
        FT_Vector d;
        if (!FT_Get_Kerning(f->face, prev, cur, FT_KERNING_DEFAULT, &d)) return (int)(d.x >> 6);
    }
#else
    (void)f; (void)prev; (void)cur;
#endif
    return 0;
}

int text_width(const struct font *cf, const char *s) {
    struct font *f = (struct font *)cf;
    if (!f) return 0;
    int w = 0;
    unsigned prev = 0;
    while (*s) {
        const struct glyph *g = glyph_or_fallback(f, utf8_next(&s));
        w += kern(f, prev, g->index) + g->advance;
        prev = g->index;
    }
    return w;
}

/* plot one glyph pixel, with the text box rotated as asked */
static void plot(struct canvas *c, int x, int y, int rotation, int W, int H, int gx, int gy, uint32_t px) {
    if (gx < 0 || gy < 0 || gx >= W || gy >= H) return;
    if (rotation == 90) canvas_blend(c, x + (H - 1 - gy), y + gx, px);
    else if (rotation == 270) canvas_blend(c, x + gy, y + (W - 1 - gx), px);
    else canvas_blend(c, x + gx, y + gy, px);
}

void draw_text(struct canvas *c, const struct font *cf, int x, int y, const char *s,
               struct color col, float opacity, int max_w, int rotation) {
    struct font *f = (struct font *)cf;
    if (!f || col.a == 0 || max_w <= 0) return;
    /* too long? cut it and finish with an ellipsis */
    const char *ell = "\xe2\x80\xa6";   /* U+2026 */
    const struct glyph *eg = get_glyph(f, 0x2026);
    if (!eg->cov) ell = "...";
    int full = text_width(f, s), ell_w = text_width(f, ell);
    int limit = full > max_w ? max_w - ell_w : max_w;
    int W = MIN(full, max_w), H = f->line_h;

    int pen = 0;
    unsigned prev = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = pass ? ell : s;
        if (pass && full <= max_w) break;
        while (*p) {
            const struct glyph *g = glyph_or_fallback(f, utf8_next(&p));
            int k = kern(f, prev, g->index);
            if (!pass && pen + k + g->advance > limit) break;
            pen += k;
            for (int gy = 0; gy < g->h; gy++)
                for (int gx = 0; gx < g->w; gx++) {
                    uint8_t a = g->cov[gy * g->w + gx];
                    if (a) plot(c, x, y, rotation, W, H, pen + g->left + gx, f->ascent - g->top + gy,
                                color_premul(col, a / 255.0f * opacity));
                }
            pen += g->advance;
            prev = g->index;
        }
    }
}
