/* css_libcss.c — the CSS engine backed by NetSurf's libcss.
 *
 * libcss is a complete CSS parser and selection engine that knows nothing about the
 * document it's styling. You hand it a stylesheet, and when you ask "what's the style
 * of this node?" it calls back into your code with questions: what's the node's name,
 * its parent, does it have class X, is it hovered... (the css_select_handler table
 * below). Our "document" is the tiny element tree wm.c builds for each window.
 *
 * Results come back as a css_computed_style, which we translate into our struct style.
 * Three things libcss doesn't support (border-radius, box-shadow, gradients) were
 * smuggled through other properties by css_rewrite_for_libcss() in css.c; we unpack
 * them here and parse them with the built-in value parser. */
#include "css.h"

#include <libcss/libcss.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* slots in css_elem.engine[] */
enum { SLOT_NODE_DATA, SLOT_RESULTS, SLOT_COMPOSED, SLOT_CLASSES };


static css_unit_ctx unit_ctx = {
    .viewport_width = 1920 * (1 << CSS_RADIX_POINT),
    .viewport_height = 1080 * (1 << CSS_RADIX_POINT),
    .font_size_default = 16 * (1 << CSS_RADIX_POINT),
    .font_size_minimum = 6 * (1 << CSS_RADIX_POINT),
    .device_dpi = 96 * (1 << CSS_RADIX_POINT),
};
static const css_media media = { .type = CSS_MEDIA_SCREEN };

/* ------------------------------------------------------------- tree queries */

static bool name_is(const struct css_elem *e, lwc_string *name) {
    size_t n = lwc_string_length(name);
    if (n == 1 && lwc_string_data(name)[0] == '*') return true;
    return strlen(e->name) == n && strncasecmp(e->name, lwc_string_data(name), n) == 0;
}

static bool has_class(const char *list, const char *cls, size_t n) {
    for (const char *p = list; p && *p;) {
        while (*p == ' ') p++;
        size_t len = strcspn(p, " ");
        if (len == n && !strncmp(p, cls, n)) return true;
        p += len;
    }
    return false;
}

/* The attributes a selector can test: [app-id], [title], and [pane-state], which the
 * rewrite uses for :maximized / :fullscreen. */
static const char *attribute(const struct css_elem *e, lwc_string *name) {
    const char *n = lwc_string_data(name);
    size_t len = lwc_string_length(name);
    if (len == 6 && !strncasecmp(n, "app-id", 6)) return e->app_id;
    if (len == 5 && !strncasecmp(n, "title", 5)) return e->title;
    if (len == 10 && !strncasecmp(n, "pane-state", 10)) {
        static const char *const states[4] = { "", "maximized", "fullscreen", "maximized fullscreen" };
        return states[((e->states & ST_MAXIMIZED) ? 1 : 0) | ((e->states & ST_FULLSCREEN) ? 2 : 0)];
    }
    return NULL;
}

#define NODE struct css_elem *e = node; (void)pw

static css_error node_name(void *pw, void *node, css_qname *qname) {
    NODE;
    qname->ns = NULL;
    return lwc_intern_string(e->name, strlen(e->name), &qname->name) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}

static css_error node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *n) {
    NODE;
    /* libcss unrefs each string after use but leaves the array to us */
    lwc_string **arr = e->engine[SLOT_CLASSES];
    uint32_t count = 0;
    for (const char *p = e->cls; p && *p;) {
        while (*p == ' ') p++;
        size_t len = strcspn(p, " ");
        if (len) {
            arr = realloc(arr, (count + 1) * sizeof *arr);
            lwc_intern_string(p, len, &arr[count++]);
        }
        p += len;
    }
    e->engine[SLOT_CLASSES] = arr;
    *classes = count ? arr : NULL;
    *n = count;
    return CSS_OK;
}

static css_error node_id(void *pw, void *node, lwc_string **id) { NODE; (void)e; *id = NULL; return CSS_OK; }

