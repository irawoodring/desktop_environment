/* server.h — shared declarations for the pane display server.
 *
 * Module map:
 *   main.c        startup, socket, launching apps, signal handling, the frame clock
 *   event.c       epoll event loop
 *   core.c        clients, object table, wl_display / wl_registry / wl_output / globals
 *   compositor.c  wl_compositor, wl_surface, wl_region, wl_subcompositor, wl_shm (pixels)
 *   xdg.c         xdg_wm_base: windows (toplevels), popups, positioners, decorations
 *   seat.c        wl_seat, wl_pointer, wl_keyboard (xkbcommon), clipboard (wl_data_device)
 *   wm.c          window management: stacking, focus, input routing, move/resize, drawing
 *   input.c       reading /dev/input/event* (evdev)
 *   render.c      software rasterizer: boxes, gradients, shadows, text, image blits
 *   css.c         the CSS engine that styles everything the server draws
 *   font.c        PSF console-font loader
 *   backend_*.c   getting pixels on screen: DRM/KMS, fbdev, or headless (testing)
 */
#pragma once
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "list.h"
#include "wire.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

struct rect { int x, y, w, h; };
static inline bool rect_contains(struct rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ===================================================================== event.c */
typedef void (*watch_fn)(int fd, uint32_t events, void *data);
struct watch;
void loop_init(void);
struct watch *watch_add(int fd, uint32_t events, watch_fn fn, void *data);
void watch_mod(struct watch *w, uint32_t events);
void watch_remove(struct watch *w);
void loop_run(volatile bool *running, void (*before_wait)(void));
uint32_t now_ms(void);

/* ====================================================================== core.c */
struct object;
struct client;

/* One Wayland interface as the server implements it. */
struct iface {
    const char *name;
    int nreqs;
    const char *const *reqs;       /* signature of each request, indexed by opcode */
    uint32_t destructors;          /* bit N set: request N destroys the object */
    void (*dispatch)(struct object *o, uint16_t op, union wire_arg *a);
    void (*destroy)(struct object *o);   /* free the server-side state */
};

struct object {
    uint32_t id, version;
    const struct iface *iface;
    struct client *client;
    void *data;
    struct list link;              /* free for whichever list wants to hold this object */
};

struct client {
    struct list link;
    struct conn conn;
    struct watch *watch;
    struct object **objs;          /* ids the client allocated: 1 .. 0xfeffffff */
    uint32_t nobjs;
    struct object **sobjs;         /* ids we allocated: 0xff000000 .. */
    uint32_t nsobjs;
    struct list pointers, keyboards, data_devices, outputs;   /* of struct object */
    bool dead;
    pid_t pid;
};

#define SERVER_ID_BASE 0xff000000u

enum { DISPLAY_ERR_INVALID_OBJECT = 0, DISPLAY_ERR_INVALID_METHOD = 1,
       DISPLAY_ERR_NO_MEMORY = 2, DISPLAY_ERR_IMPLEMENTATION = 3 };

void client_accept(int listen_fd);
void client_error(struct object *o, uint32_t code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void clients_flush_and_reap(void);
void clients_destroy_all(void);

struct object *obj_new(struct client *c, uint32_t id, const struct iface *iface,
                       uint32_t version, void *data);
struct object *obj_new_server(struct client *c, const struct iface *iface, uint32_t version, void *data);
struct object *obj_get(struct client *c, uint32_t id);
void obj_destroy(struct object *o);
void obj_send(struct object *o, uint16_t op, const char *sig, ...);
/* Resolve an object-id argument. Returns false (and kills the client) if it's the wrong type. */
bool arg_obj(struct object *self, uint32_t id, const struct iface *want, bool nullable, struct object **out);

uint32_t next_serial(void);
void core_init(void);
void global_add(const struct iface *iface, uint32_t version,
                void (*bind)(struct client *c, uint32_t id, uint32_t version));
extern const struct iface wl_callback_iface;
void output_send_enter(struct client *c, struct object *surface_obj);

/* ================================================================ compositor.c */
struct region_op { bool add; struct rect r; };
struct region { struct region_op *ops; int n, cap; };
bool region_contains(const struct region *r, int x, int y);

struct pool {
    struct object *obj;
    int fd;
    uint8_t *data;
    size_t size;
    int refs;
};

struct buffer {
    struct object *obj;
    struct pool *pool;
    int32_t offset, width, height, stride;
    uint32_t format;
    int refs;
};

struct surface_state {
    bool attached;                 /* attach was called (buffer may be NULL = unmap) */
    struct buffer *buffer;
    int32_t dx, dy;
    bool damaged;
    struct rect damage;            /* bounding box, buffer coordinates */
    bool damage_all;
    struct list frame_cbs;         /* wl_callback objects */
    bool input_set;
    bool input_infinite;
    struct region input;
    bool scale_set;
    int32_t scale;
};

enum surface_role { ROLE_NONE, ROLE_XDG, ROLE_SUBSURFACE, ROLE_CURSOR };

struct snode { struct list link; struct surface *surface; };

struct subsurface {
    struct object *obj;
    struct surface *surface, *parent;
    int x, y, pend_x, pend_y;
    bool sync;
};

struct surface {
    struct object *obj;
    struct client *client;
    struct list link;              /* srv.surfaces */

    struct surface_state pending, cached;
    bool has_cached;

    /* current, committed state. We keep our own copy of the pixels (premultiplied ARGB),
     * so the client's buffer can be released right after commit. */
    uint32_t *pixels;
    int buf_w, buf_h, scale;
    bool opaque;
    int width, height;             /* size in surface coordinates; 0x0 = no content */
    bool input_infinite;
    struct region input;
    struct list frame_cbs;

    enum surface_role role;
    struct xdg_surface *xdg;
    struct subsurface *sub;
    int cursor_hot_x, cursor_hot_y;

    struct list stack;             /* draw order of this surface + its subsurfaces */
    struct snode self_node, parent_node;
    bool sent_enter;
};

void surface_init_globals(void);
struct surface *surface_from_obj(struct object *o);
bool surface_set_role(struct surface *s, enum surface_role role, struct object *err_obj, uint32_t err_code);
/* calls fn for the surface and its subsurfaces, in drawing order, with absolute positions */
void surface_for_each(struct surface *s, int x, int y,
                      void (*fn)(struct surface *s, int x, int y, void *data), void *data);
/* topmost (sub)surface under (px, py) that accepts input, given s is drawn at (x, y) */
struct surface *surface_at(struct surface *s, int x, int y, double px, double py, double *sx, double *sy);
bool surface_abs_pos(struct surface *s, int *x, int *y);
void frame_callbacks_collect(void);
void frame_callbacks_done(uint32_t time);
void install_sigbus_handler(void);
extern const struct iface wl_surface_iface;

/* ======================================================================= xdg.c */
struct positioner {
    int w, h;
    struct rect anchor_rect;
    uint32_t anchor, gravity, adjust;
    int off_x, off_y;
};

struct xdg_surface {
    struct object *obj;
    struct surface *surface;
    struct toplevel *toplevel;
    struct popup *popup;
    bool sent_configure;           /* a configure went out since the initial commit */
    bool acked;                    /* client acked one */
    uint32_t configure_serial, acked_serial;
    bool has_geometry, geometry_pending;
    struct rect geometry, pending_geometry;
    bool mapped;
};

struct toplevel {
    struct object *obj;
    struct xdg_surface *xdg;
    struct list link;              /* wm stacking order, bottom to top */
    char title[256];
    char app_id[128];
    int x, y;                      /* screen position of the window geometry's origin */
    bool placed;
    bool maximized, fullscreen, minimized;
    bool want_maximized, want_fullscreen;
    struct rect saved;             /* geometry before maximize/fullscreen */
    int min_w, min_h, max_w, max_h;
    struct object *decoration;     /* zxdg_toplevel_decoration_v1 */
    bool ssd;                      /* we draw the title bar */
    bool activated;
    bool resizing;
    int resize_edges;
    int resize_anchor_x, resize_anchor_y;   /* the corner that must stay put */
    struct toplevel *parent;
    int conf_w, conf_h;            /* size in the last configure we sent */
};

struct popup {
    struct object *obj;
    struct xdg_surface *xdg;
    struct xdg_surface *parent;
    struct list link;              /* wm popup list, creation/map order */
    struct positioner pos;
    struct rect geom;              /* relative to the parent's window geometry */
    bool grab;
    bool mapped;
};

void xdg_init_globals(void);
void xdg_surface_commit(struct xdg_surface *x);
void popup_place(struct popup *p);
struct rect xdg_geometry(struct xdg_surface *x);
void toplevel_send_configure(struct toplevel *t, int w, int h);
void toplevel_send_close(struct toplevel *t);
void popup_send_done(struct popup *p);
bool xdg_abs_origin(struct xdg_surface *x, int *ox, int *oy);   /* screen pos of geometry origin */

/* ====================================================================== seat.c */
void seat_init(void);
void seat_finish(void);
void seat_pointer_enter(struct surface *s, double sx, double sy);
void seat_pointer_leave(void);
void seat_pointer_motion(double sx, double sy);
void seat_pointer_button(uint32_t button, bool pressed);
void seat_pointer_axis(int axis, double value, int discrete);
void seat_keyboard_focus(struct surface *s);
/* returns true if the server swallowed the key (a keybinding) */
bool seat_keyboard_key(uint32_t key, bool pressed);
uint32_t seat_modifiers(void);           /* MOD_* bits below */
void seat_surface_destroyed(struct surface *s);
void seat_client_destroyed(struct client *c);
struct surface *seat_pointer_focus(void);
struct surface *seat_keyboard_focus_surface(void);

enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8 };

/* ======================================================================== wm.c */
struct canvas;
void wm_init(void);
void wm_render(struct canvas *c);
void wm_damage(void);
void wm_theme_changed(void);

void wm_toplevel_new(struct toplevel *t);
void wm_toplevel_map(struct toplevel *t);
void wm_toplevel_unmap(struct toplevel *t);
void wm_toplevel_destroy(struct toplevel *t);
void wm_toplevel_committed(struct toplevel *t);
void wm_toplevel_request(struct toplevel *t, int what, uint32_t arg);
void wm_toplevel_title_changed(struct toplevel *t);
void wm_popup_map(struct popup *p);
void wm_popup_unmap(struct popup *p);
void wm_popup_grab(struct popup *p);
void wm_surface_destroyed(struct surface *s);
void wm_cursor_changed(void);
void wm_set_cursor(struct surface *s, int hot_x, int hot_y);
void wm_xdg_surface_destroyed(struct xdg_surface *x);
struct rect wm_maximized_content(struct toplevel *t);
void wm_popup_new(struct popup *p);

enum { REQ_MOVE, REQ_RESIZE, REQ_MAXIMIZE, REQ_UNMAXIMIZE, REQ_FULLSCREEN,
       REQ_UNFULLSCREEN, REQ_MINIMIZE };

/* input events from devices (input.c) or the headless test console */
void wm_pointer_motion(double dx, double dy);
void wm_pointer_warp(double x, double y);
void wm_pointer_button(uint32_t button, bool pressed);
void wm_pointer_axis(int axis, double value, int discrete);
void wm_key(uint32_t key, bool pressed);

/* keybinding actions (seat.c decides which keys) */
void wm_action_quit(void);
void wm_action_cycle(void);
void wm_action_close(void);
void wm_action_maximize(void);
void wm_action_terminal(void);

/* ===================================================================== input.c */
int input_init(void);
void input_finish(void);

/* ===================================================================== main.c */
struct backend;
struct server {
    volatile bool running;
    struct backend *backend;
    int width, height;
    bool dirty;
    bool frame_pending;
    struct list clients;
    struct list surfaces;
    const char *terminal;
    char socket_name[64];
};
extern struct server srv;
void spawn(const char *cmd);
void server_frame_done(void);
