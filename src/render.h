/* render.h — a small software rasterizer: everything the server draws itself. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "server.h"

struct color { uint8_t r, g, b, a; };

/* a CSS length that may be a percentage of something */
struct length { float v; bool pct; };

struct grad_stop { struct color c; float pos; };

/* An image, decoded to premultiplied ARGB (image.c). */
struct image { int w, h; uint32_t *px; };

enum { SIZE_AUTO, SIZE_COVER, SIZE_CONTAIN, SIZE_LENGTH };
enum { EXTENT_CLOSEST_SIDE, EXTENT_FARTHEST_SIDE, EXTENT_CLOSEST_CORNER, EXTENT_FARTHEST_CORNER };

/* One background layer: a color, a gradient, or an image. */
struct paint {
    enum { PAINT_NONE, PAINT_COLOR, PAINT_LINEAR, PAINT_RADIAL, PAINT_IMAGE } kind;
    struct color color;
    /* gradients */
    float angle;                   /* linear: degrees, 0 = towards the top, 90 = right */
    bool circle;                   /* radial: circle, else ellipse */
    int extent;                    /* radial: EXTENT_* */
    struct length cx, cy;          /* radial: center ("at ..."), default 50% 50% */
    int nstops;
    struct grad_stop stops[8];
    /* images: background-size / -position / -repeat */
    const struct image *image;
    int size_mode;                 /* SIZE_* */
    struct length size_w, size_h;  /* SIZE_LENGTH; v < 0 means auto */
    struct length pos_x, pos_y;
    bool repeat_x, repeat_y;
};

struct radii { float tl, tr, br, bl; };

/* per-side border, CSS order: top, right, bottom, left */
enum { TOP, RIGHT, BOTTOM, LEFT };
struct border { int w[4]; struct color c[4]; };

/* Pixels are XRGB8888: 0x00RRGGBB. Drawing is clipped to `clip`. */
struct canvas {
    uint32_t *px;
    int w, h, stride;
    struct rect clip;
};

/* A box with rounded corners and a border, filled with background layers painted bottom
 * to top inside the border. Pixels inside `hole` (if any) are skipped — used to avoid
 * painting under a window's opaque contents. */
void draw_box(struct canvas *c, struct rect r, struct radii rad, const struct border *b,
              const struct paint *layers, int nlayers, float opacity, const struct rect *hole);
/* Soft shadow of a rounded rect; `blur` is the CSS blur radius in pixels. */
void draw_shadow(struct canvas *c, struct rect r, struct radii rad, int blur, struct color col,
                 float opacity, const struct rect *hole);
/* Composite premultiplied ARGB pixels at (dx, dy). `scale` divides the source size (buffer_scale).
 * If `clip` is set, the image is clipped to that rounded rect. */
void draw_image(struct canvas *c, const uint32_t *src, int sw, int sh, int scale, int dx, int dy,
                bool opaque, float opacity, const struct rect *clip, struct radii clip_rad);

enum icon { ICON_CLOSE, ICON_MAXIMIZE, ICON_RESTORE, ICON_MINIMIZE, ICON_CHECK };
void draw_icon(struct canvas *c, struct rect r, enum icon icon, struct color col, float opacity);
void draw_cursor(struct canvas *c, int x, int y, int scale, struct color fill, struct color outline);

/* building blocks shared with font.c */
uint32_t color_premul(struct color c, float coverage);
void canvas_blend(struct canvas *c, int x, int y, uint32_t premul_px);

/* ---------------------------------------------------------------- images (image.c) */

/* Load a PNG (cached by path). NULL, with a message on stderr, if it can't be read. */
const struct image *image_load(const char *path);
/* A copy scaled to w x h (cached); smooth in both directions. */
const struct image *image_scaled(const struct image *img, int w, int h);
void image_cache_clear(void);

/* ----------------------------------------------------------------- text (font.c) */

/* Fonts come from two places: Linux console fonts (PSF, bitmap, scaled by whole
 * multiples), and with FreeType, any TrueType/OpenType font on the system.
 * `families` is a CSS font-family list ("DejaVu Sans, sans-serif"). */
struct font;
const struct font *font_get(const char *families, int weight, bool italic, int px_size);
int font_line_height(const struct font *f);
int text_width(const struct font *f, const char *utf8);
/* rotation: 0, 90 (reads top to bottom) or 270 (reads bottom to top). (x, y) is the
 * top-left of the text's box after rotation. Text longer than max_w gets an ellipsis. */
void draw_text(struct canvas *c, const struct font *f, int x, int y, const char *utf8,
               struct color col, float opacity, int max_w, int rotation);
