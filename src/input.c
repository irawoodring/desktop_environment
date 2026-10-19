/* input.c — keyboards and mice straight from /dev/input/event* (evdev).
 *
 * Each device produces fixed-size `struct input_event` records:
 *   EV_KEY  key/button code, value 1 = down, 0 = up, 2 = autorepeat
 *   EV_REL  relative motion (mice): REL_X, REL_Y, REL_WHEEL...
 *   EV_ABS  absolute position (touchpads, tablets, VM pointers): ABS_X, ABS_Y
 *   EV_SYN  SYN_REPORT: "that's one complete update, act on it now"
 * We grab every device (EVIOCGRAB) so the text console stops seeing keystrokes. */
#include "server.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_LONG (sizeof(long) * 8)
#define NLONGS(n) (((n) + BITS_LONG - 1) / BITS_LONG)

struct device {
    struct list link;
    int fd;
    struct watch *watch;
    char path[64];
    enum { DEV_REL, DEV_TABLET, DEV_TOUCHPAD, DEV_KEYS } kind;
    struct input_absinfo ax, ay;
    double dx, dy;                 /* motion accumulated until SYN_REPORT */
    int abs_x, abs_y;
    bool abs_changed, touching, have_last;
    int last_x, last_y;
};

static struct list devices;
static int inotify_fd = -1;
static struct watch *inotify_watch;

static bool bit(const unsigned long *bits, int n) { return (bits[n / BITS_LONG] >> (n % BITS_LONG)) & 1; }

static void close_device(struct device *d) {
    watch_remove(d->watch);
    ioctl(d->fd, EVIOCGRAB, 0);
    close(d->fd);
    list_remove(&d->link);
    free(d);
}

static void flush_motion(struct device *d) {
    if (d->dx || d->dy) {
        /* a little acceleration: fast flicks travel further */
        double speed = sqrt(d->dx * d->dx + d->dy * d->dy);
        double gain = speed > 4 ? 1.0 + MIN((speed - 4) / 8.0, 1.5) : 1.0;
        wm_pointer_motion(d->dx * gain, d->dy * gain);
        d->dx = d->dy = 0;
    }
    if (d->abs_changed && d->kind == DEV_TABLET) {
        double rx = d->ax.maximum > d->ax.minimum ? (double)(d->abs_x - d->ax.minimum) / (d->ax.maximum - d->ax.minimum) : 0;
        double ry = d->ay.maximum > d->ay.minimum ? (double)(d->abs_y - d->ay.minimum) / (d->ay.maximum - d->ay.minimum) : 0;
        wm_pointer_warp(rx * srv.width, ry * srv.height);
    }
    if (d->abs_changed && d->kind == DEV_TOUCHPAD && d->touching) {
        /* touchpads report finger position; the pointer moves by the change */
        if (d->have_last) {
            double res = d->ax.resolution > 0 ? d->ax.resolution : (d->ax.maximum - d->ax.minimum) / 100.0;
            double per_px = res > 0 ? res / 4.0 : 1;   /* ~4 pixels per millimeter */
            wm_pointer_motion((d->abs_x - d->last_x) / per_px, (d->abs_y - d->last_y) / per_px);
        }
        d->last_x = d->abs_x;
        d->last_y = d->abs_y;
        d->have_last = true;
    }
    d->abs_changed = false;
}

