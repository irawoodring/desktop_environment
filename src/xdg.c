/* xdg.c — xdg-shell (windows and popups) and xdg-decoration.
 *
 * xdg_wm_base turns a wl_surface into an xdg_surface, which then becomes either a
 * toplevel (a normal window) or a popup (menus, tooltips, dropdowns).
 *
 * The configure dance: the server proposes a state (size, maximized, focused...) with
 * xdg_toplevel.configure followed by xdg_surface.configure(serial). The client redraws,
 * acks the serial, and commits. A brand-new window starts by committing with no buffer
 * ("initial commit"); that's our cue to send the first configure. */
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WM_ERR_ROLE = 0, WM_ERR_INVALID_POPUP_PARENT = 3, WM_ERR_INVALID_SURFACE_STATE = 4,
       WM_ERR_INVALID_POSITIONER = 5 };
enum { XS_ERR_NOT_CONSTRUCTED = 1, XS_ERR_ALREADY_CONSTRUCTED = 2, XS_ERR_UNCONFIGURED_BUFFER = 3,
       XS_ERR_INVALID_SIZE = 5 };
enum { STATE_MAXIMIZED = 1, STATE_FULLSCREEN = 2, STATE_RESIZING = 3, STATE_ACTIVATED = 4 };

static const struct iface xdg_toplevel_iface, xdg_popup_iface, xdg_surface_iface,
    xdg_positioner_iface;

/* ------------------------------------------------------------------- helpers */

struct rect xdg_geometry(struct xdg_surface *x) {
    if (x->has_geometry) return x->geometry;
    struct surface *s = x->surface;
    return (struct rect){ 0, 0, s ? s->width : 0, s ? s->height : 0 };
}

static void send_xdg_configure(struct xdg_surface *x) {
    x->configure_serial = next_serial();
    x->sent_configure = true;
    obj_send(x->obj, 0, "u", x->configure_serial);   /* xdg_surface.configure */
}

void toplevel_send_configure(struct toplevel *t, int w, int h) {
    if (!t->xdg) return;
    uint32_t states[4];
    int n = 0;
    if (t->maximized) states[n++] = STATE_MAXIMIZED;
    if (t->fullscreen) states[n++] = STATE_FULLSCREEN;
    if (t->resizing) states[n++] = STATE_RESIZING;
    if (t->activated) states[n++] = STATE_ACTIVATED;
    struct wire_array arr = { n * 4, states };
    t->conf_w = w;
    t->conf_h = h;
    if (t->decoration) obj_send(t->decoration, 0, "u", t->ssd ? 2u : 1u);   /* decoration.configure */
    obj_send(t->obj, 0, "iia", w, h, &arr);                                  /* xdg_toplevel.configure */
    send_xdg_configure(t->xdg);
}

void toplevel_send_close(struct toplevel *t) { obj_send(t->obj, 1, ""); }

void popup_send_done(struct popup *p) {
    if (p->mapped) wm_popup_unmap(p);
    obj_send(p->obj, 1, "");   /* xdg_popup.popup_done */
}

bool xdg_abs_origin(struct xdg_surface *x, int *ox, int *oy) {
    for (int depth = 0; x && depth < 32; depth++) {
        if (x->toplevel) {
            if (!x->mapped || x->toplevel->minimized) return false;
            *ox += x->toplevel->x;
            *oy += x->toplevel->y;
            return true;
        }
        if (!x->popup) return false;
        *ox += x->popup->geom.x;
        *oy += x->popup->geom.y;
        x = x->popup->parent;
    }
    return false;
}

/* --------------------------------------------------------------- positioner */

/* where on the anchor rect the popup attaches (anchor), and which way it grows (gravity) */
static void anchor_point(struct rect r, uint32_t anchor, int *x, int *y) {
    *x = r.x + r.w / 2;
    *y = r.y + r.h / 2;
    switch (anchor) {
    case 1: *y = r.y; break;                              /* top */
    case 2: *y = r.y + r.h; break;                        /* bottom */
    case 3: *x = r.x; break;                              /* left */
    case 4: *x = r.x + r.w; break;                        /* right */
    case 5: *x = r.x; *y = r.y; break;                    /* top_left */
    case 6: *x = r.x; *y = r.y + r.h; break;              /* bottom_left */
    case 7: *x = r.x + r.w; *y = r.y; break;              /* top_right */
    case 8: *x = r.x + r.w; *y = r.y + r.h; break;        /* bottom_right */
    }
}

