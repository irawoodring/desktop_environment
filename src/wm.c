/* wm.c — the window manager: stacking, focus, input routing, move/resize, drawing.
 *
 * This is the part that makes it a *desktop*: the protocol code only knows about
 * surfaces and roles; here we decide where windows go, which one has focus, what a
 * click means, and what the title bars look like (styled entirely by the CSS theme). */
#include "server.h"
#include "backend.h"
#include "css.h"
#include "render.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum part { PART_NONE, PART_CLIENT, PART_TITLEBAR, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_BORDER };
enum { EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8 };
enum { BTN_IDX_CLOSE, BTN_IDX_MAXIMIZE, BTN_IDX_MINIMIZE };

#define RESIZE_MARGIN 6     /* invisible grab area outside the frame */
#define CORNER_SIZE 16

static struct list toplevels;   /* mapped toplevels, bottom to top */
static struct list tile_order;  /* mapped toplevels in tiling order */
static struct list popups;      /* all popups, oldest first */
static struct toplevel *focused;

static double ptr_x, ptr_y;
static struct surface *cursor_surface;
static bool client_cursor;      /* the client under the pointer chose a cursor */

static struct toplevel *hover_win, *press_win;
static enum part hover_part, press_part;

static enum { G_NONE, G_MOVE, G_RESIZE, G_IMPLICIT, G_DRAG } grab;
static struct toplevel *grab_win;
static double grab_px, grab_py;
static int grab_x, grab_y, grab_w, grab_h;
static uint32_t grab_edges;
static bool resize_pending;
static int resize_w, resize_h;
static struct surface *implicit_surface;
static int buttons_down;
static uint32_t last_click_time;
static struct toplevel *last_click_win;

static uint32_t *desktop_cache;
static int desktop_w, desktop_h;
static int cascade;

void wm_damage(void) { srv.dirty = true; }

void wm_init(void) {
    list_init(&toplevels);
    list_init(&tile_order);
    list_init(&popups);
    ptr_x = srv.width / 2.0;
    ptr_y = srv.height / 2.0;
}

static void arrange(void);

void wm_theme_changed(void) {
    free(desktop_cache);
    desktop_cache = NULL;
    arrange();   /* the layout properties may have changed */
    wm_damage();
}

/* ------------------------------------------------------------------- layout */

struct layout {
    bool decorated, has_bar, vertical;   /* vertical: the title bar is on the left or right */
    int side;                            /* which edge the title bar is on */
    struct rect content, frame, titlebar, title_area;
    struct rect inner;                   /* content + title bar: everything inside the border */
    struct rect buttons[3];
    bool shown[3];
    struct style win, bar, title, btn[3];
};

static unsigned window_states(struct toplevel *t) {
    unsigned st = 0;
    if (t == focused) st |= ST_FOCUSED;
    if (t->maximized) st |= ST_MAXIMIZED;
    if (t->fullscreen) st |= ST_FULLSCREEN;
    if (t == hover_win) st |= ST_HOVER;
    return st;
}

static const char *const button_class[3] = { "close", "maximize", "minimize" };
static const enum part button_part[3] = { PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE };

/* Build the window's element tree (window > titlebar > title, buttons), style it, and
 * release it again: like a browser restyling a tiny DOM. */
static void compute_styles(struct toplevel *t, unsigned states, struct layout *L) {
    struct css_elem ew = { .name = "window", .cls = t->classes, .states = states, .app_id = t->app_id,
                           .title = t->title };
    struct css_elem eb = { .name = "titlebar", .app_id = t->app_id, .title = t->title, .parent = &ew };
    struct css_elem et = { .name = "title", .app_id = t->app_id, .title = t->title, .parent = &eb };
    struct css_elem btn[3];
    css_compute(&ew, NULL, &L->win);
    css_compute(&eb, &L->win, &L->bar);
    css_compute(&et, &L->bar, &L->title);
    for (int i = 0; i < 3; i++) {
        unsigned bs = 0;
        if (hover_win == t && hover_part == button_part[i]) bs |= ST_HOVER;
        if (press_win == t && press_part == button_part[i] && hover_part == press_part) bs |= ST_ACTIVE;
        btn[i] = (struct css_elem){ .name = "button", .cls = button_class[i], .states = bs,
                                    .app_id = t->app_id, .title = t->title, .parent = &eb };
        css_compute(&btn[i], &L->bar, &L->btn[i]);
    }
    for (int i = 0; i < 3; i++) css_elem_release(&btn[i]);
    css_elem_release(&et);
    css_elem_release(&eb);
    css_elem_release(&ew);
}

/* style of a top-level element with no children: desktop, popup, cursor */
static void style_of(const char *name, struct style *out) {
    struct css_elem e = { .name = name };
    css_compute(&e, NULL, out);
    css_elem_release(&e);
}

/* the title bar's thickness: its height, or its width when it's on the left or right */
static int bar_thickness(const struct style *bar) {
    if (bar->hidden) return 0;
    bool vertical = bar->side == LEFT || bar->side == RIGHT;
    int v = vertical ? bar->width : bar->height;
    return v >= 0 ? v : 28;
}

/* how far the decorations reach beyond the content on each side (top, right, bottom, left) */
static void extents_of(const struct style *win, const struct style *bar, int ext[4]) {
    struct border b = style_border(win);
    for (int i = 0; i < 4; i++) ext[i] = b.w[i];
    ext[bar->side] += bar_thickness(bar);
}

static void window_extents(struct toplevel *t, unsigned states, int ext[4]) {
    memset(ext, 0, 4 * sizeof *ext);
    if (!t->ssd || t->fullscreen) return;
    struct layout L;
    compute_styles(t, states, &L);
    extents_of(&L.win, &L.bar, ext);
}

/* the screen minus the desktop's padding: where maximized and tiled windows go */
static struct rect work_area(const struct style *desktop) {
    const int *p = desktop->padding;
    return (struct rect){ p[LEFT], p[TOP], MAX(srv.width - p[LEFT] - p[RIGHT], 64),
                          MAX(srv.height - p[TOP] - p[BOTTOM], 64) };
}

