/* compositor.c — wl_compositor, wl_surface, wl_region, wl_subcompositor and wl_shm.
 *
 * A wl_surface is a rectangle of pixels with no meaning of its own. It gets a "role"
 * later: window (xdg_surface), subsurface, or cursor. All surface changes are
 * double-buffered: requests modify `pending` state and nothing is visible until
 * wl_surface.commit applies it atomically.
 *
 * Pixels arrive via wl_shm: the client creates a memfd, sends us the fd (wl_shm.create_pool),
 * and carves wl_buffers out of it. On commit we copy the damaged part into our own memory
 * and release the buffer right away, so the client can draw into it again immediately. */
#include "server.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


static struct list inflight_cbs;   /* frame callbacks for the frame being displayed */

/* ------------------------------------------------------------------- regions */

static void region_push(struct region *r, bool add, struct rect rc) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->ops = realloc(r->ops, r->cap * sizeof *r->ops);
    }
    r->ops[r->n++] = (struct region_op){ add, rc };
}

/* A region is a list of add/subtract rectangles; replaying them answers "is (x,y) in it?" */
bool region_contains(const struct region *r, int x, int y) {
    bool in = false;
    for (int i = 0; i < r->n; i++)
        if (rect_contains(r->ops[i].r, x, y)) in = r->ops[i].add;
    return in;
}

static void region_copy(struct region *dst, const struct region *src) {
    dst->n = 0;
    for (int i = 0; i < src->n; i++) region_push(dst, src->ops[i].add, src->ops[i].r);
}

static void region_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op == 1 || op == 2)   /* add / subtract */
        region_push(o->data, op == 1, (struct rect){ a[0].i, a[1].i, a[2].i, a[3].i });
}

static void region_destroy(struct object *o) {
    struct region *r = o->data;
    free(r->ops);
    free(r);
}

static const char *const region_reqs[] = { "destroy", "add iiii", "subtract iiii" };
static const struct iface wl_region_iface = {
    "wl_region", 3, region_reqs, 1u << 0, region_dispatch, region_destroy };

/* ----------------------------------------------------------------------- shm */

static sigjmp_buf bus_jmp;
static volatile sig_atomic_t bus_armed;

/* If a client shrinks its memfd under us, touching the mapping raises SIGBUS. While we're
 * copying a buffer we catch that and treat it as a client error instead of crashing. */
