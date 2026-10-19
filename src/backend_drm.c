/* backend_drm.c — modesetting with raw DRM/KMS ioctls (no libdrm).
 *
 * The steps, which are the same ones libdrm and every Wayland compositor perform:
 *   1. open /dev/dri/cardN and become "DRM master" (the one process allowed to modeset)
 *   2. list the resources: connectors (monitor ports), encoders, CRTCs (scanout engines)
 *   3. pick a connected connector and its preferred mode (resolution + refresh)
 *   4. find a CRTC that can drive that connector
 *   5. create two "dumb buffers" (plain CPU-mappable scanout memory) and mmap them
 *   6. register them as framebuffers, and point the CRTC at one (SETCRTC = modeset)
 *   7. each frame: copy into the hidden buffer and PAGE_FLIP; the kernel swaps them at
 *      the next vblank and sends us an event, which becomes our frame clock.
 *
 * The headers come from the kernel (linux API headers install <drm/drm.h>). */
#include "server.h"
#include "backend.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct dumb {
    uint32_t handle, fb_id, pitch;
    uint64_t size;
    uint8_t *map;
};

struct drm_backend {
    struct backend base;
    int fd;
    uint32_t conn_id, crtc_id;
    struct drm_mode_modeinfo mode;
    struct dumb buf[2];
    int front;
    struct drm_mode_crtc saved_crtc;
    struct watch *watch;
};

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static bool create_dumb(struct drm_backend *d, struct dumb *b) {
    struct drm_mode_create_dumb create = {
        .width = d->mode.hdisplay, .height = d->mode.vdisplay, .bpp = 32 };
    if (xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) { perror("CREATE_DUMB"); return false; }
    b->handle = create.handle;
    b->pitch = create.pitch;
    b->size = create.size;

    struct drm_mode_fb_cmd fb = {
        .width = d->mode.hdisplay, .height = d->mode.vdisplay,
        .pitch = b->pitch, .bpp = 32, .depth = 24, .handle = b->handle };
    if (xioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) { perror("ADDFB"); return false; }
    b->fb_id = fb.fb_id;

    struct drm_mode_map_dumb map = { .handle = b->handle };
    if (xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) { perror("MAP_DUMB"); return false; }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, map.offset);
    if (b->map == MAP_FAILED) { perror("mmap dumb"); b->map = NULL; return false; }
    memset(b->map, 0, b->size);
    return true;
}

static void destroy_dumb(struct drm_backend *d, struct dumb *b) {
    if (b->map) munmap(b->map, b->size);
    if (b->fb_id) xioctl(d->fd, DRM_IOCTL_MODE_RMFB, &b->fb_id);
    if (b->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = b->handle };
        xioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}

/* Pick the first connected connector, its preferred mode, and a CRTC for it. */
static bool pick_output(struct drm_backend *d) {
    struct drm_mode_card_res res = { 0 };
    if (xioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { perror("GETRESOURCES"); return false; }
    /* first call told us the counts; second call fills arrays we provide */
    uint32_t *conns = calloc(res.count_connectors + 1, 4), *crtcs = calloc(res.count_crtcs + 1, 4);
    uint32_t *encs = calloc(res.count_encoders + 1, 4), *fbs = calloc(res.count_fbs + 1, 4);
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    bool ok = false;
    if (xioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { perror("GETRESOURCES"); goto out; }

    for (uint32_t i = 0; i < res.count_connectors && !ok; i++) {
        struct drm_mode_get_connector conn = { .connector_id = conns[i] };
        if (xioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != 1 || conn.count_modes == 0) continue;   /* 1 = connected */

        struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof *modes);
        uint32_t *conn_encs = calloc(conn.count_encoders + 1, 4);
        uint32_t nmodes = conn.count_modes, nencs = conn.count_encoders;
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)conn_encs;
        conn.count_props = 0;
        conn.props_ptr = conn.prop_values_ptr = 0;
        if (xioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || conn.count_modes > nmodes ||
            conn.count_encoders > nencs) {
            free(modes);
            free(conn_encs);
            continue;
        }

        d->mode = modes[0];
        for (uint32_t m = 0; m < conn.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { d->mode = modes[m]; break; }
        d->base.phys_w_mm = conn.mm_width;
        d->base.phys_h_mm = conn.mm_height;

        /* Prefer the CRTC already driving this connector (what the console uses). */
        uint32_t crtc = 0;
        if (conn.encoder_id) {
            struct drm_mode_get_encoder enc = { .encoder_id = conn.encoder_id };
            if (xioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) crtc = enc.crtc_id;
        }
        for (uint32_t e = 0; !crtc && e < conn.count_encoders; e++) {
            struct drm_mode_get_encoder enc = { .encoder_id = conn_encs[e] };
            if (xioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) continue;
            for (uint32_t c = 0; c < res.count_crtcs; c++)
                if (enc.possible_crtcs & (1u << c)) { crtc = crtcs[c]; break; }
        }
        if (crtc) {
            d->conn_id = conns[i];
            d->crtc_id = crtc;
            ok = true;
        }
        free(modes);
        free(conn_encs);
    }
    if (!ok) fprintf(stderr, "drm: no connected monitor found\n");
out:
    free(conns);
    free(crtcs);
    free(encs);
    free(fbs);
    return ok;
}

static void drm_event(int fd, uint32_t events, void *data) {
    (void)events;
    struct drm_backend *d = data;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf);
    for (ssize_t off = 0; off + (ssize_t)sizeof(struct drm_event) <= n;) {
        struct drm_event *e = (struct drm_event *)(buf + off);
        if (e->type == DRM_EVENT_FLIP_COMPLETE) {
            d->front ^= 1;          /* the buffer we flipped to is now on screen */
            server_frame_done();
        }
        off += e->length;
    }
}