static void layout_window(struct toplevel *t, struct layout *L) {
    memset(L, 0, sizeof *L);
    compute_styles(t, window_states(t), L);
    struct rect g = xdg_geometry(t->xdg);
    struct rect c = L->content = (struct rect){ t->x, t->y, g.w, g.h };
    L->decorated = t->ssd && !t->fullscreen;
    if (!L->decorated) {
        L->frame = L->inner = c;
        return;
    }
    int ext[4], th = bar_thickness(&L->bar);
    extents_of(&L->win, &L->bar, ext);
    L->side = L->bar.side;
    L->has_bar = th > 0;
    L->vertical = L->side == LEFT || L->side == RIGHT;
    L->frame = (struct rect){ c.x - ext[LEFT], c.y - ext[TOP], c.w + ext[LEFT] + ext[RIGHT],
                              c.h + ext[TOP] + ext[BOTTOM] };
    switch (L->side) {
    case TOP: L->titlebar = (struct rect){ c.x, c.y - th, c.w, th }; break;
    case BOTTOM: L->titlebar = (struct rect){ c.x, c.y + c.h, c.w, th }; break;
    case LEFT: L->titlebar = (struct rect){ c.x - th, c.y, th, c.h }; break;
    default: L->titlebar = (struct rect){ c.x + c.w, c.y, th, c.h }; break;
    }
    struct rect b = L->titlebar;
    int x0 = MIN(c.x, b.x), y0 = MIN(c.y, b.y);
    L->inner = (struct rect){ x0, y0, MAX(c.x + c.w, b.x + b.w) - x0, MAX(c.y + c.h, b.y + b.h) - y0 };
    if (!L->has_bar) return;

    /* buttons along the bar: "float: right" ones from the far end inward, "float: left"
     * ones from the near end (the top, on a vertical bar). Order from the end: close,
     * maximize, minimize at the far end (Windows/GNOME); close, minimize, maximize at the
     * near end (macOS). */
    const struct style *bs = &L->bar;
    int near = L->vertical ? b.y + bs->padding[TOP] : b.x + bs->padding[LEFT];
    int far = L->vertical ? b.y + b.h - bs->padding[BOTTOM] : b.x + b.w - bs->padding[RIGHT];
    static const int far_order[3] = { BTN_IDX_CLOSE, BTN_IDX_MAXIMIZE, BTN_IDX_MINIMIZE };
    static const int near_order[3] = { BTN_IDX_CLOSE, BTN_IDX_MINIMIZE, BTN_IDX_MAXIMIZE };
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 3; k++) {
            int i = pass == 0 ? far_order[k] : near_order[k];
            const struct style *s = &L->btn[i];
            if (s->hidden || s->float_left != (pass == 1)) continue;
            int w = s->width >= 0 ? s->width : 16, h = s->height >= 0 ? s->height : 16;
            int len = L->vertical ? h : w;   /* along the bar */
            int m0 = L->vertical ? s->margin[TOP] : s->margin[LEFT];
            int m1 = L->vertical ? s->margin[BOTTOM] : s->margin[RIGHT];
            int at;
            if (pass == 0) { far -= m1 + len; at = far; far -= m0; }
            else { near += m0; at = near; near += len + m1; }
            L->buttons[i] = L->vertical
                ? (struct rect){ b.x + (th - w) / 2 + s->margin[LEFT] - s->margin[RIGHT], at, w, h }
                : (struct rect){ at, b.y + (th - h) / 2 + s->margin[TOP] - s->margin[BOTTOM], w, h };
            L->shown[i] = true;
        }
    }
    const int *tp = L->title.padding;
    if (L->vertical) {
        int t0 = near + tp[TOP], t1 = far - tp[BOTTOM];
        L->title_area = (struct rect){ b.x, t0, th, MAX(t1 - t0, 0) };
    } else {
        int t0 = near + tp[LEFT], t1 = far - tp[RIGHT];
        L->title_area = (struct rect){ t0, b.y, MAX(t1 - t0, 0), th };
    }
}

struct rect wm_maximized_content(struct toplevel *t) {
    struct style ds;
    style_of("desktop", &ds);
    struct rect a = work_area(&ds);
    int ext[4];
    window_extents(t, ST_FOCUSED | ST_MAXIMIZED, ext);
    return (struct rect){ a.x + ext[LEFT], a.y + ext[TOP], a.w - ext[LEFT] - ext[RIGHT],
                          a.h - ext[TOP] - ext[BOTTOM] };
}

/* ------------------------------------------------------------------- hit test */

struct hit {
    struct toplevel *t;
    struct popup *p;
    enum part part;
    int edges;
    struct surface *s;
    double sx, sy;
};

static bool surface_origin(struct xdg_surface *x, int *sx, int *sy) {
    int ox = 0, oy = 0;
    if (!x->surface || !xdg_abs_origin(x, &ox, &oy)) return false;
    struct rect g = xdg_geometry(x);
    *sx = ox - g.x;
    *sy = oy - g.y;
    return true;
}

static struct hit hit_test(double x, double y) {
    struct hit h = { 0 };
    struct popup *p;
    list_for_each_reverse(p, &popups, link) {
        int sx, sy;
        if (!p->mapped || !p->xdg || !surface_origin(p->xdg, &sx, &sy)) continue;
        h.s = surface_at(p->xdg->surface, sx, sy, x, y, &h.sx, &h.sy);
        if (h.s) {
            h.p = p;
            h.part = PART_CLIENT;
            return h;
        }
    }
    struct toplevel *t;
    list_for_each_reverse(t, &toplevels, link) {
        if (t->minimized || !t->xdg || !t->xdg->surface) continue;
        int sx, sy;
        if (!surface_origin(t->xdg, &sx, &sy)) continue;
        h.s = surface_at(t->xdg->surface, sx, sy, x, y, &h.sx, &h.sy);
        if (h.s) {
            h.t = t;
            h.part = PART_CLIENT;
            return h;
        }
        struct layout L;
        layout_window(t, &L);
        if (!L.decorated) continue;
        bool fixed = t->maximized || t->tiled;   /* these don't resize by dragging */
        int m = fixed ? 0 : RESIZE_MARGIN;
        struct rect grab_area = { L.frame.x - m, L.frame.y - m, L.frame.w + 2 * m, L.frame.h + 2 * m };
        if (!rect_contains(grab_area, x, y)) continue;
        h.t = t;
        for (int i = 0; i < 3; i++)
            if (L.shown[i] && rect_contains(L.buttons[i], x, y)) { h.part = button_part[i]; return h; }
        if (L.has_bar && rect_contains(L.titlebar, x, y)) { h.part = PART_TITLEBAR; return h; }
        h.part = PART_BORDER;
        if (fixed) return h;
        /* outside the content and title bar: which edges are we near? */
        struct rect in = L.inner;
        if (x < in.x) h.edges |= EDGE_LEFT;
        if (x >= in.x + in.w) h.edges |= EDGE_RIGHT;
        if (y < in.y) h.edges |= EDGE_TOP;
        if (y >= in.y + in.h) h.edges |= EDGE_BOTTOM;
        /* near a corner along an edge: resize both ways */
        if (h.edges & (EDGE_LEFT | EDGE_RIGHT)) {
            if (y < L.frame.y + CORNER_SIZE) h.edges |= EDGE_TOP;
            if (y >= L.frame.y + L.frame.h - CORNER_SIZE) h.edges |= EDGE_BOTTOM;
        }
        if (h.edges & (EDGE_TOP | EDGE_BOTTOM)) {
            if (x < L.frame.x + CORNER_SIZE) h.edges |= EDGE_LEFT;
            if (x >= L.frame.x + L.frame.w - CORNER_SIZE) h.edges |= EDGE_RIGHT;
        }
        return h;
    }
    return (struct hit){ 0 };
}