static void on_sigbus(int sig, siginfo_t *info, void *ctx) {
    (void)info; (void)ctx;
    if (bus_armed) siglongjmp(bus_jmp, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_sigbus_handler(void) {
    struct sigaction sa = { .sa_sigaction = on_sigbus, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigaction(SIGBUS, &sa, NULL);
}

static void pool_unref(struct pool *p) {
    if (--p->refs > 0) return;
    munmap(p->data, p->size);
    close(p->fd);
    free(p);
}

static void buffer_unref(struct buffer *b) {
    if (!b || --b->refs > 0) return;
    pool_unref(b->pool);
    free(b);
}

static void buffer_destroy(struct object *o) {
    struct buffer *b = o->data;
    b->obj = NULL;
    buffer_unref(b);
}

static const char *const buffer_reqs[] = { "destroy" };
static const struct iface wl_buffer_iface = { "wl_buffer", 1, buffer_reqs, 1u << 0, NULL, buffer_destroy };

enum { SHM_ERR_INVALID_FORMAT = 0, SHM_ERR_INVALID_STRIDE = 1, SHM_ERR_INVALID_FD = 2 };
enum { SHM_FORMAT_ARGB8888 = 0, SHM_FORMAT_XRGB8888 = 1 };

static void pool_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct pool *p = o->data;
    if (op == 0) {   /* create_buffer */
        int32_t offset = a[1].i, w = a[2].i, h = a[3].i, stride = a[4].i;
        uint32_t format = a[5].u;
        if (format != SHM_FORMAT_ARGB8888 && format != SHM_FORMAT_XRGB8888) {
            client_error(o, SHM_ERR_INVALID_FORMAT, "unsupported format %u", format);
            return;
        }
        if (offset < 0 || w <= 0 || h <= 0 || stride < w * 4 ||
            (uint64_t)offset + (uint64_t)stride * h > p->size) {
            client_error(o, SHM_ERR_INVALID_STRIDE, "buffer %dx%d stride %d doesn't fit the pool", w, h, stride);
            return;
        }
        struct buffer *b = calloc(1, sizeof *b);
        *b = (struct buffer){ .pool = p, .offset = offset, .width = w, .height = h,
                              .stride = stride, .format = format, .refs = 1 };
        b->obj = obj_new(o->client, a[0].n, &wl_buffer_iface, 1, b);
        if (!b->obj) { free(b); return; }
        p->refs++;
    } else if (op == 2) {   /* resize: pools may only grow */
        int32_t size = a[0].i;
        if (size < (int64_t)p->size) {
            client_error(o, SHM_ERR_INVALID_STRIDE, "pool can't shrink");
            return;
        }
        void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, p->fd, 0);
        if (m == MAP_FAILED) {
            client_error(o, SHM_ERR_INVALID_FD, "can't map resized pool");
            return;
        }
        munmap(p->data, p->size);
        p->data = m;
        p->size = size;
    }
}

static void pool_destroy(struct object *o) {
    struct pool *p = o->data;
    p->obj = NULL;
    pool_unref(p);   /* buffers keep it alive until they go too */
}

static const char *const pool_reqs[] = { "create_buffer niiiiu", "destroy", "resize i" };
static const struct iface wl_shm_pool_iface = {
    "wl_shm_pool", 3, pool_reqs, 1u << 1, pool_dispatch, pool_destroy };

static void shm_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op != 0) return;   /* release (v2) is a destructor */
    int fd = a[1].h;
    int32_t size = a[2].i;
    if (size <= 0) {
        client_error(o, SHM_ERR_INVALID_STRIDE, "invalid pool size %d", size);
        return;
    }
    void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        client_error(o, SHM_ERR_INVALID_FD, "can't mmap pool fd");
        return;
    }
    struct pool *p = calloc(1, sizeof *p);
    *p = (struct pool){ .fd = fd, .data = m, .size = size, .refs = 1 };
    p->obj = obj_new(o->client, a[0].n, &wl_shm_pool_iface, 1, p);
    if (!p->obj) {
        munmap(m, size);
        free(p);
        return;
    }
    a[1].h = -1;   /* we keep the fd (needed to remap on resize) */
}

static const char *const shm_reqs[] = { "create_pool nhi", "release" };
static const struct iface wl_shm_iface = { "wl_shm", 2, shm_reqs, 1u << 1, shm_dispatch, NULL };

static void shm_bind(struct client *c, uint32_t id, uint32_t version) {
    struct object *o = obj_new(c, id, &wl_shm_iface, version, NULL);
    if (!o) return;
    obj_send(o, 0, "u", SHM_FORMAT_ARGB8888);   /* format */
    obj_send(o, 0, "u", SHM_FORMAT_XRGB8888);
}

/* ------------------------------------------------------------------ surfaces */

static void state_init(struct surface_state *st) {
    memset(st, 0, sizeof *st);
    list_init(&st->frame_cbs);
    st->input_infinite = true;
}

static void cbs_detach(struct list *l) {
    struct object *cb, *tmp;
    list_for_each_safe(cb, tmp, l, link) list_remove(&cb->link);
}

static void splice(struct list *dst, struct list *src) {
    while (!list_empty(src)) {
        struct list *e = src->next;
        list_remove(e);
        list_append(dst, e);
    }
}

static void state_clear(struct surface_state *st) {
    if (st->attached) buffer_unref(st->buffer);
    cbs_detach(&st->frame_cbs);
    free(st->input.ops);
    state_init(st);
}