static struct rect place(const struct positioner *p, uint32_t anchor, uint32_t gravity) {
    int ax, ay;
    anchor_point(p->anchor_rect, anchor, &ax, &ay);
    struct rect r = { ax - p->w / 2, ay - p->h / 2, p->w, p->h };
    if (gravity == 1 || gravity == 5 || gravity == 7) r.y = ay - p->h;   /* grows up */
    if (gravity == 2 || gravity == 6 || gravity == 8) r.y = ay;          /* grows down */
    if (gravity == 3 || gravity == 5 || gravity == 6) r.x = ax - p->w;   /* grows left */
    if (gravity == 4 || gravity == 7 || gravity == 8) r.x = ax;          /* grows right */
    r.x += p->off_x;
    r.y += p->off_y;
    return r;
}

static uint32_t flip_x(uint32_t v) {
    static const uint32_t m[] = { 0, 1, 2, 4, 3, 7, 8, 5, 6 };
    return v < 9 ? m[v] : v;
}
static uint32_t flip_y(uint32_t v) {
    static const uint32_t m[] = { 0, 2, 1, 3, 4, 6, 5, 8, 7 };
    return v < 9 ? m[v] : v;
}

/* Compute the popup rectangle (relative to the parent's window geometry), then nudge it
 * so it stays on screen: flip to the other side if allowed, else slide. */
void popup_place(struct popup *p) {
    const struct positioner *pos = &p->pos;
    struct rect r = place(pos, pos->anchor, pos->gravity);
    int ox = 0, oy = 0;
    if (!p->parent || !xdg_abs_origin(p->parent, &ox, &oy)) {
        p->geom = r;
        return;
    }
    int sw = srv.width, sh = srv.height;
    bool out_x = ox + r.x < 0 || ox + r.x + r.w > sw;
    bool out_y = oy + r.y < 0 || oy + r.y + r.h > sh;
    if (out_x && (pos->adjust & 4)) {   /* flip_x */
        struct rect f = place(pos, flip_x(pos->anchor), flip_x(pos->gravity));
        if (ox + f.x >= 0 && ox + f.x + f.w <= sw) { r.x = f.x; out_x = false; }
    }
    if (out_y && (pos->adjust & 8)) {   /* flip_y */
        struct rect f = place(pos, flip_y(pos->anchor), flip_y(pos->gravity));
        if (oy + f.y >= 0 && oy + f.y + f.h <= sh) { r.y = f.y; out_y = false; }
    }
    if (out_x && (pos->adjust & 1)) {   /* slide_x */
        if (ox + r.x + r.w > sw) r.x = sw - ox - r.w;
        if (ox + r.x < 0) r.x = -ox;
    }
    if (out_y && (pos->adjust & 2)) {   /* slide_y */
        if (oy + r.y + r.h > sh) r.y = sh - oy - r.h;
        if (oy + r.y < 0) r.y = -oy;
    }
    p->geom = r;
}

static void positioner_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct positioner *p = o->data;
    switch (op) {
    case 1:
        if (a[0].i <= 0 || a[1].i <= 0) { client_error(o, 0, "invalid size"); return; }
        p->w = a[0].i;
        p->h = a[1].i;
        break;
    case 2:
        if (a[2].i < 0 || a[3].i < 0) { client_error(o, 0, "invalid anchor rect"); return; }
        p->anchor_rect = (struct rect){ a[0].i, a[1].i, a[2].i, a[3].i };
        break;
    case 3: p->anchor = a[0].u; break;
    case 4: p->gravity = a[0].u; break;
    case 5: p->adjust = a[0].u; break;
    case 6: p->off_x = a[0].i; p->off_y = a[1].i; break;
    default: break;   /* reactive, parent_size, parent_configure: hints we don't need */
    }
}

static void positioner_destroy(struct object *o) { free(o->data); }

static const char *const positioner_reqs[] = {
    "destroy", "set_size ii", "set_anchor_rect iiii", "set_anchor u", "set_gravity u",
    "set_constraint_adjustment u", "set_offset ii", "set_reactive", "set_parent_size ii",
    "set_parent_configure u",
};
static const struct iface xdg_positioner_iface = {
    "xdg_positioner", 10, positioner_reqs, 1u << 0, positioner_dispatch, positioner_destroy };

