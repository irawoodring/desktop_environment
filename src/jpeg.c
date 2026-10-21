/* jpeg.c — a JPEG decoder, for wallpapers and other background images.
 *
 * A JPEG holds the picture in 8x8 blocks, each stored as its frequency
 * content (a DCT) rather than its pixels: a few numbers say "this block is
 * mostly this bright, a little brighter at the top", and the fine detail the
 * eye barely notices is rounded away. Decoding undoes that:
 *
 *   1. entropy decoding: Huffman codes -> the blocks' (quantized) coefficients
 *   2. dequantize: multiply each coefficient back up by its quantization step
 *   3. inverse DCT: frequencies -> 8x8 pixels, per color component
 *   4. upsample the color components, often stored at half resolution, and
 *      convert YCbCr (brightness + two color differences) to RGB
 *
 * Baseline JPEGs send each block whole, in one pass. Progressive ones (common
 * on the web) send several passes over the whole image, each adding
 * coefficients or more bits of them, so this decoder collects every block's
 * coefficients first and turns them into pixels at the end, for both kinds.
 * Also handled: grayscale, any chroma subsampling, restart markers, and the
 * EXIF orientation tag that phones use instead of rotating the pixels.
 * Not handled (rare): arithmetic coding, 12-bit, lossless and CMYK JPEGs. */
#include "render.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* where coefficient k (in the order they're sent, a zigzag from low to high
 * frequency) goes in a row-by-row 8x8 block */
static const uint8_t dezigzag[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* a corrupt run can step past 63: land somewhere harmless */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

/* A Huffman table: the codes of each length 1..16, in order, stand for the
 * listed symbols in order ("canonical" codes, so the lengths are enough).
 * `fast` decodes codes of up to 9 bits with one lookup. */
#define FAST_BITS 9
struct huff {
    bool present;
    uint8_t sym[256];
    uint16_t fast[1 << FAST_BITS];   /* (length << 8 | symbol), 0 = longer code */
    int32_t maxcode[18];             /* per length: codes below this, left-aligned to 16 bits */
    int delta[17];                   /* symbol index = code + delta[length] */
};

struct comp {
    int id, h, v, tq;                /* sampling factors, quantization table */
    int bw, bh;                      /* blocks across and down, padded to whole MCUs */
    int w, h_px;                     /* the component's own size in pixels */
    int16_t *coef;                   /* bw * bh blocks of 64 coefficients, row by row */
    int dc_pred;
    struct huff *hd, *ha;
    uint8_t *px;                     /* decoded samples: bw*8 by bh*8 */
};

struct jpeg {
    const uint8_t *d;
    size_t len, pos;
    char *err;
    size_t errlen;
    uint16_t q[4][64];               /* quantization tables, row-by-row order */
    struct huff dc[4], ac[4];
    struct comp c[3];
    int nc, w, h, hmax, vmax, mcux, mcuy;
    bool progressive, adobe, adobe_rgb;
    int restart, orientation;
    /* the bit reader over the entropy-coded data */
    uint32_t buf;
    int nbits, marker, eobrun;
};

static bool fail(struct jpeg *j, const char *fmt, ...) {
    if (j->err[0]) return false;   /* keep the first reason */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, j->errlen, fmt, ap);
    va_end(ap);
    return false;
}

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* --------------------------------------------------------------- bit reader */

/* Inside the coded data a 0xFF byte is followed by 0x00 ("stuffing"), so a
 * real marker (0xFF and anything else) can't appear by accident. At a marker
 * the reader stops and feeds zeros. */
static void fill(struct jpeg *j) {
    while (j->nbits <= 24) {
        uint32_t b = 0;
        if (!j->marker && j->pos < j->len) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                uint8_t next = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0xD9;
                if (next == 0) j->pos += 2;
                else { j->marker = next; b = 0; }
            } else {
                j->pos++;
            }
        }
        j->buf |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int bits(struct jpeg *j, int n) {
    if (n == 0) return 0;
    fill(j);
    int v = (int)(j->buf >> (32 - n));
    j->buf <<= n;
    j->nbits -= n;
    return v;
}

static int bit(struct jpeg *j) { return bits(j, 1); }

