/* core.c — clients, the per-client object table, and the core protocol objects.
 *
 * A Wayland connection is a conversation between objects. The client creates most of
 * them by picking an id and sending a request that contains it as a "new_id" argument.
 * Object 1 always exists: wl_display. From there the client asks for wl_registry, which
 * lists the "globals" (singleton services such as wl_compositor), and binds the ones it
 * wants. Every later message is addressed to some object id. */
#include "server.h"
#include "backend.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static bool debug_protocol;
static uint32_t serial_counter;

uint32_t next_serial(void) { return ++serial_counter; }

/* --------------------------------------------------------------------- globals */

struct global {
    uint32_t name;
    const struct iface *iface;
    uint32_t version;
    void (*bind)(struct client *c, uint32_t id, uint32_t version);
};
static struct global globals[32];
static int nglobals;

void global_add(const struct iface *iface, uint32_t version,
                void (*bind)(struct client *c, uint32_t id, uint32_t version)) {
    globals[nglobals] = (struct global){ nglobals + 1, iface, version, bind };
    nglobals++;
}

/* -------------------------------------------------------------------- objects */

static const char *req_sig(const char *entry) {
    const char *sp = strchr(entry, ' ');
    return sp ? sp + 1 : "";
}

struct object *obj_get(struct client *c, uint32_t id) {
    if (id >= SERVER_ID_BASE) {
        id -= SERVER_ID_BASE;
        return id < c->nsobjs ? c->sobjs[id] : NULL;
    }
    return id < c->nobjs ? c->objs[id] : NULL;
}

static bool table_put(struct object ***tab, uint32_t *n, uint32_t idx, struct object *o) {
    if (idx >= *n) {
        if (idx >= (1u << 20)) return false;
        uint32_t cap = *n ? *n : 64;
        while (cap <= idx) cap *= 2;
        struct object **t = realloc(*tab, cap * sizeof *t);
        if (!t) return false;
        memset(t + *n, 0, (cap - *n) * sizeof *t);
        *tab = t;
        *n = cap;
    }
    (*tab)[idx] = o;
    return true;
}

static struct object *obj_alloc(struct client *c, uint32_t id, const struct iface *iface,
                                uint32_t version, void *data) {
    struct object *o = calloc(1, sizeof *o);
    o->id = id;
    o->version = version;
    o->iface = iface;
    o->client = c;
    o->data = data;
    list_init(&o->link);
    return o;
}

struct object *obj_new(struct client *c, uint32_t id, const struct iface *iface,
                       uint32_t version, void *data) {
    if (id == 0 || id >= SERVER_ID_BASE || obj_get(c, id)) {
        client_error(obj_get(c, 1), DISPLAY_ERR_INVALID_OBJECT, "invalid new id %u", id);
        return NULL;
    }
    struct object *o = obj_alloc(c, id, iface, version, data);
    if (!table_put(&c->objs, &c->nobjs, id, o)) {
        free(o);
        client_error(obj_get(c, 1), DISPLAY_ERR_NO_MEMORY, "too many objects");
        return NULL;
    }
    return o;
}

struct object *obj_new_server(struct client *c, const struct iface *iface, uint32_t version, void *data) {
    uint32_t idx = 0;
    while (idx < c->nsobjs && c->sobjs[idx]) idx++;
    struct object *o = obj_alloc(c, SERVER_ID_BASE + idx, iface, version, data);
    if (!table_put(&c->sobjs, &c->nsobjs, idx, o)) {
        free(o);
        return NULL;
    }
    return o;
}

void obj_destroy(struct object *o) {
    struct client *c = o->client;
    if (o->iface->destroy) o->iface->destroy(o);
    list_remove(&o->link);
    if (o->id >= SERVER_ID_BASE) {
        c->sobjs[o->id - SERVER_ID_BASE] = NULL;
    } else {
        c->objs[o->id] = NULL;
        /* tell the client the id is free for reuse */
        if (!c->dead && o->id != 1) wire_send(&c->conn, 1, 1, "u", o->id);   /* wl_display.delete_id */
    }
    free(o);
}

void obj_send(struct object *o, uint16_t op, const char *sig, ...) {
    if (o->client->dead) return;
    if (debug_protocol) fprintf(stderr, "  <- %s@%u event %u\n", o->iface->name, o->id, op);
    va_list ap;
    va_start(ap, sig);
    wire_vsend(&o->client->conn, o->id, op, sig, ap);
    va_end(ap);
}