/* ------------------------------------------------------------------- focus */

static struct popup *top_grab_popup(void) {
    struct popup *p;
    list_for_each_reverse(p, &popups, link)
        if (p->mapped && p->grab && p->xdg && p->xdg->surface) return p;
    return NULL;
}

static void update_keyboard_focus(void) {
    struct popup *p = top_grab_popup();
    if (p) seat_keyboard_focus(p->xdg->surface);
    else if (focused && focused->xdg && focused->xdg->surface && focused->xdg->mapped && !focused->minimized)
        seat_keyboard_focus(focused->xdg->surface);
    else seat_keyboard_focus(NULL);
}

static void raise_window(struct toplevel *t) {
    list_remove(&t->link);
    list_append(&toplevels, &t->link);
}

static void send_state(struct toplevel *t) {
    if (t->xdg && t->xdg->mapped) toplevel_send_configure(t, t->conf_w, t->conf_h);
}

static void focus_window(struct toplevel *t) {
    if (t) raise_window(t);
    if (focused != t) {
        struct toplevel *old = focused;
        focused = t;
        if (old) { old->activated = false; send_state(old); }
        if (t) { t->activated = true; send_state(t); }
    }
    update_keyboard_focus();
    wm_damage();
}

static void focus_topmost(void) {
    struct toplevel *t;
    list_for_each_reverse(t, &toplevels, link)
        if (!t->minimized) { focus_window(t); return; }
    focus_window(NULL);
}

/* ------------------------------------------------------------- window states */

static void maximize(struct toplevel *t, bool on) {
    if (t->maximized == on) return;
    struct rect g = xdg_geometry(t->xdg);
    if (on) {
        if (!t->fullscreen) t->saved = (struct rect){ t->x, t->y, g.w, g.h };
        t->maximized = true;
        struct rect r = wm_maximized_content(t);
        t->x = r.x;
        t->y = r.y;
        toplevel_send_configure(t, r.w, r.h);
    } else {
        t->maximized = false;
        t->x = t->saved.x;
        t->y = t->saved.y;
        toplevel_send_configure(t, t->saved.w, t->saved.h);
    }
    arrange();
    wm_damage();
}

static void fullscreen(struct toplevel *t, bool on) {
    if (t->fullscreen == on) return;
    struct rect g = xdg_geometry(t->xdg);
    if (on) {
        if (!t->maximized) t->saved = (struct rect){ t->x, t->y, g.w, g.h };
        t->fullscreen = true;
        t->x = t->y = 0;
        toplevel_send_configure(t, srv.width, srv.height);
    } else {
        t->fullscreen = false;
        if (t->maximized) {
            struct rect r = wm_maximized_content(t);
            t->x = r.x;
            t->y = r.y;
            toplevel_send_configure(t, r.w, r.h);
        } else {
            t->x = t->saved.x;
            t->y = t->saved.y;
            toplevel_send_configure(t, t->saved.w, t->saved.h);
        }
    }
    arrange();
    wm_damage();
}

static void minimize(struct toplevel *t) {
    t->minimized = true;
    if (focused == t) focus_topmost();
    arrange();
    wm_damage();
}

static bool unmaximize_on_drag;

static void start_move(struct toplevel *t) {
    if (t->tiled) {   /* lift it out of the tiling; the others close the gap */
        t->lifted = true;
        arrange();
    }
    seat_pointer_leave();
    grab = G_MOVE;
    grab_win = t;
    grab_px = ptr_x;
    grab_py = ptr_y;
    grab_x = t->x;
    grab_y = t->y;
    unmaximize_on_drag = t->maximized;
}

/* Dragging a maximized window restores it under the pointer — but only once the pointer
 * has really moved, so a plain click on the title bar doesn't. */
static void drag_unmaximize(struct toplevel *t) {
    if (fabs(ptr_x - grab_px) + fabs(ptr_y - grab_py) < 8) return;
    unmaximize_on_drag = false;
    struct layout L;
    layout_window(t, &L);
    int th = L.decorated ? L.titlebar.h : 0;
    maximize(t, false);
    int w = t->saved.w > 0 ? t->saved.w : 640;
    grab_x = t->x = (int)ptr_x - w / 2;
    grab_y = t->y = (int)ptr_y + th / 2;
    grab_px = ptr_x;
    grab_py = ptr_y;
}

static void start_resize(struct toplevel *t, uint32_t edges) {
    if (t->maximized || t->fullscreen || t->tiled || !edges) return;
    struct rect g = xdg_geometry(t->xdg);
    seat_pointer_leave();
    grab = G_RESIZE;
    grab_win = t;
    grab_px = ptr_x;
    grab_py = ptr_y;
    grab_x = t->x;
    grab_y = t->y;
    grab_w = g.w;
    grab_h = g.h;
    grab_edges = edges;
    t->resizing = true;
    t->resize_edges = edges;
    t->resize_anchor_x = t->x + g.w;   /* right edge stays put when dragging the left one */
    t->resize_anchor_y = t->y + g.h;
    resize_pending = false;
}

static void drop_tile(struct toplevel *t);

static void end_grab(void) {
    if (grab == G_MOVE && grab_win && grab_win->lifted) drop_tile(grab_win);
    if (grab == G_RESIZE && grab_win) {
        grab_win->resizing = false;
        if (resize_pending) toplevel_send_configure(grab_win, resize_w, resize_h);
        else send_state(grab_win);
    }
    grab = G_NONE;
    grab_win = NULL;
    implicit_surface = NULL;
    resize_pending = false;
}

static void send_resize(struct toplevel *t, int w, int h) {
    /* don't flood a slow client: one outstanding configure at a time */
    if (t->xdg->acked_serial < t->xdg->configure_serial) {
        resize_pending = true;
        resize_w = w;
        resize_h = h;
        return;
    }
    resize_pending = false;
    if (w != t->conf_w || h != t->conf_h) toplevel_send_configure(t, w, h);
}

/* ------------------------------------------------------------------ tiling */

/* With `desktop { window-layout: tile | columns | rows | grid | monocle }`, windows are
 * placed for you, `gap` apart, inside the desktop's padding. A window opts out with
 * `tile: no` (e.g. window.float { tile: no }); dialogs, fixed-size, maximized and
 * fullscreen windows always float. Each window remembers where it was floating, and goes
 * back there when the layout switches to floating. */