/* n bits holding a signed value: 0..2^(n-1)-1 stand for the negatives */
static int extend(struct jpeg *j, int n) {
    int v = bits(j, n);
    return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static bool build_huff(struct huff *h, const uint8_t counts[16], const uint8_t *syms, int nsyms) {
    memset(h, 0, sizeof *h);
    memcpy(h->sym, syms, (size_t)nsyms);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->delta[len] = k - code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++) {
            if (len <= FAST_BITS) {
                int first = code << (FAST_BITS - len), n = 1 << (FAST_BITS - len);
                for (int f = 0; f < n; f++) h->fast[first + f] = (uint16_t)(len << 8 | h->sym[k]);
            }
        }
        if (code > (1 << len)) return false;
        h->maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->present = true;
    return true;
}

static int decode(struct jpeg *j, const struct huff *h) {
    fill(j);
    uint32_t top = j->buf >> 16;
    uint16_t f = h->fast[top >> (16 - FAST_BITS)];
    if (f) {
        int len = f >> 8;
        j->buf <<= len;
        j->nbits -= len;
        return f & 0xff;
    }
    int len = FAST_BITS + 1;
    while (len <= 16 && (int32_t)top >= h->maxcode[len]) len++;
    if (len > 16) return -1;
    int code = (int)(top >> (16 - len));
    j->buf <<= len;
    j->nbits -= len;
    int k = code + h->delta[len];
    return k >= 0 && k < 256 ? h->sym[k] : -1;
}

/* ------------------------------------------------------------ decoding blocks */

/* baseline: one whole block */
static bool block_baseline(struct jpeg *j, struct comp *c, int16_t *b) {
    int t = decode(j, c->hd);
    if (t < 0 || t > 16) return fail(j, "corrupt data");
    c->dc_pred += extend(j, t);
    b[0] = (int16_t)c->dc_pred;
    for (int k = 1; k < 64;) {
        int rs = decode(j, c->ha);
        if (rs < 0) return fail(j, "corrupt data");
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;   /* end of block: the rest are zeros */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return fail(j, "corrupt data");
        b[dezigzag[k++]] = (int16_t)extend(j, s);
    }
    return true;
}

/* progressive, a DC pass: the first bits of the DC coefficient, or one more bit */
static bool block_dc(struct jpeg *j, struct comp *c, int16_t *b, int ah, int al) {
    if (ah == 0) {
        int t = decode(j, c->hd);
        if (t < 0 || t > 16) return fail(j, "corrupt data");
        c->dc_pred += extend(j, t);
        b[0] = (int16_t)(c->dc_pred * (1 << al));
    } else if (bit(j)) {
        b[0] |= (int16_t)(1 << al);
    }
    return true;
}

/* progressive, an AC pass's first bits of coefficients ss..se */
static bool block_ac_first(struct jpeg *j, struct comp *c, int16_t *b, int ss, int se, int al) {
    if (j->eobrun) { j->eobrun--; return true; }
    for (int k = ss; k <= se;) {
        int rs = decode(j, c->ha);
        if (rs < 0) return fail(j, "corrupt data");
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r < 15) {   /* this and the next eobrun - 1 blocks end here */
                j->eobrun = (1 << r) - 1 + bits(j, r);
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return fail(j, "corrupt data");
        b[dezigzag[k++]] = (int16_t)(extend(j, s) * (1 << al));
    }
    return true;
}

/* progressive, an AC refinement pass: one more bit of each coefficient that's
 * already nonzero, and new coefficients of +-1 (at this bit) in between */
