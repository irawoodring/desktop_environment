/* backend_fbdev.c — fallback for machines without a usable DRM driver.
 *
 * /dev/fb0 is one mmap-able block of pixels. There's no page flipping and no vblank
 * event, so we copy the frame in and pace ourselves with a 60 Hz timer. */
#include "server.h"
#include "backend.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct fb_backend {
    struct backend base;
    int fd, timer;
    uint8_t *map;
    size_t map_len;
    uint32_t line_length;
    struct fb_var_screeninfo var;
    struct watch *watch;
};

static void fb_tick(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof expirations) > 0) server_frame_done();
}

static void fb_present(struct backend *b) {
    struct fb_backend *f = (struct fb_backend *)b;
    for (int y = 0; y < b->height; y++)
        memcpy(f->map + (size_t)y * f->line_length, b->shadow + (size_t)y * b->width, b->width * 4);
    struct itimerspec its = { .it_value = { 0, 1000000000L / 60 } };
    timerfd_settime(f->timer, 0, &its, NULL);
}

static void fb_destroy(struct backend *b) {
    struct fb_backend *f = (struct fb_backend *)b;
    watch_remove(f->watch);
    memset(f->map, 0, f->map_len);
    munmap(f->map, f->map_len);
    close(f->fd);
    close(f->timer);
    free(b->shadow);
    free(f);
}

struct backend *backend_fbdev_create(const char *device) {
    const char *path = device ? device : "/dev/fb0";
    struct fb_backend *f = calloc(1, sizeof *f);
    f->fd = open(path, O_RDWR | O_CLOEXEC);
    if (f->fd < 0) { perror(path); free(f); return NULL; }

    struct fb_fix_screeninfo fix;
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) < 0 || ioctl(f->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("fbdev ioctl");
        goto fail;
    }
    /* we only speak XRGB8888 — what nearly every modern framebuffer is */
    if (f->var.bits_per_pixel != 32 || f->var.red.offset != 16 || f->var.green.offset != 8 ||
        f->var.blue.offset != 0) {
        fprintf(stderr, "fbdev: unsupported pixel format (%u bpp)\n", f->var.bits_per_pixel);
        goto fail;
    }
    f->line_length = fix.line_length;
    f->map_len = fix.smem_len;
    f->map = mmap(NULL, f->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->map == MAP_FAILED) { perror("mmap fb"); goto fail; }
    f->timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    struct backend *b = &f->base;
    b->name = "fbdev";
    b->width = f->var.xres;
    b->height = f->var.yres;
    b->refresh_mhz = 60000;
    b->phys_w_mm = f->var.width > 0 && f->var.width < 10000 ? (int)f->var.width : 0;
    b->phys_h_mm = f->var.height > 0 && f->var.height < 10000 ? (int)f->var.height : 0;
    b->shadow = calloc((size_t)b->width * b->height, 4);
    b->present = fb_present;
    b->destroy = fb_destroy;
    f->watch = watch_add(f->timer, EPOLLIN, fb_tick, f);
    fprintf(stderr, "fbdev: %s %dx%d\n", path, b->width, b->height);
    return b;
fail:
    close(f->fd);
    free(f);
    return NULL;
}
