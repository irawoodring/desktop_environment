/* css.h — a small CSS engine for styling everything the server draws.
 *
 * The server builds a tiny element tree for each thing it draws and asks for the
 * computed style of each element, exactly like a browser does for the DOM:
 *
 *   desktop
 *   window   [app-id="..."] [title="..."]   :focused :maximized :fullscreen :hover
 *     titlebar
 *       title
 *       button.close / button.maximize / button.minimize   :hover :active
 *   popup
 *   cursor
 *
 * Supported: type, .class, :pseudo, [attr=val] [attr*=val] [attr^=val] selectors,
 * descendant (space) and child (>) combinators, specificity + source order, inheritance
 * of color, font-family, font-size and text-align, and custom properties (--name: value; used via var(--name)). */
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
    const struct css_elem *parent;
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

struct style {
    struct paint background;
    struct color color;
    int border_width;
    struct color border_color;
    float radius;
    bool radius_pct;
    int padding[4], margin[4];     /* top, right, bottom, left */
    int width, height;             /* -1 = auto */
    bool shadow;
    int shadow_x, shadow_y, shadow_blur, shadow_spread;
    struct color shadow_color;
    float opacity;
    char font_family[64];
    int font_size;
    int text_align;
    bool hidden;                   /* display: none */
    bool float_left;               /* float: left (buttons) */
};

/* Load a stylesheet. Returns 0 on success; on failure the previous sheet stays active. */
int css_load_file(const char *path);
void css_load_string(const char *text, const char *name);
void css_compute(const struct css_elem *e, const struct style *parent, struct style *out);
float style_radius(const struct style *s, int w, int h);   /* resolves percentages */