static bool block_ac_refine(struct jpeg *j, struct comp *c, int16_t *b, int ss, int se, int al) {
    int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se;) {
            int rs = decode(j, c->ha);
            if (rs < 0) return fail(j, "corrupt data");
            int r = rs >> 4, s = rs & 15, value = 0;
            if (s == 0) {
                if (r < 15) {
                    j->eobrun = (1 << r) + bits(j, r);
                    break;   /* the rest of the band: refinement bits only */
                }
                /* r == 15: skip 16 zero coefficients */
            } else {
                if (s != 1) return fail(j, "corrupt data");
                value = bit(j) ? p1 : m1;
            }
            for (; k <= se; k++) {
                int16_t *p = &b[dezigzag[k]];
                if (*p != 0) {
                    if (bit(j) && (*p & p1) == 0) *p = (int16_t)(*p >= 0 ? *p + p1 : *p + m1);
                } else {
                    if (r == 0) {
                        if (value) *p = (int16_t)value;
                        k++;
                        break;
                    }
                    r--;
                }
            }
        }
    }
    if (j->eobrun > 0) {   /* inside an end-of-band run: refine what's there */
        for (; k <= se; k++) {
            int16_t *p = &b[dezigzag[k]];
            if (*p != 0 && bit(j) && (*p & p1) == 0) *p = (int16_t)(*p >= 0 ? *p + p1 : *p + m1);
        }
        j->eobrun--;
    }
    return true;
}

/* After a restart interval: realign to a byte, step over the RSTn marker, and
 * start the DC predictions afresh. */
static void restart(struct jpeg *j) {
    j->buf = 0;
    j->nbits = 0;
    if (j->marker >= 0xD0 && j->marker <= 0xD7) j->pos += 2;
    j->marker = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dc_pred = 0;
}

static bool scan(struct jpeg *j, struct comp **sc, int ns, int ss, int se, int ah, int al) {
    j->buf = 0;
    j->nbits = 0;
    j->marker = 0;
    j->eobrun = 0;
    for (int i = 0; i < ns; i++) sc[i]->dc_pred = 0;
    int todo = j->restart;
    if (ns == 1) {
        /* one component on its own: its blocks in order, only those covering it */
        struct comp *c = sc[0];
        int bx = (c->w + 7) / 8, by = (c->h_px + 7) / 8;
        for (int y = 0; y < by; y++)
            for (int x = 0; x < bx; x++) {
                int16_t *b = c->coef + ((size_t)y * c->bw + x) * 64;
                bool ok = !j->progressive ? block_baseline(j, c, b)
                        : ss == 0          ? block_dc(j, c, b, ah, al)
                        : ah == 0          ? block_ac_first(j, c, b, ss, se, al)
                                           : block_ac_refine(j, c, b, ss, se, al);
                if (!ok) return false;
                if (j->restart && --todo == 0) { restart(j); todo = j->restart; }
            }
        return true;
    }
    /* interleaved: each MCU holds h x v blocks of every component in turn */
    for (int my = 0; my < j->mcuy; my++)
        for (int mx = 0; mx < j->mcux; mx++) {
            for (int i = 0; i < ns; i++) {
                struct comp *c = sc[i];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int x = mx * c->h + h, y = my * c->v + v;
                        int16_t *b = c->coef + ((size_t)y * c->bw + x) * 64;
                        bool ok = j->progressive ? block_dc(j, c, b, ah, al) : block_baseline(j, c, b);
                        if (!ok) return false;
                    }
            }
            if (j->restart && --todo == 0) { restart(j); todo = j->restart; }
        }
    return true;
}

/* ------------------------------------------------------------ to pixels */

static float idct_cos[8][8];   /* [x][u]: C(u)/2 * cos((2x + 1) u pi / 16) */

static void idct_init(void) {
    if (idct_cos[0][0] != 0) return;
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            idct_cos[x][u] = (u == 0 ? (float)M_SQRT1_2 : 1.0f) / 2 * cosf((2 * x + 1) * u * (float)M_PI / 16);
}

/* dequantize and inverse-DCT one block into 8x8 samples */
static void idct_block(const int16_t *b, const uint16_t *q, uint8_t *out, int stride) {
    float in[64], tmp[64];
    bool row_used[8] = { false };
    for (int i = 0; i < 64; i++) {
        in[i] = (float)(b[i] * q[i]);
        if (b[i]) row_used[i / 8] = true;
    }
    /* rows (each a horizontal frequency sum), skipping the many all-zero ones */
    for (int v = 0; v < 8; v++) {
        const float *r = in + v * 8;
        for (int x = 0; x < 8; x++) {
            float s = 0;
            if (row_used[v]) for (int u = 0; u < 8; u++) s += r[u] * idct_cos[x][u];
            tmp[v * 8 + x] = s;
        }
    }
    /* then columns */
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) if (row_used[v]) s += tmp[v * 8 + x] * idct_cos[y][v];
            int p = (int)lroundf(s + 128);
            out[y * stride + x] = (uint8_t)CLAMP(p, 0, 255);
        }
}