/* ------------------------------------------------------------------ toplevel */

static void toplevel_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct toplevel *t = o->data;
    struct object *obj;
    switch (op) {
    case 1:   /* set_parent */
        if (!arg_obj(o, a[0].o, &xdg_toplevel_iface, true, &obj)) return;
        t->parent = obj ? obj->data : NULL;
        break;
    case 2:
        snprintf(t->title, sizeof t->title, "%s", a[0].s);
        wm_toplevel_title_changed(t);
        break;
    case 3:
        snprintf(t->app_id, sizeof t->app_id, "%s", a[0].s);
        wm_toplevel_title_changed(t);
        break;
    case 4: wm_show_window_menu(t, a[2].i, a[3].i); break;   /* show_window_menu(seat, serial, x, y) */
    case 5: wm_toplevel_request(t, REQ_MOVE, 0); break;
    case 6: wm_toplevel_request(t, REQ_RESIZE, a[2].u); break;
    case 7: t->max_w = a[0].i; t->max_h = a[1].i; break;
    case 8: t->min_w = a[0].i; t->min_h = a[1].i; break;
    case 9: wm_toplevel_request(t, REQ_MAXIMIZE, 0); break;
    case 10: wm_toplevel_request(t, REQ_UNMAXIMIZE, 0); break;
    case 11: wm_toplevel_request(t, REQ_FULLSCREEN, 0); break;
    case 12: wm_toplevel_request(t, REQ_UNFULLSCREEN, 0); break;
    case 13: wm_toplevel_request(t, REQ_MINIMIZE, 0); break;
    }
}

static void toplevel_destroy(struct object *o) {
    struct toplevel *t = o->data;
    wm_toplevel_destroy(t);
    if (t->decoration) t->decoration->data = NULL;
    if (t->xdg) {
        t->xdg->toplevel = NULL;
        t->xdg->mapped = false;
    }
    free(t);
}

static const char *const toplevel_reqs[] = {
    "destroy", "set_parent ?o", "set_title s", "set_app_id s", "show_window_menu ouii",
    "move ou", "resize ouu", "set_max_size ii", "set_min_size ii", "set_maximized",
    "unset_maximized", "set_fullscreen ?o", "unset_fullscreen", "set_minimized",
};
static const struct iface xdg_toplevel_iface = {
    "xdg_toplevel", 14, toplevel_reqs, 1u << 0, toplevel_dispatch, toplevel_destroy };

/* --------------------------------------------------------------------- popup */

static void popup_configure(struct popup *p) {
    popup_place(p);
    obj_send(p->obj, 0, "iiii", p->geom.x, p->geom.y, p->geom.w, p->geom.h);   /* xdg_popup.configure */
    send_xdg_configure(p->xdg);
}

static void popup_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct popup *p = o->data;
    if (op == 1) {          /* grab: menus want all input until dismissed */
        p->grab = true;
        if (p->mapped) wm_popup_grab(p);
    } else if (op == 2) {   /* reposition(positioner, token) */
        struct object *po;
        if (!arg_obj(o, a[0].o, &xdg_positioner_iface, false, &po)) return;
        p->pos = *(struct positioner *)po->data;
        if (o->version >= 3) obj_send(o, 2, "u", a[1].u);   /* repositioned */
        if (p->xdg) popup_configure(p);
    }
}

static void popup_destroy(struct object *o) {
    struct popup *p = o->data;
    if (p->mapped) wm_popup_unmap(p);
    list_remove(&p->link);
    if (p->xdg) {
        p->xdg->popup = NULL;
        p->xdg->mapped = false;
    }
    free(p);
}

static const char *const popup_reqs[] = { "destroy", "grab ou", "reposition ou" };
static const struct iface xdg_popup_iface = {
    "xdg_popup", 3, popup_reqs, 1u << 0, popup_dispatch, popup_destroy };

/* --------------------------------------------------------------- xdg_surface */