static css_error named_ancestor_node(void *pw, void *node, const css_qname *q, void **ancestor) {
    NODE;
    *ancestor = NULL;
    for (struct css_elem *a = e->parent; a; a = a->parent)
        if (name_is(a, q->name)) { *ancestor = a; break; }
    return CSS_OK;
}

static css_error named_parent_node(void *pw, void *node, const css_qname *q, void **parent) {
    NODE;
    *parent = e->parent && name_is(e->parent, q->name) ? e->parent : NULL;
    return CSS_OK;
}

/* our trees have no siblings worth matching (+ and ~ never match) */
static css_error no_sibling(void *pw, void *node, const css_qname *q, void **sibling) {
    (void)pw; (void)node; (void)q;
    *sibling = NULL;
    return CSS_OK;
}

static css_error parent_node(void *pw, void *node, void **parent) { NODE; *parent = e->parent; return CSS_OK; }
static css_error sibling_node(void *pw, void *node, void **sibling) {
    (void)pw; (void)node;
    *sibling = NULL;
    return CSS_OK;
}

static css_error node_has_name(void *pw, void *node, const css_qname *q, bool *match) {
    NODE;
    *match = name_is(e, q->name);
    return CSS_OK;
}

static css_error node_has_class(void *pw, void *node, lwc_string *name, bool *match) {
    NODE;
    *match = has_class(e->cls, lwc_string_data(name), lwc_string_length(name));
    return CSS_OK;
}

static css_error node_has_id(void *pw, void *node, lwc_string *name, bool *match) {
    (void)pw; (void)node; (void)name;
    *match = false;
    return CSS_OK;
}

static css_error node_has_attribute(void *pw, void *node, const css_qname *q, bool *match) {
    NODE;
    *match = attribute(e, q->name) != NULL;
    return CSS_OK;
}

/* the six attribute operators: =, |=, ~=, ^=, $=, *= */
enum { OP_EQUAL, OP_DASH, OP_INCLUDES, OP_PREFIX, OP_SUFFIX, OP_SUBSTRING };

static bool attr_match(const char *v, lwc_string *want, int op) {
    if (!v) return false;
    const char *w = lwc_string_data(want);
    size_t wl = lwc_string_length(want), vl = strlen(v);
    switch (op) {
    case OP_EQUAL: return vl == wl && !strncmp(v, w, wl);
    case OP_DASH: return !strncmp(v, w, wl) && (vl == wl || v[wl] == '-');
    case OP_INCLUDES: return wl > 0 && has_class(v, w, wl);
    case OP_PREFIX: return wl > 0 && vl >= wl && !strncmp(v, w, wl);
    case OP_SUFFIX: return wl > 0 && vl >= wl && !strncmp(v + vl - wl, w, wl);
    case OP_SUBSTRING:
        if (wl == 0) return false;
        for (const char *p = v; *p; p++)
            if (!strncmp(p, w, wl)) return true;
        return false;
    }
    return false;
}

#define ATTR_OP(fn, op)                                                                           \
    static css_error fn(void *pw, void *node, const css_qname *q, lwc_string *value, bool *match) { \
        NODE;                                                                                     \
        *match = attr_match(attribute(e, q->name), value, op);                                     \
        return CSS_OK;                                                                            \
    }
ATTR_OP(node_has_attribute_equal, OP_EQUAL)
ATTR_OP(node_has_attribute_dashmatch, OP_DASH)
ATTR_OP(node_has_attribute_includes, OP_INCLUDES)
ATTR_OP(node_has_attribute_prefix, OP_PREFIX)
ATTR_OP(node_has_attribute_suffix, OP_SUFFIX)
ATTR_OP(node_has_attribute_substring, OP_SUBSTRING)

static css_error node_is_root(void *pw, void *node, bool *match) { NODE; *match = !e->parent; return CSS_OK; }
static css_error node_count_siblings(void *pw, void *node, bool same_name, bool after, int32_t *count) {
    (void)pw; (void)node; (void)same_name; (void)after;
    *count = 0;
    return CSS_OK;
}

