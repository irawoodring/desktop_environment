/* css.c — tokenizer, parser, selector matching and cascade (the built-in engine), plus
 * the value parser and the libcss rewriter that both engines share.
 *
 * Pipeline, same shape as a browser's:
 *   text --tokenize--> tokens --parse--> rules (selectors + raw declarations)
 *   --resolve var()--> typed declarations
 *   element --match selectors, sort by specificity/order, apply--> computed style */
#include "css.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ tokenizer */

enum tt { T_EOF, T_WS, T_IDENT, T_FUNC, T_HASH, T_STR, T_NUM, T_DELIM, T_COLON, T_SEMI,
          T_COMMA, T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK };

struct tok {
    enum tt t;
    char s[64];                    /* ident/function/hash/string text, or the delim char */
    double num;
    char unit[8];                  /* "px", "%", "deg", "" ... */
    int line;
};

struct toks { struct tok *v; int n, cap; };

static void push(struct toks *ts, struct tok t) {
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 256;
        ts->v = realloc(ts->v, ts->cap * sizeof *ts->v);
    }
    ts->v[ts->n++] = t;
}

static bool name_char(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || (c & 0x80); }

static void tokenize(const char *p, struct toks *ts) {
    int line = 1;
    while (*p) {
        struct tok t = { .line = line };
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (*p) p += 2;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) { if (*p == '\n') line++; p++; }
            t.t = T_WS;
            push(ts, t);
            continue;
        }
        bool num_start = isdigit((unsigned char)p[0]) ||
                         (p[0] == '.' && isdigit((unsigned char)p[1])) ||
                         ((p[0] == '-' || p[0] == '+') &&
                          (isdigit((unsigned char)p[1]) || (p[1] == '.' && isdigit((unsigned char)p[2]))));
        if (num_start) {
            char *end;
            t.t = T_NUM;
            t.num = strtod(p, &end);
            p = end;
            int u = 0;
            if (*p == '%') { t.unit[u++] = *p++; }
            else while (isalpha((unsigned char)*p) && u < 7) t.unit[u++] = tolower((unsigned char)*p++);
            push(ts, t);
            continue;
        }
        if (name_char(*p) && !isdigit((unsigned char)*p)) {
            int n = 0;
            while (name_char(*p)) { if (n < 63) t.s[n++] = *p; p++; }
            if (*p == '(') { t.t = T_FUNC; p++; }
            else t.t = T_IDENT;
            push(ts, t);
            continue;
        }
        if (*p == '#') {
            int n = 0;
            p++;
            while (name_char(*p)) { if (n < 63) t.s[n++] = *p; p++; }
            t.t = T_HASH;
            push(ts, t);
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            int n = 0;
            while (*p && *p != q && *p != '\n') {
                if (*p == '\\' && p[1]) p++;
                if (n < 63) t.s[n++] = *p;
                p++;
            }
            if (*p == q) p++;
            t.t = T_STR;
            push(ts, t);
            continue;
        }
        switch (*p) {
        case ':': t.t = T_COLON; break;
        case ';': t.t = T_SEMI; break;
        case ',': t.t = T_COMMA; break;
        case '{': t.t = T_LBRACE; break;
        case '}': t.t = T_RBRACE; break;
        case '(': t.t = T_LPAREN; break;
        case ')': t.t = T_RPAREN; break;
        case '[': t.t = T_LBRACK; break;
        case ']': t.t = T_RBRACK; break;
        default: t.t = T_DELIM; t.s[0] = *p; break;
        }
        p++;
        push(ts, t);
    }
    push(ts, (struct tok){ .t = T_EOF, .line = line });
}

/* ------------------------------------------------------------------ data model */

struct attr_sel { char attr[16]; char op; char val[64]; };
struct compound {
    char name[32];                 /* "" = any */
    char cls[4][32];
    int ncls;
    unsigned states;
    struct attr_sel attrs[3];
    int nattrs;
};
#define MAX_PARTS 6
struct selector {
    struct compound parts[MAX_PARTS];
    char comb[MAX_PARTS];          /* combinator before parts[i]: ' ' or '>' */
    int nparts;
    int spec;
};

enum prop {
    P_BACKGROUND, P_COLOR, P_BORDER_WIDTH, P_BORDER_COLOR, P_BORDER_RADIUS, P_PADDING, P_MARGIN,
    P_WIDTH, P_HEIGHT, P_BOX_SHADOW, P_OPACITY, P_FONT_FAMILY, P_FONT_SIZE, P_TEXT_ALIGN,
    P_DISPLAY, P_FLOAT,
};

struct decl {
    enum prop prop;
    union {
        struct paint paint;
        struct color color;
        int len;
        int box[4];
        float num;
        char str[64];
        struct { float v; bool pct; } radius;
        struct { bool on; int x, y, blur, spread; struct color c; } shadow;
    } v;
};

struct rule {
    struct selector *sels;
    int nsels;
    struct decl *decls;
    int ndecls;
    bool ua;                       /* from the built-in defaults: any theme rule beats it */
};

struct sheet { struct rule *rules; int nrules; };

/* raw declarations before var() resolution */
struct raw_decl { char name[48]; int rule; int start, end; bool important; };   /* tokens [start, end) */

/* parse state */
struct parser {
    struct toks *ts;
    const char *file;
    int errors;
    struct raw_decl *raw;
    int nraw, capraw;
};

static void err(struct parser *ps, int line, const char *msg, const char *what) {
    fprintf(stderr, "%s:%d: %s%s%s\n", ps->file, line, msg, what ? " " : "", what ? what : "");
    ps->errors++;
}

/* ------------------------------------------------------------------ selectors */

