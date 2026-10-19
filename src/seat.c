/* seat.c — wl_seat: the pointer, the keyboard, the clipboard, and drag & drop.
 *
 * Keyboards: evdev gives us key *codes* (physical positions). Turning those into
 * characters depends on layout, Shift, AltGr, compose... — that's XKB. We compile a
 * keymap with libxkbcommon, hand the same keymap to every client as a file descriptor,
 * and then just forward raw key codes and modifier state. Each client runs its own
 * xkbcommon to get characters. The server only uses xkb itself for its keybindings.
 *
 * Clipboard: when a client copies, it creates a wl_data_source listing MIME types and
 * calls set_selection. When another client pastes, it calls receive(mime, fd) on its
 * wl_data_offer; we forward the fd to the source client, which writes the data into
 * it directly. The server never sees the clipboard contents.
 *
 * Drag and drop: with a button held, a client calls start_drag. The server grabs the
 * pointer and, instead of wl_pointer events, sends wl_data_device enter/motion/leave to
 * whatever surface is under it, each with a fresh offer. The target says which MIME type
 * (accept) and actions (set_actions) it would take; on release we send drop, and it
 * fetches the data with receive, the same way a paste does. */
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb;
static int keymap_fd = -1;
static uint32_t keymap_size;

static struct surface *ptr_focus, *kbd_focus;
static uint32_t pressed[32], swallowed[32];
static int npressed, nswallowed;
static uint32_t sent_mods[4];

struct data_source {
    struct object *obj;
    char mimes[16][128];
    int nmimes;
    struct list offers;            /* data_offer objects made from it */
    uint32_t actions;              /* drag actions it supports */
    uint32_t action;               /* the one currently chosen */
    bool accepted;                 /* the drop target accepted a MIME type */
};
static struct data_source *selection;

static const struct iface wl_pointer_iface, wl_keyboard_iface, wl_touch_iface, wl_data_offer_iface;

/* --------------------------------------------------------------- pointer */

static void pointer_frame(struct object *p) {
    if (p->version >= 5) obj_send(p, 5, "");   /* wl_pointer.frame: groups events */
}

#define each_pointer(o, c) list_for_each(o, &(c)->pointers, link)
#define each_keyboard(o, c) list_for_each(o, &(c)->keyboards, link)

void seat_pointer_enter(struct surface *s, double sx, double sy) {
    if (s == ptr_focus) return;
    seat_pointer_leave();
    ptr_focus = s;
    if (!s) return;
    struct object *p;
    uint32_t serial = next_serial();
    each_pointer(p, s->client) {
        obj_send(p, 0, "uoff", serial, s->obj->id, fixed_from_double(sx), fixed_from_double(sy));
        pointer_frame(p);
    }
}

void seat_pointer_leave(void) {
    if (!ptr_focus) return;
    struct object *p;
    uint32_t serial = next_serial();
    each_pointer(p, ptr_focus->client) {
        obj_send(p, 1, "uo", serial, ptr_focus->obj->id);
        pointer_frame(p);
    }
    ptr_focus = NULL;
}

void seat_pointer_motion(double sx, double sy) {
    if (!ptr_focus) return;
    struct object *p;
    uint32_t t = now_ms();
    each_pointer(p, ptr_focus->client) {
        obj_send(p, 2, "uff", t, fixed_from_double(sx), fixed_from_double(sy));
        pointer_frame(p);
    }
}

void seat_pointer_button(uint32_t button, bool down) {
    if (!ptr_focus) return;
    struct object *p;
    uint32_t serial = next_serial(), t = now_ms();
    each_pointer(p, ptr_focus->client) {
        obj_send(p, 3, "uuuu", serial, t, button, down ? 1u : 0u);
        pointer_frame(p);
    }
}

void seat_pointer_axis(int axis, double value, int discrete) {
    if (!ptr_focus) return;
    struct object *p;
    uint32_t t = now_ms();
    each_pointer(p, ptr_focus->client) {
        if (p->version >= 5) obj_send(p, 6, "u", 0u);                          /* axis_source: wheel */
        if (p->version >= 5 && discrete) obj_send(p, 8, "ui", (uint32_t)axis, discrete);   /* axis_discrete */
        obj_send(p, 4, "uuf", t, (uint32_t)axis, fixed_from_double(value));
        pointer_frame(p);
    }
}