/* A component's sample at an output pixel, bilinear between sample centers
 * when it's stored at lower resolution (what libjpeg calls fancy upsampling) */
static float sample_at(const struct jpeg *j, const struct comp *c, int x, int y) {
    int stride = c->bw * 8;
    if (c->h == j->hmax && c->v == j->vmax) return c->px[(size_t)y * stride + x];
    float fx = (x + 0.5f) * c->h / j->hmax - 0.5f, fy = (y + 0.5f) * c->v / j->vmax - 0.5f;
    fx = CLAMP(fx, 0.0f, (float)(c->w - 1));
    fy = CLAMP(fy, 0.0f, (float)(c->h_px - 1));
    int x0 = (int)fx, y0 = (int)fy, x1 = MIN(x0 + 1, c->w - 1), y1 = MIN(y0 + 1, c->h_px - 1);
    float ax = fx - x0, ay = fy - y0;
    const uint8_t *r0 = c->px + (size_t)y0 * stride, *r1 = c->px + (size_t)y1 * stride;
    return (r0[x0] * (1 - ax) + r0[x1] * ax) * (1 - ay) + (r1[x0] * (1 - ax) + r1[x1] * ax) * ay;
}

static uint8_t clamp8(float v) { return (uint8_t)CLAMP((int)lroundf(v), 0, 255); }

/* the EXIF orientation tag (APP1 "Exif"): how to turn the pixels for display */
static void read_exif(struct jpeg *j, const uint8_t *p, size_t n) {
    if (n < 14 || memcmp(p, "Exif\0\0", 6)) return;
    const uint8_t *t = p + 6;
    size_t tn = n - 6;
    bool le = t[0] == 'I';
    #define RD16(o) (le ? t[o] | t[(o) + 1] << 8 : t[o] << 8 | t[(o) + 1])
    #define RD32(o) (le ? (uint32_t)(t[o] | t[(o) + 1] << 8 | t[(o) + 2] << 16 | (uint32_t)t[(o) + 3] << 24) \
                        : (uint32_t)((uint32_t)t[o] << 24 | t[(o) + 1] << 16 | t[(o) + 2] << 8 | t[(o) + 3]))
    uint32_t ifd = RD32(4);
    if (ifd + 2 > tn) return;
    int count = RD16(ifd);
    for (int i = 0; i < count; i++) {
        size_t e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > tn) return;
        if (RD16(e) == 0x0112) {
            int o = RD16(e + 8);
            if (o >= 1 && o <= 8) j->orientation = o;
            return;
        }
    }
    #undef RD16
    #undef RD32
}