static bool pseudo_bit(const char *s, unsigned *bit) {
    static const struct { const char *n; unsigned b; } names[] = {
        { "focused", ST_FOCUSED }, { "focus", ST_FOCUSED }, { "hover", ST_HOVER },
        { "active", ST_ACTIVE }, { "maximized", ST_MAXIMIZED }, { "fullscreen", ST_FULLSCREEN },
        { "root", ST_ROOT },
    };
    for (size_t i = 0; i < ARRAY_LEN(names); i++)
        if (strcmp(s, names[i].n) == 0) { *bit = names[i].b; return true; }
    return false;
}

/* parse one complex selector from tokens [i, end); returns false if unsupported */
static bool parse_selector(struct parser *ps, struct tok *t, int i, int end, struct selector *sel) {
    memset(sel, 0, sizeof *sel);
    while (i < end && t[i].t == T_WS) i++;
    while (end > i && t[end - 1].t == T_WS) end--;
    if (i >= end) return false;

    struct compound *c = &sel->parts[0];
    sel->nparts = 1;
    sel->comb[0] = ' ';
    bool empty = true;   /* current compound has nothing yet */
    char pending_comb = 0;

    while (i < end) {
        struct tok *k = &t[i];
        if (k->t == T_WS || (k->t == T_DELIM && k->s[0] == '>')) {
            if (k->t == T_DELIM) pending_comb = '>';
            else if (!pending_comb) pending_comb = ' ';
            i++;
            continue;
        }
        if (pending_comb) {
            if (empty) return false;
            if (sel->nparts == MAX_PARTS) { err(ps, k->line, "selector too long", NULL); return false; }
            c = &sel->parts[sel->nparts];
            sel->comb[sel->nparts] = pending_comb;
            sel->nparts++;
            pending_comb = 0;
            empty = true;
        }
        if (k->t == T_IDENT && empty) {
            snprintf(c->name, sizeof c->name, "%s", k->s);
            i++;
        } else if (k->t == T_DELIM && k->s[0] == '*' && empty) {
            i++;
        } else if (k->t == T_DELIM && k->s[0] == '.' && i + 1 < end && t[i + 1].t == T_IDENT) {
            if (c->ncls < 4) snprintf(c->cls[c->ncls++], 32, "%s", t[i + 1].s);
            i += 2;
        } else if (k->t == T_COLON && i + 1 < end && t[i + 1].t == T_IDENT) {
            unsigned bit;
            if (!pseudo_bit(t[i + 1].s, &bit)) { err(ps, k->line, "unknown pseudo-class :", t[i + 1].s); return false; }
            c->states |= bit;
            i += 2;
        } else if (k->t == T_LBRACK) {
            /* [attr] [attr=val] [attr*=val] [attr^=val] */
            struct attr_sel a = { 0 };
            i++;
            while (i < end && t[i].t == T_WS) i++;
            if (i >= end || t[i].t != T_IDENT) return false;
            snprintf(a.attr, sizeof a.attr, "%s", t[i++].s);
            while (i < end && t[i].t == T_WS) i++;
            if (i < end && t[i].t == T_DELIM && strchr("*^$", t[i].s[0])) a.op = t[i++].s[0];
            if (i < end && t[i].t == T_DELIM && t[i].s[0] == '=') {
                if (!a.op) a.op = '=';
                i++;
                while (i < end && t[i].t == T_WS) i++;
                if (i >= end || (t[i].t != T_STR && t[i].t != T_IDENT)) return false;
                snprintf(a.val, sizeof a.val, "%s", t[i++].s);
            } else {
                a.op = 'e';   /* attribute merely present */
            }
            while (i < end && t[i].t == T_WS) i++;
            if (i >= end || t[i].t != T_RBRACK) return false;
            i++;
            if (c->nattrs < 3) c->attrs[c->nattrs++] = a;
        } else {
            err(ps, k->line, "unsupported selector syntax near", k->s[0] ? k->s : "?");
            return false;
        }
        empty = false;
    }
    if (empty) return false;

    for (int p = 0; p < sel->nparts; p++) {   /* specificity: (classes+attrs+pseudos, types) */
        struct compound *cc = &sel->parts[p];
        sel->spec += 100 * (cc->ncls + cc->nattrs + __builtin_popcount(cc->states));
        if (cc->name[0]) sel->spec += 1;
    }
    return true;
}