struct surface *seat_pointer_focus(void) { return ptr_focus; }

static void pointer_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op != 0) return;   /* set_cursor(serial, surface, hotspot_x, hotspot_y) */
    /* only the client under the pointer gets to pick the cursor */
    if (!ptr_focus || ptr_focus->client != o->client) return;
    struct object *so;
    if (!arg_obj(o, a[1].o, &wl_surface_iface, true, &so)) return;
    struct surface *s = so ? so->data : NULL;
    if (s) {
        if (!surface_set_role(s, ROLE_CURSOR, o, 0)) return;
        s->cursor_hot_x = a[2].i;
        s->cursor_hot_y = a[3].i;
    }
    wm_set_cursor(s, a[2].i, a[3].i);
}

static const char *const pointer_reqs[] = { "set_cursor u?oii", "release" };
static const struct iface wl_pointer_iface = { "wl_pointer", 2, pointer_reqs, 1u << 1, pointer_dispatch, NULL };

/* -------------------------------------------------------------- keyboard */

static void send_modifiers(struct client *c, bool force) {
    uint32_t m[4] = {
        xkb_state_serialize_mods(xkb, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(xkb, XKB_STATE_LAYOUT_EFFECTIVE),
    };
    if (!force && !memcmp(m, sent_mods, sizeof m)) return;
    memcpy(sent_mods, m, sizeof m);
    if (!c) return;
    struct object *k;
    uint32_t serial = next_serial();
    each_keyboard(k, c) obj_send(k, 4, "uuuuu", serial, m[0], m[1], m[2], m[3]);
}

static void send_selection(struct client *c);

static void keyboard_enter(struct object *k, struct surface *s) {
    uint32_t keys[32];
    int n = 0;
    for (int i = 0; i < npressed; i++) {
        bool sw = false;
        for (int j = 0; j < nswallowed; j++) sw |= swallowed[j] == pressed[i];
        if (!sw) keys[n++] = pressed[i];
    }
    struct wire_array arr = { n * 4, keys };
    obj_send(k, 1, "uoa", next_serial(), s->obj->id, &arr);
    obj_send(k, 4, "uuuuu", next_serial(),
             xkb_state_serialize_mods(xkb, XKB_STATE_MODS_DEPRESSED),
             xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LATCHED),
             xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LOCKED),
             xkb_state_serialize_layout(xkb, XKB_STATE_LAYOUT_EFFECTIVE));
}

void seat_keyboard_focus(struct surface *s) {
    if (s == kbd_focus) return;
    struct object *k;
    if (kbd_focus) {
        uint32_t serial = next_serial();
        each_keyboard(k, kbd_focus->client) obj_send(k, 2, "uo", serial, kbd_focus->obj->id);
    }
    struct client *old = kbd_focus ? kbd_focus->client : NULL;
    kbd_focus = s;
    if (!s) return;
    each_keyboard(k, s->client) keyboard_enter(k, s);
    if (s->client != old) send_selection(s->client);
}

struct surface *seat_keyboard_focus_surface(void) { return kbd_focus; }