#define STATE_QUERY(fn, expr)                                       \
    static css_error fn(void *pw, void *node, bool *match) {        \
        NODE;                                                       \
        (void)e;                                                    \
        *match = (expr);                                            \
        return CSS_OK;                                              \
    }
STATE_QUERY(node_is_empty, true)
STATE_QUERY(node_is_link, false)
STATE_QUERY(node_is_visited, false)
STATE_QUERY(node_is_hover, (e->states & ST_HOVER) != 0)
STATE_QUERY(node_is_active, (e->states & ST_ACTIVE) != 0)
STATE_QUERY(node_is_focus, (e->states & ST_FOCUSED) != 0)
STATE_QUERY(node_is_enabled, true)
STATE_QUERY(node_is_disabled, false)
STATE_QUERY(node_is_checked, false)
STATE_QUERY(node_is_target, false)

static css_error node_is_lang(void *pw, void *node, lwc_string *lang, bool *match) {
    (void)pw; (void)node; (void)lang;
    *match = false;
    return CSS_OK;
}

static css_error node_presentational_hint(void *pw, void *node, uint32_t *nhints, css_hint **hints) {
    (void)pw; (void)node;
    *nhints = 0;
    *hints = NULL;
    return CSS_OK;
}

/* initial values for the few properties whose default is up to the "user agent" (us) */
static css_error ua_default_for_property(void *pw, uint32_t property, css_hint *hint) {
    (void)pw;
    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xffffffff;
        hint->status = CSS_COLOR_COLOR;
    } else if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_MONOSPACE;
    } else if (property == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
    } else if (property == CSS_PROP_VOICE_FAMILY) {
        hint->data.strings = NULL;
        hint->status = 0;
    } else {
        return CSS_INVALID;
    }
    return CSS_OK;
}

static css_select_handler handler;

/* libcss caches selection data (a bloom filter of ancestor names, partial styles) on
 * each node. Our nodes live on the stack for one restyle, so we keep it in the element
 * and free it in css_elem_release(). */
static css_error set_node_data(void *pw, void *node, void *data) {
    NODE;
    if (e->engine[SLOT_NODE_DATA] && e->engine[SLOT_NODE_DATA] != data)
        css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, pw, node, NULL, e->engine[SLOT_NODE_DATA]);
    e->engine[SLOT_NODE_DATA] = data;
    return CSS_OK;
}

static css_error get_node_data(void *pw, void *node, void **data) {
    NODE;
    *data = e->engine[SLOT_NODE_DATA];
    return CSS_OK;
}

static css_select_handler handler = {
    .handler_version = CSS_SELECT_HANDLER_VERSION_1,
    .node_name = node_name,
    .node_classes = node_classes,
    .node_id = node_id,
    .named_ancestor_node = named_ancestor_node,
    .named_parent_node = named_parent_node,
    .named_sibling_node = no_sibling,
    .named_generic_sibling_node = no_sibling,
    .parent_node = parent_node,
    .sibling_node = sibling_node,
    .node_has_name = node_has_name,
    .node_has_class = node_has_class,
    .node_has_id = node_has_id,
    .node_has_attribute = node_has_attribute,
    .node_has_attribute_equal = node_has_attribute_equal,
    .node_has_attribute_dashmatch = node_has_attribute_dashmatch,
    .node_has_attribute_includes = node_has_attribute_includes,
    .node_has_attribute_prefix = node_has_attribute_prefix,
    .node_has_attribute_suffix = node_has_attribute_suffix,
    .node_has_attribute_substring = node_has_attribute_substring,
    .node_is_root = node_is_root,
    .node_count_siblings = node_count_siblings,
    .node_is_empty = node_is_empty,
    .node_is_link = node_is_link,
    .node_is_visited = node_is_visited,
    .node_is_hover = node_is_hover,
    .node_is_active = node_is_active,
    .node_is_focus = node_is_focus,
    .node_is_enabled = node_is_enabled,
    .node_is_disabled = node_is_disabled,
    .node_is_checked = node_is_checked,
    .node_is_target = node_is_target,
    .node_is_lang = node_is_lang,
    .node_presentational_hint = node_presentational_hint,
    .ua_default_for_property = ua_default_for_property,
    .set_libcss_node_data = set_node_data,
    .get_libcss_node_data = get_node_data,
};

