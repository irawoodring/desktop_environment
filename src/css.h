/* css.h — styling everything the server draws with CSS.
 *
 * The server builds a tiny element tree for each thing it draws and asks for the
 * computed style of each element, exactly like a browser does for the DOM:
 *
 *   desktop
 *   window.<class>  [app-id="..."] [title="..."]   :focused :maximized :fullscreen :hover
 *     titlebar
 *       title
 *       button.close / button.maximize / button.minimize   :hover :active
 *   popup
 *   cursor
 *   menu                                (the window menu)
 *     menuitem.action / menuitem.class(.checked)   :hover
 *     separator
 *     heading
 *
 * Two interchangeable engines implement this interface (pick one at build time):
 *
 *   css_libcss.c  NetSurf's libcss: full CSS 2.1 parsing, the complete CSS 3 selector
 *                 set (:not(), :nth-child(), ...), the cascade and inheritance.
 *   css.c         a small built-in engine with no dependencies.
 *
 * libcss doesn't know several properties pane uses (border-radius, box-shadow,
 * transitions, gradients, var(), the layout properties). css.c rewrites the theme before
 * it reaches libcss (see css_rewrite_for_libcss), so themes are the same file whichever
 * engine is in use. */
#pragma once
#include "render.h"

enum {
    ST_FOCUSED = 1 << 0, ST_HOVER = 1 << 1, ST_ACTIVE = 1 << 2,
    ST_MAXIMIZED = 1 << 3, ST_FULLSCREEN = 1 << 4, ST_ROOT = 1 << 5,
};

struct css_elem {
    const char *name;
    const char *cls;               /* space-separated class list, or NULL */
    unsigned states;
    const char *app_id, *title;
    struct css_elem *parent;
    void *engine[4];               /* engine-private; zero-initialize, then css_elem_release() */
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum { LAYOUT_FLOATING, LAYOUT_TILE, LAYOUT_COLUMNS, LAYOUT_ROWS, LAYOUT_GRID, LAYOUT_MONOCLE };

/* one entry of `transition`: animate `prop` over duration_ms, after delay_ms, with an easing curve */
struct transition { char prop[24]; int duration_ms, delay_ms; float bezier[4]; };

struct style {
    /* backgrounds: a color, with a gradient or image layer on top */
    struct paint background;           /* background-color (PAINT_COLOR or PAINT_NONE) */
    struct paint background_image;     /* a gradient, an image, or PAINT_NONE */
    int bg_size_mode;                  /* SIZE_*: background-size */
    struct length bg_size_w, bg_size_h;
    struct length bg_pos_x, bg_pos_y;  /* background-position */
    bool bg_repeat_x, bg_repeat_y;     /* background-repeat */
    struct color color;
    /* borders: widths and colors per side; a side is only drawn if its style isn't none */
    struct border border;
    bool border_color_set[4];          /* else it's currentColor */
    struct length radius[4];           /* corners: top-left, top-right, bottom-right, bottom-left */
    int padding[4], margin[4];         /* top, right, bottom, left */
    int width, height;                 /* -1 = auto */
    bool shadow;
    int shadow_x, shadow_y, shadow_blur, shadow_spread;
    struct color shadow_color;
    float opacity;
    char font_family[128];             /* a CSS list: "DejaVu Sans, sans-serif" */
    int font_size, font_weight;
    bool font_italic;
    int text_align;
    bool hidden;                       /* display: none */
    bool float_left;                   /* float: left (buttons) */
    int ntransitions;
    struct transition transitions[6];
    /* layout (pane-specific properties) */
    int side;                          /* titlebar: which edge it sits on (TOP, RIGHT, BOTTOM, LEFT) */
    int window_layout;                 /* desktop: LAYOUT_* */
    int gap;                           /* desktop: space between tiled windows */
    float tile_ratio;                  /* desktop: master area share in the "tile" layout */
    bool tile;                         /* window: take part in tiling layouts */
};

/* ---- the engine interface (css.c or css_libcss.c) ---- */

/* Load a stylesheet. Returns 0 on success; on failure the previous sheet stays active. */
int css_load_file(const char *path);
void css_load_string(const char *text, const char *name);
/* Compute an element's style. Compute parents before children (inheritance needs them),
 * and call css_elem_release() on every element once you're done with the tree. */
void css_compute(struct css_elem *e, const struct style *parent, struct style *out);
void css_elem_release(struct css_elem *e);
const char *css_engine_name(void);

/* ---- shared helpers (css.c) ---- */

void style_init(struct style *s, const struct style *parent);   /* initial + inherited values */
void style_finish(struct style *s);    /* resolve what depends on other properties (currentColor) */
struct radii style_radii(const struct style *s, int w, int h);  /* resolves percentages */
struct border style_border(const struct style *s);              /* only the sides actually drawn */
/* Problems found in the last theme loaded (bad values, unknown properties,
 * properties with no effect where they're used), as "file:line: message".
 * The count includes any beyond those kept. */
void css_messages_reset(void);
int css_message_count(void);
const char *css_message(int i);
/* Draw an element's box as CSS describes it: box-shadow, then background and
 * border, at `opacity` (its own opacity already multiplied in by the caller). */
void style_draw_box(struct canvas *c, struct rect r, struct radii rad, const struct style *s, float opacity);
/* true if any corner has a radius */
bool style_has_radius(const struct style *s);
/* the background as paint layers, bottom first (color, then gradient/image); returns how many */
int style_layers(const struct style *s, struct paint out[2]);
/* where theme-relative paths (url(...)) are resolved from */
void css_set_base_dir(const char *theme_path);
/* Parse one declaration with the built-in value parser and apply it, e.g. ("box-shadow", "0 4px 8px black"). */
bool css_apply_property(struct style *s, const char *name, const char *value);
/* Turn a pane theme into CSS libcss can parse: one main sheet, plus one small sheet per
 * property libcss doesn't know (each carries that property's values in `content`, so
 * libcss can run a separate cascade for it). Free with css_rewritten_free(). */
#define CSS_NEXT 12
extern const char *const css_ext_props[CSS_NEXT];
struct css_rewritten { char *main; char *ext[CSS_NEXT]; };
struct css_rewritten css_rewrite_for_libcss(const char *text, const char *file);
void css_rewritten_free(struct css_rewritten *r);
/* Defaults for things a theme may not style (the window menu); themes override them. */
extern const char css_base_sheet[];
/* The classes the loaded theme uses on windows (window.work ...): i = 0, 1, ... until NULL. */
const char *css_window_class(int i);
void css_window_classes_reset(void);
