/* backend_headless.c — no screen at all: frames live in memory and can be saved as PNG.
 *
 * Useful for developing over SSH, for automated tests, and for screenshots. Pair it with
 * the test console in main.c (commands on stdin) to inject input. */
#include "server.h"
#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <zlib.h>

struct headless {
    struct backend base;
    int timer;
    struct watch *watch;
};

static void tick(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof expirations) > 0) server_frame_done();
}

static void present(struct backend *b) {
    struct headless *h = (struct headless *)b;
    struct itimerspec its = { .it_value = { 0, 1000000000L / 60 } };
    timerfd_settime(h->timer, 0, &its, NULL);
}

static void destroy(struct backend *b) {
    struct headless *h = (struct headless *)b;
    watch_remove(h->watch);
    close(h->timer);
    free(b->shadow);
    free(h);
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t be[4] = { len >> 24, len >> 16, len >> 8, len };
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t c[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
    fwrite(c, 1, 4, f);
}

/* A PNG is a signature plus chunks; the pixels are zlib-compressed scanlines. */
int write_png(const char *path, const uint32_t *px, int w, int h) {
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;   /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * w + x];
            *p++ = v >> 16; *p++ = v >> 8; *p++ = v;
        }
    }
    uLongf zlen = compressBound(raw_len);
    uint8_t *z = malloc(zlen);
    compress2(z, &zlen, raw, raw_len, 6);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h,
                         8, 2, 0, 0, 0 };   /* 8-bit RGB */
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

static int screenshot(struct backend *b, const char *path) {
    return write_png(path, b->shadow, b->width, b->height);
}

struct backend *backend_headless_create(int width, int height) {
    struct headless *h = calloc(1, sizeof *h);
    struct backend *b = &h->base;
    b->name = "headless";
    b->width = width;
    b->height = height;
    b->refresh_mhz = 60000;
    b->shadow = calloc((size_t)width * height, 4);
    b->present = present;
    b->destroy = destroy;
    b->screenshot = screenshot;
    h->timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    h->watch = watch_add(h->timer, EPOLLIN, tick, h);
    return b;
}
