/* wire.h — the Wayland wire format, by hand.
 *
 * Every Wayland message, in either direction, looks like this:
 *
 *   word 0:  object id the message is for (or from)
 *   word 1:  (total size in bytes << 16) | opcode
 *   then the arguments, each padded to 4 bytes:
 *     i, u   32-bit int / uint
 *     f      24.8 fixed point ("wl_fixed")
 *     o      object id (0 = null, if the protocol allows it)
 *     n      new object id chosen by the sender
 *     s      uint32 length (including the NUL), bytes, padding.  length 0 = null
 *     a      uint32 length, bytes, padding
 *     h      a file descriptor — sent out of band with SCM_RIGHTS, not in the bytes
 *
 * Signatures below use exactly those letters; "?" before s or o means nullable.
 * This file is shared by the server and the demo client. */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIRE_MAX_MSG 4096       /* libwayland's limit; real messages are tiny */
#define WIRE_MAX_FDS 28         /* fds per sendmsg(), same as libwayland */
#define WIRE_IN_CAP  65536
#define WIRE_FDQ     256

typedef int32_t wl_fixed;
static inline double fixed_to_double(wl_fixed f) { return f / 256.0; }
static inline wl_fixed fixed_from_double(double d) { return (wl_fixed)(d * 256.0); }

struct wire_array { uint32_t size; const void *data; };

union wire_arg {
    int32_t i;
    uint32_t u;
    wl_fixed f;
    const char *s;          /* points into the receive buffer, valid until conn_consume() */
    uint32_t o;             /* object id */
    uint32_t n;             /* new id */
    struct wire_array a;
    int h;                  /* received fd: the handler owns it (set to -1 to keep it) */
};

struct conn {
    int fd;
    bool error;

    _Alignas(4) uint8_t in[WIRE_IN_CAP];
    size_t in_start, in_end;
    int in_fds[WIRE_FDQ];
    int in_nfds;

    uint8_t *out;
    size_t out_len, out_cap;
    int out_fds[WIRE_FDQ];
    size_t out_fd_off[WIRE_FDQ];   /* byte offset of the message each fd belongs to */
    int out_nfds;
};

void conn_init(struct conn *c, int fd);
void conn_free(struct conn *c);

/* Read whatever the socket has. 1 = ok (maybe nothing new), 0 = peer hung up, -1 = error. */
int conn_read(struct conn *c);

/* Look at the next complete message without consuming it.
 * 1 = got one, 0 = need more bytes, -1 = garbage on the wire. */
int conn_next(struct conn *c, uint32_t *id, uint16_t *op, const uint8_t **body, uint32_t *len);
void conn_consume(struct conn *c, uint32_t body_len);

/* Decode a message body according to `sig`. Returns argument count, or -1 if malformed.
 * fds are taken from the received-fd queue in order. */
int wire_parse(struct conn *c, const char *sig, const uint8_t *body, uint32_t len,
               union wire_arg *args, int max_args);

/* Encode a message into the outgoing buffer. Variadic args follow `sig`:
 * i/u/f/o/n -> 32-bit int, s -> const char *, a -> const struct wire_array *, h -> int fd (dup'd). */
void wire_send(struct conn *c, uint32_t id, uint16_t op, const char *sig, ...);
void wire_vsend(struct conn *c, uint32_t id, uint16_t op, const char *sig, va_list ap);

/* Push the outgoing buffer to the socket. 1 = all sent, 0 = socket full (wait for POLLOUT), -1 = error. */
int conn_flush(struct conn *c);
static inline bool conn_has_output(const struct conn *c) { return c->out_len > 0; }