static struct image *finish(struct jpeg *j) {
    idct_init();
    for (int i = 0; i < j->nc; i++) {
        struct comp *c = &j->c[i];
        int stride = c->bw * 8;
        c->px = malloc((size_t)stride * c->bh * 8);
        if (!c->px) return NULL;
        for (int y = 0; y < c->bh; y++)
            for (int x = 0; x < c->bw; x++)
                idct_block(c->coef + ((size_t)y * c->bw + x) * 64, j->q[c->tq],
                           c->px + (size_t)y * 8 * stride + x * 8, stride);
    }
    struct image *img = malloc(sizeof *img);
    uint32_t *px = img ? malloc((size_t)j->w * j->h * 4) : NULL;
    if (!px) { free(img); return NULL; }
    bool ycc = j->nc == 3 && !j->adobe_rgb;
    for (int y = 0; y < j->h; y++)
        for (int x = 0; x < j->w; x++) {
            uint8_t r, g, b;
            if (j->nc == 1) {
                r = g = b = j->c[0].px[(size_t)y * j->c[0].bw * 8 + x];
            } else {
                float c0 = sample_at(j, &j->c[0], x, y), c1 = sample_at(j, &j->c[1], x, y),
                      c2 = sample_at(j, &j->c[2], x, y);
                if (ycc) {   /* JFIF's YCbCr */
                    r = clamp8(c0 + 1.402f * (c2 - 128));
                    g = clamp8(c0 - 0.344136f * (c1 - 128) - 0.714136f * (c2 - 128));
                    b = clamp8(c0 + 1.772f * (c1 - 128));
                } else {
                    r = clamp8(c0); g = clamp8(c1); b = clamp8(c2);
                }
            }
            px[(size_t)y * j->w + x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }

    /* turn it as EXIF says (the pixels are stored as the camera's sensor saw them) */
    int o = j->orientation, w = j->w, h = j->h;
    if (o > 1) {
        bool swap = o >= 5;
        int ow = swap ? h : w, oh = swap ? w : h;
        uint32_t *t = malloc((size_t)ow * oh * 4);
        if (t) {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++) {
                    int sx, sy;
                    switch (o) {
                    case 2: sx = w - 1 - x; sy = y; break;
                    case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                    case 4: sx = x; sy = h - 1 - y; break;
                    case 5: sx = y; sy = x; break;
                    case 6: sx = y; sy = h - 1 - x; break;
                    case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                    default: sx = w - 1 - y; sy = x; break;   /* 8 */
                    }
                    t[(size_t)y * ow + x] = px[(size_t)sy * w + sx];
                }
            free(px);
            px = t;
            w = ow;
            h = oh;
        }
    }
    *img = (struct image){ w, h, px };
    return img;
}

/* ------------------------------------------------------------ markers */

static bool read_sof(struct jpeg *j, const uint8_t *p, size_t n) {
    if (j->nc) return fail(j, "more than one frame");
    if (n < 6 || p[0] != 8) return fail(j, "only 8-bit JPEGs are supported");
    j->h = u16(p + 1);
    j->w = u16(p + 3);
    j->nc = p[5];
    if (j->w == 0 || j->h == 0) return fail(j, "no image size");
    if ((size_t)j->w * j->h > 64u << 20) return fail(j, "too large (%dx%d)", j->w, j->h);
    if (j->nc != 1 && j->nc != 3) return fail(j, "%d color components (CMYK?) aren't supported", j->nc);
    if (n < 6 + 3 * (size_t)j->nc) return fail(j, "corrupt header");
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->nc; i++) {
        struct comp *c = &j->c[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return fail(j, "corrupt header");
        j->hmax = MAX(j->hmax, c->h);
        j->vmax = MAX(j->vmax, c->v);
    }
    if (j->nc == 1) j->c[0].h = j->c[0].v = j->hmax = j->vmax = 1;   /* one component: no MCUs to speak of */
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->nc; i++) {
        struct comp *c = &j->c[i];
        c->w = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->h_px = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->coef = calloc((size_t)c->bw * c->bh, 64 * sizeof *c->coef);
        if (!c->coef) return fail(j, "out of memory");
    }
    return true;
}

static bool read_sos(struct jpeg *j, const uint8_t *p, size_t n) {
    if (!j->nc) return fail(j, "scan before frame header");
    int ns = n ? p[0] : 0;
    if (ns < 1 || ns > j->nc || n < 4 + 2 * (size_t)ns) return fail(j, "corrupt scan header");
    struct comp *sc[3];
    for (int i = 0; i < ns; i++) {
        int id = p[1 + i * 2], t = p[2 + i * 2];
        sc[i] = NULL;
        for (int k = 0; k < j->nc; k++) if (j->c[k].id == id) sc[i] = &j->c[k];
        if (!sc[i]) return fail(j, "scan of an unknown component");
        sc[i]->hd = &j->dc[(t >> 4) & 3];
        sc[i]->ha = &j->ac[t & 3];
    }
    const uint8_t *s = p + 1 + 2 * ns;
    int ss = s[0], se = s[1], ah = s[2] >> 4, al = s[2] & 15;
    if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
    if (ss > 63 || se > 63 || ss > se || (ss == 0 && se != 0 && j->progressive) || (ss > 0 && ns != 1))
        return fail(j, "corrupt scan header");
    for (int i = 0; i < ns; i++) {
        bool need_dc = ss == 0 && ah == 0, need_ac = se > 0;
        if ((need_dc && !sc[i]->hd->present) || (need_ac && !sc[i]->ha->present))
            return fail(j, "missing Huffman table");
    }
    if (!scan(j, sc, ns, ss, se, ah, al)) return false;
    /* carry on from the marker that ended the scan */
    if (!j->marker) {
        while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 &&
                                         (j->d[j->pos + 1] < 0xD0 || j->d[j->pos + 1] > 0xD7)))
            j->pos++;
    }
    return true;
}