static void device_event(int fd, uint32_t events, void *data) {
    struct device *d = data;
    (void)events;
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(fd, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN) return;
            fprintf(stderr, "input: %s went away\n", d->path);
            close_device(d);
            return;
        }
        if (n == 0) return;
        for (size_t i = 0; i < n / sizeof *evs; i++) {
            struct input_event *e = &evs[i];
            switch (e->type) {
            case EV_REL:
                if (e->code == REL_X) d->dx += e->value;
                else if (e->code == REL_Y) d->dy += e->value;
                /* Wayland axis values: positive = down/right; a wheel notch is 15 "units" */
                else if (e->code == REL_WHEEL) wm_pointer_axis(0, -15.0 * e->value, -e->value);
                else if (e->code == REL_HWHEEL) wm_pointer_axis(1, 15.0 * e->value, e->value);
                break;
            case EV_ABS:
                if (e->code == ABS_X) { d->abs_x = e->value; d->abs_changed = true; }
                else if (e->code == ABS_Y) { d->abs_y = e->value; d->abs_changed = true; }
                break;
            case EV_KEY:
                if (e->value == 2) break;   /* autorepeat: clients do their own */
                if (e->code == BTN_TOUCH && d->kind == DEV_TOUCHPAD) {
                    d->touching = e->value;
                    d->have_last = false;
                } else if (e->code == BTN_TOOL_FINGER || (e->code >= BTN_TOOL_PEN && e->code <= BTN_TOOL_QUINTTAP)) {
                    /* touchpad finger-count tools; not buttons */
                } else if (e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK) {
                    wm_pointer_button(e->code, e->value);
                } else if (e->code == BTN_TOUCH && d->kind == DEV_TABLET) {
                    wm_pointer_button(BTN_LEFT, e->value);
                } else if (e->code < BTN_MISC || (e->code >= KEY_OK && e->code < BTN_DPAD_UP)) {
                    wm_key(e->code, e->value);
                }
                break;
            case EV_SYN:
                if (e->code == SYN_REPORT) flush_motion(d);
                break;
            }
        }
    }
}

static void open_device(const char *path) {
    struct device *d;
    list_for_each(d, &devices, link) if (!strcmp(d->path, path)) return;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    unsigned long ev[NLONGS(EV_CNT)] = { 0 }, keys[NLONGS(KEY_CNT)] = { 0 };
    unsigned long rel[NLONGS(REL_CNT)] = { 0 }, abs[NLONGS(ABS_CNT)] = { 0 }, props[NLONGS(INPUT_PROP_CNT)] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    ioctl(fd, EVIOCGPROP(sizeof props), props);

    bool has_keys = bit(ev, EV_KEY) && bit(keys, KEY_A) && bit(keys, KEY_SPACE);
    bool has_rel = bit(ev, EV_REL) && bit(rel, REL_X) && bit(rel, REL_Y);
    bool has_abs = bit(ev, EV_ABS) && bit(abs, ABS_X) && bit(abs, ABS_Y) &&
                   (bit(keys, BTN_LEFT) || bit(keys, BTN_TOUCH));
    bool touchpad = has_abs && bit(keys, BTN_TOOL_FINGER) && !bit(props, INPUT_PROP_DIRECT);

    if (!has_keys && !has_rel && !has_abs) {
        close(fd);
        return;
    }
    d = calloc(1, sizeof *d);
    d->fd = fd;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->kind = has_rel ? DEV_REL : touchpad ? DEV_TOUCHPAD : has_abs ? DEV_TABLET : DEV_KEYS;
    if (has_abs) {
        ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
    }
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    if (ioctl(fd, EVIOCGRAB, 1) < 0) fprintf(stderr, "input: can't grab %s (%s)\n", path, strerror(errno));
    static const char *const kinds[] = { "mouse", "tablet", "touchpad", "keyboard" };
    fprintf(stderr, "input: %s: %s (%s%s)\n", path, name, kinds[d->kind],
            has_keys && d->kind != DEV_KEYS ? " + keys" : "");
    d->watch = watch_add(fd, EPOLLIN, device_event, d);
    list_append(&devices, &d->link);
}

static void inotify_event_cb(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(fd, buf, sizeof buf);
    for (char *p = buf; n > 0 && p < buf + n;) {
        struct inotify_event *ev = (struct inotify_event *)p;
        if (ev->len && !strncmp(ev->name, "event", 5)) {
            char path[64];
            snprintf(path, sizeof path, "/dev/input/%s", ev->name);
            open_device(path);   /* hotplug */
        }
        p += sizeof *ev + ev->len;
    }
}

int input_init(void) {
    list_init(&devices);
    DIR *dir = opendir("/dev/input");
    if (!dir) { perror("/dev/input"); return -1; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        open_device(path);
    }
    closedir(dir);
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0 && inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB) >= 0)
        inotify_watch = watch_add(inotify_fd, EPOLLIN, inotify_event_cb, NULL);
    if (list_empty(&devices)) fprintf(stderr, "input: no devices (are you root or in the 'input' group?)\n");
    return 0;
}

void input_finish(void) {
    if (!devices.next) return;
    struct device *d, *tmp;
    list_for_each_safe(d, tmp, &devices, link) close_device(d);
    if (inotify_watch) watch_remove(inotify_watch);
    if (inotify_fd >= 0) close(inotify_fd);
}