static void rect_union(struct rect *a, struct rect b) {
    int x0 = MIN(a->x, b.x), y0 = MIN(a->y, b.y);
    int x1 = MAX(a->x + a->w, b.x + b.w), y1 = MAX(a->y + a->h, b.y + b.h);
    *a = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

static void add_damage(struct surface_state *st, struct rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (st->damaged) rect_union(&st->damage, r);
    else st->damage = r;
    st->damaged = true;
}

/* move everything set in `src` into `dst` (later state wins), leaving src empty */
static void state_take(struct surface_state *dst, struct surface_state *src) {
    if (src->attached) {
        if (dst->attached) buffer_unref(dst->buffer);
        dst->attached = true;
        dst->buffer = src->buffer;
        dst->dx += src->dx;
        dst->dy += src->dy;
    }
    if (src->damaged) add_damage(dst, src->damage);
    dst->damage_all |= src->damage_all;
    splice(&dst->frame_cbs, &src->frame_cbs);
    if (src->input_set) {
        dst->input_set = true;
        dst->input_infinite = src->input_infinite;
        region_copy(&dst->input, &src->input);
    }
    if (src->scale_set) {
        dst->scale_set = true;
        dst->scale = src->scale;
    }
    src->attached = false;
    src->buffer = NULL;
    src->dx = src->dy = 0;
    src->damaged = src->damage_all = false;
    src->input_set = src->scale_set = false;
}

static void copy_rows(struct surface *s, struct buffer *b, struct rect d) {
    const uint8_t *base = b->pool->data + b->offset;
    bool xrgb = b->format == SHM_FORMAT_XRGB8888;
    for (int y = d.y; y < d.y + d.h; y++) {
        const uint8_t *src = base + (size_t)y * b->stride + (size_t)d.x * 4;
        uint32_t *dst = s->pixels + (size_t)y * s->buf_w + d.x;
        memcpy(dst, src, (size_t)d.w * 4);
        if (xrgb) for (int x = 0; x < d.w; x++) dst[x] |= 0xff000000;   /* X = "ignore": make it opaque */
    }
}

static bool copy_pixels(struct surface *s, struct buffer *b, struct rect d) {
    if ((uint64_t)b->offset + (uint64_t)b->stride * b->height > b->pool->size) return false;
    bus_armed = 1;
    if (sigsetjmp(bus_jmp, 1)) {   /* we land here if the copy hit SIGBUS */
        bus_armed = 0;
        return false;
    }
    copy_rows(s, b, d);
    bus_armed = 0;
    return true;
}

static void surface_apply(struct surface *s, struct surface_state *st) {
    if (st->scale_set) s->scale = st->scale;
    if (st->attached) {
        struct buffer *b = st->buffer;
        if (b) {
            bool full = !s->pixels || b->width != s->buf_w || b->height != s->buf_h || st->damage_all;
            if (full) {
                free(s->pixels);
                s->buf_w = b->width;
                s->buf_h = b->height;
                s->pixels = calloc((size_t)b->width * b->height, 4);
            }
            struct rect all = { 0, 0, b->width, b->height }, d = all;
            if (!full) {
                if (!st->damaged) d.w = d.h = 0;
                else {   /* clip damage to the buffer */
                    int x0 = MAX(st->damage.x, 0), y0 = MAX(st->damage.y, 0);
                    int x1 = MIN(st->damage.x + st->damage.w, b->width);
                    int y1 = MIN(st->damage.y + st->damage.h, b->height);
                    d = (struct rect){ x0, y0, MAX(x1 - x0, 0), MAX(y1 - y0, 0) };
                }
            }
            if (d.w > 0 && d.h > 0 && !copy_pixels(s, b, d))
                client_error(s->obj, 2, "buffer memory is not accessible");   /* wl_shm invalid_fd */
            s->opaque = b->format == SHM_FORMAT_XRGB8888;
            if (b->obj) obj_send(b->obj, 0, "");   /* wl_buffer.release: we have our copy */
            buffer_unref(b);
        } else {
            free(s->pixels);
            s->pixels = NULL;
            s->buf_w = s->buf_h = 0;
        }
        int sc = s->scale > 0 ? s->scale : 1;
        s->width = s->buf_w / sc;
        s->height = s->buf_h / sc;
        if (s->role == ROLE_CURSOR || s->role == ROLE_DND_ICON) {   /* both drawn at pointer - hotspot */
            s->cursor_hot_x -= st->dx;
            s->cursor_hot_y -= st->dy;
        } else if (s->role == ROLE_XDG && s->xdg && s->xdg->toplevel) {
            s->xdg->toplevel->x += st->dx;
            s->xdg->toplevel->y += st->dy;
        }
        st->attached = false;
        st->buffer = NULL;
        st->dx = st->dy = 0;
    }
    if (st->input_set) {
        s->input_infinite = st->input_infinite;
        region_copy(&s->input, &st->input);
        st->input_set = false;
    }
    st->damaged = st->damage_all = false;
    st->scale_set = false;
    splice(&s->frame_cbs, &st->frame_cbs);
}

/* a subsurface is synchronized if it, or any ancestor subsurface, is in sync mode */
static bool effectively_sync(struct surface *s) {
    for (; s && s->sub && s->sub->parent; s = s->sub->parent)
        if (s->sub->sync) return true;
    return false;
}

static void after_apply(struct surface *s) {
    struct snode *n;
    list_for_each(n, &s->stack, link) {
        struct surface *child = n->surface;
        if (child == s || !child->sub) continue;
        child->sub->x = child->sub->pend_x;   /* positions apply on the parent's commit */
        child->sub->y = child->sub->pend_y;
        if (child->has_cached && effectively_sync(child)) {
            surface_apply(child, &child->cached);
            child->has_cached = false;
            after_apply(child);
        }
    }
    if (s->role == ROLE_XDG && s->xdg) xdg_surface_commit(s->xdg);
    if (s->role == ROLE_CURSOR || s->role == ROLE_DND_ICON) wm_cursor_changed();
    wm_damage();
}

static void surface_commit(struct surface *s) {
    state_take(&s->cached, &s->pending);
    if (s->role == ROLE_SUBSURFACE && effectively_sync(s)) {
        s->has_cached = true;   /* wait for the parent's commit */
        return;
    }
    surface_apply(s, &s->cached);
    s->has_cached = false;
    after_apply(s);
}

static void surface_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct surface *s = o->data;
    struct object *obj;
    int sc = s->pending.scale_set ? s->pending.scale : (s->scale > 0 ? s->scale : 1);
    switch (op) {
    case 1:   /* attach(buffer, x, y) */
        if (!arg_obj(o, a[0].o, &wl_buffer_iface, true, &obj)) return;
        if (s->pending.attached) buffer_unref(s->pending.buffer);
        s->pending.attached = true;
        s->pending.buffer = obj ? obj->data : NULL;
        if (s->pending.buffer) s->pending.buffer->refs++;
        if (o->version < 5) {
            s->pending.dx = a[1].i;
            s->pending.dy = a[2].i;
        }
        break;
    case 2:   /* damage (surface coordinates) */
        add_damage(&s->pending, (struct rect){ a[0].i * sc, a[1].i * sc, a[2].i * sc, a[3].i * sc });
        break;
    case 3: { /* frame(callback) */
        struct object *cb = obj_new(o->client, a[0].n, &wl_callback_iface, 1, NULL);
        if (cb) list_append(&s->pending.frame_cbs, &cb->link);
        break;
    }
    case 4:   /* set_opaque_region: an optimization hint we don't need */
        break;
    case 5:   /* set_input_region */
        if (!arg_obj(o, a[0].o, &wl_region_iface, true, &obj)) return;
        s->pending.input_set = true;
        s->pending.input_infinite = obj == NULL;
        if (obj) region_copy(&s->pending.input, obj->data);
        break;
    case 6:
        surface_commit(s);
        break;
    case 7:   /* set_buffer_transform: rotation; not supported, treated as normal */
        break;
    case 8:   /* set_buffer_scale */
        if (a[0].i < 1) { client_error(o, 0, "invalid scale %d", a[0].i); return; }
        s->pending.scale_set = true;
        s->pending.scale = a[0].i;
        break;
    case 9:   /* damage_buffer (buffer coordinates) */
        add_damage(&s->pending, (struct rect){ a[0].i, a[1].i, a[2].i, a[3].i });
        break;
    case 10:  /* offset(x, y) */
        s->pending.dx = a[0].i;
        s->pending.dy = a[1].i;
        break;
    }
}

