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
#include <stdlib.h>
#include <string.h>

enum part { PART_NONE, PART_CLIENT, PART_TITLEBAR, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_BORDER };
enum { EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8 };
enum { BTN_IDX_CLOSE, BTN_IDX_MAXIMIZE, BTN_IDX_MINIMIZE };

#define RESIZE_MARGIN 6     /* invisible grab area outside the frame */
#define CORNER_SIZE 16

static struct list toplevels;   /* mapped toplevels, bottom to top */
static struct list popups;      /* all popups, oldest first */
static struct toplevel *focused;

static double ptr_x, ptr_y;
static struct surface *cursor_surface;
static bool client_cursor;      /* the client under the pointer chose a cursor */

static struct toplevel *hover_win, *press_win;
static enum part hover_part, press_part;

static enum { G_NONE, G_MOVE, G_RESIZE, G_IMPLICIT } grab;
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
    list_init(&popups);
    ptr_x = srv.width / 2.0;
    ptr_y = srv.height / 2.0;
}

void wm_theme_changed(void) {
    free(desktop_cache);
    desktop_cache = NULL;
    wm_damage();
}

/* ------------------------------------------------------------------- layout */

struct layout {
    bool decorated;
    struct rect content, frame, titlebar, title_area;
    struct rect buttons[3];
    bool shown[3];
    float radius;
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

static void compute_styles(struct toplevel *t, unsigned states, struct layout *L) {
    struct css_elem ew = { "window", NULL, states, t->app_id, t->title, NULL };
    struct css_elem eb = { "titlebar", NULL, 0, t->app_id, t->title, &ew };
    struct css_elem et = { "title", NULL, 0, t->app_id, t->title, &eb };
    css_compute(&ew, NULL, &L->win);
    css_compute(&eb, &L->win, &L->bar);
    css_compute(&et, &L->bar, &L->title);
    for (int i = 0; i < 3; i++) {
        unsigned bs = 0;
        if (hover_win == t && hover_part == button_part[i]) bs |= ST_HOVER;
        if (press_win == t && press_part == button_part[i] && hover_part == press_part) bs |= ST_ACTIVE;
        struct css_elem e = { "button", button_class[i], bs, t->app_id, t->title, &eb };
        css_compute(&e, &L->bar, &L->btn[i]);
    }
}

static int bar_height(const struct layout *L) { return L->bar.height >= 0 ? L->bar.height : 28; }

static void layout_window(struct toplevel *t, struct layout *L) {
    memset(L, 0, sizeof *L);
    compute_styles(t, window_states(t), L);
    struct rect g = xdg_geometry(t->xdg);
    L->content = (struct rect){ t->x, t->y, g.w, g.h };
    L->decorated = t->ssd && !t->fullscreen;
    if (!L->decorated) {
        L->frame = L->content;
        return;
    }
    int bw = L->win.border_width, th = bar_height(L);
    struct rect c = L->content;
    L->frame = (struct rect){ c.x - bw, c.y - th - bw, c.w + 2 * bw, c.h + th + 2 * bw };
    L->titlebar = (struct rect){ c.x, c.y - th, c.w, th };
    L->radius = style_radius(&L->win, L->frame.w, L->frame.h);

    /* buttons: right-floating ones are placed from the right edge inward, left-floating
     * from the left edge inward. Order from the edge: close, maximize, minimize on the
     * right (Windows/GNOME), close, minimize, maximize on the left (macOS). */
    int rx = L->titlebar.x + L->titlebar.w - L->bar.padding[1];
    int lx = L->titlebar.x + L->bar.padding[3];
    static const int right_order[3] = { BTN_IDX_CLOSE, BTN_IDX_MAXIMIZE, BTN_IDX_MINIMIZE };
    static const int left_order[3] = { BTN_IDX_CLOSE, BTN_IDX_MINIMIZE, BTN_IDX_MAXIMIZE };
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 3; k++) {
            int i = pass == 0 ? right_order[k] : left_order[k];
            const struct style *b = &L->btn[i];
            if (b->hidden || b->float_left != (pass == 1)) continue;
            int w = b->width >= 0 ? b->width : 16, h = b->height >= 0 ? b->height : 16;
            int y = L->titlebar.y + (th - h) / 2 + b->margin[0] - b->margin[2];
            if (pass == 0) {
                rx -= b->margin[1] + w;
                L->buttons[i] = (struct rect){ rx, y, w, h };
                rx -= b->margin[3];
            } else {
                lx += b->margin[3];
                L->buttons[i] = (struct rect){ lx, y, w, h };
                lx += w + b->margin[1];
            }
            L->shown[i] = true;
        }
    }
    int tl = lx + L->title.padding[3], tr = rx - L->title.padding[1];
    L->title_area = (struct rect){ tl, L->titlebar.y, MAX(tr - tl, 0), th };
}

