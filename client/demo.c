/* demo.c — a Wayland client written against the raw wire protocol (no libwayland).
 *
 * It does what every toolkit does under the hood:
 *   1. connect to $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
 *   2. get the registry, bind the globals it needs
 *   3. create a surface, give it the xdg_toplevel role, ask for server-side decorations
 *   4. wait for configure, ack it, draw into shared memory, attach + commit
 *   5. redraw whenever the server says it's a good time (frame callbacks)
 *   6. print keys (via the keymap the server sent), track the pointer, quit on close/Esc
 *   7. drag and drop: drag out of the window to offer some text (with a drag icon), and
 *      drop text onto it (from another demo, a terminal, a Qt or GTK app) to receive it
 *
 * Run it with PANE_DEBUG=1 on the server to see both sides of the conversation. */
#define _GNU_SOURCE
#include "../src/wire.h"

#include <errno.h>
#include <fcntl.h>
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
            T_DECO_MANAGER, T_DECO, T_OUTPUT, T_DATA_DEVICE_MANAGER, T_DATA_DEVICE, T_DATA_OFFER,
            T_DATA_SOURCE, T_COUNT };

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
    /* data_offer, enter, leave, motion, drop, selection */
    [T_DATA_DEVICE] = (const char *const[]){ "n", "uoff?o", "", "uff", "", "?o" },
    [T_DATA_OFFER] = (const char *const[]){ "s", "u", "u" },        /* offer, source_actions, action */
    /* target, send, cancelled, dnd_drop_performed, dnd_finished, action */
    [T_DATA_SOURCE] = (const char *const[]){ "?s", "sh", "", "", "", "u" },
};
static const int event_count[T_COUNT] = {
    [T_DISPLAY] = 2, [T_REGISTRY] = 2, [T_CALLBACK] = 1, [T_SHM] = 1, [T_BUFFER] = 1,
    [T_SURFACE] = 4, [T_WM_BASE] = 1, [T_XDG_SURFACE] = 1, [T_TOPLEVEL] = 4, [T_SEAT] = 2,
    [T_POINTER] = 9, [T_KEYBOARD] = 6, [T_DECO] = 1, [T_OUTPUT] = 6,
    [T_DATA_DEVICE] = 6, [T_DATA_OFFER] = 3, [T_DATA_SOURCE] = 6,
};

static struct conn conn;
static uint8_t types[65536];          /* ids we allocate */
static uint8_t server_types[4096];    /* ids the server allocates (0xff000000 + n): data offers */
static uint32_t next_id = 2;          /* 1 is wl_display */

static uint8_t *type_of(uint32_t id) {
    static uint8_t none;
    if (id < 65536) return &types[id];
    if (id >= 0xff000000u && id - 0xff000000u < 4096) return &server_types[id - 0xff000000u];
    none = T_NONE;
    return &none;
}

static uint32_t free_ids[256];   /* ids the server released with delete_id, for reuse */
static int nfree;

static uint32_t new_id(enum type t) {
    uint32_t id = nfree ? free_ids[--nfree] : next_id++;
    *type_of(id) = t;
    return id;
}

#define send(id, op, ...) wire_send(&conn, id, op, __VA_ARGS__)

/* globals we bind */
static uint32_t compositor, shm, wm_base, seat, deco_manager, data_device_manager, data_device;
static uint32_t surface, xdg_surface, toplevel, pointer, keyboard, sync_cb, frame_cb;

/* window state */
static int width = 480, height = 320, pending_w, pending_h;
static bool configured, running = true, roundtrip_done;
static double ptr_x = -1, ptr_y = -1;
static int palette, radius = 40;

/* drag and drop */
static bool pressed, dragging, drag_over;
static uint32_t press_serial;
static double press_x, press_y;
static uint32_t drag_source, drag_icon, icon_pool, icon_buf;
static void *icon_map;
static uint32_t dnd_offer, receiving_offer, selection_offer;
static int drop_fd = -1, drops;
static char drop_text[4096];
static size_t drop_len;