static bool has_class(const char *list, const char *cls) {
    if (!list) return false;
    size_t n = strlen(cls);
    for (const char *p = list; (p = strstr(p, cls)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) return true;
    return false;
}

static bool match_compound(const struct compound *c, const struct css_elem *e) {
    if (c->name[0] && strcmp(c->name, e->name) != 0) return false;
    for (int i = 0; i < c->ncls; i++)
        if (!has_class(e->cls, c->cls[i])) return false;
    unsigned states = e->states | (e->parent ? 0 : ST_ROOT);
    if ((c->states & states) != c->states) return false;
    for (int i = 0; i < c->nattrs; i++) {
        const struct attr_sel *a = &c->attrs[i];
        const char *v = strcmp(a->attr, "app-id") == 0 ? e->app_id :
                        strcmp(a->attr, "title") == 0 ? e->title : NULL;
        if (!v) return false;
        size_t vl = strlen(v), al = strlen(a->val);
        switch (a->op) {
        case '=': if (strcmp(v, a->val) != 0) return false; break;
        case '*': if (!strstr(v, a->val)) return false; break;
        case '^': if (strncmp(v, a->val, al) != 0) return false; break;
        case '$': if (vl < al || strcmp(v + vl - al, a->val) != 0) return false; break;
        default: break;
        }
    }
    return true;
}

/* match right-to-left, walking up the tree for combinators */
__attribute__((unused)) static bool match_from(const struct selector *s, int idx, const struct css_elem *e) {
    if (!match_compound(&s->parts[idx], e)) return false;
    if (idx == 0) return true;
    if (s->comb[idx] == '>') return e->parent && match_from(s, idx - 1, e->parent);
    for (const struct css_elem *a = e->parent; a; a = a->parent)
        if (match_from(s, idx - 1, a)) return true;
    return false;
}

/* ---------------------------------------------------------------- value parsing */

struct vcur { struct tok *t; int i, end; };

static struct tok *peek(struct vcur *v) {
    while (v->i < v->end && v->t[v->i].t == T_WS) v->i++;
    return v->i < v->end ? &v->t[v->i] : NULL;
}
static struct tok *next(struct vcur *v) {
    struct tok *k = peek(v);
    if (k) v->i++;
    return k;
}
static bool at_end(struct vcur *v) { return peek(v) == NULL; }

/* tokens of a function call's arguments: returns a cursor over [i, matching ')') */
static struct vcur func_args(struct vcur *v) {
    struct vcur a = { v->t, v->i, v->i };
    int depth = 1;
    while (v->i < v->end) {
        enum tt t = v->t[v->i].t;
        if (t == T_FUNC || t == T_LPAREN) depth++;
        if (t == T_RPAREN && --depth == 0) { a.end = v->i; v->i++; return a; }
        v->i++;
    }
    a.end = v->i;
    return a;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_hex(const char *h, struct color *c) {
    size_t n = strlen(h);
    for (size_t i = 0; i < n; i++) if (hexval(h[i]) < 0) return false;
    if (n == 3 || n == 4) {
        c->r = hexval(h[0]) * 17; c->g = hexval(h[1]) * 17; c->b = hexval(h[2]) * 17;
        c->a = n == 4 ? hexval(h[3]) * 17 : 255;
        return true;
    }
    if (n == 6 || n == 8) {
        c->r = hexval(h[0]) * 16 + hexval(h[1]);
        c->g = hexval(h[2]) * 16 + hexval(h[3]);
        c->b = hexval(h[4]) * 16 + hexval(h[5]);
        c->a = n == 8 ? hexval(h[6]) * 16 + hexval(h[7]) : 255;
        return true;
    }
    return false;
}

static const struct { const char *name; uint32_t rgb; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 },
    { "blue", 0x0000ff }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 },
    { "orange", 0xffa500 }, { "yellow", 0xffff00 }, { "purple", 0x800080 }, { "pink", 0xffc0cb },
    { "cyan", 0x00ffff }, { "aqua", 0x00ffff }, { "magenta", 0xff00ff }, { "fuchsia", 0xff00ff },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "maroon", 0x800000 }, { "olive", 0x808000 },
    { "lime", 0x00ff00 }, { "crimson", 0xdc143c }, { "gold", 0xffd700 }, { "indigo", 0x4b0082 },
    { "coral", 0xff7f50 }, { "salmon", 0xfa8072 }, { "tomato", 0xff6347 }, { "skyblue", 0x87ceeb },
    { "steelblue", 0x4682b4 }, { "slategray", 0x708090 }, { "darkslategray", 0x2f4f4f },
    { "dimgray", 0x696969 }, { "lightgray", 0xd3d3d3 }, { "whitesmoke", 0xf5f5f5 },
};

static float channel(struct tok *k, float scale) {   /* number or percentage -> 0..scale */
    if (strcmp(k->unit, "%") == 0) return CLAMP(k->num / 100.0 * scale, 0, scale);
    return CLAMP(k->num, 0, scale);
}

static bool parse_color(struct vcur *v, struct color *c) {
    struct tok *k = peek(v);
    if (!k) return false;
    if (k->t == T_HASH) {
        if (!parse_hex(k->s, c)) return false;
        v->i++;
        return true;
    }
    if (k->t == T_IDENT) {
        if (strcasecmp(k->s, "transparent") == 0) { *c = (struct color){ 0, 0, 0, 0 }; v->i++; return true; }
        for (size_t i = 0; i < ARRAY_LEN(named_colors); i++) {
            if (strcasecmp(k->s, named_colors[i].name) == 0) {
                uint32_t x = named_colors[i].rgb;
                *c = (struct color){ x >> 16, (x >> 8) & 0xff, x & 0xff, 255 };
                v->i++;
                return true;
            }
        }
        return false;
    }
    if (k->t == T_FUNC && (strcasecmp(k->s, "rgb") == 0 || strcasecmp(k->s, "rgba") == 0)) {
        int save = v->i;
        v->i++;
        struct vcur a = func_args(v);
        float ch[4] = { 0, 0, 0, 1 };
        int n = 0;
        struct tok *x;
        while ((x = next(&a))) {
            if (x->t == T_COMMA || (x->t == T_DELIM && x->s[0] == '/')) continue;
            if (x->t != T_NUM || n == 4) { v->i = save; return false; }
            ch[n] = n < 3 ? channel(x, 255) : channel(x, 1);
            n++;
        }
        if (n < 3) { v->i = save; return false; }
        *c = (struct color){ ch[0] + 0.5f, ch[1] + 0.5f, ch[2] + 0.5f, ch[3] * 255 + 0.5f };
        return true;
    }
    return false;
}

static bool parse_length(struct vcur *v, int *out) {
    struct tok *k = peek(v);
    if (!k || k->t != T_NUM) return false;
    if (k->unit[0] && strcmp(k->unit, "px") != 0) return false;
    *out = (int)lround(k->num);
    v->i++;
    return true;
}

