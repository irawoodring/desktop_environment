/* css.h — styling everything the server draws with CSS.
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
 * Two interchangeable engines implement this interface (pick one at build time):
 *
 *   css_libcss.c  NetSurf's libcss: full CSS 2.1 parsing, the complete CSS 3 selector
 *                 set (:not(), :nth-child(), ...), the cascade and inheritance.
 *   css.c         a small built-in engine with no dependencies.
 *
 * libcss doesn't know border-radius, box-shadow, gradients or var(). css.c rewrites
 * those before the sheet reaches libcss (see css_rewrite_for_libcss), so themes are
 * the same file whichever engine is in use. */
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
float style_radius(const struct style *s, int w, int h);       /* resolves percentages */
/* Parse one declaration with the built-in value parser and apply it, e.g. ("box-shadow", "0 4px 8px black"). */
bool css_apply_property(struct style *s, const char *name, const char *value);
/* Turn a pane theme into plain CSS libcss can parse. Returns a malloc'd string. */
char *css_rewrite_for_libcss(const char *text, const char *file);
