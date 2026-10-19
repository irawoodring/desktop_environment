/* demo.c — a Wayland client written against the raw wire protocol (no libwayland).
 *
 * It does what every toolkit does under the hood:
 *   1. connect to $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
 *   2. get the registry, bind the globals it needs
 *   3. create a surface, give it the xdg_toplevel role, ask for server-side decorations
 *   4. wait for configure, ack it, draw into shared memory, attach + commit
 *   5. redraw whenever the server says it's a good time (frame callbacks)
 *   6. print keys (via the keymap the server sent), track the pointer, quit on close/Esc
 *
 * Run it with PANE_DEBUG=1 on the server to see both sides of the conversation. */
#define _GNU_SOURCE
#include "../src/wire.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* What each object id is, so we know how to decode its events. */
enum type { T_NONE, T_DISPLAY, T_REGISTRY, T_CALLBACK, T_COMPOSITOR, T_SHM, T_POOL, T_BUFFER,
            T_SURFACE, T_WM_BASE, T_XDG_SURFACE, T_TOPLEVEL, T_SEAT, T_POINTER, T_KEYBOARD,
            T_DECO_MANAGER, T_DECO, T_OUTPUT, T_COUNT };

/* event signatures for each interface, indexed by opcode */
static const char *const *event_sigs[T_COUNT] = {
    [T_DISPLAY] = (const char *const[]){ "ous", "u" },              /* error, delete_id */
    [T_REGISTRY] = (const char *const[]){ "usu", "u" },             /* global, global_remove */
    [T_CALLBACK] = (const char *const[]){ "u" },                    /* done */
    [T_SHM] = (const char *const[]){ "u" },                         /* format */
    [T_BUFFER] = (const char *const[]){ "" },                       /* release */
    [T_SURFACE] = (const char *const[]){ "o", "o", "i", "u" },      /* enter, leave, ... */
    [T_WM_BASE] = (const char *const[]){ "u" },                     /* ping */
    [T_XDG_SURFACE] = (const char *const[]){ "u" },                 /* configure */
    [T_TOPLEVEL] = (const char *const[]){ "iia", "", "ii", "a" },   /* configure, close, ... */
    [T_SEAT] = (const char *const[]){ "u", "s" },                   /* capabilities, name */
    [T_POINTER] = (const char *const[]){ "uoff", "uo", "uff", "uuuu", "uuf", "", "u", "uu", "ui" },
    [T_KEYBOARD] = (const char *const[]){ "uhu", "uoa", "uo", "uuuu", "uuuuu", "ii" },
    [T_DECO] = (const char *const[]){ "u" },                        /* configure(mode) */
    [T_OUTPUT] = (const char *const[]){ "iiiiissi", "uiii", "", "i", "s", "s" },
};
static const int event_count[T_COUNT] = {
    [T_DISPLAY] = 2, [T_REGISTRY] = 2, [T_CALLBACK] = 1, [T_SHM] = 1, [T_BUFFER] = 1,
    [T_SURFACE] = 4, [T_WM_BASE] = 1, [T_XDG_SURFACE] = 1, [T_TOPLEVEL] = 4, [T_SEAT] = 2,
    [T_POINTER] = 9, [T_KEYBOARD] = 6, [T_DECO] = 1, [T_OUTPUT] = 6,
};

static struct conn conn;
static uint8_t types[65536];
static uint32_t next_id = 2;   /* 1 is wl_display */

static uint32_t free_ids[256];   /* ids the server released with delete_id, for reuse */
static int nfree;

static uint32_t new_id(enum type t) {
    uint32_t id = nfree ? free_ids[--nfree] : next_id++;
    types[id] = t;
    return id;
}

#define send(id, op, ...) wire_send(&conn, id, op, __VA_ARGS__)

/* globals we bind */
static uint32_t compositor, shm, wm_base, seat, deco_manager;
static uint32_t surface, xdg_surface, toplevel, pointer, keyboard, sync_cb, frame_cb;