struct rect wm_maximized_content(struct toplevel *t) {
    struct rect screen = { 0, 0, srv.width, srv.height };
    if (!t->ssd) return screen;
    struct layout L;
    compute_styles(t, ST_FOCUSED | ST_MAXIMIZED, &L);
    int bw = L.win.border_width, th = bar_height(&L);
    return (struct rect){ bw, th + bw, srv.width - 2 * bw, srv.height - th - 2 * bw };
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
        int m = t->maximized ? 0 : RESIZE_MARGIN;
        struct rect grab_area = { L.frame.x - m, L.frame.y - m, L.frame.w + 2 * m, L.frame.h + 2 * m };
        if (!rect_contains(grab_area, x, y)) continue;
        h.t = t;
        for (int i = 0; i < 3; i++)
            if (L.shown[i] && rect_contains(L.buttons[i], x, y)) { h.part = button_part[i]; return h; }
        if (rect_contains(L.titlebar, x, y)) { h.part = PART_TITLEBAR; return h; }
        h.part = PART_BORDER;
        if (t->maximized) return h;
        struct rect c = L.content;
        if (x < c.x) h.edges |= EDGE_LEFT;
        if (x >= c.x + c.w) h.edges |= EDGE_RIGHT;
        if (y < L.titlebar.y) h.edges |= EDGE_TOP;
        if (y >= c.y + c.h) h.edges |= EDGE_BOTTOM;
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
    wm_damage();
}

static void minimize(struct toplevel *t) {
    t->minimized = true;
    if (focused == t) focus_topmost();
    wm_damage();
}

static bool unmaximize_on_drag;