static bool parse_gradient(struct vcur *v, struct paint *p) {
    v->i++;   /* the linear-gradient( token */
    struct vcur a = func_args(v);
    memset(p, 0, sizeof *p);
    p->kind = PAINT_GRADIENT;
    p->angle = 180;   /* default: to bottom */

    struct tok *k = peek(&a);
    if (k && k->t == T_NUM && strcmp(k->unit, "deg") == 0) {
        p->angle = k->num;
        a.i++;
        k = next(&a);
        if (!k || k->t != T_COMMA) return false;
    } else if (k && k->t == T_IDENT && strcmp(k->s, "to") == 0) {
        a.i++;
        bool top = false, bottom = false, left = false, right = false;
        while ((k = next(&a)) && k->t == T_IDENT) {
            if (!strcmp(k->s, "top")) top = true;
            else if (!strcmp(k->s, "bottom")) bottom = true;
            else if (!strcmp(k->s, "left")) left = true;
            else if (!strcmp(k->s, "right")) right = true;
            else return false;
        }
        if (!k || k->t != T_COMMA) return false;
        float dx = right ? 1 : left ? -1 : 0, dy = bottom ? 1 : top ? -1 : 0;
        p->angle = atan2f(dx, -dy) * 180 / (float)M_PI;
    }
    while (!at_end(&a)) {
        if (p->nstops == 8) return false;
        struct grad_stop *s = &p->stops[p->nstops];
        if (!parse_color(&a, &s->c)) return false;
        s->pos = -1;
        k = peek(&a);
        if (k && k->t == T_NUM && strcmp(k->unit, "%") == 0) { s->pos = k->num / 100.0f; a.i++; }
        p->nstops++;
        k = next(&a);
        if (k && k->t != T_COMMA) return false;
    }
    if (p->nstops < 2) return false;
    /* fill in missing stop positions: ends at 0/1, gaps spread evenly */
    if (p->stops[0].pos < 0) p->stops[0].pos = 0;
    if (p->stops[p->nstops - 1].pos < 0) p->stops[p->nstops - 1].pos = 1;
    for (int i = 1; i < p->nstops; i++) {
        if (p->stops[i].pos >= 0) {
            if (p->stops[i].pos < p->stops[i - 1].pos) p->stops[i].pos = p->stops[i - 1].pos;
            continue;
        }
        int j = i;
        while (p->stops[j].pos < 0) j++;
        float from = p->stops[i - 1].pos, to = p->stops[j].pos;
        for (int m = i; m < j; m++) p->stops[m].pos = from + (to - from) * (m - i + 1) / (j - i + 1);
    }
    return true;
}

static bool parse_paint(struct vcur *v, struct paint *p) {
    struct tok *k = peek(v);
    if (!k) return false;
    if (k->t == T_IDENT && strcmp(k->s, "none") == 0) { p->kind = PAINT_NONE; v->i++; return true; }
    if (k->t == T_FUNC && strcmp(k->s, "linear-gradient") == 0) return parse_gradient(v, p);
    p->kind = PAINT_COLOR;
    return parse_color(v, &p->color);
}

