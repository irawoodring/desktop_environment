/* menu.c — the window menu.
 *
 * Opened by right-clicking a title bar, by Alt+Space, or by a client that draws its own
 * title bar (GTK) asking for it with xdg_toplevel.show_window_menu. It holds the window
 * actions (minimize, maximize, fullscreen, close) and one checkable entry per class the
 * theme styles on windows (window.work, window.dim ...): toggling one adds or removes
 * that class on this window, and the CSS takes it from there.
 *
 * The server draws the menu itself, styled with CSS like everything else:
 *
 *   menu
 *     menuitem.action.<name>    e.g. menuitem.close
 *     menuitem.class(.checked)
 *     separator
 *     heading
 *
 * While it's open, the pointer and keyboard belong to it: Up/Down/Enter/Escape work,
 * and clicking outside closes it. */
#include "server.h"
#include "css.h"
#include "render.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum kind { ITEM_ACTION, ITEM_CLASS, ITEM_SEPARATOR, ITEM_HEADING };
enum action { ACT_MINIMIZE, ACT_MAXIMIZE, ACT_FULLSCREEN, ACT_CLOSE };
static const char *const action_class[] = { "action minimize", "action maximize", "action fullscreen",
                                            "action close" };

#define MAX_ITEMS 48

struct item {
    enum kind kind;
    int action;
    char label[64];
    bool checked;
    struct rect r;
};

static struct {
    bool open;
    struct toplevel *win;
    int x, y;                      /* where it was asked to open */
    struct item items[MAX_ITEMS];
    int n;
    int hover;                     /* highlighted item, or -1 */
    bool moved;                    /* the pointer moved since opening */
    bool pressed_inside;
    struct rect box;
} m;

static bool selectable(int i) {
    return i >= 0 && i < m.n && (m.items[i].kind == ITEM_ACTION || m.items[i].kind == ITEM_CLASS);
}

static void add(enum kind kind, int action, const char *label, bool checked) {
    if (m.n == MAX_ITEMS) return;
    struct item *it = &m.items[m.n++];
    *it = (struct item){ .kind = kind, .action = action, .checked = checked };
    snprintf(it->label, sizeof it->label, "%s", label);
}

static bool listed(const char *cls) {
    for (int i = 0; i < m.n; i++)
        if (m.items[i].kind == ITEM_CLASS && !strcmp(m.items[i].label, cls)) return true;
    return false;
}

static void build(struct toplevel *t) {
    m.n = 0;
    add(ITEM_ACTION, ACT_MINIMIZE, "Minimize", false);
    add(ITEM_ACTION, ACT_MAXIMIZE, t->maximized ? "Restore" : "Maximize", false);
    add(ITEM_ACTION, ACT_FULLSCREEN, t->fullscreen ? "Leave fullscreen" : "Fullscreen", false);
    add(ITEM_ACTION, ACT_CLOSE, "Close", false);
    add(ITEM_SEPARATOR, 0, "", false);
    add(ITEM_HEADING, 0, "Classes", false);
    const char *cls;
    for (int i = 0; (cls = css_window_class(i)); i++) add(ITEM_CLASS, 0, cls, wm_window_has_class(t, cls));
    /* classes the window still has but the (edited) theme no longer mentions, so they can be removed */
    char copy[sizeof t->classes];
    snprintf(copy, sizeof copy, "%s", t->classes);
    for (char *save, *c = strtok_r(copy, " ", &save); c; c = strtok_r(NULL, " ", &save))
        if (!listed(c)) add(ITEM_CLASS, 0, c, true);
    if (m.items[m.n - 1].kind == ITEM_HEADING) add(ITEM_HEADING, 0, "(none in the theme)", false);
}

/* ------------------------------------------------------------------ layout */

static const char *element_of(const struct item *it) {
    switch (it->kind) {
    case ITEM_SEPARATOR: return "separator";
    case ITEM_HEADING: return "heading";
    default: return "menuitem";
    }
}