void xdg_surface_commit(struct xdg_surface *x) {
    struct surface *s = x->surface;
    if (x->geometry_pending) {
        x->geometry = x->pending_geometry;
        x->has_geometry = true;
        x->geometry_pending = false;
    }
    if (!x->toplevel && !x->popup) return;
    bool has_buffer = s->pixels != NULL;

    if (!x->sent_configure) {   /* the initial commit */
        if (has_buffer) {
            client_error(x->obj, XS_ERR_UNCONFIGURED_BUFFER, "buffer committed before configure");
            return;
        }
        if (x->toplevel) {
            struct toplevel *t = x->toplevel;
            int w = 0, h = 0;   /* 0x0 = "pick your own size" */
            if (t->want_fullscreen) {
                t->fullscreen = true;
                w = srv.width;
                h = srv.height;
            } else if (t->want_maximized) {
                t->maximized = true;
                struct rect r = wm_maximized_content(t);
                w = r.w;
                h = r.h;
            }
            toplevel_send_configure(t, w, h);
        } else {
            popup_configure(x->popup);
        }
        return;
    }
    if (has_buffer && !x->acked) {
        client_error(x->obj, XS_ERR_UNCONFIGURED_BUFFER, "buffer committed before ack_configure");
        return;
    }
    if (has_buffer && !x->mapped) {
        x->mapped = true;
        if (!s->sent_enter) {
            output_send_enter(s->client, s->obj);
            s->sent_enter = true;
        }
        if (x->toplevel) wm_toplevel_map(x->toplevel);
        else {
            x->popup->mapped = true;
            wm_popup_map(x->popup);
        }
    } else if (!has_buffer && x->mapped) {
        /* unmapping resets the surface; the client starts over with an initial commit */
        x->mapped = false;
        x->sent_configure = x->acked = false;
        if (x->toplevel) wm_toplevel_unmap(x->toplevel);
        else if (x->popup->mapped) wm_popup_unmap(x->popup);
    } else if (x->mapped && x->toplevel) {
        wm_toplevel_committed(x->toplevel);
    }
}

static void xdg_surface_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct xdg_surface *x = o->data;
    if (!x->surface && op != 0) return;
    switch (op) {
    case 1: {   /* get_toplevel */
        if (x->toplevel || x->popup) {
            client_error(o, XS_ERR_ALREADY_CONSTRUCTED, "already has a role");
            return;
        }
        struct toplevel *t = calloc(1, sizeof *t);
        t->xdg = x;
        list_init(&t->link);
        t->obj = obj_new(o->client, a[0].n, &xdg_toplevel_iface, o->version, t);
        if (!t->obj) { free(t); return; }
        x->toplevel = t;
        wm_toplevel_new(t);
        break;
    }
    case 2: {   /* get_popup(id, parent, positioner) */
        if (x->toplevel || x->popup) {
            client_error(o, XS_ERR_ALREADY_CONSTRUCTED, "already has a role");
            return;
        }
        struct object *parent, *po;
        if (!arg_obj(o, a[1].o, &xdg_surface_iface, true, &parent)) return;
        if (!arg_obj(o, a[2].o, &xdg_positioner_iface, false, &po)) return;
        if (!parent) {
            client_error(o, WM_ERR_INVALID_POPUP_PARENT, "popups need a parent here");
            return;
        }
        struct positioner *pos = po->data;
        if (pos->w <= 0 || pos->h <= 0) {
            client_error(o, WM_ERR_INVALID_POSITIONER, "positioner has no size");
            return;
        }
        struct popup *p = calloc(1, sizeof *p);
        p->xdg = x;
        p->parent = parent->data;
        p->pos = *pos;
        list_init(&p->link);
        p->obj = obj_new(o->client, a[0].n, &xdg_popup_iface, o->version, p);
        if (!p->obj) { free(p); return; }
        x->popup = p;
        wm_popup_new(p);
        break;
    }
    case 3:     /* set_window_geometry */
        if (a[2].i <= 0 || a[3].i <= 0) {
            client_error(o, XS_ERR_INVALID_SIZE, "invalid geometry");
            return;
        }
        x->pending_geometry = (struct rect){ a[0].i, a[1].i, a[2].i, a[3].i };
        x->geometry_pending = true;
        break;
    case 4:     /* ack_configure */
        if (a[0].u <= x->configure_serial) {
            x->acked = true;
            x->acked_serial = a[0].u;
        }
        break;
    }
}

static void xdg_surface_destroy(struct object *o) {
    struct xdg_surface *x = o->data;
    wm_xdg_surface_destroyed(x);
    if (x->toplevel) { x->toplevel->xdg = NULL; wm_toplevel_destroy(x->toplevel); }
    if (x->popup) {
        if (x->popup->mapped) wm_popup_unmap(x->popup);
        x->popup->xdg = NULL;
    }
    if (x->surface) x->surface->xdg = NULL;
    free(x);
}