static void surface_destroy(struct object *o) {
    struct surface *s = o->data;
    wm_surface_destroyed(s);
    seat_surface_destroyed(s);
    if (s->xdg) s->xdg->surface = NULL;
    if (s->sub) {
        list_remove(&s->parent_node.link);
        s->sub->surface = NULL;
    }
    struct snode *n, *tmp;
    list_for_each_safe(n, tmp, &s->stack, link) {
        list_remove(&n->link);
        if (n->surface != s && n->surface->sub) n->surface->sub->parent = NULL;
    }
    state_clear(&s->pending);
    state_clear(&s->cached);
    cbs_detach(&s->frame_cbs);
    free(s->input.ops);
    free(s->pixels);
    list_remove(&s->link);
    free(s);
}

static const char *const surface_reqs[] = {
    "destroy", "attach ?oii", "damage iiii", "frame n", "set_opaque_region ?o",
    "set_input_region ?o", "commit", "set_buffer_transform i", "set_buffer_scale i",
    "damage_buffer iiii", "offset ii",
};
const struct iface wl_surface_iface = {
    "wl_surface", 11, surface_reqs, 1u << 0, surface_dispatch, surface_destroy };

struct surface *surface_from_obj(struct object *o) {
    return o && o->iface == &wl_surface_iface ? o->data : NULL;
}