static void compute(struct style *ms, struct style *is) {
    struct css_elem em = { .name = "menu" };
    css_compute(&em, NULL, ms);
    for (int i = 0; i < m.n; i++) {
        const struct item *it = &m.items[i];
        const char *cls = it->kind == ITEM_ACTION ? action_class[it->action] :
                          it->kind == ITEM_CLASS ? (it->checked ? "class checked" : "class") : NULL;
        struct css_elem e = { .name = element_of(it), .cls = cls,
                              .states = i == m.hover ? ST_HOVER : 0, .parent = &em };
        css_compute(&e, ms, &is[i]);
        css_elem_release(&e);
    }
    css_elem_release(&em);
}

static const struct font *font_of(const struct style *s) {
    return font_get(s->font_family, s->font_weight, s->font_italic, s->font_size);
}

static int text_height(const struct style *s) { return font_line_height(font_of(s)); }

/* place the items; the check column is as wide as the text is tall */
static void layout(struct style *ms, struct style *is) {
    int inner_w = 0, h = 0, heights[MAX_ITEMS];
    for (int i = 0; i < m.n; i++) {
        const struct style *s = &is[i];
        if (s->hidden) {   /* display: none */
            heights[i] = 0;
        } else if (m.items[i].kind == ITEM_SEPARATOR) {
            heights[i] = (s->height >= 0 ? s->height : 1) + s->margin[0] + s->margin[2];
        } else {
            int th = text_height(s);
            heights[i] = s->height >= 0 ? s->height : th + 8;
            int w = text_width(font_of(s), m.items[i].label) + th + 6 + s->padding[1] + s->padding[3];
            inner_w = MAX(inner_w, w);
        }
        h += heights[i];
    }
    struct border b = style_border(ms);
    int pad_w = ms->padding[1] + ms->padding[3] + b.w[LEFT] + b.w[RIGHT];
    int pad_h = ms->padding[0] + ms->padding[2] + b.w[TOP] + b.w[BOTTOM];
    if (ms->width >= 0) inner_w = MAX(inner_w, ms->width - pad_w);
    m.box = (struct rect){ m.x, m.y, inner_w + pad_w, h + pad_h };
    /* keep it on screen */
    if (m.box.x + m.box.w > srv.width) m.box.x = srv.width - m.box.w;
    if (m.box.y + m.box.h > srv.height) m.box.y = srv.height - m.box.h;
    m.box.x = MAX(m.box.x, 0);
    m.box.y = MAX(m.box.y, 0);
    int y = m.box.y + b.w[TOP] + ms->padding[0];
    for (int i = 0; i < m.n; i++) {
        m.items[i].r = (struct rect){ m.box.x + b.w[LEFT] + ms->padding[3], y, inner_w, heights[i] };
        y += heights[i];
    }
}

static void relayout(void) {
    struct style ms, is[MAX_ITEMS];
    compute(&ms, is);
    layout(&ms, is);
}

/* ----------------------------------------------------------------- drawing */

void menu_draw(struct canvas *c) {
    if (!m.open) return;
    struct style ms, is[MAX_ITEMS];
    compute(&ms, is);
    layout(&ms, is);
    anim_style(&m, 0, &ms);
    for (int i = 0; i < m.n; i++) anim_style(&m, 1 + i, &is[i]);   /* e.g. hover highlights fading */
    float op = ms.opacity;
    struct radii radius = style_radii(&ms, m.box.w, m.box.h);
    struct paint layers[2];
    int n;
    if (ms.shadow) {
        int sp = ms.shadow_spread;
        struct rect sr = { m.box.x + ms.shadow_x - sp, m.box.y + ms.shadow_y - sp, m.box.w + 2 * sp, m.box.h + 2 * sp };
        struct radii srad = { radius.tl + sp, radius.tr + sp, radius.br + sp, radius.bl + sp };
        draw_shadow(c, sr, srad, ms.shadow_blur, ms.shadow_color, op, op >= 1 ? &m.box : NULL);
    }
    struct border mb = style_border(&ms);
    n = style_layers(&ms, layers);
    draw_box(c, m.box, radius, &mb, layers, n, op, NULL);

    for (int i = 0; i < m.n; i++) {
        const struct item *it = &m.items[i];
        struct style *s = &is[i];
        struct rect r = it->r;
        float iop = op * s->opacity;   /* opacity multiplies the menu's, as in CSS */
        if (it->kind == ITEM_SEPARATOR) {
            struct rect line = { r.x + s->margin[3], r.y + s->margin[0], r.w - s->margin[1] - s->margin[3],
                                 s->height >= 0 ? s->height : 1 };
            style_draw_box(c, line, style_radii(s, line.w, line.h), s, iop);
            continue;
        }
        if (s->hidden) continue;
        style_draw_box(c, r, style_radii(s, r.w, r.h), s, iop);
        const struct font *f = font_of(s);
        int th = text_height(s);
        int tx = r.x + s->padding[3], ty = r.y + (r.h - th) / 2;
        if (it->kind != ITEM_HEADING) {
            if (it->checked) draw_icon(c, (struct rect){ tx, ty, th, th }, ICON_CHECK, s->color, iop);
            tx += th + 6;
        }
        draw_text(c, f, tx, ty, it->label, s->color, iop, r.x + r.w - s->padding[1] - tx, 0);
    }
}