static const char *const xdg_surface_reqs[] = {
    "destroy", "get_toplevel n", "get_popup n?oo", "set_window_geometry iiii", "ack_configure u" };
static const struct iface xdg_surface_iface = {
    "xdg_surface", 5, xdg_surface_reqs, 1u << 0, xdg_surface_dispatch, xdg_surface_destroy };

/* --------------------------------------------------------------- xdg_wm_base */

static void wm_base_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op == 1) {   /* create_positioner */
        struct positioner *p = calloc(1, sizeof *p);
        if (!obj_new(o->client, a[0].n, &xdg_positioner_iface, o->version, p)) free(p);
    } else if (op == 2) {   /* get_xdg_surface(id, surface) */
        struct object *so;
        if (!arg_obj(o, a[1].o, &wl_surface_iface, false, &so)) return;
        struct surface *s = so->data;
        if (s->xdg) {
            client_error(o, WM_ERR_ROLE, "surface already has an xdg_surface");
            return;
        }
        if (s->pixels) {
            client_error(o, WM_ERR_INVALID_SURFACE_STATE, "surface already has a buffer");
            return;
        }
        if (!surface_set_role(s, ROLE_XDG, o, WM_ERR_ROLE)) return;
        struct xdg_surface *x = calloc(1, sizeof *x);
        x->surface = s;
        x->obj = obj_new(o->client, a[0].n, &xdg_surface_iface, o->version, x);
        if (!x->obj) { free(x); return; }
        s->xdg = x;
    }
    /* op 3, pong: we never ping, so nothing to do */
}

static const char *const wm_base_reqs[] = {
    "destroy", "create_positioner n", "get_xdg_surface no", "pong u" };
static const struct iface xdg_wm_base_iface = {
    "xdg_wm_base", 4, wm_base_reqs, 1u << 0, wm_base_dispatch, NULL };

static void wm_base_bind(struct client *c, uint32_t id, uint32_t version) {
    obj_new(c, id, &xdg_wm_base_iface, version, NULL);
}

/* ----------------------------------------------------------- xdg-decoration */

/* Clients that support it ask "who draws the title bar?". We say: us (server side),
 * unless the client insists on drawing its own. */
static void decoration_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct toplevel *t = o->data;
    if (!t) return;
    bool ssd = op == 1 ? a[0].u == 2 : true;   /* set_mode(mode) / unset_mode */
    t->ssd = ssd;
    if (t->xdg && t->xdg->sent_configure) toplevel_send_configure(t, t->conf_w, t->conf_h);
    wm_damage();
}

static void decoration_destroy(struct object *o) {
    struct toplevel *t = o->data;
    if (!t) return;
    t->decoration = NULL;
    t->ssd = false;
    wm_damage();
}

static const char *const decoration_reqs[] = { "destroy", "set_mode u", "unset_mode" };
static const struct iface decoration_iface = {
    "zxdg_toplevel_decoration_v1", 3, decoration_reqs, 1u << 0, decoration_dispatch, decoration_destroy };

static void decoration_manager_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op != 1) return;
    struct object *to;
    if (!arg_obj(o, a[1].o, &xdg_toplevel_iface, false, &to)) return;
    struct toplevel *t = to->data;
    if (t->decoration) {
        client_error(o, 1, "toplevel already has a decoration object");   /* already_constructed */
        return;
    }
    t->decoration = obj_new(o->client, a[0].n, &decoration_iface, 1, t);
    if (!t->decoration) return;
    t->ssd = true;
    if (t->xdg && t->xdg->sent_configure) toplevel_send_configure(t, t->conf_w, t->conf_h);
}

static const char *const decoration_manager_reqs[] = { "destroy", "get_toplevel_decoration no" };
static const struct iface decoration_manager_iface = {
    "zxdg_decoration_manager_v1", 2, decoration_manager_reqs, 1u << 0, decoration_manager_dispatch, NULL };

static void decoration_manager_bind(struct client *c, uint32_t id, uint32_t version) {
    obj_new(c, id, &decoration_manager_iface, version, NULL);
}

void xdg_init_globals(void) {
    global_add(&xdg_wm_base_iface, 3, wm_base_bind);
    global_add(&decoration_manager_iface, 1, decoration_manager_bind);
}