/* Parse the value tokens for one property, appending typed declarations to the rule. */
static bool parse_decl(struct parser *ps, const char *name, struct vcur *v, struct rule *r, int line) {
    struct decl d[2];
    int nd = 0;
    memset(d, 0, sizeof d);
    struct tok *k;

    if (!strcmp(name, "background") || !strcmp(name, "background-color") || !strcmp(name, "background-image")) {
        d[0].prop = P_BACKGROUND;
        if (!parse_paint(v, &d[0].v.paint)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "color")) {
        d[0].prop = P_COLOR;
        if (!parse_color(v, &d[0].v.color)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "border")) {
        /* border: <width> || <style> || <color> */
        d[0].prop = P_BORDER_WIDTH;
        d[1].prop = P_BORDER_COLOR;
        d[1].v.color = (struct color){ 0, 0, 0, 255 };
        while ((k = peek(v))) {
            if (k->t == T_NUM) { if (!parse_length(v, &d[0].v.len)) goto bad; }
            else if (k->t == T_IDENT && !strcmp(k->s, "none")) { d[0].v.len = 0; v->i++; }
            else if (k->t == T_IDENT && (!strcmp(k->s, "solid") || !strcmp(k->s, "dashed") ||
                                         !strcmp(k->s, "dotted") || !strcmp(k->s, "double"))) v->i++;
            else if (!parse_color(v, &d[1].v.color)) goto bad;
        }
        nd = 2;
    } else if (!strcmp(name, "border-width")) {
        d[0].prop = P_BORDER_WIDTH;
        if (!parse_length(v, &d[0].v.len)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "border-color")) {
        d[0].prop = P_BORDER_COLOR;
        if (!parse_color(v, &d[0].v.color)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "border-radius")) {
        d[0].prop = P_BORDER_RADIUS;
        k = next(v);
        if (!k || k->t != T_NUM) goto bad;
        d[0].v.radius.v = k->num;
        d[0].v.radius.pct = !strcmp(k->unit, "%");
        if (k->unit[0] && strcmp(k->unit, "px") && !d[0].v.radius.pct) goto bad;
        nd = 1;
    } else if (!strcmp(name, "padding") || !strcmp(name, "margin")) {
        d[0].prop = name[0] == 'p' ? P_PADDING : P_MARGIN;
        int vals[4], n = 0;
        while (!at_end(v) && n < 4) if (!parse_length(v, &vals[n++])) goto bad;
        if (n == 0) goto bad;
        int *b = d[0].v.box;   /* CSS 1-4 value expansion: top right bottom left */
        b[0] = vals[0];
        b[1] = n > 1 ? vals[1] : vals[0];
        b[2] = n > 2 ? vals[2] : vals[0];
        b[3] = n > 3 ? vals[3] : b[1];
        nd = 1;
    } else if (!strcmp(name, "width") || !strcmp(name, "height")) {
        d[0].prop = name[0] == 'w' ? P_WIDTH : P_HEIGHT;
        k = peek(v);
        if (k && k->t == T_IDENT && !strcmp(k->s, "auto")) { d[0].v.len = -1; v->i++; }
        else if (!parse_length(v, &d[0].v.len)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "box-shadow")) {
        d[0].prop = P_BOX_SHADOW;
        k = peek(v);
        if (k && k->t == T_IDENT && !strcmp(k->s, "none")) { v->i++; nd = 1; }
        else {
            int lens[4], n = 0;
            struct color c = { 0, 0, 0, 128 };
            while ((k = peek(v)) && k->t != T_COMMA) {
                if (k->t == T_NUM) { if (n == 4 || !parse_length(v, &lens[n++])) goto bad; }
                else if (!parse_color(v, &c)) goto bad;
            }
            if (n < 2) goto bad;
            while (!at_end(v)) v->i++;   /* only the first shadow of a list is used */
            d[0].v.shadow.on = true;
            d[0].v.shadow.x = lens[0];
            d[0].v.shadow.y = lens[1];
            d[0].v.shadow.blur = n > 2 ? MAX(lens[2], 0) : 0;
            d[0].v.shadow.spread = n > 3 ? lens[3] : 0;
            d[0].v.shadow.c = c;
            nd = 1;
        }
    } else if (!strcmp(name, "opacity")) {
        d[0].prop = P_OPACITY;
        k = next(v);
        if (!k || k->t != T_NUM) goto bad;
        d[0].v.num = CLAMP(!strcmp(k->unit, "%") ? k->num / 100 : k->num, 0, 1);
        nd = 1;
    } else if (!strcmp(name, "font-family")) {
        d[0].prop = P_FONT_FAMILY;
        k = next(v);
        if (!k || (k->t != T_IDENT && k->t != T_STR)) goto bad;
        snprintf(d[0].v.str, sizeof d[0].v.str, "%s", k->s);
        while (!at_end(v)) v->i++;   /* ignore fallbacks */
        nd = 1;
    } else if (!strcmp(name, "font-size")) {
        d[0].prop = P_FONT_SIZE;
        if (!parse_length(v, &d[0].v.len)) goto bad;
        nd = 1;
    } else if (!strcmp(name, "text-align")) {
        d[0].prop = P_TEXT_ALIGN;
        k = next(v);
        if (!k || k->t != T_IDENT) goto bad;
        if (!strcmp(k->s, "left") || !strcmp(k->s, "start")) d[0].v.len = ALIGN_LEFT;
        else if (!strcmp(k->s, "center")) d[0].v.len = ALIGN_CENTER;
        else if (!strcmp(k->s, "right") || !strcmp(k->s, "end")) d[0].v.len = ALIGN_RIGHT;
        else goto bad;
        nd = 1;
    } else if (!strcmp(name, "display")) {
        d[0].prop = P_DISPLAY;
        k = next(v);
        if (!k || k->t != T_IDENT) goto bad;
        d[0].v.len = !strcmp(k->s, "none");
        nd = 1;
    } else if (!strcmp(name, "float")) {
        d[0].prop = P_FLOAT;
        k = next(v);
        if (!k || k->t != T_IDENT || (strcmp(k->s, "left") && strcmp(k->s, "right"))) goto bad;
        d[0].v.len = !strcmp(k->s, "left");
        nd = 1;
    } else {
        err(ps, line, "unknown property", name);
        return false;
    }
    if (!at_end(v)) goto bad;
    r->decls = realloc(r->decls, (r->ndecls + nd) * sizeof *r->decls);
    memcpy(r->decls + r->ndecls, d, nd * sizeof *d);
    r->ndecls += nd;
    return true;
bad:
    err(ps, line, "invalid value for", name);
    return false;
}

/* ------------------------------------------------------------------- parser */

static void parse_block(struct parser *ps, int *pi, int rule_idx) {
    struct tok *t = ps->ts->v;
    int i = *pi;
    while (t[i].t != T_EOF && t[i].t != T_RBRACE) {
        if (t[i].t == T_WS || t[i].t == T_SEMI) { i++; continue; }
        if (t[i].t != T_IDENT) {   /* skip junk up to the next ; */
            err(ps, t[i].line, "expected a property name", NULL);
            while (t[i].t != T_EOF && t[i].t != T_SEMI && t[i].t != T_RBRACE) i++;
            continue;
        }
        struct raw_decl rd = { .rule = rule_idx };
        snprintf(rd.name, sizeof rd.name, "%s", t[i].s);
        int line = t[i].line;
        i++;
        while (t[i].t == T_WS) i++;
        if (t[i].t != T_COLON) {
            err(ps, line, "expected ':' after", rd.name);
            while (t[i].t != T_EOF && t[i].t != T_SEMI && t[i].t != T_RBRACE) i++;
            continue;
        }
        i++;
        rd.start = i;
        int depth = 0;
        while (t[i].t != T_EOF) {
            if (t[i].t == T_FUNC || t[i].t == T_LPAREN) depth++;
            if (t[i].t == T_RPAREN && depth > 0) depth--;
            if (depth == 0 && (t[i].t == T_SEMI || t[i].t == T_RBRACE)) break;
            i++;
        }
        rd.end = i;
        /* a trailing !important: the built-in engine ignores it, the libcss rewrite keeps it */
        for (int j = rd.start; j + 1 < rd.end; j++)
            if (t[j].t == T_DELIM && t[j].s[0] == '!' && t[j + 1].t == T_IDENT && !strcmp(t[j + 1].s, "important")) {
                rd.end = j;
                rd.important = true;
            }
        if (ps->nraw == ps->capraw) {
            ps->capraw = ps->capraw ? ps->capraw * 2 : 64;
            ps->raw = realloc(ps->raw, ps->capraw * sizeof *ps->raw);
        }
        ps->raw[ps->nraw++] = rd;
    }
    if (t[i].t == T_RBRACE) i++;
    *pi = i;
}

