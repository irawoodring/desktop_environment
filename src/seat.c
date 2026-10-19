/* seat.c — wl_seat: the pointer, the keyboard, and the clipboard.
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
 * it directly. The server never sees the clipboard contents. */
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
    struct list offers;
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

/* -------------------------------------------------------------- clipboard */

static void send_selection(struct client *c) {
    struct object *dev;
    list_for_each(dev, &c->data_devices, link) {
        if (!selection) {
            obj_send(dev, 5, "?o", 0u);   /* selection(null) */
            continue;
        }
        struct object *offer = obj_new_server(c, &wl_data_offer_iface, dev->version, selection);
        if (!offer) continue;
        list_append(&selection->offers, &offer->link);
        obj_send(dev, 0, "n", offer->id);   /* data_offer: introduces the new object */
        for (int i = 0; i < selection->nmimes; i++) obj_send(offer, 0, "s", selection->mimes[i]);
        obj_send(dev, 5, "?o", offer->id);
    }
}

static void set_selection(struct data_source *src) {
    if (selection && selection != src) obj_send(selection->obj, 2, "");   /* cancelled */
    selection = src;
    if (kbd_focus) send_selection(kbd_focus->client);
}

static void offer_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct data_source *src = o->data;
    if (op == 1 && src)   /* receive(mime, fd): hand the fd to the source; it writes the data */
        obj_send(src->obj, 1, "sh", a[0].s, a[1].h);
}

static const char *const offer_reqs[] = { "accept u?s", "receive sh", "destroy", "finish", "set_actions uu" };
static const struct iface wl_data_offer_iface = {
    "wl_data_offer", 5, offer_reqs, 1u << 2, offer_dispatch, NULL };

static void source_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct data_source *src = o->data;
    if (op == 0 && src->nmimes < 16) snprintf(src->mimes[src->nmimes++], 128, "%s", a[0].s);
}

static void source_destroy(struct object *o) {
    struct data_source *src = o->data;
    struct object *offer, *tmp;
    list_for_each_safe(offer, tmp, &src->offers, link) {
        offer->data = NULL;
        list_remove(&offer->link);
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

static void device_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct object *so;
    if (op == 0) {          /* start_drag: drag-and-drop isn't implemented; cancel it */
        if (!arg_obj(o, a[0].o, &wl_data_source_iface, true, &so)) return;
        if (so) obj_send(so, 2, "");
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
}

void seat_client_destroyed(struct client *c) {
    if (ptr_focus && ptr_focus->client == c) ptr_focus = NULL;
    if (kbd_focus && kbd_focus->client == c) kbd_focus = NULL;
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