static bool mod_active(const char *name) {
    return xkb_state_mod_name_is_active(xkb, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

uint32_t seat_modifiers(void) {
    uint32_t m = 0;
    if (mod_active(XKB_MOD_NAME_SHIFT)) m |= MOD_SHIFT;
    if (mod_active(XKB_MOD_NAME_CTRL)) m |= MOD_CTRL;
    if (mod_active(XKB_MOD_NAME_ALT)) m |= MOD_ALT;
    if (mod_active(XKB_MOD_NAME_LOGO)) m |= MOD_SUPER;
    return m;
}

/* The server's own shortcuts. Change them here. */
static bool keybinding(xkb_keysym_t sym, uint32_t mods) {
    if (mods == (MOD_CTRL | MOD_ALT) && sym == XKB_KEY_BackSpace) { wm_action_quit(); return true; }
    if ((mods & ~MOD_SHIFT) == MOD_ALT && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
        wm_action_cycle();
        return true;
    }
    if (mods == MOD_ALT && sym == XKB_KEY_F4) { wm_action_close(); return true; }
    if ((mods == MOD_SUPER && sym == XKB_KEY_Up) || (mods == MOD_ALT && sym == XKB_KEY_F10)) {
        wm_action_maximize();
        return true;
    }
    if ((mods == MOD_SUPER && sym == XKB_KEY_Return) || (mods == (MOD_CTRL | MOD_ALT) && sym == XKB_KEY_t)) {
        wm_action_terminal();
        return true;
    }
    return false;
}

static void list_del(uint32_t *v, int *n, uint32_t key) {
    for (int i = 0; i < *n; i++)
        if (v[i] == key) { v[i] = v[--*n]; return; }
}

bool seat_keyboard_key(uint32_t key, bool down) {
    /* evdev codes are xkb codes minus 8 (an X11 legacy) */
    xkb_state_update_key(xkb, key + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (down) {
        if (npressed < 32) pressed[npressed++] = key;
    } else {
        list_del(pressed, &npressed, key);
    }

    bool eaten = false;
    if (down) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb, key + 8);
        if (keybinding(sym, seat_modifiers())) {
            eaten = true;
            if (nswallowed < 32) swallowed[nswallowed++] = key;
        }
    } else {
        for (int i = 0; i < nswallowed; i++)
            if (swallowed[i] == key) eaten = true;
        list_del(swallowed, &nswallowed, key);
    }

    struct client *c = kbd_focus ? kbd_focus->client : NULL;
    if (!eaten && c) {
        struct object *k;
        uint32_t serial = next_serial(), t = now_ms();
        each_keyboard(k, c) obj_send(k, 3, "uuuu", serial, t, key, down ? 1u : 0u);
    }
    send_modifiers(c, false);
    return eaten;
}

static const char *const keyboard_reqs[] = { "release" };
static const struct iface wl_keyboard_iface = { "wl_keyboard", 1, keyboard_reqs, 1u << 0, NULL, NULL };

static const char *const touch_reqs[] = { "release" };
static const struct iface wl_touch_iface = { "wl_touch", 1, touch_reqs, 1u << 0, NULL, NULL };

/* ------------------------------------------------------------------- seat */

static void seat_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct client *c = o->client;
    struct object *n;
    switch (op) {
    case 0:   /* get_pointer */
        if (!(n = obj_new(c, a[0].n, &wl_pointer_iface, o->version, NULL))) return;
        list_append(&c->pointers, &n->link);
        break;
    case 1:   /* get_keyboard */
        if (!(n = obj_new(c, a[0].n, &wl_keyboard_iface, o->version, NULL))) return;
        list_append(&c->keyboards, &n->link);
        obj_send(n, 0, "uhu", 1u, keymap_fd, keymap_size);   /* keymap: format xkb_v1 */
        if (o->version >= 4) obj_send(n, 5, "ii", 25, 600);   /* repeat_info: 25/s after 600ms */
        if (kbd_focus && kbd_focus->client == c) keyboard_enter(n, kbd_focus);
        break;
    case 2:   /* get_touch: we never report touch capability, but must accept the request */
        obj_new(c, a[0].n, &wl_touch_iface, o->version, NULL);
        break;
    }
}

static const char *const seat_reqs[] = { "get_pointer n", "get_keyboard n", "get_touch n", "release" };
static const struct iface wl_seat_iface = { "wl_seat", 4, seat_reqs, 1u << 3, seat_dispatch, NULL };

static void seat_bind(struct client *c, uint32_t id, uint32_t version) {
    struct object *o = obj_new(c, id, &wl_seat_iface, version, NULL);
    if (!o) return;
    obj_send(o, 0, "u", 3u);                       /* capabilities: pointer | keyboard */
    if (version >= 2) obj_send(o, 1, "s", "seat0");
}

/* ------------------------------------------------- clipboard and drag & drop */

/* Both use the same objects. A data_source is "I have data in these MIME types"; a
 * data_offer is what another client sees of it. Data moves directly from the source to
 * the receiver through a pipe; we only pass the fd along. */

enum { DND_NONE = 0, DND_COPY = 1, DND_MOVE = 2, DND_ASK = 4 };

struct data_offer {
    struct object *obj;
    struct data_source *src;       /* NULL once the source is gone */
    bool dnd, dropped, finished;
    uint32_t actions, preferred, action;
};