/* which text type each offer (by id) advertised, so we know what to ask for */
static struct { uint32_t id; char mime[64]; } offer_text[16];
static char *text_mime_of(uint32_t id, bool create) {
    for (int i = 0; i < 16; i++) if (offer_text[i].id == id) return offer_text[i].mime;
    if (!create) return NULL;
    static int next;
    int i = next++ % 16;
    offer_text[i].id = id;
    offer_text[i].mime[0] = '\0';
    return offer_text[i].mime;
}

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
            if (drag_over && (x < 4 || y < 4 || x >= width - 4 || y >= height - 4)) {
                r = c[0]; g = c[1]; bl = c[2]; a = 1;   /* "you can drop here" */
            }
            if (y >= 10 && y < 22 && x >= 10 && x < 10 + 18 * drops && (x - 10) % 18 < 12) {
                r = 0.9; g = 0.9; bl = 0.9; a = 1;      /* one square per successful drop */
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
    else if (!strcmp(iface, "wl_data_device_manager")) BIND(data_device_manager, T_DATA_DEVICE_MANAGER, 3);
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

/* ------------------------------------------------------------ drag & drop */

static void start_drag(void) {
    dragging = true;
    drag_source = new_id(T_DATA_SOURCE);
    send(data_device_manager, 0, "n", drag_source);          /* create_data_source */
    send(drag_source, 0, "s", "text/plain;charset=utf-8");   /* offer: types we can provide */
    send(drag_source, 0, "s", "text/plain");
    send(drag_source, 2, "u", 1u | 2u);                      /* set_actions: copy | move */

    /* the drag icon is just another surface; the server draws it under the pointer */
    const int s = 40;
    int fd = memfd_create("pane-demo-icon", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, s * s * 4) < 0) { perror("memfd"); exit(1); }
    icon_map = mmap(NULL, s * s * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    static const double pal[3][3] = { { 0.48, 0.64, 0.97 }, { 0.62, 0.81, 0.42 }, { 0.97, 0.46, 0.56 } };
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            double k = fmin(fmax((s / 2.0 - 1 - hypot(x - s / 2.0 + 0.5, y - s / 2.0 + 0.5)), 0), 1) * 0.85;
            ((uint32_t *)icon_map)[y * s + x] = argb(k, pal[palette][0], pal[palette][1], pal[palette][2]);
        }
    drag_icon = new_id(T_SURFACE);
    send(compositor, 0, "n", drag_icon);
    /* start_drag(source, origin, icon, serial of the button press that started it) */
    send(data_device, 0, "?oo?ou", drag_source, surface, drag_icon, press_serial);
    /* now the icon has its role: its top-left sits at the pointer, and attach's offset
     * moves it from there, so -s/2 centers it */
    icon_pool = new_id(T_POOL);
    send(shm, 0, "nhi", icon_pool, fd, s * s * 4);
    close(fd);
    icon_buf = new_id(T_BUFFER);
    send(icon_pool, 0, "niiiiu", icon_buf, 0, s, s, s * 4, 0u);
    send(drag_icon, 1, "?oii", icon_buf, -s / 2, -s / 2);
    send(drag_icon, 9, "iiii", 0, 0, s, s);
    send(drag_icon, 6, "");
    printf("drag started\n");
}

static void end_drag(const char *how) {
    printf("drag %s\n", how);
    send(drag_source, 1, "");   /* destroy */
    send(icon_buf, 0, "");
    send(icon_pool, 1, "");
    send(drag_icon, 0, "");
    munmap(icon_map, 40 * 40 * 4);
    dragging = false;
    drag_source = 0;
}

static void drop_done(void) {   /* the dropped data has all arrived */
    printf("dropped: %.*s\n", (int)drop_len, drop_text);
    fflush(stdout);
    drops++;
    send(receiving_offer, 3, "");   /* finish */
    send(receiving_offer, 2, "");   /* destroy */
    *type_of(receiving_offer) = T_NONE;
    receiving_offer = 0;
    close(drop_fd);
    drop_fd = -1;
    draw();
}