/* window state */
static int width = 480, height = 320, pending_w, pending_h;
static bool configured, running = true, roundtrip_done;
static double ptr_x = -1, ptr_y = -1;
static int palette, radius = 40;

struct buf { uint32_t id; uint32_t *px; bool busy; };
static struct buf bufs[2];
static uint32_t pool;
static void *pool_map;
static size_t pool_size;
static int buf_w, buf_h;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ----------------------------------------------------------------- buffers */

static void destroy_buffers(void) {
    if (!pool) return;
    for (int i = 0; i < 2; i++) send(bufs[i].id, 0, "");   /* wl_buffer.destroy */
    send(pool, 1, "");                                      /* wl_shm_pool.destroy */
    munmap(pool_map, pool_size);
    pool = 0;
}

/* Two buffers in one shared-memory file: draw into one while the server reads the other. */
static void create_buffers(int w, int h) {
    destroy_buffers();
    int stride = w * 4;
    size_t size = (size_t)stride * h;
    pool_size = size * 2;
    int fd = memfd_create("pane-demo", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, pool_size) < 0) { perror("memfd"); exit(1); }
    pool_map = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    pool = new_id(T_POOL);
    send(shm, 0, "nhi", pool, fd, (int)pool_size);   /* wl_shm.create_pool(id, fd, size) */
    close(fd);   /* wire_send dup'd it; the server has its own copy once it's sent */
    for (int i = 0; i < 2; i++) {
        bufs[i].id = new_id(T_BUFFER);
        bufs[i].px = (uint32_t *)((uint8_t *)pool_map + size * i);
        bufs[i].busy = false;
        /* create_buffer(id, offset, width, height, stride, format=argb8888) */
        send(pool, 0, "niiiiu", bufs[i].id, (int)(size * i), w, h, stride, 0u);
    }
    buf_w = w;
    buf_h = h;
}

/* ------------------------------------------------------------------ drawing */

static uint32_t argb(double a, double r, double g, double b) {   /* premultiplied */
    a = fmin(fmax(a, 0), 1);
    return (uint32_t)(a * 255) << 24 | (uint32_t)(r * a * 255) << 16 |
           (uint32_t)(g * a * 255) << 8 | (uint32_t)(b * a * 255);
}

static void draw(void) {
    if (!configured) return;
    if (buf_w != width || buf_h != height) create_buffers(width, height);
    struct buf *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
    if (!b) return;   /* both still with the server; the next release will let us draw */

    double t = now_s();
    static const double pal[3][3] = { { 0.48, 0.64, 0.97 }, { 0.62, 0.81, 0.42 }, { 0.97, 0.46, 0.56 } };
    const double *c = pal[palette];
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            /* slightly translucent background so you can see the server compositing */
            double wave = 0.5 + 0.5 * sin(x * 0.03 + y * 0.02 + t * 2.0);
            double r = 0.10 + 0.08 * wave, g = 0.11 + 0.06 * wave, bl = 0.16 + 0.10 * wave;
            double a = 0.92;
            if (ptr_x >= 0) {   /* a soft disc under the pointer */
                double d = hypot(x - ptr_x, y - ptr_y);
                double k = fmin(fmax((radius - d) / 2.0, 0), 1);
                r += (c[0] - r) * k; g += (c[1] - g) * k; bl += (c[2] - bl) * k;
                a += (1 - a) * k;
            }
            b->px[y * width + x] = argb(a, r, g, bl);
        }
    }
    b->busy = true;
    frame_cb = new_id(T_CALLBACK);
    send(surface, 3, "n", frame_cb);                  /* wl_surface.frame */
    send(surface, 1, "?oii", b->id, 0, 0);            /* attach */
    send(surface, 9, "iiii", 0, 0, width, height);    /* damage_buffer */
    send(surface, 6, "");                             /* commit */
}

/* ------------------------------------------------------------------- events */

