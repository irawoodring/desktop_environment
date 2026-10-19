/* wire.c — encode/decode Wayland messages and move them over a Unix socket. */
#define _GNU_SOURCE
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OUT_MAX (4u << 20)   /* a client that stops reading gets cut off past 4 MiB */

void conn_init(struct conn *c, int fd) {
    memset(c, 0, offsetof(struct conn, in));
    c->fd = fd;
    c->in_start = c->in_end = 0;
    c->in_nfds = 0;
    c->out = NULL;
    c->out_len = c->out_cap = 0;
    c->out_nfds = 0;
}

void conn_free(struct conn *c) {
    for (int i = 0; i < c->in_nfds; i++) close(c->in_fds[i]);
    for (int i = 0; i < c->out_nfds; i++) close(c->out_fds[i]);
    c->in_nfds = c->out_nfds = 0;
    free(c->out);
    c->out = NULL;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

int conn_read(struct conn *c) {
    if (c->in_start > 0) {   /* slide unread bytes to the front */
        memmove(c->in, c->in + c->in_start, c->in_end - c->in_start);
        c->in_end -= c->in_start;
        c->in_start = 0;
    }
    if (c->in_end == WIRE_IN_CAP) return 1;

    char cbuf[CMSG_SPACE(sizeof(int) * WIRE_MAX_FDS)];
    struct iovec iov = { c->in + c->in_end, WIRE_IN_CAP - c->in_end };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t n;
    do n = recvmsg(c->fd, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    while (n < 0 && errno == EINTR);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : -1;

    /* file descriptors ride along as "ancillary data" */
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        int count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *fds = (int *)CMSG_DATA(cm);
        for (int i = 0; i < count; i++) {
            if (c->in_nfds < WIRE_FDQ) c->in_fds[c->in_nfds++] = fds[i];
            else { close(fds[i]); c->error = true; }
        }
    }
    if (m.msg_flags & MSG_CTRUNC) return -1;
    if (n == 0) return 0;
    c->in_end += n;
    return c->error ? -1 : 1;
}

int conn_next(struct conn *c, uint32_t *id, uint16_t *op, const uint8_t **body, uint32_t *len) {
    size_t avail = c->in_end - c->in_start;
    if (avail < 8) return 0;
    uint32_t hdr[2];
    memcpy(hdr, c->in + c->in_start, 8);
    uint32_t size = hdr[1] >> 16;
    if (size < 8 || size % 4 || size > WIRE_MAX_MSG) return -1;
    if (avail < size) return 0;
    *id = hdr[0];
    *op = hdr[1] & 0xffff;
    *body = c->in + c->in_start + 8;
    *len = size - 8;
    return 1;
}

void conn_consume(struct conn *c, uint32_t body_len) { c->in_start += body_len + 8; }

static int take_fd(struct conn *c) {
    if (c->in_nfds == 0) return -1;
    int fd = c->in_fds[0];
    memmove(c->in_fds, c->in_fds + 1, (c->in_nfds - 1) * sizeof(int));
    c->in_nfds--;
    return fd;
}

int wire_parse(struct conn *c, const char *sig, const uint8_t *p, uint32_t len,
               union wire_arg *a, int max) {
    const uint8_t *end = p + len;
    int n = 0, taken[8], ntaken = 0;
    bool nullable = false;

    for (const char *s = sig; *s; s++) {
        if (*s == '?') { nullable = true; continue; }
        if (n >= max) goto bad;
        if (*s == 'h') {
            int fd = take_fd(c);
            if (fd < 0) goto bad;
            if (ntaken < 8) taken[ntaken++] = fd;
            a[n++].h = fd;
            nullable = false;
            continue;
        }
        if (end - p < 4) goto bad;
        uint32_t v;
        memcpy(&v, p, 4);
        p += 4;
        switch (*s) {
        case 'i': case 'u': case 'f':
            a[n].u = v;
            break;
        case 'o':
            if (v == 0 && !nullable) goto bad;
            a[n].o = v;
            break;
        case 'n':
            if (v == 0) goto bad;
            a[n].n = v;
            break;
        case 's': {
            if (v == 0) {
                if (!nullable) goto bad;
                a[n].s = NULL;
                break;
            }
            if (v > (uint32_t)(end - p)) goto bad;
            uint32_t padded = (v + 3) & ~3u;
            if (padded > (uint32_t)(end - p) || p[v - 1] != '\0') goto bad;
            a[n].s = (const char *)p;
            p += padded;
            break;
        }
        case 'a': {
            if (v > (uint32_t)(end - p)) goto bad;
            uint32_t padded = (v + 3) & ~3u;
            if (padded > (uint32_t)(end - p)) goto bad;
            a[n].a.size = v;
            a[n].a.data = p;
            p += padded;
            break;
        }
        default:
            goto bad;
        }
        n++;
        nullable = false;
    }
    if (p != end) goto bad;
    return n;
bad:
    for (int i = 0; i < ntaken; i++) close(taken[i]);
    return -1;
}

static bool out_reserve(struct conn *c, size_t n) {
    if (c->out_len + n <= c->out_cap) return true;
    size_t cap = c->out_cap ? c->out_cap : 4096;
    while (cap < c->out_len + n) cap *= 2;
    if (cap > OUT_MAX) { c->error = true; return false; }
    uint8_t *p = realloc(c->out, cap);
    if (!p) { c->error = true; return false; }
    c->out = p;
    c->out_cap = cap;
    return true;
}

static void put32(struct conn *c, uint32_t v) {
    memcpy(c->out + c->out_len, &v, 4);
    c->out_len += 4;
}

static void put_bytes(struct conn *c, const void *data, uint32_t len) {
    uint32_t padded = (len + 3) & ~3u;
    put32(c, len);
    memcpy(c->out + c->out_len, data, len);
    memset(c->out + c->out_len + len, 0, padded - len);
    c->out_len += padded;
}

void wire_vsend(struct conn *c, uint32_t id, uint16_t op, const char *sig, va_list ap) {
    if (c->error || !out_reserve(c, 8)) return;
    size_t start = c->out_len;
    put32(c, id);
    put32(c, 0);   /* size patched in at the end */

    for (const char *s = sig; *s; s++) {
        switch (*s) {
        case '?':
            break;
        case 'i': case 'u': case 'f': case 'o': case 'n': {
            uint32_t v = va_arg(ap, uint32_t);
            if (!out_reserve(c, 4)) return;
            put32(c, v);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) {
                if (!out_reserve(c, 4)) return;
                put32(c, 0);
                break;
            }
            uint32_t l = strlen(str) + 1;
            if (!out_reserve(c, 4 + ((l + 3) & ~3u))) return;
            put_bytes(c, str, l);
            break;
        }
        case 'a': {
            const struct wire_array *arr = va_arg(ap, const struct wire_array *);
            if (!out_reserve(c, 4 + ((arr->size + 3) & ~3u))) return;
            put_bytes(c, arr->data, arr->size);
            break;
        }
        case 'h': {
            int fd = va_arg(ap, int);
            if (c->out_nfds == WIRE_FDQ) { c->error = true; return; }
            int dup = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            if (dup < 0) { c->error = true; return; }
            c->out_fds[c->out_nfds] = dup;
            c->out_fd_off[c->out_nfds] = start;
            c->out_nfds++;
            break;
        }
        }
    }
    uint32_t size = c->out_len - start;
    if (size > WIRE_MAX_MSG) { c->error = true; return; }
    uint32_t w = (size << 16) | op;
    memcpy(c->out + start + 4, &w, 4);
}