static struct {
    bool active;
    struct client *client;         /* who started it */
    struct data_source *source;    /* NULL: a drag inside one client, no data offered */
    struct surface *focus;         /* surface under the pointer */
    struct data_offer *offer;      /* what that surface was offered */
    struct surface *icon;
} drag;

static struct data_offer *new_offer(struct object *dev, struct data_source *src, bool dnd) {
    struct data_offer *off = calloc(1, sizeof *off);
    off->src = src;
    off->dnd = dnd;
    off->obj = obj_new_server(dev->client, &wl_data_offer_iface, dev->version, off);
    if (!off->obj) { free(off); return NULL; }
    list_append(&src->offers, &off->obj->link);
    obj_send(dev, 0, "n", off->obj->id);   /* data_offer: introduces the new object */
    for (int i = 0; i < src->nmimes; i++) obj_send(off->obj, 0, "s", src->mimes[i]);
    if (dnd && off->obj->version >= 3)      /* source_actions */
        obj_send(off->obj, 1, "u", src->obj->version >= 3 ? src->actions : (uint32_t)DND_COPY);
    return off;
}

static void send_selection(struct client *c) {
    struct object *dev;
    list_for_each(dev, &c->data_devices, link) {
        struct data_offer *off = selection ? new_offer(dev, selection, false) : NULL;
        obj_send(dev, 5, "?o", off ? off->obj->id : 0u);   /* selection */
    }
}

static void set_selection(struct data_source *src) {
    if (selection && selection != src) obj_send(selection->obj, 2, "");   /* cancelled */
    selection = src;
    if (kbd_focus) send_selection(kbd_focus->client);
}

/* Pick the drag action both sides allow, honoring the receiver's preference. Version 1-2
 * clients predate actions and mean "copy". */
static void update_action(struct data_offer *off) {
    struct data_source *src = off->src;
    if (!src) return;
    uint32_t offered = src->obj->version >= 3 ? src->actions : DND_COPY;
    uint32_t wanted = off->obj->version >= 3 ? off->actions : DND_COPY;
    uint32_t preferred = off->obj->version >= 3 ? off->preferred : DND_COPY;
    uint32_t both = offered & wanted, chosen = DND_NONE;
    if (both & preferred) chosen = preferred;
    else if (both & DND_COPY) chosen = DND_COPY;
    else if (both & DND_MOVE) chosen = DND_MOVE;
    else if (both & DND_ASK) chosen = DND_ASK;
    if (chosen == off->action) return;
    off->action = src->action = chosen;
    if (off->obj->version >= 3) obj_send(off->obj, 2, "u", chosen);   /* offer.action */
    if (src->obj->version >= 3) obj_send(src->obj, 5, "u", chosen);   /* source.action */
}

static void offer_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct data_offer *off = o->data;
    struct data_source *src = off->src;
    if (!src) return;
    switch (op) {
    case 0:   /* accept(serial, mime): which type the drop target would take, or null */
        if (!off->dnd) break;
        src->accepted = a[1].s != NULL;
        obj_send(src->obj, 0, "?s", a[1].s);   /* source.target */
        break;
    case 1:   /* receive(mime, fd): hand the fd to the source; it writes the data */
        obj_send(src->obj, 1, "sh", a[0].s, a[1].h);
        break;
    case 3:   /* finish: the drop target has everything */
        if (off->dnd && off->dropped && !off->finished) {
            off->finished = true;
            if (src->obj->version >= 3) obj_send(src->obj, 4, "");   /* dnd_finished */
        }
        break;
    case 4:   /* set_actions(actions, preferred) */
        off->actions = a[0].u & (DND_COPY | DND_MOVE | DND_ASK);
        off->preferred = a[1].u;
        update_action(off);
        break;
    }
}

static void offer_destroy(struct object *o) {
    struct data_offer *off = o->data;
    if (drag.offer == off) drag.offer = NULL;
    /* a drop target that never said finish: older ones never do, newer ones gave up */
    if (off->src && off->dnd && off->dropped && !off->finished && off->src->obj->version >= 3)
        obj_send(off->src->obj, off->obj->version >= 3 ? 2 : 4, "");   /* cancelled / dnd_finished */
    free(off);
}