static void on_global(uint32_t name, const char *iface, uint32_t version) {
    /* registry.bind(name, interface, version, new_id) — the untyped new_id carries its type */
    #define BIND(var, type, v) do { var = new_id(type); \
        send(2, 0, "usun", name, iface, (uint32_t)(version < (v) ? version : (v)), var); } while (0)
    if (!strcmp(iface, "wl_compositor")) BIND(compositor, T_COMPOSITOR, 4);
    else if (!strcmp(iface, "wl_shm")) BIND(shm, T_SHM, 1);
    else if (!strcmp(iface, "xdg_wm_base")) BIND(wm_base, T_WM_BASE, 1);
    else if (!strcmp(iface, "wl_seat")) BIND(seat, T_SEAT, 5);
    else if (!strcmp(iface, "zxdg_decoration_manager_v1")) BIND(deco_manager, T_DECO_MANAGER, 1);
    #undef BIND
}

static void on_key(uint32_t key, bool down) {
    if (!xkb_st || !down) return;
    char text[64];
    xkb_state_key_get_utf8(xkb_st, key + 8, text, sizeof text);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
    char name[64];
    xkb_keysym_get_name(sym, name, sizeof name);
    printf("key %3u  %-12s %s\n", key, name, text[0] >= ' ' ? text : "");
    fflush(stdout);
    if (sym == XKB_KEY_Escape) running = false;
}

static void dispatch(uint32_t id, uint16_t op, union wire_arg *a) {
    switch (types[id]) {
    case T_DISPLAY:
        if (op == 0) {
            fprintf(stderr, "protocol error on object %u: %s\n", a[0].o, a[2].s);
            exit(1);
        }
        if (a[0].u < 65536) {    /* delete_id: the server is done with this id; reuse it */
            types[a[0].u] = T_NONE;
            if (nfree < 256) free_ids[nfree++] = a[0].u;
        }
        break;
    case T_REGISTRY:
        if (op == 0) on_global(a[0].u, a[1].s, a[2].u);
        break;
    case T_CALLBACK:
        if (id == sync_cb) roundtrip_done = true;
        else if (id == frame_cb) draw();   /* the server is ready for another frame */
        break;
    case T_BUFFER:
        for (int i = 0; i < 2; i++) if (bufs[i].id == id) bufs[i].busy = false;
        break;
    case T_WM_BASE:
        send(wm_base, 3, "u", a[0].u);   /* pong, or we'd be considered frozen */
        break;
    case T_TOPLEVEL:
        if (op == 0) { pending_w = a[0].i; pending_h = a[1].i; }   /* 0 = our choice */
        else if (op == 1) running = false;                          /* close */
        break;
    case T_XDG_SURFACE:   /* the configure is complete: ack it, then show the new state */
        send(xdg_surface, 4, "u", a[0].u);
        if (pending_w > 0) width = pending_w;
        if (pending_h > 0) height = pending_h;
        bool first = !configured;
        configured = true;
        if (first) draw();
        break;
    case T_DECO:
        printf("decorations: %s side\n", a[0].u == 2 ? "server" : "client");
        break;
    case T_SEAT:
        if (op == 0) {
            if ((a[0].u & 1) && !pointer) { pointer = new_id(T_POINTER); send(seat, 0, "n", pointer); }
            if ((a[0].u & 2) && !keyboard) { keyboard = new_id(T_KEYBOARD); send(seat, 1, "n", keyboard); }
        }
        break;
    case T_POINTER:
        if (op == 0 || op == 2) {   /* enter / motion */
            ptr_x = fixed_to_double(op == 0 ? a[2].f : a[1].f);
            ptr_y = fixed_to_double(op == 0 ? a[3].f : a[2].f);
        } else if (op == 1) {
            ptr_x = ptr_y = -1;
        } else if (op == 3 && a[3].u == 1) {
            palette = (palette + 1) % 3;
        } else if (op == 4 && a[1].u == 0) {
            radius = (int)fmin(fmax(radius - fixed_to_double(a[2].f) / 3, 8), 200);
        }
        break;
    case T_KEYBOARD:
        if (op == 0) {   /* keymap(format, fd, size): compile it like every toolkit does */
            char *map = mmap(NULL, a[2].u, PROT_READ, MAP_PRIVATE, a[1].h, 0);
            close(a[1].h);
            if (map == MAP_FAILED) break;
            xkb_ctx = xkb_context_new(0);
            xkb_map = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
            munmap(map, a[2].u);
            if (xkb_map) xkb_st = xkb_state_new(xkb_map);
            printf("got a %u byte keymap\n", a[2].u);
        } else if (op == 3) {
            on_key(a[2].u, a[3].u == 1);
        } else if (op == 4 && xkb_st) {
            xkb_state_update_mask(xkb_st, a[1].u, a[2].u, a[3].u, 0, 0, a[4].u);
        }
        break;
    default:
        break;
    }
}