void wire_send(struct conn *c, uint32_t id, uint16_t op, const char *sig, ...) {
    va_list ap;
    va_start(ap, sig);
    wire_vsend(c, id, op, sig, ap);
    va_end(ap);
}

int conn_flush(struct conn *c) {
    if (c->error) return -1;
    while (c->out_len > 0) {
        /* An fd must arrive no later than the message that uses it. We can attach at most
         * WIRE_MAX_FDS per sendmsg, so if more are queued, stop this chunk at the message
         * that owns the first fd we can't fit. */
        int k = c->out_nfds < WIRE_MAX_FDS ? c->out_nfds : WIRE_MAX_FDS;
        size_t len = c->out_nfds > k ? c->out_fd_off[k] : c->out_len;
        if (len == 0) len = c->out_len;

        struct iovec iov = { c->out, len };
        char cbuf[CMSG_SPACE(sizeof(int) * WIRE_MAX_FDS)];
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (k > 0) {
            memset(cbuf, 0, sizeof cbuf);
            m.msg_control = cbuf;
            m.msg_controllen = CMSG_SPACE(sizeof(int) * k);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * k);
            memcpy(CMSG_DATA(cm), c->out_fds, sizeof(int) * k);
        }
        ssize_t n;
        do n = sendmsg(c->fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT);
        while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            c->error = true;
            return -1;
        }
        /* the fds went out with the first byte; we can close our copies */
        for (int i = 0; i < k; i++) close(c->out_fds[i]);
        memmove(c->out_fds, c->out_fds + k, (c->out_nfds - k) * sizeof(int));
        memmove(c->out_fd_off, c->out_fd_off + k, (c->out_nfds - k) * sizeof(size_t));
        c->out_nfds -= k;
        for (int i = 0; i < c->out_nfds; i++) c->out_fd_off[i] -= n;
        memmove(c->out, c->out + n, c->out_len - n);
        c->out_len -= n;
    }
    return 1;
}
