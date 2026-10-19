/* list.h — intrusive doubly linked list (the same idea as wl_list / the kernel's list_head).
 *
 * Embed a `struct list` in your struct, then link those nodes together.
 * A removed node points at itself, so removing twice is harmless. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct list { struct list *prev, *next; };

static inline void list_init(struct list *l) { l->prev = l->next = l; }
static inline bool list_empty(const struct list *l) { return l->next == l; }

/* insert `e` right after `at` (so list_insert(head, e) pushes to the front) */
static inline void list_insert(struct list *at, struct list *e) {
    e->prev = at;
    e->next = at->next;
    at->next->prev = e;
    at->next = e;
}
static inline void list_append(struct list *head, struct list *e) { list_insert(head->prev, e); }
static inline void list_remove(struct list *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->prev = e->next = e;
}

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_for_each(pos, head, member)                                         \
    for (pos = container_of((head)->next, __typeof__(*pos), member);            \
         &pos->member != (head);                                                 \
         pos = container_of(pos->member.next, __typeof__(*pos), member))

#define list_for_each_reverse(pos, head, member)                                 \
    for (pos = container_of((head)->prev, __typeof__(*pos), member);            \
         &pos->member != (head);                                                 \
         pos = container_of(pos->member.prev, __typeof__(*pos), member))

/* safe against removal of `pos` inside the loop body */
#define list_for_each_safe(pos, tmp, head, member)                               \
    for (pos = container_of((head)->next, __typeof__(*pos), member),            \
         tmp = container_of(pos->member.next, __typeof__(*pos), member);        \
         &pos->member != (head);                                                 \
         pos = tmp, tmp = container_of(pos->member.next, __typeof__(*pos), member))