static const char *const offer_reqs[] = { "accept u?s", "receive sh", "destroy", "finish", "set_actions uu" };
static const struct iface wl_data_offer_iface = {
    "wl_data_offer", 5, offer_reqs, 1u << 2, offer_dispatch, offer_destroy };

static void drag_end(void);

static void source_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct data_source *src = o->data;
    if (op == 0 && src->nmimes < 16) snprintf(src->mimes[src->nmimes++], 128, "%s", a[0].s);
    if (op == 2) src->actions = a[0].u & (DND_COPY | DND_MOVE | DND_ASK);
}

static void source_destroy(struct object *o) {
    struct data_source *src = o->data;
    struct object *offer, *tmp;
    list_for_each_safe(offer, tmp, &src->offers, link) {
        ((struct data_offer *)offer->data)->src = NULL;
        list_remove(&offer->link);
    }
    if (drag.active && drag.source == src) {   /* the dragging client gave up */
        drag.source = NULL;
        seat_drag_cancel();
    }
    if (selection == src) {
        selection = NULL;
        if (kbd_focus && kbd_focus->client != o->client) send_selection(kbd_focus->client);
    }
    free(src);
}

static const char *const source_reqs[] = { "offer s", "destroy", "set_actions u" };
static const struct iface wl_data_source_iface = {
    "wl_data_source", 3, source_reqs, 1u << 1, source_dispatch, source_destroy };

/* ------------------------------------------------------------- drag session */

#define each_device(o, c) list_for_each(o, &(c)->data_devices, link)

static void drag_leave(void) {
    if (!drag.focus) return;
    struct object *dev;
    each_device(dev, drag.focus->client) obj_send(dev, 2, "");   /* leave */
    drag.focus = NULL;
    drag.offer = NULL;
}

static void drag_enter(struct surface *s, double sx, double sy) {
    drag.focus = s;
    if (drag.source) {
        drag.source->accepted = false;
        drag.source->action = DND_NONE;
    }
    uint32_t serial = next_serial();
    struct object *dev;
    each_device(dev, s->client) {
        struct data_offer *off = drag.source ? new_offer(dev, drag.source, true) : NULL;
        if (off) drag.offer = off;
        obj_send(dev, 1, "uoff?o", serial, s->obj->id, fixed_from_double(sx), fixed_from_double(sy),
                 off ? off->obj->id : 0u);   /* enter */
    }
}

static void drag_end(void) {
    drag.active = false;
    drag.source = NULL;
    drag.client = NULL;
    drag.icon = NULL;
    drag.focus = NULL;
    drag.offer = NULL;
    wm_drag_ended();
}

bool seat_drag_active(void) { return drag.active; }
struct surface *seat_drag_icon(void) { return drag.icon; }

void seat_drag_motion(struct surface *s, double sx, double sy) {
    if (!drag.active) return;
    if (s && !drag.source && s->client != drag.client) s = NULL;   /* source-less drags stay home */
    if (s != drag.focus) {
        drag_leave();
        if (s) drag_enter(s, sx, sy);
        return;
    }
    if (!s) return;
    struct object *dev;
    uint32_t t = now_ms();
    each_device(dev, s->client) obj_send(dev, 3, "uff", t, fixed_from_double(sx), fixed_from_double(sy));
}

/* button released: drop if the target accepted a type and an action, else cancel */
void seat_drag_drop(void) {
    if (!drag.active) return;
    struct data_source *src = drag.source;
    bool ok = drag.focus && (!src || (src->accepted && (src->obj->version < 3 || src->action != DND_NONE)));
    if (ok) {
        struct object *dev;
        each_device(dev, drag.focus->client) obj_send(dev, 4, "");   /* drop */
        if (drag.offer) drag.offer->dropped = true;
        if (src && src->obj->version >= 3) obj_send(src->obj, 3, "");   /* dnd_drop_performed */
        /* no leave after a drop: the target still needs its offer to receive the data */
        drag.focus = NULL;
    } else {
        if (src) obj_send(src->obj, 2, "");   /* cancelled */
        drag_leave();
    }
    drag_end();
}

void seat_drag_cancel(void) {
    if (!drag.active) return;
    if (drag.source) obj_send(drag.source->obj, 2, "");
    drag_leave();
    drag_end();
}