/* ---------------------------------------------------------------- loading */

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs) {
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);   /* keep url("...") text exactly: a path, or a gradient */
    return CSS_OK;
}

static css_stylesheet *parse(const char *css, const char *name) {
    css_stylesheet_params params = {
        .params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
        .level = CSS_LEVEL_DEFAULT,
        .charset = "UTF-8",
        .url = name,
        .title = name,
        .resolve = resolve_url,
    };
    css_stylesheet *sh = NULL;
    css_error err = css_stylesheet_create(&params, &sh);
    if (err == CSS_OK) {
        err = css_stylesheet_append_data(sh, (const uint8_t *)css, strlen(css));
        if (err == CSS_NEEDDATA) err = CSS_OK;
    }
    if (err == CSS_OK) err = css_stylesheet_data_done(sh);
    if (err != CSS_OK) {
        fprintf(stderr, "pane: libcss rejected %s: %s\n", name, css_error_to_string(err));
        if (sh) css_stylesheet_destroy(sh);
        return NULL;
    }
    return sh;
}

/* A loaded theme: the main sheet, plus a sheet per extension property (css.c explains),
 * each paired with the same thing from the built-in defaults, in its own selection context. */
struct sheets {
    css_stylesheet *ua[1 + CSS_NEXT], *author[1 + CSS_NEXT];
    css_select_ctx *ctx[1 + CSS_NEXT];   /* [0] = main; NULL where nothing sets the property */
};
static struct sheets active, ua;

static void sheets_free(struct sheets *s, bool with_ua) {
    for (int i = 0; i <= CSS_NEXT; i++) {
        if (s->ctx[i]) css_select_ctx_destroy(s->ctx[i]);
        if (s->author[i]) css_stylesheet_destroy(s->author[i]);
        if (with_ua && s->ua[i]) css_stylesheet_destroy(s->ua[i]);
    }
    memset(s, 0, sizeof *s);
}

static bool parse_all(const char *text, const char *name, css_stylesheet *out[1 + CSS_NEXT]) {
    struct css_rewritten rw = css_rewrite_for_libcss(text, name);
    if (getenv("PANE_CSS_DEBUG")) {
        fprintf(stderr, "--- %s as given to libcss:\n%s", name, rw.main);
        for (int i = 0; i < CSS_NEXT; i++)
            if (rw.ext[i]) fprintf(stderr, "--- %s, %s:\n%s", name, css_ext_props[i], rw.ext[i]);
        fprintf(stderr, "---\n");
    }
    bool ok = (out[0] = parse(rw.main, name)) != NULL;
    for (int i = 0; ok && i < CSS_NEXT; i++)
        if (rw.ext[i] && !(out[1 + i] = parse(rw.ext[i], name))) ok = false;
    css_rewritten_free(&rw);
    return ok;
}