bool surface_set_role(struct surface *s, enum surface_role role, struct object *err_obj, uint32_t code) {
    if (s->role != ROLE_NONE && s->role != role) {
        client_error(err_obj, code, "surface already has another role");
        return false;
    }
    s->role = role;
    return true;
}

void surface_for_each(struct surface *s, int x, int y,
                      void (*fn)(struct surface *s, int x, int y, void *data), void *data) {
    struct snode *n;
    list_for_each(n, &s->stack, link) {
        if (n->surface == s) fn(s, x, y, data);
        else if (n->surface->sub)
            surface_for_each(n->surface, x + n->surface->sub->x, y + n->surface->sub->y, fn, data);
    }
}

struct surface *surface_at(struct surface *s, int x, int y, double px, double py, double *sx, double *sy) {
    struct snode *n;
    list_for_each_reverse(n, &s->stack, link) {   /* topmost first */
        if (n->surface != s) {
            if (!n->surface->sub) continue;
            struct surface *hit = surface_at(n->surface, x + n->surface->sub->x,
                                             y + n->surface->sub->y, px, py, sx, sy);
            if (hit) return hit;
            continue;
        }
        if (!s->pixels) continue;
        double lx = px - x, ly = py - y;
        if (lx < 0 || ly < 0 || lx >= s->width || ly >= s->height) continue;
        if (!s->input_infinite && !region_contains(&s->input, (int)lx, (int)ly)) continue;
        *sx = lx;
        *sy = ly;
        return s;
    }
    return NULL;
}

bool surface_abs_pos(struct surface *s, int *x, int *y) {
    int ax = 0, ay = 0;
    while (s->sub) {
        if (!s->sub->parent) return false;
        ax += s->sub->x;
        ay += s->sub->y;
        s = s->sub->parent;
    }
    if (s->role != ROLE_XDG || !s->xdg || !s->xdg->mapped) return false;
    int ox, oy;
    if (!xdg_abs_origin(s->xdg, &ox, &oy)) return false;
    struct rect g = xdg_geometry(s->xdg);
    *x = ax + ox - g.x;
    *y = ay + oy - g.y;
    return true;
}

void frame_callbacks_collect(void) {
    struct surface *s;
    list_for_each(s, &srv.surfaces, link) splice(&inflight_cbs, &s->frame_cbs);
}

void frame_callbacks_done(uint32_t time) {
    struct object *cb, *tmp;
    list_for_each_safe(cb, tmp, &inflight_cbs, link) {
        obj_send(cb, 0, "u", time);   /* wl_callback.done */
        obj_destroy(cb);
    }
}

/* --------------------------------------------------------------- subsurfaces */

static void subsurface_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    struct subsurface *sub = o->data;
    struct surface *s = sub->surface;
    if (!s || !sub->parent) return;   /* inert: its surface or parent is gone */
    struct surface *parent = sub->parent;
    switch (op) {
    case 1:   /* set_position */
        sub->pend_x = a[0].i;
        sub->pend_y = a[1].i;
        break;
    case 2: case 3: {   /* place_above / place_below a sibling (or the parent) */
        struct object *so;
        if (!arg_obj(o, a[0].o, &wl_surface_iface, false, &so)) return;
        struct surface *sib = so->data;
        struct snode *ref;
        if (sib == parent) ref = &parent->self_node;
        else if (sib->sub && sib->sub->parent == parent && sib != s) ref = &sib->parent_node;
        else { client_error(o, 0, "not a sibling"); return; }
        list_remove(&s->parent_node.link);
        if (op == 2) list_insert(&ref->link, &s->parent_node.link);
        else list_insert(ref->link.prev, &s->parent_node.link);
        break;
    }
    case 4: sub->sync = true; break;
    case 5:
        sub->sync = false;
        /* cached state, if any, goes out with the next commit (surface_commit merges) */
        break;
    }
}

