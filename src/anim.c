/* anim.c — CSS transitions.
 *
 * Styles are recomputed from scratch every frame, so to animate we remember, for each
 * element (identified by an owner pointer plus a part number), the target style it had
 * last time and what we actually showed. When the target changes, say the window gained
 * focus, we start blending from what's on screen now towards the new target, timed and
 * eased by the element's `transition` list. While anything is still moving, we keep
 * asking for frames.
 *
 * Animated: colors (background, text, borders, shadow), opacity, border-radius and
 * box-shadow geometry, and gradients with the same number of stops. Everything else
 * (sizes, images, fonts) switches immediately. */
#include "server.h"
#include "css.h"

#include <math.h>
#include <string.h>

struct entry {
    const void *owner;
    int part;
    bool used, running;
    uint32_t start, last_use;
    struct style target, from, shown;
};

static struct entry entries[128];
static bool running;

/* CSS easing: a cubic Bézier from (0,0) to (1,1) with control points (x1,y1), (x2,y2).
 * Given progress in time (x), find the curve parameter t by bisection, then return y. */
static float bezier(const float b[4], float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 30; i++) {
        t = (lo + hi) / 2;
        float u = 1 - t;
        float bx = 3 * u * u * t * b[0] + 3 * u * t * t * b[2] + t * t * t;
        if (bx < x) lo = t;
        else hi = t;
    }
    float u = 1 - t;
    return 3 * u * u * t * b[1] + 3 * u * t * t * b[3] + t * t * t;
}

/* How far along the transition for `prop` is (0..1); 1 if it doesn't transition.
 * Later entries in the list win, and "all" matches everything. */
static float progress(const struct entry *e, const char *prop, const char *alias, uint32_t now, bool *done) {
    const struct style *s = &e->target;
    const struct transition *t = NULL;
    for (int i = 0; i < s->ntransitions; i++) {
        const char *p = s->transitions[i].prop;
        if (!strcmp(p, "all") || !strcmp(p, prop) || (alias && !strcmp(p, alias))) t = &s->transitions[i];
    }
    if (!t || t->duration_ms <= 0) return 1;
    float x = ((float)(int32_t)(now - e->start) - t->delay_ms) / t->duration_ms;
    if (x < 1) *done = false;
    return bezier(t->bezier, CLAMP(x, 0.0f, 1.0f));
}

static uint8_t mix8(uint8_t a, uint8_t b, float f) { return (uint8_t)lroundf(a + (b - a) * f); }

static struct color mix_color(struct color a, struct color b, float f) {
    /* fading from transparent shouldn't pass through black: borrow the other side's hue */
    if (a.a == 0) { a.r = b.r; a.g = b.g; a.b = b.b; }
    if (b.a == 0) { b.r = a.r; b.g = a.g; b.b = a.b; }
    return (struct color){ mix8(a.r, b.r, f), mix8(a.g, b.g, f), mix8(a.b, b.b, f), mix8(a.a, b.a, f) };
}

static float mixf(float a, float b, float f) { return a + (b - a) * f; }
static int mixi(int a, int b, float f) { return (int)lroundf(a + (b - a) * f); }

static void mix_paint(struct paint *out, const struct paint *a, const struct paint *b, float f) {
    if (f >= 1) { *out = *b; return; }
    /* color <-> none: fade the alpha */
    if ((a->kind == PAINT_COLOR || a->kind == PAINT_NONE) && (b->kind == PAINT_COLOR || b->kind == PAINT_NONE)) {
        struct color ca = a->kind == PAINT_COLOR ? a->color : (struct color){ 0 };
        struct color cb = b->kind == PAINT_COLOR ? b->color : (struct color){ 0 };
        *out = (struct paint){ .kind = PAINT_COLOR, .color = mix_color(ca, cb, f) };
        return;
    }
    if (a->kind == b->kind && (a->kind == PAINT_LINEAR || a->kind == PAINT_RADIAL) && a->nstops == b->nstops) {
        *out = *b;
        out->angle = mixf(a->angle, b->angle, f);
        for (int i = 0; i < b->nstops; i++) {
            out->stops[i].c = mix_color(a->stops[i].c, b->stops[i].c, f);
            out->stops[i].pos = mixf(a->stops[i].pos, b->stops[i].pos, f);
        }
        return;
    }
    *out = f < 0.5f ? *a : *b;   /* nothing sensible in between: switch halfway */
}