static int load_text(const char *text, const char *name) {
    if (!ua.ua[0] && !parse_all(css_base_sheet, "(built-in defaults)", ua.ua)) return -1;
    css_window_classes_reset();
    struct sheets next = { 0 };
    if (!parse_all(text, name, next.author)) {
        sheets_free(&next, false);
        return -1;
    }
    /* the cascade puts author (theme) rules above user-agent rules, whatever their specificity */
    for (int i = 0; i <= CSS_NEXT; i++) {
        if (!ua.ua[i] && !next.author[i]) continue;
        css_error err = css_select_ctx_create(&next.ctx[i]);
        if (err == CSS_OK && ua.ua[i]) err = css_select_ctx_append_sheet(next.ctx[i], ua.ua[i], CSS_ORIGIN_UA, NULL);
        if (err == CSS_OK && next.author[i])
            err = css_select_ctx_append_sheet(next.ctx[i], next.author[i], CSS_ORIGIN_AUTHOR, NULL);
        if (err != CSS_OK) {
            fprintf(stderr, "pane: libcss: %s\n", css_error_to_string(err));
            sheets_free(&next, false);
            return -1;
        }
    }
    sheets_free(&active, false);
    active = next;
    return 0;
}

int css_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc(len + 1);
    size_t n = fread(text, 1, len, f);
    text[n] = '\0';
    fclose(f);
    css_set_base_dir(path);
    image_cache_clear();   /* pick up edited images too */
    int r = load_text(text, path);
    free(text);
    if (r == 0) fprintf(stderr, "pane: loaded %s with libcss\n", path);
    return r;
}

void css_load_string(const char *text, const char *name) { load_text(text, name); }

const char *css_engine_name(void) { return "libcss"; }

/* ------------------------------------------------------------- computing */

static int to_px(const css_computed_style *cs, css_fixed len, css_unit unit) {
    return (int)lroundf(FIXTOFLT(css_unit_len2device_px(cs, &unit_ctx, len, unit)));
}

static struct length to_length(const css_computed_style *cs, css_fixed len, css_unit unit) {
    if (unit == CSS_UNIT_PCT) return (struct length){ FIXTOFLT(len), true };
    return (struct length){ to_px(cs, len, unit), false };
}

