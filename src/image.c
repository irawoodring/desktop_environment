/* image.c — a PNG decoder, and smooth scaling, for CSS background images.
 *
 * A PNG file is an 8-byte signature followed by chunks: [length][type][data][crc].
 *   IHDR  width, height, bit depth, color type, interlace method
 *   PLTE  the palette, for palette images
 *   tRNS  transparency for palette (per entry) or gray/RGB (one color) images
 *   IDAT  the pixels, zlib-compressed (possibly split across several IDAT chunks)
 *   IEND  the end
 * After inflating, each row starts with a filter byte. Filters predict each byte from its
 * neighbors (left, above, upper-left) and store only the difference, which compresses
 * better; decoding adds the prediction back. Interlaced (Adam7) images store seven
 * progressively finer sub-images one after another. */
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <math.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* undo the per-row filters in place; rows are 1 + rowbytes long */
static bool unfilter(uint8_t *data, int h, size_t rowbytes, int bpp) {
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *row = data + (size_t)y * (rowbytes + 1);
        uint8_t f = row[0], *cur = row + 1;
        for (size_t i = 0; i < rowbytes; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: cur[i] += a; break;
            case 2: cur[i] += b; break;
            case 3: cur[i] += (a + b) / 2; break;
            case 4: cur[i] += paeth(a, b, c); break;
            default: return false;
            }
        }
        prev = cur;
    }
    return true;
}

struct png {
    int w, h, depth, ctype, channels;
    uint8_t palette[256][4];
    int npalette;
    bool has_trns;
    uint16_t trns[3];              /* the transparent gray / RGB value */
};

/* sample n of a row (bit depths below 8 pack several samples per byte; 16 keeps the high byte) */
static int sample(const struct png *p, const uint8_t *row, int n) {
    switch (p->depth) {
    case 16: return row[n * 2];
    case 8: return row[n];
    default: {
        int per = 8 / p->depth, shift = 8 - p->depth * (n % per + 1);
        return (row[n / per] >> shift) & ((1 << p->depth) - 1);
    }
    }
}

static int sample16(const struct png *p, const uint8_t *row, int n) {   /* raw value, for tRNS */
    return p->depth == 16 ? row[n * 2] << 8 | row[n * 2 + 1] : sample(p, row, n);
}

/* one decoded (unfiltered) row -> premultiplied ARGB at out[0..w) with the given stride */
static void convert_row(const struct png *p, const uint8_t *row, int w, uint32_t *out, int step) {
    int scale = p->ctype == 3 ? 1 : p->depth < 8 ? 255 / ((1 << p->depth) - 1) : 1;
    for (int x = 0; x < w; x++) {
        int r, g, b, a = 255;
        switch (p->ctype) {
        case 0:   /* gray */
            r = g = b = sample(p, row, x) * scale;
            if (p->has_trns && sample16(p, row, x) == p->trns[0]) a = 0;
            break;
        case 2:   /* RGB */
            r = sample(p, row, x * 3);
            g = sample(p, row, x * 3 + 1);
            b = sample(p, row, x * 3 + 2);
            if (p->has_trns && sample16(p, row, x * 3) == p->trns[0] &&
                sample16(p, row, x * 3 + 1) == p->trns[1] && sample16(p, row, x * 3 + 2) == p->trns[2]) a = 0;
            break;
        case 3: { /* palette */
            int i = sample(p, row, x);
            r = p->palette[i][0]; g = p->palette[i][1]; b = p->palette[i][2]; a = p->palette[i][3];
            break;
        }
        case 4:   /* gray + alpha */
            r = g = b = sample(p, row, x * 2);
            a = sample(p, row, x * 2 + 1);
            break;
        default:  /* 6: RGBA */
            r = sample(p, row, x * 4);
            g = sample(p, row, x * 4 + 1);
            b = sample(p, row, x * 4 + 2);
            a = sample(p, row, x * 4 + 3);
            break;
        }
        out[(size_t)x * step] = (uint32_t)a << 24 | (uint32_t)(r * a / 255) << 16 |
                                (uint32_t)(g * a / 255) << 8 | (uint32_t)(b * a / 255);
    }
}

static size_t row_bytes(const struct png *p, int w) { return ((size_t)w * p->channels * p->depth + 7) / 8; }