static void start_move(struct toplevel *t) {
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
    if (t->maximized || t->fullscreen || !edges) return;
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

static void end_grab(void) {
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

/* --------------------------------------------------------- toplevel lifecycle */

void wm_toplevel_new(struct toplevel *t) { (void)t; }

void wm_toplevel_map(struct toplevel *t) {
    struct rect g = xdg_geometry(t->xdg);
    if (t->fullscreen) {
        t->x = t->y = 0;
    } else if (t->maximized) {
        struct rect r = wm_maximized_content(t);
        t->x = r.x;
        t->y = r.y;
    } else if (!t->placed) {
        int top = 0, side = 0;
        if (t->ssd) {
            struct layout L;
            compute_styles(t, ST_FOCUSED, &L);
            top = bar_height(&L) + L.win.border_width;
            side = L.win.border_width;
        }
        if (t->parent && t->parent->xdg && t->parent->xdg->mapped) {   /* dialogs: center on parent */
            struct rect pg = xdg_geometry(t->parent->xdg);
            t->x = t->parent->x + (pg.w - g.w) / 2;
            t->y = t->parent->y + (pg.h - g.h) / 2;
        } else {
            t->x = (srv.width - g.w) / 2 + (cascade - 2) * 32;
            t->y = (srv.height - g.h + top) / 2 + (cascade - 2) * 32;
            cascade = (cascade + 1) % 5;
        }
        t->x = MAX(t->x, side);
        t->y = MAX(t->y, top);
    }
    if (!t->maximized && !t->fullscreen && t->saved.w == 0)
        t->saved = (struct rect){ t->x, t->y, g.w, g.h };
    t->placed = true;
    list_remove(&t->link);
    list_append(&toplevels, &t->link);
    focus_window(t);
}

static struct toplevel *popup_root(struct popup *p) {
    struct xdg_surface *x = p->parent;
    for (int i = 0; x && x->popup && i < 32; i++) x = x->popup->parent;
    return x ? x->toplevel : NULL;
}

static void forget_window(struct toplevel *t) {
    struct popup *p, *tmp;
    list_for_each_safe(p, tmp, &popups, link)
        if (p->mapped && popup_root(p) == t) popup_send_done(p);
    if (hover_win == t) { hover_win = NULL; hover_part = PART_NONE; }
    if (press_win == t) { press_win = NULL; press_part = PART_NONE; }
    if (last_click_win == t) last_click_win = NULL;
    if (grab_win == t) end_grab();
    list_remove(&t->link);
    if (focused == t) {
        focused = NULL;
        focus_topmost();
    }
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

/* --------------------------------------------------------------------- input */

static void process_motion(void) {
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
    if (grab == G_MOVE || grab == G_RESIZE) return;
    seat_pointer_axis(axis, value, discrete);
}

void wm_key(uint32_t key, bool pressed) { seat_keyboard_key(key, pressed); }

/* ------------------------------------------------------------------- actions */

void wm_action_quit(void) { srv.running = false; }

void wm_action_cycle(void) {
    /* bring the bottom-most window to the top; repeated presses walk through all of them */
    if (list_empty(&toplevels)) return;
    struct toplevel *t = container_of(toplevels.next, struct toplevel, link);
    t->minimized = false;
    focus_window(t);
}

void wm_action_close(void) { if (focused) toplevel_send_close(focused); }
void wm_action_maximize(void) { if (focused) maximize(focused, !focused->maximized); }
void wm_action_terminal(void) { if (srv.terminal) spawn(srv.terminal); }

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

static void draw_title(struct canvas *c, struct layout *L, const char *text, float op) {
    const struct font *f = font_get(L->title.font_family);
    if (!f || !text[0] || L->title_area.w <= 0) return;
    int scale = MAX(1, (int)lroundf((float)L->title.font_size / f->height));
    int tw = MIN(text_width(f, scale, text), L->title_area.w);
    int x = L->title_area.x;
    if (L->title.text_align == ALIGN_CENTER) {
        /* center on the whole title bar if that doesn't collide with buttons */
        x = L->titlebar.x + (L->titlebar.w - tw) / 2;
        x = CLAMP(x, L->title_area.x, L->title_area.x + L->title_area.w - tw);
    } else if (L->title.text_align == ALIGN_RIGHT) {
        x = L->title_area.x + L->title_area.w - tw;
    }
    int y = L->titlebar.y + (L->titlebar.h - f->height * scale) / 2 + L->title.padding[0] - L->title.padding[2];
    draw_text(c, f, scale, x, y, text, L->title.color, op, L->title_area.w);
}

static void draw_window(struct canvas *c, struct toplevel *t) {
    struct layout L;
    layout_window(t, &L);
    float op = L.win.opacity;
    struct draw_ctx d = { c, op, NULL, uniform(0) };

    if (L.decorated) {
        struct style *w = &L.win;
        int bw = w->border_width;
        if (w->shadow) {
            struct rect sr = { L.frame.x + w->shadow_x - w->shadow_spread, L.frame.y + w->shadow_y - w->shadow_spread,
                               L.frame.w + 2 * w->shadow_spread, L.frame.h + 2 * w->shadow_spread };
            draw_shadow(c, sr, L.radius + w->shadow_spread, w->shadow_blur, w->shadow_color, op,
                        op >= 1 ? &L.frame : NULL);
        }
        draw_box(c, L.frame, uniform(L.radius), bw, &w->background, w->border_color, op, &L.content);
        float ir = fmaxf(L.radius - bw, 0);
        draw_box(c, L.titlebar, (struct radii){ ir, ir, 0, 0 }, 0, &L.bar.background, (struct color){ 0 }, op, NULL);
        for (int i = 0; i < 3; i++) {
            if (!L.shown[i]) continue;
            struct style *b = &L.btn[i];
            struct rect r = L.buttons[i];
            float br = style_radius(b, r.w, r.h);
            draw_box(c, r, uniform(br), b->border_width, &b->background, b->border_color, op, NULL);
            enum icon ic = i == BTN_IDX_CLOSE ? ICON_CLOSE : i == BTN_IDX_MINIMIZE ? ICON_MINIMIZE :
                           t->maximized ? ICON_RESTORE : ICON_MAXIMIZE;
            draw_icon(c, r, ic, b->color, op);
        }
        draw_title(c, &L, t->title[0] ? t->title : t->app_id, op);
        if (ir > 0) {   /* round the bottom corners of the client's content to match */
            d.clip = &L.content;
            d.clip_rad = (struct radii){ 0, 0, ir, ir };
        } else {
            d.clip = &L.content;
        }
    }
    int sx, sy;
    if (surface_origin(t->xdg, &sx, &sy)) surface_for_each(t->xdg->surface, sx, sy, draw_surface, &d);
}

static void draw_popup(struct canvas *c, struct popup *p) {
    int sx, sy, ox = 0, oy = 0;
    if (!p->xdg || !surface_origin(p->xdg, &sx, &sy) || !xdg_abs_origin(p->xdg, &ox, &oy)) return;
    struct css_elem e = { "popup", NULL, 0, NULL, NULL, NULL };
    struct style st;
    css_compute(&e, NULL, &st);
    struct rect g = xdg_geometry(p->xdg);
    struct rect r = { ox, oy, g.w, g.h };
    float rad = style_radius(&st, r.w, r.h);
    if (st.shadow) {
        struct rect sr = { r.x + st.shadow_x - st.shadow_spread, r.y + st.shadow_y - st.shadow_spread,
                           r.w + 2 * st.shadow_spread, r.h + 2 * st.shadow_spread };
        draw_shadow(c, sr, rad + st.shadow_spread, st.shadow_blur, st.shadow_color, st.opacity, &r);
    }
    struct draw_ctx d = { c, st.opacity, NULL, uniform(0) };
    surface_for_each(p->xdg->surface, sx, sy, draw_surface, &d);
    if (st.border_width > 0) {
        int b = st.border_width;
        struct rect br = { r.x - b, r.y - b, r.w + 2 * b, r.h + 2 * b };
        struct paint none = { .kind = PAINT_NONE };
        draw_box(c, br, uniform(rad + b), b, &none, st.border_color, st.opacity, &r);
    }
}

static void draw_desktop(struct canvas *c) {
    if (!desktop_cache || desktop_w != c->w || desktop_h != c->h) {
        free(desktop_cache);
        desktop_w = c->w;
        desktop_h = c->h;
        desktop_cache = calloc((size_t)c->w * c->h, 4);
        struct canvas dc = { desktop_cache, c->w, c->h, c->w, { 0, 0, c->w, c->h } };
        struct css_elem e = { "desktop", NULL, 0, NULL, NULL, NULL };
        struct style st;
        css_compute(&e, NULL, &st);
        if (st.background.kind == PAINT_NONE) {
            st.background.kind = PAINT_COLOR;
            st.background.color = (struct color){ 0x30, 0x30, 0x38, 255 };
        }
        draw_box(&dc, dc.clip, uniform(0), 0, &st.background, (struct color){ 0 }, 1, NULL);
    }
    for (int y = 0; y < c->h; y++)
        memcpy(c->px + (size_t)y * c->stride, desktop_cache + (size_t)y * c->w, (size_t)c->w * 4);
}

static void draw_pointer(struct canvas *c) {
    int x = (int)ptr_x, y = (int)ptr_y;
    if (client_cursor && seat_pointer_focus()) {
        if (cursor_surface && cursor_surface->pixels) {
            struct draw_ctx d = { c, 1, NULL, uniform(0) };
            surface_for_each(cursor_surface, x - cursor_surface->cursor_hot_x,
                             y - cursor_surface->cursor_hot_y, draw_surface, &d);
        }
        return;   /* NULL cursor surface = the client hid the pointer */
    }
    struct css_elem e = { "cursor", NULL, 0, NULL, NULL, NULL };
    struct style st;
    css_compute(&e, NULL, &st);
    struct color outline = st.border_color.a ? st.border_color : (struct color){ 0, 0, 0, 255 };
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
    draw_pointer(c);
}