/* Expand var(--x) references into a fresh token list. Variables are global (last wins). */
static bool expand_vars(struct parser *ps, int start, int end, struct toks *out, int depth) {
    struct tok *t = ps->ts->v;
    if (depth > 8) return false;
    for (int i = start; i < end; i++) {
        if (t[i].t == T_FUNC && !strcmp(t[i].s, "var")) {
            int j = i + 1;
            while (j < end && t[j].t == T_WS) j++;
            if (j >= end || t[j].t != T_IDENT) return false;
            const char *name = t[j].s;
            int close = j + 1;
            while (close < end && t[close].t != T_RPAREN) close++;
            struct raw_decl *def = NULL;
            for (int r = 0; r < ps->nraw; r++)
                if (!strcmp(ps->raw[r].name, name)) def = &ps->raw[r];
            if (!def) {
                err(ps, t[i].line, "undefined variable", name);
                return false;
            }
            if (!expand_vars(ps, def->start, def->end, out, depth + 1)) return false;
            i = close;
            continue;
        }
        push(out, t[i]);
    }
    return true;
}


static void free_sheet(struct sheet *s) {
    for (int i = 0; i < s->nrules; i++) {
        free(s->rules[i].sels);
        free(s->rules[i].decls);
    }
    free(s->rules);
    s->rules = NULL;
    s->nrules = 0;
}

/* ------------------------------------------------------- rewriting for libcss */

struct strbuf { char *s; size_t len, cap; };