static bool wants_tiling(struct toplevel *t) {
    if (!t->xdg || !t->xdg->mapped || t->minimized || t->fullscreen || t->maximized || t->parent || t->lifted)
        return false;
    if (t->min_w > 0 && t->min_w == t->max_w && t->min_h > 0 && t->min_h == t->max_h) return false;
    struct layout L;
    compute_styles(t, window_states(t), &L);
    return L.win.tile;
}

/* frame rectangles for n windows; integer remainders go to the last one in a row/column */
static void place_tiles(int layout, struct rect a, int gap, float ratio, int n, struct rect *out) {
    if (layout == LAYOUT_MONOCLE || n == 1) {
        for (int i = 0; i < n; i++) out[i] = a;
        return;
    }
    if (layout == LAYOUT_TILE) {   /* one big "master" on the left, the rest stacked on the right */
        int mw = (int)lroundf((a.w - gap) * ratio);
        out[0] = (struct rect){ a.x, a.y, mw, a.h };
        struct rect stack = { a.x + mw + gap, a.y, a.w - mw - gap, a.h };
        int m = n - 1, h = (stack.h - gap * (m - 1)) / m;
        for (int i = 0; i < m; i++) {
            int y = stack.y + i * (h + gap);
            out[1 + i] = (struct rect){ stack.x, y, stack.w, i == m - 1 ? stack.y + stack.h - y : h };
        }
        return;
    }
    int cols = layout == LAYOUT_COLUMNS ? n : layout == LAYOUT_ROWS ? 1 : (int)ceil(sqrt(n));
    int rows = (n + cols - 1) / cols;
    int h = (a.h - gap * (rows - 1)) / rows;
    for (int r = 0, i = 0; r < rows; r++) {
        int in_row = MIN(cols, n - i);   /* the last row may be short: its cells widen */
        int w = (a.w - gap * (in_row - 1)) / in_row;
        int y = a.y + r * (h + gap), rh = r == rows - 1 ? a.y + a.h - y : h;
        for (int c = 0; c < in_row; c++, i++) {
            int x = a.x + c * (w + gap);
            out[i] = (struct rect){ x, y, c == in_row - 1 ? a.x + a.w - x : w, rh };
        }
    }
}

static void tile_to(struct toplevel *t, struct rect frame) {
    bool was = t->tiled;
    if (!t->float_saved) {
        struct rect g = xdg_geometry(t->xdg);
        t->float_geom = (struct rect){ t->x, t->y, g.w, g.h };
        t->float_saved = true;
    }
    t->tiled = true;
    int ext[4];
    window_extents(t, window_states(t), ext);
    int w = MAX(frame.w - ext[LEFT] - ext[RIGHT], 32), h = MAX(frame.h - ext[TOP] - ext[BOTTOM], 32);
    t->x = frame.x + ext[LEFT];
    t->y = frame.y + ext[TOP];
    if (!was || w != t->conf_w || h != t->conf_h) toplevel_send_configure(t, w, h);
}

static void untile(struct toplevel *t) {
    t->tiled = false;
    /* maximized or fullscreen or minimized for now: keep the floating geometry for later */
    if (t->maximized || t->fullscreen || t->minimized || !t->float_saved) {
        if (t->xdg && t->xdg->mapped && !t->maximized && !t->fullscreen) send_state(t);
        return;
    }
    t->float_saved = false;
    struct rect f = t->float_geom;
    t->x = f.x;
    t->y = f.y;
    toplevel_send_configure(t, f.w, f.h);
}

static void arrange(void) {
    struct style ds;
    style_of("desktop", &ds);
    struct toplevel *list[64], *t;
    int n = 0;
    list_for_each(t, &tile_order, tile_link) {
        if (t->lifted) continue;   /* being dragged: keep its size, don't touch it */
        if (ds.window_layout != LAYOUT_FLOATING && wants_tiling(t) && n < 64) list[n++] = t;
        else if (t->tiled || (t->float_saved && !t->maximized && !t->fullscreen && !t->minimized)) untile(t);
    }
    if (n == 0) return;
    struct rect frames[64];
    place_tiles(ds.window_layout, work_area(&ds), ds.gap, ds.tile_ratio, n, frames);
    for (int i = 0; i < n; i++) tile_to(list[i], frames[i]);
    wm_damage();
}

/* a dragged tile was let go: if it's over another tile, they trade places */
static void drop_tile(struct toplevel *t) {
    t->lifted = false;
    struct toplevel *o, *target = NULL;
    list_for_each(o, &tile_order, tile_link) {
        if (o == t || !o->tiled) continue;
        struct layout L;
        layout_window(o, &L);
        if (rect_contains(L.frame, ptr_x, ptr_y)) target = o;
    }
    if (target) {   /* swap their places in the order */
        struct list *tp = t->tile_link.prev;
        list_remove(&t->tile_link);
        list_insert(&target->tile_link, &t->tile_link);
        list_remove(&target->tile_link);
        list_insert(tp == &target->tile_link ? &t->tile_link : tp, &target->tile_link);
    }
    arrange();
}

/* --------------------------------------------------------- toplevel lifecycle */

void wm_toplevel_new(struct toplevel *t) { list_init(&t->tile_link); }

void wm_toplevel_map(struct toplevel *t) {
    struct rect g = xdg_geometry(t->xdg);
    if (t->fullscreen) {
        t->x = t->y = 0;
    } else if (t->maximized) {
        struct rect r = wm_maximized_content(t);
        t->x = r.x;
        t->y = r.y;
    } else if (!t->placed) {
        int ext[4];
        window_extents(t, ST_FOCUSED, ext);
        struct style ds;
        style_of("desktop", &ds);
        struct rect a = work_area(&ds);
        if (t->parent && t->parent->xdg && t->parent->xdg->mapped) {   /* dialogs: center on parent */
            struct rect pg = xdg_geometry(t->parent->xdg);
            t->x = t->parent->x + (pg.w - g.w) / 2;
            t->y = t->parent->y + (pg.h - g.h) / 2;
        } else {   /* centered (decorations included), each new window a little further along */
            t->x = a.x + (a.w - g.w - ext[LEFT] - ext[RIGHT]) / 2 + ext[LEFT] + (cascade - 2) * 32;
            t->y = a.y + (a.h - g.h - ext[TOP] - ext[BOTTOM]) / 2 + ext[TOP] + (cascade - 2) * 32;
            cascade = (cascade + 1) % 5;
        }
        t->x = MAX(t->x, a.x + ext[LEFT]);   /* keep the title bar reachable */
        t->y = MAX(t->y, a.y + ext[TOP]);
    }
    if (!t->maximized && !t->fullscreen && t->saved.w == 0)
        t->saved = (struct rect){ t->x, t->y, g.w, g.h };
    t->placed = true;
    list_remove(&t->link);
    list_append(&toplevels, &t->link);
    list_remove(&t->tile_link);
    list_append(&tile_order, &t->tile_link);
    focus_window(t);
    arrange();
}