static struct color to_color(css_color c) {   /* libcss colors are 0xAARRGGBB */
    return (struct color){ (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, c >> 24 };
}

typedef uint8_t (*len_getter)(const css_computed_style *, css_fixed *, css_unit *);

static void side_lengths(const css_computed_style *cs, int out[4], const len_getter get[4]) {
    for (int i = 0; i < 4; i++) {
        css_fixed len;
        css_unit unit;
        get[i](cs, &len, &unit);
        out[i] = unit == CSS_UNIT_PCT ? 0 : to_px(cs, len, unit);
    }
}

static void lwc_copy(char *out, size_t n, lwc_string *s) {
    snprintf(out, n, "%.*s", (int)lwc_string_length(s), lwc_string_data(s));
}

static void translate(const css_computed_style *cs, struct style *s) {
    css_color c;
    css_fixed len, len2;
    css_unit unit, unit2;
    lwc_string *str;

    if (css_computed_color(cs, &c) == CSS_COLOR_COLOR) s->color = to_color(c);

    if (css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR && (c >> 24))
        s->background = (struct paint){ .kind = PAINT_COLOR, .color = to_color(c) };
    if (css_computed_background_image(cs, &str) == CSS_BACKGROUND_IMAGE_IMAGE && str) {
        char buf[1024];
        lwc_copy(buf, sizeof buf, str);
        if (strchr(buf, '(')) css_apply_property(s, "background-image", buf);   /* a gradient */
        else {
            char url[1100];
            snprintf(url, sizeof url, "url(\"%s\")", buf);
            css_apply_property(s, "background-image", url);
        }
    }
    if (css_computed_background_position(cs, &len, &unit, &len2, &unit2) == CSS_BACKGROUND_POSITION_SET) {
        s->bg_pos_x = to_length(cs, len, unit);
        s->bg_pos_y = to_length(cs, len2, unit2);
    }
    switch (css_computed_background_repeat(cs)) {
    case CSS_BACKGROUND_REPEAT_REPEAT_X: s->bg_repeat_x = true; s->bg_repeat_y = false; break;
    case CSS_BACKGROUND_REPEAT_REPEAT_Y: s->bg_repeat_x = false; s->bg_repeat_y = true; break;
    case CSS_BACKGROUND_REPEAT_NO_REPEAT: s->bg_repeat_x = s->bg_repeat_y = false; break;
    default: s->bg_repeat_x = s->bg_repeat_y = true; break;
    }

    /* borders: libcss resolves currentColor; a side with style none draws nothing */
    static const len_getter widths[4] = { css_computed_border_top_width, css_computed_border_right_width,
                                          css_computed_border_bottom_width, css_computed_border_left_width };
    static uint8_t (*const colors[4])(const css_computed_style *, css_color *) = {
        css_computed_border_top_color, css_computed_border_right_color,
        css_computed_border_bottom_color, css_computed_border_left_color };
    static uint8_t (*const styles[4])(const css_computed_style *) = {
        css_computed_border_top_style, css_computed_border_right_style,
        css_computed_border_bottom_style, css_computed_border_left_style };
    for (int i = 0; i < 4; i++) {
        uint8_t st = styles[i](cs);
        s->border_drawn[i] = st != CSS_BORDER_STYLE_NONE && st != CSS_BORDER_STYLE_HIDDEN;
        if (widths[i](cs, &len, &unit) == CSS_BORDER_WIDTH_WIDTH) s->border.w[i] = MAX(to_px(cs, len, unit), 0);
        if (colors[i](cs, &c) == CSS_BORDER_COLOR_COLOR) {
            s->border.c[i] = to_color(c);
            s->border_color_set[i] = true;
        }
    }

    static const len_getter pad[4] = { css_computed_padding_top, css_computed_padding_right,
                                       css_computed_padding_bottom, css_computed_padding_left };
    static const len_getter mar[4] = { css_computed_margin_top, css_computed_margin_right,
                                       css_computed_margin_bottom, css_computed_margin_left };
    side_lengths(cs, s->padding, pad);
    side_lengths(cs, s->margin, mar);

    s->width = css_computed_width(cs, &len, &unit) == CSS_WIDTH_SET && unit != CSS_UNIT_PCT ? to_px(cs, len, unit) : -1;
    s->height = css_computed_height(cs, &len, &unit) == CSS_HEIGHT_SET && unit != CSS_UNIT_PCT ? to_px(cs, len, unit) : -1;

    css_fixed op;
    if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET) s->opacity = CLAMP(FIXTOFLT(op), 0.0f, 1.0f);

    /* font-family: the named families, then the generic one if the list ended with one */
    lwc_string **names = NULL;
    uint8_t generic = css_computed_font_family(cs, &names);
    char fam[128] = "";
    for (int i = 0; names && names[i]; i++) {
        char one[64];
        lwc_copy(one, sizeof one, names[i]);
        if (fam[0]) strncat(fam, ",", sizeof fam - strlen(fam) - 1);
        strncat(fam, one, sizeof fam - strlen(fam) - 1);
    }
    static const char *const generics[] = { [CSS_FONT_FAMILY_SERIF] = "serif",
                                            [CSS_FONT_FAMILY_SANS_SERIF] = "sans-serif",
                                            [CSS_FONT_FAMILY_MONOSPACE] = "monospace" };
    if (generic < ARRAY_LEN(generics) && generics[generic] && (fam[0] || generic != CSS_FONT_FAMILY_MONOSPACE)) {
        if (fam[0]) strncat(fam, ",", sizeof fam - strlen(fam) - 1);
        strncat(fam, generics[generic], sizeof fam - strlen(fam) - 1);
    }
    if (fam[0]) snprintf(s->font_family, sizeof s->font_family, "%s", fam);
    if (css_computed_font_size(cs, &len, &unit) == CSS_FONT_SIZE_DIMENSION)
        s->font_size = MAX(to_px(cs, len, unit), 1);
    uint8_t fw = css_computed_font_weight(cs);
    if (fw == CSS_FONT_WEIGHT_NORMAL) s->font_weight = 400;
    else if (fw == CSS_FONT_WEIGHT_BOLD) s->font_weight = 700;
    else if (fw == CSS_FONT_WEIGHT_BOLDER) s->font_weight = MIN(s->font_weight + 300, 900);
    else if (fw == CSS_FONT_WEIGHT_LIGHTER) s->font_weight = MAX(s->font_weight - 300, 100);
    else if (fw >= CSS_FONT_WEIGHT_100 && fw <= CSS_FONT_WEIGHT_900) s->font_weight = (fw - CSS_FONT_WEIGHT_100 + 1) * 100;
    uint8_t fs = css_computed_font_style(cs);
    if (fs != CSS_FONT_STYLE_INHERIT) s->font_italic = fs == CSS_FONT_STYLE_ITALIC || fs == CSS_FONT_STYLE_OBLIQUE;

    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER: s->text_align = ALIGN_CENTER; break;
    case CSS_TEXT_ALIGN_RIGHT: s->text_align = ALIGN_RIGHT; break;
    case CSS_TEXT_ALIGN_LEFT: case CSS_TEXT_ALIGN_JUSTIFY: s->text_align = ALIGN_LEFT; break;
    default: break;
    }
    s->hidden = css_computed_display(cs, false) == CSS_DISPLAY_NONE;
    s->float_left = css_computed_float(cs) == CSS_FLOAT_LEFT;
}