static void device_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct object *so;
    if (op == 0) {          /* start_drag(source, origin, icon, serial) */
        struct object *origin, *icon;
        if (!arg_obj(o, a[0].o, &wl_data_source_iface, true, &so)) return;
        if (!arg_obj(o, a[1].o, &wl_surface_iface, false, &origin)) return;
        if (!arg_obj(o, a[2].o, &wl_surface_iface, true, &icon)) return;
        struct surface *icon_s = icon ? icon->data : NULL;
        if (icon_s && !surface_set_role(icon_s, ROLE_DND_ICON, o, 0)) return;   /* error: role */
        /* only while this client holds the pointer with a button down */
        if (drag.active || !wm_start_drag(o->client)) {
            if (so) obj_send(so, 2, "");   /* cancelled */
            return;
        }
        drag = (typeof(drag)){ .active = true, .client = o->client, .source = so ? so->data : NULL,
                               .icon = icon_s };
        wm_drag_started();
    } else if (op == 1) {   /* set_selection(source, serial) */
        if (!arg_obj(o, a[0].o, &wl_data_source_iface, true, &so)) return;
        set_selection(so ? so->data : NULL);
    }
}

static const char *const device_reqs[] = { "start_drag ?oo?ou", "set_selection ?ou", "release" };
static const struct iface wl_data_device_iface = {
    "wl_data_device", 3, device_reqs, 1u << 2, device_dispatch, NULL };

static void ddm_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct client *c = o->client;
    if (op == 0) {
        struct data_source *src = calloc(1, sizeof *src);
        list_init(&src->offers);
        src->obj = obj_new(c, a[0].n, &wl_data_source_iface, o->version, src);
        if (!src->obj) free(src);
    } else {
        struct object *dev = obj_new(c, a[0].n, &wl_data_device_iface, o->version, NULL);
        if (!dev) return;
        list_append(&c->data_devices, &dev->link);
        if (kbd_focus && kbd_focus->client == c) send_selection(c);
    }
}

static const char *const ddm_reqs[] = { "create_data_source n", "get_data_device no" };
static const struct iface wl_data_device_manager_iface = {
    "wl_data_device_manager", 2, ddm_reqs, 0, ddm_dispatch, NULL };

static void ddm_bind(struct client *c, uint32_t id, uint32_t version) {
    obj_new(c, id, &wl_data_device_manager_iface, version, NULL);
}

/* ------------------------------------------------------------- lifecycle */

void seat_surface_destroyed(struct surface *s) {
    if (ptr_focus == s) ptr_focus = NULL;
    if (kbd_focus == s) kbd_focus = NULL;
    if (drag.focus == s) { drag.focus = NULL; drag.offer = NULL; }
    if (drag.icon == s) drag.icon = NULL;
}

void seat_client_destroyed(struct client *c) {
    if (ptr_focus && ptr_focus->client == c) ptr_focus = NULL;
    if (kbd_focus && kbd_focus->client == c) kbd_focus = NULL;
    if (drag.focus && drag.focus->client == c) { drag.focus = NULL; drag.offer = NULL; }
    if (drag.active && drag.client == c) seat_drag_cancel();
}

void seat_init(void) {
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    /* NULL names = defaults, overridable with XKB_DEFAULT_LAYOUT=de etc. */
    keymap = xkb_ctx ? xkb_keymap_new_from_names(xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (!keymap) {
        fprintf(stderr, "pane: can't compile an XKB keymap (is xkeyboard-config installed?)\n");
        exit(1);
    }
    xkb = xkb_state_new(keymap);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    keymap_size = strlen(text) + 1;
    keymap_fd = memfd_create("pane-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (keymap_fd < 0 || write(keymap_fd, text, keymap_size) != (ssize_t)keymap_size) {
        perror("keymap memfd");
        exit(1);
    }
    /* every client maps this same file; seal it so none of them can change it */
    fcntl(keymap_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    free(text);

    global_add(&wl_seat_iface, 7, seat_bind);
    global_add(&wl_data_device_manager_iface, 3, ddm_bind);
}

void seat_finish(void) {
    if (keymap_fd >= 0) close(keymap_fd);
    xkb_state_unref(xkb);
    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_ctx);
}