static struct toplevel *popup_root(struct popup *p) {
    struct xdg_surface *x = p->parent;
    for (int i = 0; x && x->popup && i < 32; i++) x = x->popup->parent;
    return x ? x->toplevel : NULL;
}

static void forget_window(struct toplevel *t) {
    menu_forget(t);
    struct popup *p, *tmp;
    list_for_each_safe(p, tmp, &popups, link)
        if (p->mapped && popup_root(p) == t) popup_send_done(p);
    if (hover_win == t) { hover_win = NULL; hover_part = PART_NONE; }
    if (press_win == t) { press_win = NULL; press_part = PART_NONE; }
    if (last_click_win == t) last_click_win = NULL;
    if (grab_win == t) end_grab();
    list_remove(&t->link);
    list_remove(&t->tile_link);
    t->tiled = t->lifted = t->float_saved = false;
    anim_forget(t);
    if (focused == t) {
        focused = NULL;
        focus_topmost();
    }
    arrange();
    wm_damage();
}

void wm_toplevel_unmap(struct toplevel *t) {
    forget_window(t);
    t->placed = false;
    t->maximized = t->fullscreen = t->minimized = false;
}

void wm_toplevel_destroy(struct toplevel *t) { forget_window(t); }

void wm_toplevel_committed(struct toplevel *t) {
    if (t->resizing) {
        struct rect g = xdg_geometry(t->xdg);
        if (t->resize_edges & EDGE_LEFT) t->x = t->resize_anchor_x - g.w;
        if (t->resize_edges & EDGE_TOP) t->y = t->resize_anchor_y - g.h;
        if (resize_pending && grab_win == t) send_resize(t, resize_w, resize_h);
    }
    wm_damage();
}

void wm_toplevel_title_changed(struct toplevel *t) { (void)t; wm_damage(); }

void wm_toplevel_request(struct toplevel *t, int what, uint32_t arg) {
    bool mapped = t->xdg && t->xdg->mapped;
    switch (what) {
    case REQ_MOVE:
        if (mapped && buttons_down > 0 && grab != G_MOVE && grab != G_RESIZE) start_move(t);
        break;
    case REQ_RESIZE:
        if (mapped && buttons_down > 0 && grab != G_MOVE && grab != G_RESIZE) start_resize(t, arg);
        break;
    case REQ_MAXIMIZE:
    case REQ_UNMAXIMIZE:
        t->want_maximized = what == REQ_MAXIMIZE;
        if (mapped) maximize(t, t->want_maximized);
        else if (t->xdg && t->xdg->sent_configure) {   /* before mapping: re-configure */
            t->maximized = t->want_maximized;
            struct rect r = wm_maximized_content(t);
            toplevel_send_configure(t, t->maximized ? r.w : 0, t->maximized ? r.h : 0);
        }
        break;
    case REQ_FULLSCREEN:
    case REQ_UNFULLSCREEN:
        t->want_fullscreen = what == REQ_FULLSCREEN;
        if (mapped) fullscreen(t, t->want_fullscreen);
        break;
    case REQ_MINIMIZE:
        if (mapped) minimize(t);
        break;
    }
}

/* -------------------------------------------------------------------- popups */

void wm_popup_new(struct popup *p) { list_append(&popups, &p->link); }

void wm_popup_map(struct popup *p) {
    list_remove(&p->link);   /* newest mapped popup is on top */
    list_append(&popups, &p->link);
    if (p->grab) update_keyboard_focus();
    wm_damage();
}

void wm_popup_grab(struct popup *p) { (void)p; update_keyboard_focus(); }

void wm_popup_unmap(struct popup *p) {
    p->mapped = false;
    struct popup *c, *tmp;
    list_for_each_safe(c, tmp, &popups, link)   /* submenus go with their parent menu */
        if (c->mapped && c->parent == p->xdg) popup_send_done(c);
    if (implicit_surface && p->xdg && implicit_surface == p->xdg->surface) {
        grab = G_NONE;
        implicit_surface = NULL;
    }
    update_keyboard_focus();
    wm_damage();
}

void wm_xdg_surface_destroyed(struct xdg_surface *x) {
    struct popup *p, *tmp;
    list_for_each_safe(p, tmp, &popups, link) {
        if (p->parent != x) continue;
        if (p->mapped) popup_send_done(p);
        p->parent = NULL;
    }
}

static void dismiss_popups(void) {
    struct popup *p;
    while ((p = top_grab_popup())) popup_send_done(p);
}

void wm_surface_destroyed(struct surface *s) {
    if (implicit_surface == s) {
        implicit_surface = NULL;
        if (grab == G_IMPLICIT) grab = G_NONE;
    }
    if (cursor_surface == s) cursor_surface = NULL;
    wm_damage();
}

/* -------------------------------------------------------------------- cursor */

void wm_set_cursor(struct surface *s, int hot_x, int hot_y) {
    (void)hot_x; (void)hot_y;
    cursor_surface = s;
    client_cursor = true;
    wm_damage();
}

void wm_cursor_changed(void) { wm_damage(); }

/* ------------------------------------------------------------- drag & drop */

/* A client may start a drag only from a press it is still holding. */
bool wm_start_drag(struct client *c) {
    return grab == G_IMPLICIT && implicit_surface && implicit_surface->client == c && buttons_down > 0;
}

static void process_motion(void);
static void open_menu(struct toplevel *t, int x, int y, bool keyboard);

void wm_drag_started(void) {
    grab = G_DRAG;          /* pointer events now drive the drag, not wl_pointer */
    implicit_surface = NULL;
    seat_pointer_leave();
    client_cursor = false;
    process_motion();
    wm_damage();
}

void wm_drag_ended(void) {
    if (grab != G_DRAG) return;
    grab = G_NONE;
    process_motion();       /* give the pointer back to whatever is under it */
    wm_damage();
}

/* --------------------------------------------------------------------- input */