bool arg_obj(struct object *self, uint32_t id, const struct iface *want, bool nullable, struct object **out) {
    *out = NULL;
    if (id == 0) {
        if (nullable) return true;
        client_error(self, DISPLAY_ERR_INVALID_OBJECT, "null object not allowed");
        return false;
    }
    struct object *o = obj_get(self->client, id);
    if (!o || o->iface != want) {
        client_error(self, DISPLAY_ERR_INVALID_OBJECT, "object %u is not a %s", id, want->name);
        return false;
    }
    *out = o;
    return true;
}

void client_error(struct object *o, uint32_t code, const char *fmt, ...) {
    if (!o) return;
    struct client *c = o->client;
    if (c->dead) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "pane: client %d: protocol error on %s@%u: %s\n", c->pid, o->iface->name, o->id, msg);
    wire_send(&c->conn, 1, 0, "ous", o->id, code, msg);   /* wl_display.error */
    c->dead = true;
}

/* ------------------------------------------------------------------ wl_callback */

const struct iface wl_callback_iface = { "wl_callback", 0, NULL, 0, NULL, NULL };

/* ------------------------------------------------------------------ wl_registry */

static void registry_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    (void)op;   /* bind is the only request */
    uint32_t name = a[0].u, version = a[2].u, id = a[3].n;
    const char *iface = a[1].s;
    if (name == 0 || name > (uint32_t)nglobals) {
        client_error(o, DISPLAY_ERR_INVALID_OBJECT, "invalid global %u", name);
        return;
    }
    struct global *g = &globals[name - 1];
    if (!iface || strcmp(iface, g->iface->name) != 0 || version == 0 || version > g->version) {
        client_error(o, DISPLAY_ERR_INVALID_OBJECT, "bad bind of %s v%u (have %s v%u)",
                     iface ? iface : "?", version, g->iface->name, g->version);
        return;
    }
    g->bind(o->client, id, version);
}

static const char *const registry_reqs[] = { "bind usun" };
static const struct iface wl_registry_iface = {
    "wl_registry", 1, registry_reqs, 0, registry_dispatch, NULL };

/* ------------------------------------------------------------------- wl_display */

static void display_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct client *c = o->client;
    if (op == 0) {   /* sync: reply as soon as everything before it is processed — i.e. now */
        struct object *cb = obj_new(c, a[0].n, &wl_callback_iface, 1, NULL);
        if (!cb) return;
        obj_send(cb, 0, "u", next_serial());
        obj_destroy(cb);
    } else {         /* get_registry */
        struct object *r = obj_new(c, a[0].n, &wl_registry_iface, 1, NULL);
        if (!r) return;
        for (int i = 0; i < nglobals; i++)
            obj_send(r, 0, "usu", globals[i].name, globals[i].iface->name, globals[i].version);
    }
}

static const char *const display_reqs[] = { "sync n", "get_registry n" };
static const struct iface wl_display_iface = {
    "wl_display", 2, display_reqs, 0, display_dispatch, NULL };

/* -------------------------------------------------------------------- wl_output */

static const char *const output_reqs[] = { "release" };
static const struct iface wl_output_iface = { "wl_output", 1, output_reqs, 1u << 0, NULL, NULL };

static void output_bind(struct client *c, uint32_t id, uint32_t version) {
    struct object *o = obj_new(c, id, &wl_output_iface, version, NULL);
    if (!o) return;
    list_append(&c->outputs, &o->link);
    struct backend *b = srv.backend;
    obj_send(o, 0, "iiiiissi", 0, 0, b->phys_w_mm, b->phys_h_mm, 0, "pane", b->name, 0);  /* geometry */
    obj_send(o, 1, "uiii", 3u, srv.width, srv.height, b->refresh_mhz);                    /* mode */
    if (version >= 2) obj_send(o, 3, "i", 1);                                              /* scale */
    if (version >= 4) {
        obj_send(o, 4, "s", "pane-0");                                                     /* name */
        obj_send(o, 5, "s", "pane display");                                               /* description */
    }
    if (version >= 2) obj_send(o, 2, "");                                                  /* done */
}

void output_send_enter(struct client *c, struct object *surface_obj) {
    struct object *out;
    list_for_each(out, &c->outputs, link) obj_send(surface_obj, 0, "o", out->id);   /* wl_surface.enter */
}

/* -------------------------------------------------------------------- clients */