static void sb_put(struct strbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}
static void sb_puts(struct strbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_quoted(struct strbuf *b, const char *s) {
    sb_puts(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_puts(b, "\\");
        sb_put(b, s, 1);
    }
    sb_puts(b, "\"");
}

/* write a token back out as CSS text (comments and exact spacing are not preserved) */
static void emit_tok(struct strbuf *b, const struct tok *t) {
    char buf[96];
    switch (t->t) {
    case T_WS: sb_puts(b, " "); break;
    case T_IDENT: sb_puts(b, t->s); break;
    case T_FUNC: sb_puts(b, t->s); sb_puts(b, "("); break;
    case T_HASH: sb_puts(b, "#"); sb_puts(b, t->s); break;
    case T_STR: sb_quoted(b, t->s); break;
    case T_NUM: snprintf(buf, sizeof buf, "%g%s", t->num, t->unit); sb_puts(b, buf); break;
    case T_DELIM: sb_put(b, t->s, 1); break;
    case T_COLON: sb_puts(b, ":"); break;
    case T_SEMI: sb_puts(b, ";"); break;
    case T_COMMA: sb_puts(b, ","); break;
    case T_LBRACE: sb_puts(b, "{"); break;
    case T_RBRACE: sb_puts(b, "}"); break;
    case T_LPAREN: sb_puts(b, "("); break;
    case T_RPAREN: sb_puts(b, ")"); break;
    case T_LBRACK: sb_puts(b, "["); break;
    case T_RBRACK: sb_puts(b, "]"); break;
    case T_EOF: break;
    }
}

static void emit_range(struct strbuf *b, const struct tok *t, int start, int end) {
    for (int i = start; i < end; i++) emit_tok(b, &t[i]);
}

/* Selectors: our window states aren't standard pseudo-classes, so express them in terms
 * libcss understands. :focused becomes :focus; :maximized and :fullscreen become an
 * attribute the libcss engine reports, [pane-state~="maximized"]. */
static void emit_selector(struct strbuf *b, const struct tok *t, int start, int end) {
    for (int i = start; i < end; i++) {
        if (t[i].t == T_COLON && i + 1 < end && t[i + 1].t == T_IDENT) {
            const char *p = t[i + 1].s;
            if (!strcmp(p, "focused")) { sb_puts(b, ":focus"); i++; continue; }
            if (!strcmp(p, "maximized") || !strcmp(p, "fullscreen")) {
                sb_puts(b, "[pane-state~=\"");
                sb_puts(b, p);
                sb_puts(b, "\"]");
                i++;
                continue;
            }
        }
        emit_tok(b, &t[i]);
    }
}

static bool has_gradient(const struct toks *v) {
    for (int i = 0; i < v->n; i++)
        if (v->v[i].t == T_FUNC && !strcmp(v->v[i].s, "linear-gradient")) return true;
    return false;
}

/* One declaration, with var() already expanded. libcss drops properties it doesn't
 * know, so the three it lacks travel inside properties pane doesn't otherwise use:
 *   border-radius: R        ->  max-width: R
 *   box-shadow: S           ->  content: "S"
 *   background: <gradient>  ->  background: url("<gradient>")
 * css_libcss.c reads them back out and hands the text to the built-in value parser. */
static void emit_decl(struct strbuf *b, const char *name, const struct toks *v, bool important) {
    struct strbuf val = { 0 };
    emit_range(&val, v->v, 0, v->n);
    const char *text = val.s ? val.s : "";
    while (*text == ' ') text++;

    if (!strcmp(name, "border-radius")) {
        sb_puts(b, "max-width:");
        sb_puts(b, text);
    } else if (!strcmp(name, "box-shadow")) {
        sb_puts(b, "content:");
        sb_quoted(b, text);
    } else if ((!strcmp(name, "background") || !strcmp(name, "background-image")) && has_gradient(v)) {
        sb_puts(b, name);
        sb_puts(b, ":url(");
        sb_quoted(b, text);
        sb_puts(b, ")");
    } else {
        sb_puts(b, name);
        sb_puts(b, ":");
        sb_puts(b, text);
    }
    if (important) sb_puts(b, " !important");
    sb_puts(b, ";\n");
    free(val.s);
}

/* ------------------------------------------------------------ window classes */

/* Every class the theme uses on a window (window.work, window.dim titlebar, ...) is
 * offered in the window menu, so styling a class is all it takes to make it usable. */
static char window_classes[32][32];
static int nwindow_classes;

void css_window_classes_reset(void) { nwindow_classes = 0; }

const char *css_window_class(int i) { return i >= 0 && i < nwindow_classes ? window_classes[i] : NULL; }

static void add_window_class(const char *name) {
    for (int i = 0; i < nwindow_classes; i++)
        if (!strcmp(window_classes[i], name)) return;
    if (nwindow_classes < 32) snprintf(window_classes[nwindow_classes++], 32, "%s", name);
}

static bool is_combinator(const struct tok *t) {
    return t->t == T_WS || t->t == T_COMMA || (t->t == T_DELIM && (t->s[0] == '>' || t->s[0] == '+' || t->s[0] == '~'));
}

/* find "window" compounds in a selector and note their .classes */
static void collect_window_classes(const struct tok *t, int start, int end) {
    for (int i = start; i < end; i++) {
        if (t[i].t != T_IDENT || strcmp(t[i].s, "window") || (i > start && !is_combinator(&t[i - 1]))) continue;
        for (int j = i + 1; j < end && !is_combinator(&t[j]); j++)
            if (t[j].t == T_DELIM && t[j].s[0] == '.' && j + 1 < end && t[j + 1].t == T_IDENT)
                add_window_class(t[j + 1].s);
    }
}

/* The "user agent stylesheet": defaults for things a theme may not mention (like the
 * window menu, which older themes predate). Theme rules always win over these. */
const char css_base_sheet[] =
    "menu { background: #2a2a2e; color: #e6e6e6; border: 1px solid #4a4a52; padding: 4px;"
    "       box-shadow: 0 4px 14px rgba(0, 0, 0, 0.5); }\n"
    "menuitem { padding: 0 10px; }\n"
    "menuitem:hover { background: #4a6fa5; color: white; }\n"
    "separator { height: 1px; margin: 4px 6px; background: #4a4a52; }\n"
    "heading { padding: 0 10px; color: #8a8a94; }\n";

/* ------------------------------------------------------------------- parsing */

/* Parse a whole sheet. With `out`, build the built-in engine's rules (and report every
 * problem). With `rewrite`, produce libcss-ready CSS instead, and only check the values
 * libcss can't check for us. */
static int parse_sheet(const char *text, const char *file, struct sheet *out, struct strbuf *rewrite) {
    struct toks ts = { 0 };
    tokenize(text, &ts);
    struct parser ps = { .ts = &ts, .file = file };
    struct tok *t = ts.v;
    struct sheet sh = { 0 };
    int *pre_start = NULL, *pre_end = NULL;
    int i = 0;

    while (t[i].t != T_EOF) {
        if (t[i].t == T_WS) { i++; continue; }
        if (t[i].t == T_DELIM && t[i].s[0] == '@') {   /* at-rules: skip */
            int depth = 0;
            while (t[i].t != T_EOF) {
                if (t[i].t == T_LBRACE) depth++;
                if (t[i].t == T_RBRACE && --depth <= 0) { i++; break; }
                if (t[i].t == T_SEMI && depth == 0) { i++; break; }
                i++;
            }
            continue;
        }
        int start = i;
        while (t[i].t != T_EOF && t[i].t != T_LBRACE) i++;
        if (t[i].t == T_EOF) { err(&ps, t[start].line, "expected '{'", NULL); break; }
        int end = i++;

        struct rule r = { 0 };
        int s = start;
        for (int j = start; j <= end && out; j++) {   /* split the prelude on commas */
            if (j == end || t[j].t == T_COMMA) {
                struct selector sel;
                if (parse_selector(&ps, t, s, j, &sel)) {
                    r.sels = realloc(r.sels, (r.nsels + 1) * sizeof *r.sels);
                    r.sels[r.nsels++] = sel;
                }
                s = j + 1;
            }
        }
        sh.rules = realloc(sh.rules, (sh.nrules + 1) * sizeof *sh.rules);
        sh.rules[sh.nrules] = r;
        pre_start = realloc(pre_start, (sh.nrules + 1) * sizeof *pre_start);
        pre_end = realloc(pre_end, (sh.nrules + 1) * sizeof *pre_end);
        pre_start[sh.nrules] = start;
        pre_end[sh.nrules] = end;
        collect_window_classes(t, start, end);
        parse_block(&ps, &i, sh.nrules);
        sh.nrules++;
    }

    int d = 0;
    for (int r = 0; r < sh.nrules; r++) {
        if (rewrite) {
            emit_selector(rewrite, t, pre_start[r], pre_end[r]);
            sb_puts(rewrite, " {\n");
        }
        for (; d < ps.nraw && ps.raw[d].rule == r; d++) {
            struct raw_decl *rd = &ps.raw[d];
            if (rd->name[0] == '-' && rd->name[1] == '-') continue;   /* custom property */
            struct toks expanded = { 0 };
            if (expand_vars(&ps, rd->start, rd->end, &expanded, 0)) {
                struct vcur v = { expanded.v, 0, expanded.n };
                int line = expanded.n ? expanded.v[0].line : 0;
                if (out) {
                    parse_decl(&ps, rd->name, &v, &sh.rules[r], line);
                } else {
                    bool ours = !strcmp(rd->name, "border-radius") || !strcmp(rd->name, "box-shadow") ||
                                has_gradient(&expanded);
                    struct rule scratch = { 0 };
                    if (ours) parse_decl(&ps, rd->name, &v, &scratch, line);   /* just to report errors */
                    free(scratch.decls);
                }
                if (rewrite) emit_decl(rewrite, rd->name, &expanded, rd->important);
            }
            free(expanded.v);
        }
        if (rewrite) sb_puts(rewrite, "}\n");
    }
    free(pre_start);
    free(pre_end);
    free(ps.raw);
    free(ts.v);
    if (out) *out = sh;
    else free_sheet(&sh);
    return ps.errors;
}

char *css_rewrite_for_libcss(const char *text, const char *file) {
    struct strbuf b = { 0 };
    sb_puts(&b, "");
    parse_sheet(text, file, NULL, &b);
    return b.s;
}

/* -------------------------------------------------------------- computed style */

void style_init(struct style *s, const struct style *parent) {
    memset(s, 0, sizeof *s);
    /* initial values; color, font and text-align inherit */
    s->color = parent ? parent->color : (struct color){ 255, 255, 255, 255 };
    snprintf(s->font_family, sizeof s->font_family, "%s", parent ? parent->font_family : "Lat2-Terminus16");
    s->font_size = parent ? parent->font_size : 16;
    s->text_align = parent ? parent->text_align : ALIGN_LEFT;
    s->width = s->height = -1;
    s->opacity = 1;
}

static void apply_decl(struct style *s, const struct decl *d) {
    switch (d->prop) {
    case P_BACKGROUND: s->background = d->v.paint; break;
    case P_COLOR: s->color = d->v.color; break;
    case P_BORDER_WIDTH: s->border_width = MAX(d->v.len, 0); break;
    case P_BORDER_COLOR: s->border_color = d->v.color; break;
    case P_BORDER_RADIUS: s->radius = fmaxf(d->v.radius.v, 0); s->radius_pct = d->v.radius.pct; break;
    case P_PADDING: memcpy(s->padding, d->v.box, sizeof s->padding); break;
    case P_MARGIN: memcpy(s->margin, d->v.box, sizeof s->margin); break;
    case P_WIDTH: s->width = d->v.len; break;
    case P_HEIGHT: s->height = d->v.len; break;
    case P_BOX_SHADOW:
        s->shadow = d->v.shadow.on;
        s->shadow_x = d->v.shadow.x;
        s->shadow_y = d->v.shadow.y;
        s->shadow_blur = d->v.shadow.blur;
        s->shadow_spread = d->v.shadow.spread;
        s->shadow_color = d->v.shadow.c;
        break;
    case P_OPACITY: s->opacity = d->v.num; break;
    case P_FONT_FAMILY: snprintf(s->font_family, sizeof s->font_family, "%s", d->v.str); break;
    case P_FONT_SIZE: s->font_size = MAX(d->v.len, 1); break;
    case P_TEXT_ALIGN: s->text_align = d->v.len; break;
    case P_DISPLAY: s->hidden = d->v.len; break;
    case P_FLOAT: s->float_left = d->v.len; break;
    }
}

bool css_apply_property(struct style *s, const char *name, const char *value) {
    struct toks ts = { 0 };
    tokenize(value, &ts);
    struct parser ps = { .ts = &ts, .file = "(theme)" };
    struct vcur v = { ts.v, 0, ts.n - 1 };   /* without the EOF token */
    struct rule r = { 0 };
    bool ok = parse_decl(&ps, name, &v, &r, 0);
    for (int i = 0; i < r.ndecls; i++) apply_decl(s, &r.decls[i]);
    free(r.decls);
    free(ts.v);
    return ok;
}

float style_radius(const struct style *s, int w, int h) {
    return s->radius_pct ? s->radius / 100.0f * MIN(w, h) : s->radius;
}

/* ---------------------------------------------------------- built-in engine */
#ifndef PANE_LIBCSS

static struct sheet active;

/* the defaults first, then the theme */
static void activate(struct sheet *theme) {
    struct sheet base;
    parse_sheet(css_base_sheet, "(built-in defaults)", &base, NULL);
    for (int i = 0; i < base.nrules; i++) base.rules[i].ua = true;
    base.rules = realloc(base.rules, (base.nrules + theme->nrules) * sizeof *base.rules);
    memcpy(base.rules + base.nrules, theme->rules, theme->nrules * sizeof *theme->rules);
    base.nrules += theme->nrules;
    free(theme->rules);
    free_sheet(&active);
    active = base;
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
    struct sheet sh;
    css_window_classes_reset();
    int errors = parse_sheet(text, path, &sh, NULL);
    free(text);
    /* keep going with whatever parsed: like a browser, bad declarations are just skipped */
    activate(&sh);
    fprintf(stderr, "pane: loaded %s (%d rules%s)\n", path, sh.nrules, errors ? ", with errors" : "");
    return 0;
}

void css_load_string(const char *text, const char *name) {
    struct sheet sh;
    css_window_classes_reset();
    parse_sheet(text, name, &sh, NULL);
    activate(&sh);
}

struct match { int spec, order; const struct rule *r; };

/* cascade order: defaults before the theme, then specificity, then source order */
static int cmp_match(const void *a, const void *b) {
    const struct match *x = a, *y = b;
    if (x->r->ua != y->r->ua) return x->r->ua ? -1 : 1;
    if (x->spec != y->spec) return x->spec - y->spec;
    return x->order - y->order;
}

void css_compute(struct css_elem *e, const struct style *parent, struct style *s) {
    style_init(s, parent);
    struct match m[64];
    int n = 0;
    for (int i = 0; i < active.nrules && n < 64; i++) {
        const struct rule *r = &active.rules[i];
        int best = -1;
        for (int j = 0; j < r->nsels; j++)
            if (r->sels[j].spec > best && match_from(&r->sels[j], r->sels[j].nparts - 1, e))
                best = r->sels[j].spec;
        if (best >= 0) m[n++] = (struct match){ best, i, r };
    }
    qsort(m, n, sizeof *m, cmp_match);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m[i].r->ndecls; j++) apply_decl(s, &m[i].r->decls[j]);
}

void css_elem_release(struct css_elem *e) { (void)e; }

const char *css_engine_name(void) { return "built-in"; }

#endif
