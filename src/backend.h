/* backend.h — how finished frames get onto a screen.
 *
 * Every backend gives the compositor a plain system-memory XRGB8888 "shadow" buffer to
 * draw into. On present() it copies the shadow into display memory and shows it, then
 * calls server_frame_done() when the frame is actually on screen (after vblank for DRM).
 * Drawing in system RAM matters: display memory is often write-combined, and reading it
 * back (which alpha blending does) is extremely slow. */
#pragma once
#include <stdint.h>

struct backend {
    const char *name;
    int width, height;
    int refresh_mhz;               /* refresh rate in millihertz, e.g. 60000 */
    int phys_w_mm, phys_h_mm;
    uint32_t *shadow;              /* draw here: width * height pixels, stride == width */
    void (*present)(struct backend *b);
    void (*destroy)(struct backend *b);
    /* headless only: write the current frame to a PNG */
    int (*screenshot)(struct backend *b, const char *path);
};

struct backend *backend_drm_create(const char *device);
struct backend *backend_fbdev_create(const char *device);
struct backend *backend_headless_create(int width, int height);

int write_png(const char *path, const uint32_t *px, int w, int h);
