/* render.h — a small software rasterizer: everything the server draws itself. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "server.h"

struct color { uint8_t r, g, b, a; };

struct grad_stop { struct color c; float pos; };

struct paint {
    enum { PAINT_NONE, PAINT_COLOR, PAINT_GRADIENT } kind;
    struct color color;
    float angle;                   /* degrees, CSS convention: 0 = towards the top, 90 = right */
    int nstops;
    struct grad_stop stops[8];
};

struct radii { float tl, tr, br, bl; };

/* Pixels are XRGB8888: 0x00RRGGBB. Drawing is clipped to `clip`. */
struct canvas {
    uint32_t *px;
    int w, h, stride;
    struct rect clip;
};

struct font;

/* Filled, optionally rounded box with an inside border. Pixels inside `hole` (if any)
 * are skipped — used to avoid painting under a window's opaque contents. */
void draw_box(struct canvas *c, struct rect r, struct radii rad, int border_width,
              const struct paint *bg, struct color border, float opacity, const struct rect *hole);
/* Soft shadow of a rounded rect; `blur` is the CSS blur radius in pixels. */
void draw_shadow(struct canvas *c, struct rect r, float radius, int blur, struct color col,
                 float opacity, const struct rect *hole);
/* Composite premultiplied ARGB pixels at (dx, dy). `scale` divides the source size (buffer_scale).
 * If `clip` is set, the image is clipped to that rounded rect. */
void draw_image(struct canvas *c, const uint32_t *src, int sw, int sh, int scale, int dx, int dy,
                bool opaque, float opacity, const struct rect *clip, struct radii clip_rad);
int text_width(const struct font *f, int scale, const char *utf8);
void draw_text(struct canvas *c, const struct font *f, int scale, int x, int y, const char *utf8,
               struct color col, float opacity, int max_w);

enum icon { ICON_CLOSE, ICON_MAXIMIZE, ICON_RESTORE, ICON_MINIMIZE };
void draw_icon(struct canvas *c, struct rect r, enum icon icon, struct color col, float opacity);
void draw_cursor(struct canvas *c, int x, int y, int scale, struct color fill, struct color outline);

/* ---------------------------------------------------------------------- fonts */
struct font {
    int width, height, row_bytes, nglyphs;
    uint8_t *glyphs;
    int nmap;                      /* unicode -> glyph, sorted by code point */
    uint32_t *map_cp;
    uint16_t *map_glyph;
};
const struct font *font_get(const char *family);
int font_glyph(const struct font *f, uint32_t cp);