static void subsurface_destroy(struct object *o) {
    struct subsurface *sub = o->data;
    if (sub->surface) {
        list_remove(&sub->surface->parent_node.link);
        sub->surface->sub = NULL;
    }
    free(sub);
    wm_damage();
}

static const char *const subsurface_reqs[] = {
    "destroy", "set_position ii", "place_above o", "place_below o", "set_sync", "set_desync" };
static const struct iface wl_subsurface_iface = {
    "wl_subsurface", 6, subsurface_reqs, 1u << 0, subsurface_dispatch, subsurface_destroy };

static bool is_ancestor(struct surface *maybe, struct surface *s) {
    for (; s; s = s->sub ? s->sub->parent : NULL)
        if (s == maybe) return true;
    return false;
}

static void subcompositor_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op != 1) return;
    struct object *so, *po;
    if (!arg_obj(o, a[1].o, &wl_surface_iface, false, &so)) return;
    if (!arg_obj(o, a[2].o, &wl_surface_iface, false, &po)) return;
    struct surface *s = so->data, *parent = po->data;
    if (s->sub || is_ancestor(s, parent)) {
        client_error(o, 1, "bad subsurface parent");   /* bad_parent */
        return;
    }
    if (!surface_set_role(s, ROLE_SUBSURFACE, o, 0)) return;
    struct subsurface *sub = calloc(1, sizeof *sub);
    sub->surface = s;
    sub->parent = parent;
    sub->sync = true;
    sub->obj = obj_new(o->client, a[0].n, &wl_subsurface_iface, 1, sub);
    if (!sub->obj) { free(sub); return; }
    s->sub = sub;
    list_append(&parent->stack, &s->parent_node.link);   /* on top of its siblings */
}

static const char *const subcompositor_reqs[] = { "destroy", "get_subsurface noo" };
static const struct iface wl_subcompositor_iface = {
    "wl_subcompositor", 2, subcompositor_reqs, 1u << 0, subcompositor_dispatch, NULL };

static void subcompositor_bind(struct client *c, uint32_t id, uint32_t version) {
    obj_new(c, id, &wl_subcompositor_iface, version, NULL);
}

/* --------------------------------------------------------------- compositor */

static void compositor_dispatch(struct object *o, uint16_t op, union wire_arg *a) {
    if (op == 0) {   /* create_surface */
        struct surface *s = calloc(1, sizeof *s);
        s->client = o->client;
        s->scale = 1;
        s->input_infinite = true;
        state_init(&s->pending);
        state_init(&s->cached);
        list_init(&s->frame_cbs);
        list_init(&s->stack);
        s->self_node.surface = s;
        s->parent_node.surface = s;
        list_init(&s->parent_node.link);
        list_append(&s->stack, &s->self_node.link);
        s->obj = obj_new(o->client, a[0].n, &wl_surface_iface, o->version, s);
        if (!s->obj) { free(s); return; }
        list_append(&srv.surfaces, &s->link);
    } else {         /* create_region */
        struct region *r = calloc(1, sizeof *r);
        if (!obj_new(o->client, a[0].n, &wl_region_iface, 1, r)) free(r);
    }
}

static const char *const compositor_reqs[] = { "create_surface n", "create_region n" };
static const struct iface wl_compositor_iface = {
    "wl_compositor", 2, compositor_reqs, 0, compositor_dispatch, NULL };

static void compositor_bind(struct client *c, uint32_t id, uint32_t version) {
    obj_new(c, id, &wl_compositor_iface, version, NULL);
}

void surface_init_globals(void) {
    list_init(&inflight_cbs);
    global_add(&wl_compositor_iface, 5, compositor_bind);
    global_add(&wl_subcompositor_iface, 1, subcompositor_bind);
    global_add(&wl_shm_iface, 1, shm_bind);
}