static void client_destroy(struct client *c) {
    c->dead = true;
    seat_client_destroyed(c);
    /* newest objects first: roles (xdg_toplevel) go before the surfaces they sit on */
    for (uint32_t i = c->nsobjs; i-- > 0;)
        if (c->sobjs[i]) obj_destroy(c->sobjs[i]);
    for (uint32_t i = c->nobjs; i-- > 0;)
        if (c->objs[i]) obj_destroy(c->objs[i]);
    free(c->objs);
    free(c->sobjs);
    watch_remove(c->watch);
    conn_free(&c->conn);
    list_remove(&c->link);
    free(c);
}

static void close_unclaimed_fds(const char *sig, union wire_arg *a, int n) {
    int i = 0;
    for (const char *s = sig; *s && i < n; s++) {
        if (*s == '?') continue;
        if (*s == 'h' && a[i].h >= 0) close(a[i].h);
        i++;
    }
}

static void client_dispatch(struct client *c) {
    uint32_t id, len;
    uint16_t op;
    const uint8_t *body;
    int r;
    while (!c->dead && (r = conn_next(&c->conn, &id, &op, &body, &len)) == 1) {
        struct object *o = obj_get(c, id);
        if (!o) {
            /* The client may be talking to an object it destroyed a moment ago and we
             * haven't confirmed (delete_id) yet; libwayland ignores those, so do we. */
            conn_consume(&c->conn, len);
            continue;
        }
        if (op >= o->iface->nreqs) {
            client_error(o, DISPLAY_ERR_INVALID_METHOD, "invalid opcode %u", op);
            break;
        }
        const char *entry = o->iface->reqs[op];
        const char *sig = req_sig(entry);
        union wire_arg a[20];
        int n = wire_parse(&c->conn, sig, body, len, a, 20);
        if (n < 0) {
            client_error(o, DISPLAY_ERR_INVALID_METHOD, "malformed request %.*s",
                         (int)strcspn(entry, " "), entry);
            break;
        }
        if (debug_protocol)
            fprintf(stderr, "-> client %d: %s@%u.%.*s\n", c->pid, o->iface->name, id,
                    (int)strcspn(entry, " "), entry);
        if (o->iface->dispatch) o->iface->dispatch(o, op, a);
        close_unclaimed_fds(sig, a, n);
        if (((o->iface->destructors >> op) & 1) && obj_get(c, id) == o) obj_destroy(o);
        conn_consume(&c->conn, len);
    }
    if (!c->dead && r < 0) client_error(obj_get(c, 1), DISPLAY_ERR_INVALID_METHOD, "bad message framing");
}

static void client_event(int fd, uint32_t events, void *data) {
    (void)fd;
    struct client *c = data;
    if (c->dead) return;
    if (events & EPOLLIN) {
        int r = conn_read(&c->conn);
        if (r <= 0) { c->dead = true; return; }
        client_dispatch(c);
    }
    if (events & EPOLLOUT) {
        if (conn_flush(&c->conn) < 0) c->dead = true;
    }
    if ((events & (EPOLLHUP | EPOLLERR)) && !(events & EPOLLIN)) c->dead = true;
}

void client_accept(int listen_fd) {
    int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    struct client *c = calloc(1, sizeof *c);
    conn_init(&c->conn, fd);
    list_init(&c->pointers);
    list_init(&c->keyboards);
    list_init(&c->data_devices);
    list_init(&c->outputs);
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) c->pid = cred.pid;
    obj_new(c, 1, &wl_display_iface, 1, NULL);
    c->watch = watch_add(fd, EPOLLIN, client_event, c);
    list_append(&srv.clients, &c->link);
    if (debug_protocol) fprintf(stderr, "pane: client %d connected\n", c->pid);
}

void clients_flush_and_reap(void) {
    struct client *c, *tmp;
    list_for_each_safe(c, tmp, &srv.clients, link) {
        int r = conn_flush(&c->conn);   /* even a dying client gets its error message */
        if (c->dead || r < 0 || c->conn.error) {
            if (debug_protocol) fprintf(stderr, "pane: client %d gone\n", c->pid);
            client_destroy(c);
            continue;
        }
        watch_mod(c->watch, r == 0 ? (EPOLLIN | EPOLLOUT) : EPOLLIN);
    }
}

void clients_destroy_all(void) {
    struct client *c, *tmp;
    list_for_each_safe(c, tmp, &srv.clients, link) client_destroy(c);
}

void core_init(void) {
    const char *dbg = getenv("PANE_DEBUG");
    debug_protocol = dbg && *dbg && strcmp(dbg, "0") != 0;
    global_add(&wl_output_iface, 4, output_bind);
}