static void process_motion(void) {
    if (menu_is_open()) {   /* the menu has the pointer */
        menu_motion(ptr_x, ptr_y);
        return;
    }
    if (grab == G_DRAG) {
        struct hit h = hit_test(ptr_x, ptr_y);
        seat_drag_motion(h.s, h.sx, h.sy);
        return;
    }
    if (grab == G_MOVE && grab_win) {
        if (unmaximize_on_drag) {
            drag_unmaximize(grab_win);
            if (unmaximize_on_drag) return;
        }
        grab_win->x = grab_x + (int)(ptr_x - grab_px);
        grab_win->y = grab_y + (int)(ptr_y - grab_py);
        wm_damage();
        return;
    }
    if (grab == G_RESIZE && grab_win) {
        int dx = (int)(ptr_x - grab_px), dy = (int)(ptr_y - grab_py);
        int w = grab_w, h = grab_h;
        if (grab_edges & EDGE_RIGHT) w += dx;
        if (grab_edges & EDGE_LEFT) w -= dx;
        if (grab_edges & EDGE_BOTTOM) h += dy;
        if (grab_edges & EDGE_TOP) h -= dy;
        struct toplevel *t = grab_win;
        w = MAX(w, MAX(t->min_w, 64));
        h = MAX(h, MAX(t->min_h, 32));
        if (t->max_w > 0) w = MIN(w, t->max_w);
        if (t->max_h > 0) h = MIN(h, t->max_h);
        send_resize(t, w, h);
        return;
    }
    if (grab == G_IMPLICIT && implicit_surface) {   /* a button is held: motion goes to the pressed surface */
        int ax, ay;
        if (surface_abs_pos(implicit_surface, &ax, &ay)) seat_pointer_motion(ptr_x - ax, ptr_y - ay);
        return;
    }
    struct hit h = hit_test(ptr_x, ptr_y);
    if (h.t != hover_win || h.part != hover_part) {
        hover_win = h.t;
        hover_part = h.part;
        wm_damage();
    }
    struct surface *before = seat_pointer_focus();
    if (h.s) {
        seat_pointer_enter(h.s, h.sx, h.sy);
        if (seat_pointer_focus() != before) {
            client_cursor = false;   /* until the new client sets one */
            cursor_surface = NULL;
        }
        seat_pointer_motion(h.sx, h.sy);
    } else {
        seat_pointer_leave();
        client_cursor = false;
    }
}

void wm_pointer_warp(double x, double y) {
    ptr_x = CLAMP(x, 0, srv.width - 1);
    ptr_y = CLAMP(y, 0, srv.height - 1);
    process_motion();
    wm_damage();
}

void wm_pointer_motion(double dx, double dy) { wm_pointer_warp(ptr_x + dx, ptr_y + dy); }

static void button_action(struct toplevel *t, enum part part) {
    switch (part) {
    case PART_CLOSE: toplevel_send_close(t); break;
    case PART_MAXIMIZE: maximize(t, !t->maximized); break;
    case PART_MINIMIZE: minimize(t); break;
    default: break;
    }
}

void wm_pointer_button(uint32_t button, bool pressed) {
    if (pressed) buttons_down++;
    else if (buttons_down > 0) buttons_down--;

    if (menu_is_open()) {
        menu_button(button, pressed, ptr_x, ptr_y);
        return;
    }

    if (grab == G_DRAG) {
        if (!pressed && buttons_down == 0) seat_drag_drop();
        return;
    }
    if (grab == G_MOVE || grab == G_RESIZE) {
        if (!pressed && buttons_down == 0) {
            end_grab();
            process_motion();
        }
        return;
    }
    if (grab == G_IMPLICIT) {
        seat_pointer_button(button, pressed);
        if (!pressed && buttons_down == 0) {
            grab = G_NONE;
            implicit_surface = NULL;
            process_motion();
        }
        return;
    }
    if (!pressed) {   /* release over a title bar button triggers it */
        if (press_win && press_part != PART_NONE && hover_win == press_win && hover_part == press_part)
            button_action(press_win, press_part);
        press_win = NULL;
        press_part = PART_NONE;
        wm_damage();
        return;
    }

    struct hit h = hit_test(ptr_x, ptr_y);
    struct popup *gp = top_grab_popup();
    if (gp && (!h.s || h.s->client != gp->obj->client)) {   /* click outside the menu: close it */
        dismiss_popups();
        return;
    }
    if (h.t) {
        focus_window(h.t);
        uint32_t mods = seat_modifiers();
        if ((mods & MOD_ALT) && button == BTN_LEFT) { start_move(h.t); return; }
        if ((mods & MOD_ALT) && button == BTN_RIGHT) {   /* resize from the nearest corner */
            struct rect g = xdg_geometry(h.t->xdg);
            uint32_t e = (ptr_x < h.t->x + g.w / 2 ? EDGE_LEFT : EDGE_RIGHT) |
                         (ptr_y < h.t->y + g.h / 2 ? EDGE_TOP : EDGE_BOTTOM);
            start_resize(h.t, e);
            return;
        }
    }
    switch (h.part) {
    case PART_CLIENT:
        if (h.s != seat_pointer_focus()) seat_pointer_enter(h.s, h.sx, h.sy);
        seat_pointer_button(button, true);
        grab = G_IMPLICIT;
        implicit_surface = h.s;
        break;
    case PART_CLOSE: case PART_MAXIMIZE: case PART_MINIMIZE:
        if (button == BTN_LEFT) {
            press_win = h.t;
            press_part = h.part;
            wm_damage();
        }
        break;
    case PART_TITLEBAR:
        if (button == BTN_LEFT) {
            uint32_t now = now_ms();
            if (last_click_win == h.t && now - last_click_time < 400) {   /* double-click */
                maximize(h.t, !h.t->maximized);
                last_click_win = NULL;
                break;
            }
            last_click_win = h.t;
            last_click_time = now;
            start_move(h.t);
        } else if (button == BTN_MIDDLE) {
            minimize(h.t);
        } else if (button == BTN_RIGHT) {
            open_menu(h.t, (int)ptr_x, (int)ptr_y, false);
        }
        break;
    case PART_BORDER:
        if (button == BTN_LEFT) start_resize(h.t, h.edges);
        break;
    default:
        break;
    }
}

void wm_pointer_axis(int axis, double value, int discrete) {
    if (grab == G_MOVE || grab == G_RESIZE || grab == G_DRAG || menu_is_open()) return;
    seat_pointer_axis(axis, value, discrete);
}

void wm_key(uint32_t key, bool pressed) {
    if (grab == G_DRAG && key == KEY_ESC) {   /* Escape cancels a drag */
        if (pressed) seat_drag_cancel();
        return;
    }
    if (menu_is_open()) {   /* keep modifier state right, but the app doesn't see these keys */
        seat_keyboard_key(key, pressed, true);
        menu_key(key, pressed);
        return;
    }
    seat_keyboard_key(key, pressed, false);
}

/* ------------------------------------------------------------------- actions */

void wm_action_quit(void) { srv.running = false; }

void wm_action_cycle(void) {
    /* bring the bottom-most window to the top; repeated presses walk through all of them */
    if (list_empty(&toplevels)) return;
    struct toplevel *t = container_of(toplevels.next, struct toplevel, link);
    bool was_minimized = t->minimized;
    t->minimized = false;
    focus_window(t);
    if (was_minimized) arrange();
}

