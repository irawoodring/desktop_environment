/* render.c — software drawing into XRGB8888 memory.
 *
 * Alpha compositing ("over"): out = src + dst * (1 - src_alpha), with src premultiplied
 * (its color channels already multiplied by its alpha). Wayland clients hand us
 * premultiplied ARGB, so their pixels drop straight into this formula.
 *
 * Rounded corners and soft shadows use signed distance: for each pixel, how far is its
 * center from the shape's edge? Inside is negative. Converting distance into coverage
 * gives anti-aliased edges for free. */
#include "render.h"

#include <math.h>
#include <string.h>

static inline uint32_t mul255(uint32_t a, uint32_t b) {   /* a*b/255, rounded */
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/* blend a premultiplied ARGB pixel over an opaque XRGB pixel */
static inline uint32_t over(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 255) return src & 0xffffff;
    if (a == 0 && (src & 0xffffff) == 0) return dst;
    uint32_t ia = 255 - a;
    uint32_t r = ((src >> 16) & 0xff) + mul255((dst >> 16) & 0xff, ia);
    uint32_t g = ((src >> 8) & 0xff) + mul255((dst >> 8) & 0xff, ia);
    uint32_t b = (src & 0xff) + mul255(dst & 0xff, ia);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

/* straight color + extra coverage (0..1) -> premultiplied ARGB */
static inline uint32_t premul(struct color c, float coverage) {
    uint32_t a = (uint32_t)(c.a * coverage + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (mul255(c.r, a) << 16) | (mul255(c.g, a) << 8) | mul255(c.b, a);
}

static inline uint32_t scale_premul(uint32_t px, uint32_t k) {   /* multiply all channels by k/255 */
    if (k >= 255) return px;
    return (mul255(px >> 24, k) << 24) | (mul255((px >> 16) & 0xff, k) << 16) |
           (mul255((px >> 8) & 0xff, k) << 8) | mul255(px & 0xff, k);
}

static bool clip_rect(const struct canvas *c, struct rect r, int *x0, int *y0, int *x1, int *y1) {
    *x0 = MAX(r.x, c->clip.x);
    *y0 = MAX(r.y, c->clip.y);
    *x1 = MIN(r.x + r.w, c->clip.x + c->clip.w);
    *y1 = MIN(r.y + r.h, c->clip.y + c->clip.h);
    return *x0 < *x1 && *y0 < *y1;
}

/* ------------------------------------------------------------------ geometry */

/* Coverage of pixel (px, py) by a rounded rect [x0,x1)x[y0,y1) with per-corner radii.
 * Edges are integer-aligned, so only the corners need real math. */
static float rrect_coverage(float x0, float y0, float x1, float y1, struct radii r, float px, float py) {
    float cx = px + 0.5f, cy = py + 0.5f;
    if (cx < x0 || cy < y0 || cx > x1 || cy > y1) return 0;
    float rad, ox, oy;
    if (r.tl > 0 && cx < x0 + r.tl && cy < y0 + r.tl) { rad = r.tl; ox = x0 + rad; oy = y0 + rad; }
    else if (r.tr > 0 && cx > x1 - r.tr && cy < y0 + r.tr) { rad = r.tr; ox = x1 - rad; oy = y0 + rad; }
    else if (r.br > 0 && cx > x1 - r.br && cy > y1 - r.br) { rad = r.br; ox = x1 - rad; oy = y1 - rad; }
    else if (r.bl > 0 && cx < x0 + r.bl && cy > y1 - r.bl) { rad = r.bl; ox = x0 + rad; oy = y1 - rad; }
    else return 1;
    float d = hypotf(cx - ox, cy - oy) - rad;   /* > 0 means outside the arc */
    return CLAMP(0.5f - d, 0.0f, 1.0f);
}

/* signed distance from (px, py) to a rounded rect with one radius; negative inside */
static float rrect_sdf(float x0, float y0, float x1, float y1, float r, float px, float py) {
    float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2;
    r = fminf(r, fminf(hw, hh));
    float qx = fabsf(px - (x0 + hw)) - hw + r;
    float qy = fabsf(py - (y0 + hh)) - hh + r;
    return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}

static struct radii radii_clamp(struct radii r, int w, int h) {
    float m = fminf(w, h) / 2.0f;
    r.tl = CLAMP(r.tl, 0, m); r.tr = CLAMP(r.tr, 0, m);
    r.br = CLAMP(r.br, 0, m); r.bl = CLAMP(r.bl, 0, m);
    return r;
}

/* -------------------------------------------------------------------- paints */

struct paint_ctx {
    const struct paint *p;
    float cx, cy, dx, dy, len;     /* gradient line through the box center */
};

static void paint_setup(struct paint_ctx *pc, const struct paint *p, struct rect box) {
    pc->p = p;
    if (p->kind != PAINT_GRADIENT) return;
    /* CSS: the gradient line passes through the center at `angle`, and is just long
     * enough that the corners get the first/last colors. */
    float a = p->angle * (float)M_PI / 180.0f;
    pc->dx = sinf(a);
    pc->dy = -cosf(a);
    pc->len = fabsf(box.w * pc->dx) + fabsf(box.h * pc->dy);
    if (pc->len < 1) pc->len = 1;
    pc->cx = box.x + box.w / 2.0f;
    pc->cy = box.y + box.h / 2.0f;
}

static struct color paint_at(const struct paint_ctx *pc, int x, int y) {
    const struct paint *p = pc->p;
    if (p->kind == PAINT_COLOR) return p->color;
    float t = ((x + 0.5f - pc->cx) * pc->dx + (y + 0.5f - pc->cy) * pc->dy) / pc->len + 0.5f;
    const struct grad_stop *s = p->stops;
    if (t <= s[0].pos) return s[0].c;
    for (int i = 1; i < p->nstops; i++) {
        if (t <= s[i].pos) {
            float span = s[i].pos - s[i - 1].pos;
            float f = span > 0 ? (t - s[i - 1].pos) / span : 1;
            struct color a = s[i - 1].c, b = s[i].c;
            return (struct color){ a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f,
                                   a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f };
        }
    }
    return s[p->nstops - 1].c;
}

/* -------------------------------------------------------------------- shapes */

void draw_box(struct canvas *c, struct rect r, struct radii rad, int bw,
              const struct paint *bg, struct color border, float opacity, const struct rect *hole) {
    int x0, y0, x1, y1;
    if (!clip_rect(c, r, &x0, &y0, &x1, &y1) || opacity <= 0) return;
    rad = radii_clamp(rad, r.w, r.h);
    bw = CLAMP(bw, 0, MIN(r.w, r.h) / 2);
    float ox0 = r.x, oy0 = r.y, ox1 = r.x + r.w, oy1 = r.y + r.h;
    float ix0 = ox0 + bw, iy0 = oy0 + bw, ix1 = ox1 - bw, iy1 = oy1 - bw;
    struct radii irad = { fmaxf(rad.tl - bw, 0), fmaxf(rad.tr - bw, 0),
                          fmaxf(rad.br - bw, 0), fmaxf(rad.bl - bw, 0) };
    bool has_bg = bg && bg->kind != PAINT_NONE;
    bool has_border = bw > 0 && border.a > 0;
    struct paint_ctx pc = { 0 };
    if (has_bg) paint_setup(&pc, bg, r);

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        bool in_hole_rows = hole && y >= hole->y && y < hole->y + hole->h;
        for (int x = x0; x < x1; x++) {
            if (in_hole_rows && x >= hole->x && x < hole->x + hole->w) {
                x = hole->x + hole->w - 1;
                continue;
            }
            float outer = rrect_coverage(ox0, oy0, ox1, oy1, rad, x, y);
            if (outer <= 0) continue;
            float inner = bw > 0 ? rrect_coverage(ix0, iy0, ix1, iy1, irad, x, y) : outer;
            if (has_border && outer > inner)
                row[x] = over(row[x], premul(border, (outer - inner) * opacity));
            if (has_bg && inner > 0)
                row[x] = over(row[x], premul(paint_at(&pc, x, y), inner * opacity));
        }
    }
}

static float smoothstep(float e0, float e1, float x) {
    float t = CLAMP((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

void draw_shadow(struct canvas *c, struct rect r, float radius, int blur, struct color col,
                 float opacity, const struct rect *hole) {
    if (col.a == 0 || opacity <= 0) return;
    struct rect area = { r.x - blur, r.y - blur, r.w + 2 * blur, r.h + 2 * blur };
    int x0, y0, x1, y1;
    if (!clip_rect(c, area, &x0, &y0, &x1, &y1)) return;
    float half = blur > 0 ? blur / 2.0f : 0.5f;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        bool in_hole_rows = hole && y >= hole->y && y < hole->y + hole->h;
        for (int x = x0; x < x1; x++) {
            if (in_hole_rows && x >= hole->x && x < hole->x + hole->w) {
                x = hole->x + hole->w - 1;
                continue;
            }
            float d = rrect_sdf(r.x, r.y, r.x + r.w, r.y + r.h, radius, x + 0.5f, y + 0.5f);
            float a = 1 - smoothstep(-half, half, d);
            if (a > 0.002f) row[x] = over(row[x], premul(col, a * opacity));
        }
    }
}

void draw_image(struct canvas *c, const uint32_t *src, int sw, int sh, int scale, int dx, int dy,
                bool opaque, float opacity, const struct rect *clip, struct radii clip_rad) {
    if (scale < 1) scale = 1;
    struct rect r = { dx, dy, sw / scale, sh / scale };
    int x0, y0, x1, y1;
    if (!clip_rect(c, r, &x0, &y0, &x1, &y1) || opacity <= 0) return;
    float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    float corner_top = 0, corner_bot = 0;
    if (clip) {
        x0 = MAX(x0, clip->x); y0 = MAX(y0, clip->y);
        x1 = MIN(x1, clip->x + clip->w); y1 = MIN(y1, clip->y + clip->h);
        clip_rad = radii_clamp(clip_rad, clip->w, clip->h);
        cx0 = clip->x; cy0 = clip->y; cx1 = clip->x + clip->w; cy1 = clip->y + clip->h;
        corner_top = cy0 + fmaxf(clip_rad.tl, clip_rad.tr);
        corner_bot = cy1 - fmaxf(clip_rad.bl, clip_rad.br);
    }
    uint32_t k = (uint32_t)(opacity * 255 + 0.5f);

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        const uint32_t *srow = src + (size_t)(y - dy) * scale * sw;
        bool corner_row = clip && (y < corner_top || y + 1 > corner_bot);
        if (opaque && k >= 255 && !corner_row && scale == 1) {   /* fast path: straight copy */
            for (int x = x0; x < x1; x++) row[x] = srow[x - dx] & 0xffffff;
            continue;
        }
        for (int x = x0; x < x1; x++) {
            uint32_t s = srow[(x - dx) * scale];
            if (opaque) s |= 0xff000000;
            uint32_t kk = k;
            if (corner_row) {
                float cov = rrect_coverage(cx0, cy0, cx1, cy1, clip_rad, x, y);
                if (cov <= 0) continue;
                kk = (uint32_t)(kk * cov);
            }
            row[x] = over(row[x], scale_premul(s, kk));
        }
    }
}

/* ---------------------------------------------------------------------- text */

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

int text_width(const struct font *f, int scale, const char *s) {
    if (!f) return 0;
    int n = 0;
    while (*s) { utf8_next(&s); n++; }
    return n * f->width * scale;
}

static void draw_glyph(struct canvas *c, const struct font *f, int scale, int x, int y, int g,
                       uint32_t px) {
    const uint8_t *bits = f->glyphs + (size_t)g * f->row_bytes * f->height;
    for (int gy = 0; gy < f->height; gy++) {
        for (int gx = 0; gx < f->width; gx++) {
            if (!(bits[gy * f->row_bytes + gx / 8] & (0x80 >> (gx % 8)))) continue;
            for (int sy = 0; sy < scale; sy++) {
                int yy = y + gy * scale + sy;
                if (yy < c->clip.y || yy >= c->clip.y + c->clip.h) continue;
                uint32_t *row = c->px + (size_t)yy * c->stride;
                for (int sx = 0; sx < scale; sx++) {
                    int xx = x + gx * scale + sx;
                    if (xx >= c->clip.x && xx < c->clip.x + c->clip.w) row[xx] = over(row[xx], px);
                }
            }
        }
    }
}

void draw_text(struct canvas *c, const struct font *f, int scale, int x, int y, const char *s,
               struct color col, float opacity, int max_w) {
    if (!f || col.a == 0) return;
    uint32_t px = premul(col, opacity);
    int adv = f->width * scale;
    int total = text_width(f, scale, s);
    int ell = font_glyph(f, 0x2026);   /* "…" */
    int ell_w = ell >= 0 ? adv : 3 * adv;
    bool truncate = total > max_w;
    int limit = truncate ? max_w - ell_w : max_w;
    int cx = x;
    while (*s) {
        if (cx + adv - x > limit) break;
        int g = font_glyph(f, utf8_next(&s));
        if (g < 0) g = font_glyph(f, '?');
        if (g >= 0) draw_glyph(c, f, scale, cx, y, g, px);
        cx += adv;
    }
    if (truncate) {
        if (ell >= 0) draw_glyph(c, f, scale, cx, y, ell, px);
        else for (int i = 0, d = font_glyph(f, '.'); i < 3 && d >= 0; i++)
            draw_glyph(c, f, scale, cx + i * adv, y, d, px);
    }
}

/* --------------------------------------------------------------------- icons */

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = CLAMP((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return hypotf(wx - t * vx, wy - t * vy);
}

void draw_icon(struct canvas *c, struct rect r, enum icon icon, struct color col, float opacity) {
    if (col.a == 0) return;
    int x0, y0, x1, y1;
    if (!clip_rect(c, r, &x0, &y0, &x1, &y1)) return;
    float s = fminf(r.w, r.h);
    float m = s * 0.3f;   /* inset */
    float l = r.x + (r.w - s) / 2 + m, t = r.y + (r.h - s) / 2 + m;
    float rr = l + s - 2 * m, b = t + s - 2 * m;
    float th = fmaxf(s / 12.0f, 0.75f);   /* half stroke width */
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, d;
            switch (icon) {
            case ICON_CLOSE:
                d = fminf(seg_dist(px, py, l, t, rr, b), seg_dist(px, py, rr, t, l, b));
                break;
            case ICON_MINIMIZE: {
                float my = (t + b) / 2;
                d = seg_dist(px, py, l, my, rr, my);
                break;
            }
            case ICON_MAXIMIZE:
                d = fabsf(rrect_sdf(l, t, rr, b, 0, px, py));
                break;
            default: {   /* ICON_RESTORE: two overlapping squares */
                float o = (rr - l) * 0.3f;
                float d1 = fabsf(rrect_sdf(l, t + o, rr - o, b, 0, px, py));
                float d2 = fabsf(rrect_sdf(l + o, t, rr, b - o, 0, px, py));
                bool hidden = px > l && px < rr - o && py > t + o && py < b;   /* behind the front one */
                d = hidden ? d1 : fminf(d1, d2);
                break;
            }
            }
            float cov = CLAMP(th + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0) row[x] = over(row[x], premul(col, cov * opacity));
        }
    }
}

/* The classic arrow. X = outline, . = fill. */
static const char *const arrow[] = {
    "X",           "XX",          "X.X",         "X..X",        "X...X",
    "X....X",      "X.....X",     "X......X",    "X.......X",   "X........X",
    "X.........X", "X......XXXXX", "X...X..X",   "X..XX..X",    "X.X  X..X",
    "XX   X..X",   "X     X..X",  "      X..X",  "       XX",
};

void draw_cursor(struct canvas *c, int x, int y, int scale, struct color fill, struct color outline) {
    uint32_t pf = premul(fill, 1), po = premul(outline, 1);
    for (int row = 0; row < (int)ARRAY_LEN(arrow); row++) {
        for (int col = 0; arrow[row][col]; col++) {
            char ch = arrow[row][col];
            if (ch == ' ') continue;
            uint32_t px = ch == 'X' ? po : pf;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int xx = x + col * scale + sx, yy = y + row * scale + sy;
                    if (rect_contains(c->clip, xx, yy))
                        c->px[(size_t)yy * c->stride + xx] = over(c->px[(size_t)yy * c->stride + xx], px);
                }
        }
    }
}