/* ----------------------------------------------------------------- input */

bool menu_is_open(void) { return m.open; }

void menu_open(struct toplevel *t, int x, int y, bool keyboard) {
    m.open = true;
    m.win = t;
    m.x = x;
    m.y = y;
    m.hover = -1;
    m.moved = keyboard;
    m.pressed_inside = false;
    build(t);
    anim_forget(&m);   /* a fresh menu: nothing to animate from */
    if (keyboard)   /* opened from the keyboard: highlight the first entry for arrow keys */
        for (int i = 0; i < m.n && m.hover < 0; i++) if (selectable(i)) m.hover = i;
    relayout();
    wm_damage();
}

void menu_close(void) {
    if (!m.open) return;
    m.open = false;
    m.win = NULL;
    wm_damage();
    wm_menu_closed();
}

void menu_forget(struct toplevel *t) {
    if (m.open && m.win == t) menu_close();
}

static int item_at(double x, double y) {
    for (int i = 0; i < m.n; i++)
        if (rect_contains(m.items[i].r, x, y)) return i;
    return -1;
}

static void activate(int i) {
    struct toplevel *t = m.win;
    struct item it = m.items[i];
    menu_close();
    if (!t) return;
    if (it.kind == ITEM_CLASS) {
        wm_window_toggle_class(t, it.label);
        return;
    }
    switch (it.action) {
    case ACT_MINIMIZE: wm_window_minimize(t); break;
    case ACT_MAXIMIZE: wm_window_maximize(t, !t->maximized); break;
    case ACT_FULLSCREEN: wm_window_fullscreen(t, !t->fullscreen); break;
    case ACT_CLOSE: toplevel_send_close(t); break;
    }
}

void menu_motion(double x, double y) {
    if (!m.open) return;
    if (fabs(x - m.x) + fabs(y - m.y) > 4) m.moved = true;
    int i = item_at(x, y);
    int hover = selectable(i) ? i : -1;
    if (hover != m.hover) {
        m.hover = hover;
        wm_damage();
    }
}

/* Press outside closes. Release over an entry picks it, if it was a click on the menu or
 * a press-drag-release from where the menu opened (not the release of the opening click). */
void menu_button(uint32_t button, bool pressed, double x, double y) {
    (void)button;
    if (!m.open) return;
    if (pressed) {
        if (!rect_contains(m.box, x, y)) menu_close();
        else m.pressed_inside = true;
        return;
    }
    int i = item_at(x, y);
    if (selectable(i) && (m.pressed_inside || m.moved)) activate(i);
    m.pressed_inside = false;
}

void menu_key(uint32_t key, bool pressed) {
    if (!m.open || !pressed) return;
    int step = 0;
    switch (key) {
    case KEY_ESC: menu_close(); return;
    case KEY_UP: step = -1; break;
    case KEY_DOWN: step = 1; break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        if (selectable(m.hover)) activate(m.hover);
        return;
    default: return;
    }
    for (int k = 0, i = m.hover; k < m.n; k++) {   /* next selectable entry, wrapping around */
        i = ((i < 0 ? (step > 0 ? -1 : 0) : i) + step + m.n) % m.n;
        if (selectable(i)) { m.hover = i; break; }
    }
    wm_damage();
}