static void dispatch(uint32_t id, uint16_t op, union wire_arg *a) {
    switch (*type_of(id)) {
    case T_DISPLAY:
        if (op == 0) {
            fprintf(stderr, "protocol error on object %u: %s\n", a[0].o, a[2].s);
            exit(1);
        }
        if (a[0].u < 65536) {    /* delete_id: the server is done with this id; reuse it */
            *type_of(a[0].u) = T_NONE;
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
            /* pressed and moved far enough: that's a drag */
            if (pressed && !dragging && data_device && hypot(ptr_x - press_x, ptr_y - press_y) > 12)
                start_drag();
        } else if (op == 1) {
            ptr_x = ptr_y = -1;
        } else if (op == 3 && a[2].u == 0x110) {   /* left button */
            if (a[3].u == 1) {
                pressed = true;
                press_serial = a[0].u;
                press_x = ptr_x;
                press_y = ptr_y;
            } else {
                if (pressed && !dragging) palette = (palette + 1) % 3;   /* a click */
                pressed = false;
            }
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
    case T_DATA_SOURCE:   /* we're dragging */
        switch (op) {
        case 0: printf("target %s\n", a[0].s ? a[0].s : "can't take it"); break;
        case 1: {   /* send(mime, fd): write the data straight to the receiver */
            char text[128];
            int n = snprintf(text, sizeof text, "Hello from pane-demo (pid %d)!", (int)getpid());
            if (write(a[1].h, text, n) < 0) perror("write");
            close(a[1].h);
            break;
        }
        case 2: end_drag("cancelled"); break;
        case 3: printf("dropped; waiting for the target to finish\n"); break;
        case 4: end_drag("finished"); break;
        case 5: printf("action: %s\n", a[0].u == 1 ? "copy" : a[0].u == 2 ? "move" : a[0].u == 4 ? "ask" : "none"); break;
        }
        fflush(stdout);
        break;
    case T_DATA_DEVICE:   /* something is being dragged over us, or the clipboard changed */
        switch (op) {
        case 0:   /* data_offer(id): a new offer object; its types follow as offer events */
            *type_of(a[0].n) = T_DATA_OFFER;
            text_mime_of(a[0].n, true);
            break;
        case 1: { /* enter(serial, surface, x, y, offer) */
            dnd_offer = a[4].o;
            drag_over = true;
            ptr_x = fixed_to_double(a[2].f);
            ptr_y = fixed_to_double(a[3].f);
            char *mime = dnd_offer ? text_mime_of(dnd_offer, false) : NULL;
            if (dnd_offer) {
                /* say what we'd take (or null), and which actions */
                send(dnd_offer, 0, "u?s", a[0].u, mime && mime[0] ? mime : NULL);   /* accept */
                send(dnd_offer, 4, "uu", 1u | 2u, 1u);                              /* set_actions */
            }
            break;
        }
        case 2:   /* leave */
            if (dnd_offer) { send(dnd_offer, 2, ""); *type_of(dnd_offer) = T_NONE; }
            dnd_offer = 0;
            drag_over = false;
            ptr_x = ptr_y = -1;
            break;
        case 3:   /* motion */
            ptr_x = fixed_to_double(a[1].f);
            ptr_y = fixed_to_double(a[2].f);
            break;
        case 4: { /* drop: ask the source for the data through a pipe */
            char *mime = dnd_offer ? text_mime_of(dnd_offer, false) : NULL;
            drag_over = false;
            if (!mime || !mime[0]) break;
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) < 0) break;
            send(dnd_offer, 1, "sh", mime, fds[1]);   /* receive(mime, fd) */
            close(fds[1]);
            drop_fd = fds[0];
            drop_len = 0;
            receiving_offer = dnd_offer;
            dnd_offer = 0;
            break;
        }
        case 5:   /* selection: the clipboard offer replaces the previous one */
            if (selection_offer && selection_offer != a[0].o) {
                send(selection_offer, 2, "");
                *type_of(selection_offer) = T_NONE;
            }
            selection_offer = a[0].o;
            break;
        }
        break;
    case T_DATA_OFFER:
        if (op == 0 && !strncmp(a[0].s, "text/plain", 10)) {   /* prefer the utf-8 one */
            char *m = text_mime_of(id, true);
            if (!m[0] || strstr(a[0].s, "utf-8")) snprintf(m, 64, "%s", a[0].s);
        } else if (op == 2) {
            printf("drop action: %s\n", a[0].u == 1 ? "copy" : a[0].u == 2 ? "move" : "none");
            fflush(stdout);
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
        enum type t = *type_of(id);
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
    if (data_device_manager && seat) {
        data_device = new_id(T_DATA_DEVICE);
        send(data_device_manager, 1, "no", data_device, seat);   /* get_data_device */
    }
    send(surface, 6, "");   /* initial commit, no buffer: "please configure me" */
    flush();

    printf("move the mouse over the window, click to change color, scroll to resize the dot,\n"
           "type to see keys decoded with the server's keymap, drag out of the window to\n"
           "drag some text, drop text onto it. Esc or the close button quits.\n");
    fflush(stdout);
    while (running) {
        struct pollfd p[2] = { { conn.fd, POLLIN, 0 }, { drop_fd, POLLIN, 0 } };
        if (poll(p, drop_fd >= 0 ? 2 : 1, -1) < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLIN | POLLHUP)) read_events();
        if (drop_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {   /* dropped data arriving */
            ssize_t n = read(drop_fd, drop_text + drop_len, sizeof drop_text - 1 - drop_len);
            if (n > 0) drop_len += n;
            else drop_done();
        }
        flush();
    }
    destroy_buffers();
    flush();
    conn_free(&conn);
    return 0;
}
