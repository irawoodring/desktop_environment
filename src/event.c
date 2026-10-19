/* event.c — a tiny epoll event loop.
 *
 * Everything the server reacts to is a file descriptor: the listening socket, each
 * client socket, each input device, the DRM device (page-flip events), timers,
 * signals (signalfd) and inotify (theme reloads). */
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

struct watch {
    int fd;
    watch_fn fn;
    void *data;
    bool removed;
    struct list link;
};

static int epfd = -1;
static struct list dead;   /* removed watches, freed after the current batch */

void loop_init(void) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }
    list_init(&dead);
}

struct watch *watch_add(int fd, uint32_t events, watch_fn fn, void *data) {
    struct watch *w = calloc(1, sizeof *w);
    w->fd = fd;
    w->fn = fn;
    w->data = data;
    list_init(&w->link);
    struct epoll_event ev = { .events = events, .data.ptr = w };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl add");
        free(w);
        return NULL;
    }
    return w;
}

void watch_mod(struct watch *w, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.ptr = w };
    epoll_ctl(epfd, EPOLL_CTL_MOD, w->fd, &ev);
}

void watch_remove(struct watch *w) {
    if (!w || w->removed) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, w->fd, NULL);
    w->removed = true;
    list_append(&dead, &w->link);
}

uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void loop_run(volatile bool *running, void (*before_wait)(void)) {
    struct epoll_event evs[64];
    while (*running) {
        before_wait();
        int n = epoll_wait(epfd, evs, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            struct watch *w = evs[i].data.ptr;
            if (!w->removed) w->fn(w->fd, evs[i].events, w->data);
        }
        struct watch *w, *tmp;
        list_for_each_safe(w, tmp, &dead, link) {
            list_remove(&w->link);
            free(w);
        }
    }
}