static struct image *decode(const uint8_t *d, size_t len, const char *path) {
    if (len < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        fprintf(stderr, "pane: %s: not a PNG (only PNG backgrounds are supported)\n", path);
        return NULL;
    }
    struct png p = { 0 };
    uint8_t *z = NULL;
    size_t zlen = 0;
    bool interlaced = false, have_ihdr = false;
    for (size_t off = 8; off + 12 <= len;) {
        uint32_t n = be32(d + off);
        const uint8_t *type = d + off + 4, *body = d + off + 8;
        if (n > len - off - 12) break;
        if (!memcmp(type, "IHDR", 4) && n >= 13) {
            p.w = be32(body);
            p.h = be32(body + 4);
            p.depth = body[8];
            p.ctype = body[9];
            interlaced = body[12] == 1;
            have_ihdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            p.npalette = MIN(n / 3, 256u);
            for (int i = 0; i < p.npalette; i++) {
                memcpy(p.palette[i], body + i * 3, 3);
                p.palette[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (p.ctype == 3) for (uint32_t i = 0; i < n && i < 256; i++) p.palette[i][3] = body[i];
            else if (p.ctype == 0 && n >= 2) { p.has_trns = true; p.trns[0] = body[0] << 8 | body[1]; }
            else if (p.ctype == 2 && n >= 6) {
                p.has_trns = true;
                for (int i = 0; i < 3; i++) p.trns[i] = body[i * 2] << 8 | body[i * 2 + 1];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            z = realloc(z, zlen + n);
            memcpy(z + zlen, body, n);
            zlen += n;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + n;
    }
    static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (!have_ihdr || p.w <= 0 || p.h <= 0 || p.w > 16384 || p.h > 16384 || p.ctype > 6 ||
        !channels[p.ctype] || !(p.depth == 1 || p.depth == 2 || p.depth == 4 || p.depth == 8 || p.depth == 16)) {
        fprintf(stderr, "pane: %s: unsupported PNG\n", path);
        free(z);
        return NULL;
    }
    p.channels = channels[p.ctype];
    int bpp = MAX(1, p.channels * p.depth / 8);

    /* Adam7: pass n covers pixels starting at (x0, y0), every (dx, dy) */
    static const int a7[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
                                  { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
    int npasses = interlaced ? 7 : 1;
    size_t raw_len = 0;
    for (int i = 0; i < npasses; i++) {
        int pw = interlaced ? (p.w - a7[i][0] + a7[i][2] - 1) / a7[i][2] : p.w;
        int ph = interlaced ? (p.h - a7[i][1] + a7[i][3] - 1) / a7[i][3] : p.h;
        if (pw > 0 && ph > 0) raw_len += (size_t)ph * (row_bytes(&p, pw) + 1);
    }
    uint8_t *raw = malloc(raw_len);
    uLongf got = raw_len;
    if (!raw || uncompress(raw, &got, z, zlen) != Z_OK || got != raw_len) {
        fprintf(stderr, "pane: %s: corrupt PNG data\n", path);
        free(raw);
        free(z);
        return NULL;
    }
    free(z);

    struct image *img = malloc(sizeof *img);
    img->w = p.w;
    img->h = p.h;
    img->px = calloc((size_t)p.w * p.h, 4);
    uint8_t *pass = raw;
    for (int i = 0; i < npasses; i++) {
        int x0 = interlaced ? a7[i][0] : 0, y0 = interlaced ? a7[i][1] : 0;
        int dx = interlaced ? a7[i][2] : 1, dy = interlaced ? a7[i][3] : 1;
        int pw = (p.w - x0 + dx - 1) / dx, ph = (p.h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = row_bytes(&p, pw);
        if (!unfilter(pass, ph, rb, bpp)) {
            fprintf(stderr, "pane: %s: bad PNG filter\n", path);
            free(img->px);
            free(img);
            free(raw);
            return NULL;
        }
        for (int y = 0; y < ph; y++)
            convert_row(&p, pass + (size_t)y * (rb + 1) + 1, pw,
                        img->px + (size_t)(y0 + y * dy) * p.w + x0, dx);
        pass += (size_t)ph * (rb + 1);
    }
    free(raw);
    return img;
}

/* --------------------------------------------------------------------- caches */

struct loaded { char path[512]; struct image *img; };
static struct loaded loaded[16];
static int nloaded;

struct scaled { const struct image *src; struct image img; unsigned last_use; };
static struct scaled scaled[8];
static unsigned use_clock;

const struct image *image_load(const char *path) {
    for (int i = 0; i < nloaded; i++)
        if (!strcmp(loaded[i].path, path)) return loaded[i].img;
    FILE *f = fopen(path, "rb");
    struct image *img = NULL;
    if (!f) {
        fprintf(stderr, "pane: can't open image %s\n", path);
    } else {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = len > 0 ? malloc(len) : NULL;
        if (data && fread(data, 1, len, f) == (size_t)len) img = decode(data, len, path);
        free(data);
        fclose(f);
    }
    /* remember failures too, so a bad path isn't retried every frame */
    if (nloaded < 16) {
        snprintf(loaded[nloaded].path, sizeof loaded[nloaded].path, "%s", path);
        loaded[nloaded++].img = img;
    }
    return img;
}

/* halve until within 2x of the target (averaging 2x2 blocks), then bilinear: cheap, and
 * no shimmering when a big wallpaper is shrunk to fit */
static struct image halve(const struct image *s) {
    struct image d = { MAX(s->w / 2, 1), MAX(s->h / 2, 1), NULL };
    d.px = malloc((size_t)d.w * d.h * 4);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            uint32_t q[4] = { s->px[(size_t)(2 * y) * s->w + 2 * x],
                              s->px[(size_t)(2 * y) * s->w + MIN(2 * x + 1, s->w - 1)],
                              s->px[(size_t)MIN(2 * y + 1, s->h - 1) * s->w + 2 * x],
                              s->px[(size_t)MIN(2 * y + 1, s->h - 1) * s->w + MIN(2 * x + 1, s->w - 1)] };
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t sum = 0;
                for (int i = 0; i < 4; i++) sum += (q[i] >> sh) & 0xff;
                out |= ((sum + 2) / 4) << sh;
            }
            d.px[(size_t)y * d.w + x] = out;
        }
    return d;
}

static struct image bilinear(const struct image *s, int w, int h) {
    struct image d = { w, h, malloc((size_t)w * h * 4) };
    for (int y = 0; y < h; y++) {
        float sy = fmaxf((y + 0.5f) * s->h / h - 0.5f, 0);
        int y0 = MIN((int)sy, s->h - 1), y1 = MIN(y0 + 1, s->h - 1);
        float fy = sy - y0;
        for (int x = 0; x < w; x++) {
            float sx = fmaxf((x + 0.5f) * s->w / w - 0.5f, 0);
            int x0 = MIN((int)sx, s->w - 1), x1 = MIN(x0 + 1, s->w - 1);
            float fx = sx - x0;
            uint32_t a = s->px[(size_t)y0 * s->w + x0], b = s->px[(size_t)y0 * s->w + x1];
            uint32_t c = s->px[(size_t)y1 * s->w + x0], e = s->px[(size_t)y1 * s->w + x1];
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                float top = ((a >> sh) & 0xff) * (1 - fx) + ((b >> sh) & 0xff) * fx;
                float bot = ((c >> sh) & 0xff) * (1 - fx) + ((e >> sh) & 0xff) * fx;
                out |= (uint32_t)(top * (1 - fy) + bot * fy + 0.5f) << sh;
            }
            d.px[(size_t)y * w + x] = out;
        }
    }
    return d;
}

const struct image *image_scaled(const struct image *img, int w, int h) {
    if (img->w == w && img->h == h) return img;
    use_clock++;
    struct scaled *slot = &scaled[0];
    for (int i = 0; i < 8; i++) {
        if (scaled[i].src == img && scaled[i].img.w == w && scaled[i].img.h == h) {
            scaled[i].last_use = use_clock;
            return &scaled[i].img;
        }
        if (scaled[i].last_use < slot->last_use) slot = &scaled[i];
    }
    struct image cur = *img;
    bool owned = false;
    while (cur.w >= 2 * w && cur.h >= 2 * h) {
        struct image next = halve(&cur);
        if (owned) free(cur.px);
        cur = next;
        owned = true;
    }
    struct image out = bilinear(&cur, w, h);
    if (owned) free(cur.px);
    free(slot->img.px);
    *slot = (struct scaled){ img, out, use_clock };
    return &slot->img;
}

void image_cache_clear(void) {
    for (int i = 0; i < 8; i++) {
        free(scaled[i].img.px);
        scaled[i] = (struct scaled){ 0 };
    }
    for (int i = 0; i < nloaded; i++) {
        if (loaded[i].img) free(loaded[i].img->px);
        free(loaded[i].img);
    }
    nloaded = 0;
}