static void drm_present(struct backend *b) {
    struct drm_backend *d = (struct drm_backend *)b;
    struct dumb *back = &d->buf[d->front ^ 1];
    for (int y = 0; y < b->height; y++)
        memcpy(back->map + (size_t)y * back->pitch, b->shadow + (size_t)y * b->width, b->width * 4);

    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = d->crtc_id, .fb_id = back->fb_id, .flags = DRM_MODE_PAGE_FLIP_EVENT };
    if (xioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
        /* Some drivers refuse flips in odd states; fall back to a plain modeset. */
        struct drm_mode_crtc crtc = {
            .crtc_id = d->crtc_id, .fb_id = back->fb_id,
            .set_connectors_ptr = (uintptr_t)&d->conn_id, .count_connectors = 1,
            .mode = d->mode, .mode_valid = 1 };
        xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
        d->front ^= 1;
        server_frame_done();
    }
}

static void drm_destroy(struct backend *b) {
    struct drm_backend *d = (struct drm_backend *)b;
    /* put back whatever the console was showing */
    if (d->saved_crtc.fb_id) {
        d->saved_crtc.set_connectors_ptr = (uintptr_t)&d->conn_id;
        d->saved_crtc.count_connectors = 1;
        xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &d->saved_crtc);
    }
    watch_remove(d->watch);
    destroy_dumb(d, &d->buf[0]);
    destroy_dumb(d, &d->buf[1]);
    xioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL);
    close(d->fd);
    free(b->shadow);
    free(d);
}

struct backend *backend_drm_create(const char *device) {
    struct drm_backend *d = calloc(1, sizeof *d);
    d->fd = open(device ? device : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (d->fd < 0) { perror(device ? device : "/dev/dri/card0"); free(d); return NULL; }

    struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
    if (xioctl(d->fd, DRM_IOCTL_GET_CAP, &cap) < 0 || !cap.value) {
        fprintf(stderr, "drm: driver has no dumb buffers\n");
        goto fail;
    }
    /* Opening the device usually makes us master already; asking again is harmless. */
    xioctl(d->fd, DRM_IOCTL_SET_MASTER, NULL);

    if (!pick_output(d)) goto fail;
    d->saved_crtc.crtc_id = d->crtc_id;
    xioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &d->saved_crtc);

    if (!create_dumb(d, &d->buf[0]) || !create_dumb(d, &d->buf[1])) goto fail;

    struct drm_mode_crtc crtc = {
        .crtc_id = d->crtc_id, .fb_id = d->buf[0].fb_id,
        .set_connectors_ptr = (uintptr_t)&d->conn_id, .count_connectors = 1,
        .mode = d->mode, .mode_valid = 1 };
    if (xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) { perror("SETCRTC"); goto fail; }
    d->front = 0;

    struct backend *b = &d->base;
    b->name = "drm";
    b->width = d->mode.hdisplay;
    b->height = d->mode.vdisplay;
    /* mode.clock is in kHz; refresh = pixel clock / total pixels per frame */
    b->refresh_mhz = (int)((uint64_t)d->mode.clock * 1000000 / ((uint64_t)d->mode.htotal * d->mode.vtotal));
    b->shadow = calloc((size_t)b->width * b->height, 4);
    b->present = drm_present;
    b->destroy = drm_destroy;
    d->watch = watch_add(d->fd, EPOLLIN, drm_event, d);
    fprintf(stderr, "drm: %s %dx%d@%.2fHz on connector %u, crtc %u\n", device ? device : "/dev/dri/card0",
            b->width, b->height, b->refresh_mhz / 1000.0, d->conn_id, d->crtc_id);
    return b;

fail:
    destroy_dumb(d, &d->buf[0]);
    destroy_dumb(d, &d->buf[1]);
    close(d->fd);
    free(d);
    return NULL;
}