static css_select_results *select_in(css_select_ctx *ctx, struct css_elem *e) {
    css_select_results *res = NULL;
    if (css_select_style(ctx, e, &unit_ctx, &media, NULL, &handler, NULL, &res) != CSS_OK) return NULL;
    return res;
}

void css_compute(struct css_elem *e, const struct style *parent, struct style *s) {
    style_init(s, parent);
    if (!active.ctx[0]) return;
    css_select_results *res = select_in(active.ctx[0], e);
    if (!res) return;
    e->engine[SLOT_RESULTS] = res;
    css_computed_style *cs = res->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (!cs) return;
    /* selection gives a partial style ("inherit" left unresolved); composing with the
     * parent's final style resolves inheritance */
    css_computed_style *parent_cs = e->parent ? e->parent->engine[SLOT_COMPOSED] : NULL;
    if (!parent_cs && e->parent && e->parent->engine[SLOT_RESULTS])
        parent_cs = ((css_select_results *)e->parent->engine[SLOT_RESULTS])->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (parent_cs) {
        css_computed_style *composed;
        if (css_computed_style_compose(parent_cs, cs, &unit_ctx, &composed) == CSS_OK) {
            e->engine[SLOT_COMPOSED] = composed;
            cs = composed;
        }
    }
    translate(cs, s);

    /* each extension property: its own cascade, its winning value in `content` */
    for (int i = 0; i < CSS_NEXT; i++) {
        if (!active.ctx[1 + i]) continue;
        css_select_results *r = select_in(active.ctx[1 + i], e);
        if (!r) continue;
        const css_computed_content_item *item;
        css_computed_style *xs = r->styles[CSS_PSEUDO_ELEMENT_NONE];
        if (xs && css_computed_content(xs, &item) == CSS_CONTENT_SET && item &&
            item->type == CSS_COMPUTED_CONTENT_STRING) {
            char value[512];
            lwc_copy(value, sizeof value, item->data.string);
            css_apply_property(s, css_ext_props[i], value);
        }
        css_select_results_destroy(r);
    }
    style_finish(s);
}

void css_elem_release(struct css_elem *e) {
    if (e->engine[SLOT_COMPOSED]) css_computed_style_destroy(e->engine[SLOT_COMPOSED]);
    if (e->engine[SLOT_RESULTS]) css_select_results_destroy(e->engine[SLOT_RESULTS]);
    if (e->engine[SLOT_NODE_DATA])
        css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, NULL, e, NULL, e->engine[SLOT_NODE_DATA]);
    free(e->engine[SLOT_CLASSES]);
    memset(e->engine, 0, sizeof e->engine);
}