static void read_events(void) {
    int r = conn_read(&conn);
    if (r <= 0) { fprintf(stderr, "server went away\n"); exit(1); }
    uint32_t id, len;
    uint16_t op;
    const uint8_t *body;
    while (conn_next(&conn, &id, &op, &body, &len) == 1) {
        enum type t = types[id];
        if (t == T_NONE || op >= event_count[t]) {   /* object we don't track: skip */
            conn_consume(&conn, len);
            continue;
        }
        union wire_arg a[8];
        if (wire_parse(&conn, event_sigs[t][op], body, len, a, 8) < 0) {
            fprintf(stderr, "malformed event\n");
            exit(1);
        }
        dispatch(id, op, a);
        conn_consume(&conn, len);
    }
}

static void flush(void) {
    while (conn_flush(&conn) == 0) {
        struct pollfd p = { conn.fd, POLLOUT, 0 };
        poll(&p, 1, -1);
    }
}

/* send wl_display.sync and process events until its callback fires */
static void roundtrip(void) {
    sync_cb = new_id(T_CALLBACK);
    send(1, 0, "n", sync_cb);
    roundtrip_done = false;
    flush();
    while (!roundtrip_done) {
        struct pollfd p = { conn.fd, POLLIN, 0 };
        poll(&p, 1, -1);
        read_events();
    }
}

int main(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR"), *name = getenv("WAYLAND_DISPLAY");
    if (!dir) { fprintf(stderr, "XDG_RUNTIME_DIR is not set\n"); return 1; }
    if (!name) name = "wayland-0";
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, name);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror(addr.sun_path); return 1; }
    conn_init(&conn, fd);
    types[1] = T_DISPLAY;

    uint32_t registry = new_id(T_REGISTRY);   /* = 2, which on_global relies on */
    send(1, 1, "n", registry);                /* wl_display.get_registry */
    roundtrip();                              /* now every global has been announced and bound */
    if (!compositor || !shm || !wm_base) { fprintf(stderr, "missing required globals\n"); return 1; }

    surface = new_id(T_SURFACE);
    send(compositor, 0, "n", surface);                   /* create_surface */
    xdg_surface = new_id(T_XDG_SURFACE);
    send(wm_base, 2, "no", xdg_surface, surface);        /* get_xdg_surface */
    toplevel = new_id(T_TOPLEVEL);
    send(xdg_surface, 1, "n", toplevel);                 /* get_toplevel */
    send(toplevel, 2, "s", "pane demo — raw protocol");  /* set_title */
    send(toplevel, 3, "s", "pane-demo");                 /* set_app_id */
    if (deco_manager) {
        uint32_t deco = new_id(T_DECO);
        send(deco_manager, 1, "no", deco, toplevel);     /* get_toplevel_decoration */
        send(deco, 1, "u", 2u);                          /* set_mode(server_side) */
    }
    send(surface, 6, "");   /* initial commit, no buffer: "please configure me" */
    flush();

    printf("move the mouse over the window, click to change color, scroll to resize the dot,\n"
           "type to see keys decoded with the server's keymap. Esc or the close button quits.\n");
    fflush(stdout);
    while (running) {
        struct pollfd p = { conn.fd, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0 && errno != EINTR) break;
        if (p.revents & (POLLIN | POLLHUP)) read_events();
        flush();
    }
    destroy_buffers();
    flush();
    conn_free(&conn);
    return 0;
}