struct image *jpeg_decode(const uint8_t *d, size_t len, char *err, size_t errlen) {
    struct jpeg *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->d = d;
    j->len = len;
    j->err = err;
    j->errlen = errlen;
    err[0] = '\0';
    j->pos = 2;   /* after SOI */
    struct image *img = NULL;
    /* read markers up to the end of the image: baseline JPEGs usually have one
     * scan, but may send each component in its own */
    while (j->pos + 4 <= len && !err[0]) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }   /* tolerate junk between markers */
        int m = d[j->pos + 1];
        if (m == 0xFF) { j->pos++; continue; }            /* fill bytes */
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { j->pos += 2; continue; }
        if (m == 0xD9) break;                              /* end of image */
        size_t seglen = (size_t)u16(d + j->pos + 2);
        if (seglen < 2 || j->pos + 2 + seglen > len) { fail(j, "truncated"); break; }
        const uint8_t *p = d + j->pos + 4;
        size_t n = seglen - 2;
        j->pos += 2 + seglen;
        switch (m) {
        case 0xC0: case 0xC1: read_sof(j, p, n); break;                    /* baseline, extended */
        case 0xC2: j->progressive = true; read_sof(j, p, n); break;       /* progressive */
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            fail(j, "this kind of JPEG (lossless, hierarchical or arithmetic-coded) isn't supported");
            break;
        case 0xC4:   /* Huffman tables */
            for (size_t o = 0; o < n && !err[0];) {
                if (o + 17 > n) { fail(j, "corrupt Huffman table"); break; }
                int tc = p[o] >> 4, th = p[o] & 3, total = 0;
                for (int i = 0; i < 16; i++) total += p[o + 1 + i];
                if (total > 256 || o + 17 + total > n || tc > 1) { fail(j, "corrupt Huffman table"); break; }
                if (!build_huff(tc ? &j->ac[th] : &j->dc[th], p + o + 1, p + o + 17, total))
                    fail(j, "corrupt Huffman table");
                o += 17 + (size_t)total;
            }
            break;
        case 0xDB:   /* quantization tables */
            for (size_t o = 0; o < n && !err[0];) {
                int pq = p[o] >> 4, tq = p[o] & 3;
                size_t sz = pq ? 128 : 64;
                if (o + 1 + sz > n) { fail(j, "corrupt quantization table"); break; }
                for (int k = 0; k < 64; k++)
                    j->q[tq][dezigzag[k]] = pq ? (uint16_t)u16(p + o + 1 + k * 2) : p[o + 1 + k];
                o += 1 + sz;
            }
            break;
        case 0xDD: if (n >= 2) j->restart = u16(p); break;   /* restart interval */
        case 0xDA: read_sos(j, p, n); break;   /* a scan: its coded data follows the header */
        case 0xE1: read_exif(j, p, n); break;
        case 0xEE:   /* Adobe: says whether 3 components are YCbCr or plain RGB */
            if (n >= 12 && !memcmp(p, "Adobe", 5)) { j->adobe = true; j->adobe_rgb = p[11] == 0; }
            break;
        default: break;   /* other application data and comments */
        }
    }
    if (!err[0] && !j->nc) fail(j, "no image in it");
    if (!err[0]) {
        /* a 3-component JPEG without JFIF or Adobe markers is YCbCr, unless its
         * component ids spell out R, G, B */
        if (j->nc == 3 && !j->adobe && j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B') j->adobe_rgb = true;
        img = finish(j);
        if (!img) fail(j, "out of memory");
    }
    for (int i = 0; i < 3; i++) { free(j->c[i].coef); free(j->c[i].px); }
    free(j);
    return img;
}