static struct entry *find(const void *owner, int part, bool create) {
    struct entry *free_slot = NULL, *oldest = &entries[0];
    for (size_t i = 0; i < ARRAY_LEN(entries); i++) {
        struct entry *e = &entries[i];
        if (e->used && e->owner == owner && e->part == part) return e;
        if (!e->used && !free_slot) free_slot = e;
        if (e->last_use < oldest->last_use) oldest = e;
    }
    if (!create) return NULL;
    struct entry *e = free_slot ? free_slot : oldest;
    memset(e, 0, sizeof *e);
    e->used = true;
    e->owner = owner;
    e->part = part;
    return e;
}

void anim_style(const void *owner, int part, struct style *s) {
    uint32_t now = now_ms();
    struct entry *e = find(owner, part, false);
    if (!e) {   /* first sight: nothing to animate from */
        e = find(owner, part, true);
        e->target = e->shown = *s;
        e->last_use = now;
        return;
    }
    e->last_use = now;
    if (memcmp(&e->target, s, sizeof *s)) {   /* the target changed: animate from what's showing */
        e->from = e->shown;
        e->target = *s;
        e->start = now;
        e->running = s->ntransitions > 0;
    }
    if (!e->running) {
        e->shown = *s;
        return;
    }
    bool done = true;
    struct style out = *s;
    const struct style *a = &e->from;
    float f;

    f = progress(e, "background-color", "background", now, &done);
    mix_paint(&out.background, &a->background, &s->background, f);
    f = progress(e, "background-image", "background", now, &done);
    mix_paint(&out.background_image, &a->background_image, &s->background_image, f);
    f = progress(e, "color", NULL, now, &done);
    out.color = mix_color(a->color, s->color, f);
    f = progress(e, "border-color", "border", now, &done);
    for (int i = 0; i < 4; i++) out.border.c[i] = mix_color(a->border.c[i], s->border.c[i], f);
    f = progress(e, "border-radius", NULL, now, &done);
    for (int i = 0; i < 4; i++)
        if (a->radius[i].pct == s->radius[i].pct) out.radius[i].v = mixf(a->radius[i].v, s->radius[i].v, f);
    f = progress(e, "opacity", NULL, now, &done);
    out.opacity = mixf(a->opacity, s->opacity, f);
    f = progress(e, "box-shadow", NULL, now, &done);
    if (a->shadow || s->shadow) {
        /* a shadow appearing or vanishing fades its color */
        struct color ca = a->shadow ? a->shadow_color : (struct color){ 0 };
        struct color cb = s->shadow ? s->shadow_color : (struct color){ 0 };
        const struct style *geo = s->shadow ? s : a;
        out.shadow = true;
        out.shadow_color = mix_color(ca, cb, f);
        out.shadow_x = mixi(a->shadow ? a->shadow_x : geo->shadow_x, geo->shadow_x, f);
        out.shadow_y = mixi(a->shadow ? a->shadow_y : geo->shadow_y, geo->shadow_y, f);
        out.shadow_blur = mixi(a->shadow ? a->shadow_blur : geo->shadow_blur, geo->shadow_blur, f);
        out.shadow_spread = mixi(a->shadow ? a->shadow_spread : geo->shadow_spread, geo->shadow_spread, f);
        if (f >= 1) out.shadow = s->shadow;
    }
    e->shown = out;
    *s = out;
    e->running = !done;
    if (!done) running = true;
}

void anim_forget(const void *owner) {
    for (size_t i = 0; i < ARRAY_LEN(entries); i++)
        if (entries[i].used && entries[i].owner == owner) entries[i].used = false;
}

/* call once per frame, after drawing: true if something is mid-transition */
bool anim_frame_done(void) {
    bool r = running;
    running = false;
    return r;
}