void wm_action_close(void) { if (focused) toplevel_send_close(focused); }
void wm_action_maximize(void) { if (focused) maximize(focused, !focused->maximized); }
void wm_action_terminal(void) { if (srv.terminal) spawn(srv.terminal); }

/* -------------------------------------------------------------- window menu */

static void open_menu(struct toplevel *t, int x, int y, bool keyboard) {
    if (grab == G_MOVE || grab == G_RESIZE || grab == G_DRAG || !t->xdg || !t->xdg->mapped) return;
    /* the menu takes the pointer, even from a client that's holding a button down */
    grab = G_NONE;
    implicit_surface = NULL;
    seat_pointer_leave();
    client_cursor = false;
    dismiss_popups();
    focus_window(t);
    menu_open(t, x, y, keyboard);
}

void wm_action_window_menu(void) {
    if (!focused) return;
    struct layout L;
    layout_window(focused, &L);
    struct rect anchor = L.decorated ? L.titlebar : L.content;   /* under the title bar's left end */
    open_menu(focused, anchor.x, anchor.y + (L.decorated ? anchor.h : 0), true);
}

void wm_show_window_menu(struct toplevel *t, int x, int y) { open_menu(t, t->x + x, t->y + y, false); }

void wm_menu_closed(void) {
    process_motion();   /* give the pointer back to whatever is under it */
    wm_damage();
}

void wm_window_minimize(struct toplevel *t) { minimize(t); }
void wm_window_maximize(struct toplevel *t, bool on) { maximize(t, on); }
void wm_window_fullscreen(struct toplevel *t, bool on) { fullscreen(t, on); }

bool wm_window_has_class(struct toplevel *t, const char *cls) {
    size_t n = strlen(cls);
    for (const char *p = t->classes; (p = strstr(p, cls)); p += n)
        if ((p == t->classes || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) return true;
    return false;
}

void wm_window_toggle_class(struct toplevel *t, const char *cls) {
    char out[sizeof t->classes] = "", copy[sizeof t->classes];
    bool had = wm_window_has_class(t, cls);
    snprintf(copy, sizeof copy, "%s", t->classes);
    for (char *save, *c = strtok_r(copy, " ", &save); c; c = strtok_r(NULL, " ", &save)) {
        if (!strcmp(c, cls)) continue;
        if (strlen(out) + strlen(c) + 2 < sizeof out) { if (out[0]) strcat(out, " "); strcat(out, c); }
    }
    if (!had && strlen(out) + strlen(cls) + 2 < sizeof out) { if (out[0]) strcat(out, " "); strcat(out, cls); }
    snprintf(t->classes, sizeof t->classes, "%s", out);
    arrange();   /* a class can change tiling (tile: no) or decorations */
    wm_damage();
}

/* ------------------------------------------------------------------ drawing */

struct draw_ctx {
    struct canvas *c;
    float opacity;
    const struct rect *clip;
    struct radii clip_rad;
};

static void draw_surface(struct surface *s, int x, int y, void *data) {
    struct draw_ctx *d = data;
    if (!s->pixels) return;
    draw_image(d->c, s->pixels, s->buf_w, s->buf_h, s->scale, x, y, s->opaque, d->opacity,
               d->clip, d->clip_rad);
}

static struct radii uniform(float r) { return (struct radii){ r, r, r, r }; }

static const struct font *font_of(const struct style *s) {
    return font_get(s->font_family, s->font_weight, s->font_italic, s->font_size);
}

static void draw_title(struct canvas *c, struct layout *L, const char *text, float op) {
    const struct font *f = font_of(&L->title);
    struct rect a = L->title_area, bar = L->titlebar;
    int len = L->vertical ? a.h : a.w;   /* room along the bar */
    if (!f || !text[0] || len <= 0) return;
    int tw = MIN(text_width(f, text), len), lh = font_line_height(f);
    int start = L->vertical ? a.y : a.x, bar_start = L->vertical ? bar.y : bar.x;
    int bar_len = L->vertical ? bar.h : bar.w;
    int at = start;   /* where the text begins along the bar */
    if (L->title.text_align == ALIGN_CENTER) {
        /* center on the whole bar if that doesn't collide with the buttons */
        at = CLAMP(bar_start + (bar_len - tw) / 2, start, start + len - tw);
    } else if (L->title.text_align == ALIGN_RIGHT) {
        at = start + len - tw;
    }
    const int *tp = L->title.padding;
    if (!L->vertical) {
        int y = bar.y + (bar.h - lh) / 2 + tp[TOP] - tp[BOTTOM];
        draw_text(c, f, at, y, text, L->title.color, op, len, 0);
    } else {
        /* on the left the text reads bottom to top, on the right top to bottom */
        int rot = L->side == LEFT ? 270 : 90;
        if (rot == 270 && L->title.text_align != ALIGN_CENTER) at = start + len - tw - (at - start);
        int x = bar.x + (bar.w - lh) / 2 + tp[LEFT] - tp[RIGHT];
        draw_text(c, f, x, at, text, L->title.color, op, len, rot);
    }
}


/* a corner's radius just inside the border, as draw_box computes it */
static float inner_corner(float r, int w1, int w2) { return fmaxf(r - MAX(w1, w2), 0); }

static void draw_window(struct canvas *c, struct toplevel *t) {
    struct layout L;
    layout_window(t, &L);
    /* sizes come from the target style; colors, shadows, radii and opacity may be mid-transition */
    anim_style(t, 0, &L.win);
    anim_style(t, 1, &L.bar);
    anim_style(t, 2, &L.title);
    for (int i = 0; i < 3; i++) anim_style(t, 3 + i, &L.btn[i]);
    float op = L.win.opacity;
    struct draw_ctx d = { c, op, NULL, uniform(0) };
    struct paint layers[2];
    int n;

    if (L.decorated) {
        struct style *w = &L.win;
        struct border b = style_border(w);
        struct radii r = style_radii(w, L.frame.w, L.frame.h);
        if (w->shadow) {
            int sp = w->shadow_spread;
            struct rect sr = { L.frame.x + w->shadow_x - sp, L.frame.y + w->shadow_y - sp,
                               L.frame.w + 2 * sp, L.frame.h + 2 * sp };
            struct radii srad = { r.tl + sp, r.tr + sp, r.br + sp, r.bl + sp };
            draw_shadow(c, sr, srad, w->shadow_blur, w->shadow_color, op, op >= 1 ? &L.frame : NULL);
        }
        n = style_layers(w, layers);
        draw_box(c, L.frame, r, &b, layers, n, op, &L.content);
        /* the corners inside the border; the bar takes the ones on its side, the content the rest */
        struct radii in = { inner_corner(r.tl, b.w[LEFT], b.w[TOP]), inner_corner(r.tr, b.w[RIGHT], b.w[TOP]),
                            inner_corner(r.br, b.w[RIGHT], b.w[BOTTOM]), inner_corner(r.bl, b.w[LEFT], b.w[BOTTOM]) };
        struct radii bar_r = { 0 }, content_r = in;
        if (L.has_bar) {
            switch (L.side) {
            case TOP: bar_r.tl = in.tl; bar_r.tr = in.tr; content_r.tl = content_r.tr = 0; break;
            case BOTTOM: bar_r.bl = in.bl; bar_r.br = in.br; content_r.bl = content_r.br = 0; break;
            case LEFT: bar_r.tl = in.tl; bar_r.bl = in.bl; content_r.tl = content_r.bl = 0; break;
            default: bar_r.tr = in.tr; bar_r.br = in.br; content_r.tr = content_r.br = 0; break;
            }
            struct border bb = style_border(&L.bar);
            n = style_layers(&L.bar, layers);
            draw_box(c, L.titlebar, bar_r, &bb, layers, n, op, NULL);
            for (int i = 0; i < 3; i++) {
                if (!L.shown[i]) continue;
                struct style *s = &L.btn[i];
                struct rect br = L.buttons[i];
                struct border sb = style_border(s);
                n = style_layers(s, layers);
                draw_box(c, br, style_radii(s, br.w, br.h), &sb, layers, n, op, NULL);
                enum icon ic = i == BTN_IDX_CLOSE ? ICON_CLOSE : i == BTN_IDX_MINIMIZE ? ICON_MINIMIZE :
                               t->maximized ? ICON_RESTORE : ICON_MAXIMIZE;
                draw_icon(c, br, ic, s->color, op);
            }
            draw_title(c, &L, t->title[0] ? t->title : t->app_id, op);
        }
        d.clip = &L.content;   /* round the content's free corners to match */
        d.clip_rad = content_r;
    }
    int sx, sy;
    if (surface_origin(t->xdg, &sx, &sy)) surface_for_each(t->xdg->surface, sx, sy, draw_surface, &d);
}

static void draw_popup(struct canvas *c, struct popup *p) {
    int sx, sy, ox = 0, oy = 0;
    if (!p->xdg || !surface_origin(p->xdg, &sx, &sy) || !xdg_abs_origin(p->xdg, &ox, &oy)) return;
    struct style st;
    style_of("popup", &st);
    anim_style(p, 0, &st);
    struct rect g = xdg_geometry(p->xdg);
    struct rect r = { ox, oy, g.w, g.h };
    struct radii rad = style_radii(&st, r.w, r.h);
    if (st.shadow) {
        int sp = st.shadow_spread;
        struct rect sr = { r.x + st.shadow_x - sp, r.y + st.shadow_y - sp, r.w + 2 * sp, r.h + 2 * sp };
        struct radii srad = { rad.tl + sp, rad.tr + sp, rad.br + sp, rad.bl + sp };
        draw_shadow(c, sr, srad, st.shadow_blur, st.shadow_color, st.opacity, &r);
    }
    struct draw_ctx d = { c, st.opacity, NULL, uniform(0) };
    surface_for_each(p->xdg->surface, sx, sy, draw_surface, &d);
    struct border b = style_border(&st);   /* drawn around the popup, outside it */
    if (b.w[0] || b.w[1] || b.w[2] || b.w[3]) {
        struct rect br = { r.x - b.w[LEFT], r.y - b.w[TOP], r.w + b.w[LEFT] + b.w[RIGHT], r.h + b.w[TOP] + b.w[BOTTOM] };
        struct radii orad = { rad.tl + MAX(b.w[LEFT], b.w[TOP]), rad.tr + MAX(b.w[RIGHT], b.w[TOP]),
                              rad.br + MAX(b.w[RIGHT], b.w[BOTTOM]), rad.bl + MAX(b.w[LEFT], b.w[BOTTOM]) };
        draw_box(c, br, orad, &b, NULL, 0, st.opacity, &r);
    }
}

static void draw_desktop(struct canvas *c) {
    if (!desktop_cache || desktop_w != c->w || desktop_h != c->h) {
        free(desktop_cache);
        desktop_w = c->w;
        desktop_h = c->h;
        desktop_cache = calloc((size_t)c->w * c->h, 4);
        struct canvas dc = { desktop_cache, c->w, c->h, c->w, { 0, 0, c->w, c->h } };
        struct style st;
        style_of("desktop", &st);
        struct paint layers[3] = { { .kind = PAINT_COLOR, .color = { 0x30, 0x30, 0x38, 255 } } };
        int n = 1 + style_layers(&st, layers + 1);   /* a dark base under whatever the theme paints */
        draw_box(&dc, dc.clip, uniform(0), NULL, layers, n, 1, NULL);
    }
    for (int y = 0; y < c->h; y++)
        memcpy(c->px + (size_t)y * c->stride, desktop_cache + (size_t)y * c->w, (size_t)c->w * 4);
}

static void draw_pointer(struct canvas *c) {
    int x = (int)ptr_x, y = (int)ptr_y;
    struct surface *icon = seat_drag_icon();
    if (grab == G_DRAG && icon && icon->pixels) {   /* what's being dragged follows the pointer */
        struct draw_ctx d = { c, 1, NULL, uniform(0) };
        surface_for_each(icon, x - icon->cursor_hot_x, y - icon->cursor_hot_y, draw_surface, &d);
    }
    if (client_cursor && seat_pointer_focus()) {
        if (cursor_surface && cursor_surface->pixels) {
            struct draw_ctx d = { c, 1, NULL, uniform(0) };
            surface_for_each(cursor_surface, x - cursor_surface->cursor_hot_x,
                             y - cursor_surface->cursor_hot_y, draw_surface, &d);
        }
        return;   /* NULL cursor surface = the client hid the pointer */
    }
    struct style st;
    style_of("cursor", &st);
    struct color outline = st.border_color_set[TOP] ? st.border.c[TOP] : (struct color){ 0, 0, 0, 255 };
    int scale = st.height > 0 ? MAX(1, (int)lroundf(st.height / 19.0f)) : 1;
    draw_cursor(c, x, y, scale, st.color, outline);
}

void wm_render(struct canvas *c) {
    draw_desktop(c);
    struct toplevel *t;
    list_for_each(t, &toplevels, link)
        if (!t->minimized && t->xdg && t->xdg->surface) draw_window(c, t);
    struct popup *p;
    list_for_each(p, &popups, link)
        if (p->mapped) draw_popup(c, p);
    menu_draw(c);
    draw_pointer(c);
}
