/* css.c — tokenizer, parser, selector matching and cascade (the built-in engine), plus
 * the value parser and the libcss rewriter that both engines share.
 *
 * Pipeline, same shape as a browser's:
 *   text --tokenize--> tokens --parse--> rules (selectors + raw declarations)
 *   --resolve var()--> typed declarations
 *   element --match selectors, sort by specificity/order, apply--> computed style */
#include "css.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ tokenizer */

enum tt { T_EOF, T_WS, T_IDENT, T_FUNC, T_HASH, T_STR, T_NUM, T_DELIM, T_COLON, T_SEMI,
          T_COMMA, T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_URL };

struct tok {
    enum tt t;
    char s[256];                   /* ident/function/hash/string/url text, or the delim char */
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
            while (name_char(*p)) { if (n < 255) t.s[n++] = *p; p++; }
            if (*p == '(' && !strcasecmp(t.s, "url")) {   /* url(...) is one token: a path */
                p++;
                while (isspace((unsigned char)*p)) p++;
                char q = (*p == '"' || *p == '\'') ? *p++ : 0;
                n = 0;
                while (*p && (q ? *p != q : (*p != ')' && !isspace((unsigned char)*p)))) {
                    if (*p == '\\' && p[1]) p++;
                    if (n < 255) t.s[n++] = *p;
                    p++;
                }
                t.s[n] = '\0';
                if (q && *p == q) p++;
                while (*p && *p != ')') p++;
                if (*p == ')') p++;
                t.t = T_URL;
            } else if (*p == '(') { t.t = T_FUNC; p++; }
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

/* Each parsed declaration sets one "longhand"; shorthands like border or background
 * expand into several. `idx` picks the side or corner for the per-side ones. */
enum prop {
    P_BG_COLOR, P_BG_IMAGE, P_BG_SIZE, P_BG_POS, P_BG_REPEAT, P_COLOR,
    P_BORDER_WIDTH, P_BORDER_COLOR, P_BORDER_STYLE, P_RADIUS, P_PADDING, P_MARGIN,
    P_WIDTH, P_HEIGHT, P_BOX_SHADOW, P_OPACITY, P_FONT_FAMILY, P_FONT_SIZE, P_FONT_WEIGHT,
    P_FONT_STYLE, P_TEXT_ALIGN, P_DISPLAY, P_FLOAT, P_TRANSITION, P_SIDE, P_WINDOW_LAYOUT,
    P_GAP, P_TILE_RATIO, P_TILE,
};

struct decl {
    enum prop prop;
    int idx;
    union {
        struct paint paint;
        struct color color;
        int len;
        float num;
        bool flag;
        char str[128];
        struct length l;
        struct { struct length x, y; } pos;
        struct { int mode; struct length w, h; } size;
        struct { bool x, y; } repeat;
        struct { bool on; int x, y, blur, spread; struct color c; } shadow;
        struct { int n; struct transition t[6]; } trans;
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
    bool quiet;                    /* re-reading values already checked: say nothing */
    int errors;
    struct raw_decl *raw;
    int nraw, capraw;
};

/* Problems with the theme, kept so pane can show them on screen: its stderr is
 * usually the console hidden behind the graphics. */
#define MAX_MESSAGES 24
static char messages[MAX_MESSAGES][200];
static int nmessages, nmessages_total;
static bool said_freetype;

void css_messages_reset(void) { nmessages = nmessages_total = 0; said_freetype = false; }
int css_message_count(void) { return nmessages_total; }
const char *css_message(int i) { return i >= 0 && i < nmessages ? messages[i] : NULL; }

static void err(struct parser *ps, int line, const char *msg, const char *what) {
    if (ps->quiet) return;
    fprintf(stderr, "%s:%d: %s%s%s\n", ps->file, line, msg, what ? " " : "", what ? what : "");
    const char *base = strrchr(ps->file, '/');
    if (nmessages < MAX_MESSAGES)
        snprintf(messages[nmessages++], sizeof messages[0], "%s:%d: %s%s%s", base ? base + 1 : ps->file, line,
                 msg, what ? " " : "", what ? what : "");
    nmessages_total++;
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

/* -------------------------------------------------------------- paths and lengths */

static char base_dir[PATH_MAX] = ".";

/* absolute, so a path already resolved (the libcss rewrite does it once) stays put */
void css_set_base_dir(const char *theme_path) {
    if (!realpath(theme_path, base_dir)) snprintf(base_dir, sizeof base_dir, "%s", theme_path);
    char *slash = strrchr(base_dir, '/');
    if (slash) *slash = '\0';
    else snprintf(base_dir, sizeof base_dir, ".");
}

/* url() paths: ~/ is home, relative ones are relative to the theme file; file:// URLs work too */
static void resolve_path(const char *in, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!strncmp(in, "file://", 7)) in += 7;   /* as file managers copy paths: file:///home/... */
    if (in[0] == '~' && in[1] == '/' && home) snprintf(out, n, "%s/%s", home, in + 2);
    else if (in[0] == '/') snprintf(out, n, "%s", in);
    else snprintf(out, n, "%s/%s", base_dir, in);
}

static bool parse_length_pct(struct vcur *v, struct length *out) {
    struct tok *k = peek(v);
    if (!k || k->t != T_NUM) return false;
    if (!strcmp(k->unit, "%")) *out = (struct length){ k->num, true };
    else if (!k->unit[0] || !strcmp(k->unit, "px")) *out = (struct length){ k->num, false };
    else return false;
    v->i++;
    return true;
}

static bool ident_is(struct tok *k, const char *s) { return k && k->t == T_IDENT && !strcasecmp(k->s, s); }

/* ----------------------------------------------------------------- images */

static bool parse_stops(struct vcur *a, struct paint *p) {
    struct tok *k;
    while (!at_end(a)) {
        if (p->nstops == 8) return false;
        struct grad_stop *s = &p->stops[p->nstops];
        if (!parse_color(a, &s->c)) return false;
        s->pos = -1;
        k = peek(a);
        if (k && k->t == T_NUM && strcmp(k->unit, "%") == 0) { s->pos = k->num / 100.0f; a->i++; }
        p->nstops++;
        k = next(a);
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

static bool parse_linear(struct vcur *v, struct paint *p) {
    v->i++;   /* the linear-gradient( token */
    struct vcur a = func_args(v);
    memset(p, 0, sizeof *p);
    p->kind = PAINT_LINEAR;
    p->angle = 180;   /* default: to bottom */
    struct tok *k = peek(&a);
    if (k && k->t == T_NUM && strcmp(k->unit, "deg") == 0) {
        p->angle = k->num;
        a.i++;
        k = next(&a);
        if (!k || k->t != T_COMMA) return false;
    } else if (ident_is(k, "to")) {
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
    return parse_stops(&a, p);
}

/* background-position style: one or two of left/center/right/top/bottom or lengths */
static bool parse_position(struct vcur *v, struct length *x, struct length *y) {
    struct length vals[2];
    int axis[2], n = 0;   /* axis: 0 = horizontal, 1 = vertical, -1 = either */
    struct tok *k;
    while (n < 2 && (k = peek(v)) && (k->t == T_NUM || k->t == T_IDENT)) {
        if (k->t == T_NUM) {
            if (!parse_length_pct(v, &vals[n])) return false;
            axis[n++] = -1;
            continue;
        }
        if (!strcmp(k->s, "left")) { vals[n] = (struct length){ 0, true }; axis[n] = 0; }
        else if (!strcmp(k->s, "right")) { vals[n] = (struct length){ 100, true }; axis[n] = 0; }
        else if (!strcmp(k->s, "top")) { vals[n] = (struct length){ 0, true }; axis[n] = 1; }
        else if (!strcmp(k->s, "bottom")) { vals[n] = (struct length){ 100, true }; axis[n] = 1; }
        else if (!strcmp(k->s, "center")) { vals[n] = (struct length){ 50, true }; axis[n] = -1; }
        else break;
        v->i++;
        n++;
    }
    if (n == 0) return false;
    *x = *y = (struct length){ 50, true };
    if (n == 1) {
        if (axis[0] == 1) *y = vals[0];
        else *x = vals[0];
        return true;
    }
    if (axis[0] == 1 || axis[1] == 0) { *y = vals[0]; *x = vals[1]; }   /* "top left" */
    else { *x = vals[0]; *y = vals[1]; }
    return true;
}

static bool parse_radial(struct vcur *v, struct paint *p) {
    v->i++;   /* the radial-gradient( token */
    struct vcur a = func_args(v);
    memset(p, 0, sizeof *p);
    p->kind = PAINT_RADIAL;
    p->extent = EXTENT_FARTHEST_CORNER;
    p->cx = p->cy = (struct length){ 50, true };
    struct tok *k = peek(&a);
    struct color dummy;
    int save = a.i;
    bool first_is_color = parse_color(&a, &dummy);
    a.i = save;
    if (!first_is_color) {   /* [shape] [extent] [at position] , */
        while ((k = peek(&a)) && k->t != T_COMMA) {
            if (ident_is(k, "circle")) p->circle = true;
            else if (ident_is(k, "ellipse")) p->circle = false;
            else if (ident_is(k, "closest-side")) p->extent = EXTENT_CLOSEST_SIDE;
            else if (ident_is(k, "farthest-side")) p->extent = EXTENT_FARTHEST_SIDE;
            else if (ident_is(k, "closest-corner")) p->extent = EXTENT_CLOSEST_CORNER;
            else if (ident_is(k, "farthest-corner")) p->extent = EXTENT_FARTHEST_CORNER;
            else if (ident_is(k, "at")) {
                a.i++;
                if (!parse_position(&a, &p->cx, &p->cy)) return false;
                continue;
            } else return false;
            a.i++;
        }
        if (!next(&a)) return false;   /* the comma */
    }
    return parse_stops(&a, p);
}

/* the declaration being parsed, for reporting a background image that won't load */
static struct parser *decl_ps;
static int decl_line;

/* background-image: none, a gradient, or url(file.png / .jpg) */
static bool parse_image(struct vcur *v, struct paint *p) {
    struct tok *k = peek(v);
    if (!k) return false;
    memset(p, 0, sizeof *p);
    if (ident_is(k, "none")) { v->i++; return true; }
    if (k->t == T_FUNC && !strcasecmp(k->s, "linear-gradient")) return parse_linear(v, p);
    if (k->t == T_FUNC && !strcasecmp(k->s, "radial-gradient")) return parse_radial(v, p);
    if (k->t == T_URL) {
        char path[768];
        resolve_path(k->s, path, sizeof path);
        v->i++;
        p->image = image_load(path);
        p->kind = p->image ? PAINT_IMAGE : PAINT_NONE;
        if (!p->image && decl_ps) {
            char what[1024];
            snprintf(what, sizeof what, "%s: %s", path, image_error());
            err(decl_ps, decl_line, "background image", what);
        }
        return true;
    }
    return false;
}

static bool parse_bg_size(struct vcur *v, int *mode, struct length *w, struct length *h) {
    struct tok *k = peek(v);
    if (ident_is(k, "cover") || ident_is(k, "contain")) {
        *mode = ident_is(k, "cover") ? SIZE_COVER : SIZE_CONTAIN;
        v->i++;
        return true;
    }
    struct length l[2];
    int n = 0;
    while (n < 2 && (k = peek(v)) && (k->t == T_NUM || ident_is(k, "auto"))) {
        if (k->t == T_IDENT) { l[n++] = (struct length){ -1, false }; v->i++; }
        else if (!parse_length_pct(v, &l[n++])) return false;
    }
    if (n == 0) return false;
    *mode = SIZE_LENGTH;
    *w = l[0];
    *h = n > 1 ? l[1] : (struct length){ -1, false };
    if (w->v < 0 && h->v < 0) *mode = SIZE_AUTO;
    return true;
}

static bool parse_repeat(struct vcur *v, bool *x, bool *y) {
    struct tok *k = peek(v);
    if (ident_is(k, "repeat")) *x = *y = true;
    else if (ident_is(k, "no-repeat")) *x = *y = false;
    else if (ident_is(k, "repeat-x")) { *x = true; *y = false; }
    else if (ident_is(k, "repeat-y")) { *x = false; *y = true; }
    else return false;
    v->i++;
    return true;
}

/* ------------------------------------------------------------------ misc */

static bool parse_border_width(struct vcur *v, int *out) {
    struct tok *k = peek(v);
    if (ident_is(k, "thin")) { *out = 1; v->i++; return true; }
    if (ident_is(k, "medium")) { *out = 3; v->i++; return true; }
    if (ident_is(k, "thick")) { *out = 5; v->i++; return true; }
    return parse_length(v, out);
}

/* a border-style keyword, as enum border_style */
static bool border_style_ident(struct tok *k, int *style) {
    static const char *const styles[] = { "none", "solid", "dashed", "dotted", "double", "groove", "ridge",
                                          "inset", "outset" };
    if (ident_is(k, "hidden")) { *style = BORDER_NONE; return true; }
    for (size_t i = 0; i < ARRAY_LEN(styles); i++)
        if (ident_is(k, styles[i])) { *style = (int)i; return true; }
    return false;
}

static bool parse_time(struct tok *k, int *ms) {
    if (!k || k->t != T_NUM) return false;
    if (!strcmp(k->unit, "ms")) *ms = (int)lround(k->num);
    else if (!strcmp(k->unit, "s")) *ms = (int)lround(k->num * 1000);
    else return false;
    return true;
}

static bool easing_ident(struct tok *k, float b[4]) {
    static const struct { const char *name; float b[4]; } curves[] = {
        { "linear", { 0, 0, 1, 1 } },      { "ease", { 0.25f, 0.1f, 0.25f, 1 } },
        { "ease-in", { 0.42f, 0, 1, 1 } }, { "ease-out", { 0, 0, 0.58f, 1 } },
        { "ease-in-out", { 0.42f, 0, 0.58f, 1 } },
    };
    for (size_t i = 0; i < ARRAY_LEN(curves); i++)
        if (ident_is(k, curves[i].name)) { memcpy(b, curves[i].b, sizeof curves[i].b); return true; }
    return false;
}

/* transition: [property] [duration] [easing] [delay], ... */
static bool parse_transition(struct vcur *v, struct decl *d) {
    d->v.trans.n = 0;
    if (ident_is(peek(v), "none")) { v->i++; return true; }
    while (!at_end(v)) {
        if (d->v.trans.n == 6) return false;
        struct transition *t = &d->v.trans.t[d->v.trans.n++];
        *t = (struct transition){ "all", 0, 0, { 0.25f, 0.1f, 0.25f, 1 } };
        bool have_duration = false;
        struct tok *k;
        while ((k = peek(v)) && k->t != T_COMMA) {
            int ms;
            if (parse_time(k, &ms)) {
                if (!have_duration) t->duration_ms = ms;
                else t->delay_ms = ms;
                have_duration = true;
                v->i++;
            } else if (easing_ident(k, t->bezier)) {
                v->i++;
            } else if (k->t == T_FUNC && !strcasecmp(k->s, "cubic-bezier")) {
                v->i++;
                struct vcur a = func_args(v);
                for (int j = 0; j < 4; j++) {
                    struct tok *n = next(&a);
                    if (!n || n->t != T_NUM) return false;
                    t->bezier[j] = n->num;
                    if (j < 3 && (!(n = next(&a)) || n->t != T_COMMA)) return false;
                }
            } else if (k->t == T_IDENT) {
                snprintf(t->prop, sizeof t->prop, "%s", k->s);
                v->i++;
            } else {
                return false;
            }
        }
        if (peek(v)) v->i++;   /* the comma */
    }
    return true;
}

static void append(char *out, size_t n, const char *s) {
    size_t len = strlen(out);
    if (len + 1 < n) snprintf(out + len, n - len, "%s", s);
}

/* font-family: a comma-separated list of names (quoted, or bare words) */
static bool parse_family_list(struct vcur *v, char *out, size_t n) {
    out[0] = '\0';
    bool need_space = false;
    struct tok *k;
    while ((k = next(v))) {
        if (k->t == T_COMMA) { append(out, n, ","); need_space = false; continue; }
        if (k->t != T_IDENT && k->t != T_STR) return false;
        if (need_space) append(out, n, " ");   /* bare words join: font-family: DejaVu Sans */
        append(out, n, k->s);
        need_space = k->t == T_IDENT;
    }
    return out[0] != '\0';
}

static int side_index(const char *s) {
    static const char *const names[4] = { "top", "right", "bottom", "left" };
    for (int i = 0; i < 4; i++) if (!strcmp(s, names[i])) return i;
    return -1;
}

static int corner_index(const char *s) {
    static const char *const names[4] = { "top-left", "top-right", "bottom-right", "bottom-left" };
    for (int i = 0; i < 4; i++) if (!strcmp(s, names[i])) return i;
    return -1;
}

/* "border-left-color" -> 3 (left); -1 if the name isn't border-<side>-width/style/color */
static int border_side_part(const char *name) {
    if (strncmp(name, "border-", 7)) return -1;
    const char *dash = strrchr(name, '-');
    if (dash <= name + 7 || (strcmp(dash, "-width") && strcmp(dash, "-style") && strcmp(dash, "-color"))) return -1;
    char side[16];
    snprintf(side, sizeof side, "%.*s", (int)(dash - (name + 7)), name + 7);
    return side_index(side);
}

/* the CSS 1-4 value rule: top/all, right/left, bottom, left */
static int box_index(int n, int side) {
    static const int map[5][4] = { { 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    return map[n][side];
}

/* ------------------------------------------------------------- declarations */

#define MAXD 16
#define ADD(...) do { if (nd < MAXD) d[nd++] = (struct decl){ __VA_ARGS__ }; } while (0)
#define ADD_DECL(x) do { if (nd < MAXD) d[nd++] = (x); } while (0)

/* Parse the value tokens for one property, appending typed declarations to the rule. */
static bool parse_decl(struct parser *ps, const char *name, struct vcur *v, struct rule *r, int line) {
    decl_ps = ps;
    decl_line = line;
    struct decl d[MAXD];
    int nd = 0;
    struct tok *k;
    memset(d, 0, sizeof d);
    int idx;

    if (!strcmp(name, "background")) {
        struct paint col = { .kind = PAINT_NONE }, img = { .kind = PAINT_NONE };
        struct length px = { 0, true }, py = { 0, true }, sw = { -1, false }, sh = { -1, false };
        int mode = SIZE_AUTO;
        bool rx = true, ry = true;
        while ((k = peek(v))) {
            if (k->t == T_URL || ident_is(k, "none") ||
                (k->t == T_FUNC && (!strcasecmp(k->s, "linear-gradient") || !strcasecmp(k->s, "radial-gradient")))) {
                if (!parse_image(v, &img)) goto bad;
            } else if (k->t == T_DELIM && k->s[0] == '/') {
                v->i++;
                if (!parse_bg_size(v, &mode, &sw, &sh)) goto bad;
            } else if (parse_repeat(v, &rx, &ry)) {
            } else if (k->t == T_NUM || ident_is(k, "left") || ident_is(k, "right") || ident_is(k, "top") ||
                       ident_is(k, "bottom") || ident_is(k, "center")) {
                if (!parse_position(v, &px, &py)) goto bad;
            } else {
                col.kind = PAINT_COLOR;
                if (!parse_color(v, &col.color)) goto bad;
            }
        }
        ADD(.prop = P_BG_COLOR, .v.paint = col);
        ADD(.prop = P_BG_IMAGE, .v.paint = img);
        ADD(.prop = P_BG_POS, .v.pos = { px, py });
        ADD(.prop = P_BG_SIZE, .v.size = { mode, sw, sh });
        ADD(.prop = P_BG_REPEAT, .v.repeat = { rx, ry });
    } else if (!strcmp(name, "background-color")) {
        struct paint col = { .kind = PAINT_COLOR };
        if (!parse_color(v, &col.color)) goto bad;
        ADD(.prop = P_BG_COLOR, .v.paint = col);
    } else if (!strcmp(name, "background-image")) {
        struct paint img;
        if (!parse_image(v, &img)) goto bad;
        ADD(.prop = P_BG_IMAGE, .v.paint = img);
    } else if (!strcmp(name, "background-size")) {
        struct decl x = { .prop = P_BG_SIZE };
        if (!parse_bg_size(v, &x.v.size.mode, &x.v.size.w, &x.v.size.h)) goto bad;
        ADD_DECL(x);
    } else if (!strcmp(name, "background-position")) {
        struct decl x = { .prop = P_BG_POS };
        if (!parse_position(v, &x.v.pos.x, &x.v.pos.y)) goto bad;
        ADD_DECL(x);
    } else if (!strcmp(name, "background-repeat")) {
        struct decl x = { .prop = P_BG_REPEAT };
        if (!parse_repeat(v, &x.v.repeat.x, &x.v.repeat.y)) goto bad;
        ADD_DECL(x);
    } else if (!strcmp(name, "color")) {
        struct color c;
        if (!parse_color(v, &c)) goto bad;
        ADD(.prop = P_COLOR, .v.color = c);
    } else if (!strcmp(name, "border") || (!strncmp(name, "border-", 7) && side_index(name + 7) >= 0)) {
        /* border / border-<side>: <width> || <style> || <color>; unset parts reset */
        int w = 3, bstyle = BORDER_NONE;
        struct paint col = { .kind = PAINT_NONE };   /* none = currentColor */
        while ((k = peek(v))) {
            if (k->t == T_NUM || ident_is(k, "thin") || ident_is(k, "medium") || ident_is(k, "thick")) {
                if (!parse_border_width(v, &w)) goto bad;
            } else if (border_style_ident(k, &bstyle)) {
                v->i++;
            } else {
                col.kind = PAINT_COLOR;
                if (!parse_color(v, &col.color)) goto bad;
            }
        }
        int only = name[6] ? side_index(name + 7) : -1;
        for (int s = 0; s < 4; s++) {
            if (only >= 0 && s != only) continue;
            ADD(.prop = P_BORDER_WIDTH, .idx = s, .v.len = w);
            ADD(.prop = P_BORDER_STYLE, .idx = s, .v.len = bstyle);
            ADD(.prop = P_BORDER_COLOR, .idx = s, .v.paint = col);
        }
    } else if (!strcmp(name, "border-width") || !strcmp(name, "border-style") || !strcmp(name, "border-color")) {
        char kind = name[7];   /* w, s or c */
        struct decl vals[4];
        int n = 0;
        while (!at_end(v) && n < 4) {
            vals[n] = (struct decl){ 0 };
            if (kind == 'w') { if (!parse_border_width(v, &vals[n].v.len)) goto bad; }
            else if (kind == 's') { if (!border_style_ident(peek(v), &vals[n].v.len)) goto bad; v->i++; }
            else { vals[n].v.paint.kind = PAINT_COLOR; if (!parse_color(v, &vals[n].v.paint.color)) goto bad; }
            n++;
        }
        if (n == 0) goto bad;
        enum prop p = kind == 'w' ? P_BORDER_WIDTH : kind == 's' ? P_BORDER_STYLE : P_BORDER_COLOR;
        for (int s = 0; s < 4; s++) {
            struct decl x = vals[box_index(n, s)];
            x.prop = p;
            x.idx = s;
            ADD_DECL(x);
        }
    } else if ((idx = border_side_part(name)) >= 0) {
        /* border-<side>-width / -style / -color */
        int s = idx;
        const char *what = strrchr(name, '-') + 1;
        struct decl x = { .idx = s };
        if (*what == 'w') { x.prop = P_BORDER_WIDTH; if (!parse_border_width(v, &x.v.len)) goto bad; }
        else if (*what == 's') { x.prop = P_BORDER_STYLE; if (!border_style_ident(peek(v), &x.v.len)) goto bad; v->i++; }
        else { x.prop = P_BORDER_COLOR; x.v.paint.kind = PAINT_COLOR; if (!parse_color(v, &x.v.paint.color)) goto bad; }
        ADD_DECL(x);
    } else if (!strcmp(name, "border-radius")) {
        struct length vals[4];
        int n = 0;
        while (n < 4 && (k = peek(v)) && k->t == T_NUM)
            if (!parse_length_pct(v, &vals[n++])) goto bad;
        if (n == 0) goto bad;
        if (peek(v)) {
            err(ps, line, "elliptical corners (\"/ ...\") aren't supported; using the first radii in", name);
            while (peek(v)) v->i++;
        }
        static const int corner_map[5][4] = { { 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
        for (int c = 0; c < 4; c++) ADD(.prop = P_RADIUS, .idx = c, .v.l = vals[corner_map[n][c]]);
    } else if (!strncmp(name, "border-", 7) && strstr(name, "-radius")) {
        char corner[24];
        snprintf(corner, sizeof corner, "%.*s", (int)(strstr(name, "-radius") - (name + 7)), name + 7);
        int c = corner_index(corner);
        if (c < 0) goto unknown;
        struct length l;
        if (!parse_length_pct(v, &l)) goto bad;
        while (peek(v)) v->i++;
        ADD(.prop = P_RADIUS, .idx = c, .v.l = l);
    } else if (!strcmp(name, "padding") || !strcmp(name, "margin")) {
        enum prop p = name[0] == 'p' ? P_PADDING : P_MARGIN;
        int vals[4], n = 0;
        while (!at_end(v) && n < 4) if (!parse_length(v, &vals[n++])) goto bad;
        if (n == 0) goto bad;
        for (int s = 0; s < 4; s++) ADD(.prop = p, .idx = s, .v.len = vals[box_index(n, s)]);
    } else if ((!strncmp(name, "padding-", 8) && side_index(name + 8) >= 0) ||
               (!strncmp(name, "margin-", 7) && side_index(name + 7) >= 0)) {
        bool pad = name[0] == 'p';
        int len;
        if (!parse_length(v, &len)) goto bad;
        ADD(.prop = pad ? P_PADDING : P_MARGIN, .idx = side_index(name + (pad ? 8 : 7)), .v.len = len);
    } else if (!strcmp(name, "width") || !strcmp(name, "height")) {
        int len = -1;
        if (ident_is(peek(v), "auto")) v->i++;
        else if (!parse_length(v, &len)) goto bad;
        ADD(.prop = name[0] == 'w' ? P_WIDTH : P_HEIGHT, .v.len = len);
    } else if (!strcmp(name, "box-shadow")) {
        struct decl x = { .prop = P_BOX_SHADOW };
        if (ident_is(peek(v), "none")) v->i++;
        else {
            int lens[4], n = 0;
            struct color c = { 0, 0, 0, 128 };
            while ((k = peek(v)) && k->t != T_COMMA) {
                if (k->t == T_NUM) { if (n == 4 || !parse_length(v, &lens[n++])) goto bad; }
                else if (!parse_color(v, &c)) goto bad;
            }
            if (n < 2) goto bad;
            if (!at_end(v)) {
                err(ps, line, "only the first of several shadows is drawn in", name);
                while (!at_end(v)) v->i++;
            }
            x.v.shadow.on = true;
            x.v.shadow.x = lens[0];
            x.v.shadow.y = lens[1];
            x.v.shadow.blur = n > 2 ? MAX(lens[2], 0) : 0;
            x.v.shadow.spread = n > 3 ? lens[3] : 0;
            x.v.shadow.c = c;
        }
        ADD_DECL(x);
    } else if (!strcmp(name, "opacity") || !strcmp(name, "tile-ratio")) {
        k = next(v);
        if (!k || k->t != T_NUM) goto bad;
        float f = !strcmp(k->unit, "%") ? k->num / 100 : k->num;
        ADD(.prop = name[0] == 'o' ? P_OPACITY : P_TILE_RATIO, .v.num = name[0] == 'o' ? CLAMP(f, 0, 1) : CLAMP(f, 0.1f, 0.9f));
    } else if (!strcmp(name, "font-family")) {
        struct decl x = { .prop = P_FONT_FAMILY };
        if (!parse_family_list(v, x.v.str, sizeof x.v.str)) goto bad;
        ADD_DECL(x);
    } else if (!strcmp(name, "font-size") || !strcmp(name, "gap")) {
        int len;
        if (!parse_length(v, &len)) goto bad;
        ADD(.prop = name[0] == 'f' ? P_FONT_SIZE : P_GAP, .v.len = MAX(len, name[0] == 'f' ? 1 : 0));
    } else if (!strcmp(name, "font-weight")) {
        k = next(v);
        int w;
        if (ident_is(k, "normal")) w = 400;
        else if (ident_is(k, "bold")) w = 700;
        else if (ident_is(k, "lighter")) w = 300;
        else if (ident_is(k, "bolder")) w = 800;
        else if (k && k->t == T_NUM && !k->unit[0]) w = CLAMP((int)k->num, 1, 1000);
        else goto bad;
        ADD(.prop = P_FONT_WEIGHT, .v.len = w);
    } else if (!strcmp(name, "font-style")) {
        k = next(v);
        if (!ident_is(k, "normal") && !ident_is(k, "italic") && !ident_is(k, "oblique")) goto bad;
        ADD(.prop = P_FONT_STYLE, .v.flag = !ident_is(k, "normal"));
    } else if (!strcmp(name, "text-align")) {
        k = next(v);
        int a;
        if (ident_is(k, "left") || ident_is(k, "start")) a = ALIGN_LEFT;
        else if (ident_is(k, "center")) a = ALIGN_CENTER;
        else if (ident_is(k, "right") || ident_is(k, "end")) a = ALIGN_RIGHT;
        else goto bad;
        ADD(.prop = P_TEXT_ALIGN, .v.len = a);
    } else if (!strcmp(name, "display")) {
        k = next(v);
        if (!k || k->t != T_IDENT) goto bad;
        ADD(.prop = P_DISPLAY, .v.flag = ident_is(k, "none"));
    } else if (!strcmp(name, "float")) {
        k = next(v);
        if (!ident_is(k, "left") && !ident_is(k, "right")) goto bad;
        ADD(.prop = P_FLOAT, .v.flag = ident_is(k, "left"));
    } else if (!strcmp(name, "transition")) {
        struct decl x = { .prop = P_TRANSITION };
        if (!parse_transition(v, &x)) goto bad;
        ADD_DECL(x);
    } else if (!strcmp(name, "side")) {
        k = next(v);
        int s = k && k->t == T_IDENT ? side_index(k->s) : -1;
        if (s < 0) goto bad;
        ADD(.prop = P_SIDE, .v.len = s);
    } else if (!strcmp(name, "window-layout")) {
        static const char *const layouts[] = { "floating", "tile", "columns", "rows", "grid", "monocle" };
        k = next(v);
        int l = -1;
        for (int i = 0; i < 6; i++) if (ident_is(k, layouts[i])) l = i;
        if (l < 0) goto bad;
        ADD(.prop = P_WINDOW_LAYOUT, .v.len = l);
    } else if (!strcmp(name, "tile")) {
        k = next(v);
        if (!k || k->t != T_IDENT) goto bad;
        ADD(.prop = P_TILE, .v.flag = !(ident_is(k, "no") || ident_is(k, "none") || ident_is(k, "false")));
    } else {
        goto unknown;
    }
    if (!at_end(v)) goto bad;
    r->decls = realloc(r->decls, (r->ndecls + nd) * sizeof *r->decls);
    memcpy(r->decls + r->ndecls, d, nd * sizeof *d);
    r->ndecls += nd;
    return true;
unknown:
    err(ps, line, "unknown property", name);
    return false;
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
    case T_URL: {   /* gradients pass through; file paths are made absolute */
        char path[768];
        if (strchr(t->s, '(')) snprintf(path, sizeof path, "%s", t->s);
        else resolve_path(t->s, path, sizeof path);
        sb_puts(b, "url(");
        sb_quoted(b, path);
        sb_puts(b, ")");
        break;
    }
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

/* The properties libcss doesn't know. Each gets its own little stylesheet in which every
 * rule that sets it becomes `<same selector> { content: "<value>" }`; selecting against
 * that sheet makes libcss resolve the cascade for just that property, and the winning
 * value comes back as the content string. */
const char *const css_ext_props[CSS_NEXT] = {
    "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
    "border-bottom-left-radius", "box-shadow", "transition", "background-size", "side",
    "window-layout", "gap", "tile-ratio", "tile",
};

static int ext_index(const char *name) {
    for (int i = 0; i < CSS_NEXT; i++) if (!strcmp(css_ext_props[i], name)) return i;
    return -1;
}

struct rewrite { struct strbuf main, ext[CSS_NEXT]; };

static void emit_ext(struct rewrite *rw, int i, const char *sel, const char *value, bool important) {
    struct strbuf *b = &rw->ext[i];
    sb_puts(b, sel);
    sb_puts(b, " { content: ");
    sb_quoted(b, value);
    if (important) sb_puts(b, " !important");
    sb_puts(b, "; }\n");
}

static char *text_of(const struct tok *t, int start, int end) {
    struct strbuf b = { 0 };
    sb_puts(&b, "");
    emit_range(&b, t, start, end);
    char *s = b.s;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
    for (size_t n = strlen(s); n && s[n - 1] == ' '; n--) s[n - 1] = '\0';
    return s;
}

/* one declaration (var() already expanded) into the main sheet or an extension sheet */
static void emit_decl(struct rewrite *rw, const char *sel, const char *name, struct toks *v, bool important) {
    int e = ext_index(name);
    if (e >= 0) {
        char *text = text_of(v->v, 0, v->n);
        emit_ext(rw, e, sel, text, important);
        free(text);
        return;
    }
    if (!strcmp(name, "border-radius")) {   /* expand to the four corners: 1-4 values */
        int idx[4], n = 0;
        for (int i = 0; i < v->n && n < 4; i++) {
            if (v->v[i].t == T_DELIM && v->v[i].s[0] == '/') break;
            if (v->v[i].t != T_WS) idx[n++] = i;
        }
        static const int corner_map[5][4] = { { 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
        for (int c = 0; c < 4 && n > 0; c++) {
            int t = idx[corner_map[n][c]];
            char *text = text_of(v->v, t, t + 1);
            emit_ext(rw, c, sel, text, important);
            free(text);
        }
        return;
    }
    bool bg = !strcmp(name, "background");
    if (bg || !strcmp(name, "background-image")) {
        /* gradients travel as url("linear-gradient(...)"), which libcss stores verbatim */
        for (int i = 0; i < v->n; i++) {
            if (v->v[i].t != T_FUNC || (strcmp(v->v[i].s, "linear-gradient") && strcmp(v->v[i].s, "radial-gradient")))
                continue;
            int depth = 0, j = i;
            for (; j < v->n; j++) {
                if (v->v[j].t == T_FUNC || v->v[j].t == T_LPAREN) depth++;
                if (v->v[j].t == T_RPAREN && --depth == 0) break;
            }
            char *text = text_of(v->v, i, MIN(j + 1, v->n));
            struct tok url = { .t = T_URL };
            snprintf(url.s, sizeof url.s, "%s", text);
            free(text);
            v->v[i] = url;
            memmove(&v->v[i + 1], &v->v[j + 1], (v->n - j - 1) * sizeof *v->v);
            v->n -= j - i;
        }
    }
    if (bg) {   /* CSS 2.1 has no "/ size" in the shorthand: pull it out into background-size */
        char *size = NULL;
        for (int i = 0; i < v->n; i++) {
            if (v->v[i].t != T_DELIM || v->v[i].s[0] != '/') continue;
            int j = i + 1, taken = 0;
            while (j < v->n && taken < 2) {
                if (v->v[j].t == T_WS) { j++; continue; }
                if (v->v[j].t == T_NUM || ident_is(&v->v[j], "cover") || ident_is(&v->v[j], "contain") ||
                    ident_is(&v->v[j], "auto")) { j++; taken++; }
                else break;
            }
            size = text_of(v->v, i + 1, j);
            memmove(&v->v[i], &v->v[j], (v->n - j) * sizeof *v->v);
            v->n -= j - i;
            break;
        }
        emit_ext(rw, ext_index("background-size"), sel, size ? size : "auto", important);
        free(size);
    }
    sb_puts(&rw->main, name);
    sb_puts(&rw->main, ":");
    emit_range(&rw->main, v->v, 0, v->n);
    if (important) sb_puts(&rw->main, " !important");
    sb_puts(&rw->main, ";\n");
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
/* ------------------------------------------------ what applies where */

/* Which properties each element actually uses, so a theme setting one that does
 * nothing there hears about it instead of silently seeing no change. Text
 * properties count on elements whose children inherit them. */
enum { A_BG = 1 << 0, A_BORDER = 1 << 1, A_RADIUS = 1 << 2, A_SHADOW = 1 << 3, A_OPACITY = 1 << 4,
       A_PADDING = 1 << 5, A_MARGIN = 1 << 6, A_WIDTH = 1 << 7, A_HEIGHT = 1 << 8, A_DISPLAY = 1 << 9,
       A_FLOAT = 1 << 10, A_TRANSITION = 1 << 11, A_COLOR = 1 << 12, A_FONT = 1 << 13, A_ALIGN = 1 << 14,
       A_SIDE = 1 << 15, A_LAYOUT = 1 << 16, A_TILE = 1 << 17 };
#define A_BOX (A_BG | A_BORDER | A_RADIUS | A_SHADOW | A_OPACITY | A_TRANSITION)

static const struct { const char *name; unsigned props; const char *why; } elements[] = {
    { "window", A_BOX | A_TILE | A_COLOR | A_FONT | A_ALIGN, "(the app decides its window's size and contents)" },
    { "titlebar", A_BOX | A_PADDING | A_WIDTH | A_HEIGHT | A_DISPLAY | A_SIDE | A_COLOR | A_FONT | A_ALIGN, NULL },
    { "title", A_BOX | A_PADDING | A_DISPLAY | A_COLOR | A_FONT | A_ALIGN, NULL },
    { "button", A_BOX | A_MARGIN | A_WIDTH | A_HEIGHT | A_DISPLAY | A_FLOAT | A_COLOR, NULL },
    { "desktop", A_BG | A_PADDING | A_LAYOUT, NULL },
    { "popup", A_BORDER | A_RADIUS | A_SHADOW | A_OPACITY | A_TRANSITION, "(the app draws its menus' contents)" },
    { "cursor", A_COLOR | A_BORDER | A_HEIGHT, "(color fills it, border-color outlines it, height scales it)" },
    { "menu", A_BOX | A_PADDING | A_WIDTH | A_COLOR | A_FONT, NULL },
    { "menuitem", A_BOX | A_PADDING | A_HEIGHT | A_DISPLAY | A_COLOR | A_FONT, NULL },
    { "heading", A_BOX | A_PADDING | A_HEIGHT | A_DISPLAY | A_COLOR | A_FONT, NULL },
    { "separator", A_BOX | A_MARGIN | A_HEIGHT | A_DISPLAY, NULL },
};

static unsigned prop_group(const char *n) {
    if (!strncmp(n, "background", 10)) return A_BG;
    if (strstr(n, "radius")) return A_RADIUS;
    if (!strncmp(n, "border", 6)) return A_BORDER;
    if (!strcmp(n, "box-shadow")) return A_SHADOW;
    if (!strcmp(n, "opacity")) return A_OPACITY;
    if (!strncmp(n, "padding", 7)) return A_PADDING;
    if (!strncmp(n, "margin", 6)) return A_MARGIN;
    if (!strcmp(n, "width")) return A_WIDTH;
    if (!strcmp(n, "height")) return A_HEIGHT;
    if (!strcmp(n, "display")) return A_DISPLAY;
    if (!strcmp(n, "float")) return A_FLOAT;
    if (!strncmp(n, "transition", 10)) return A_TRANSITION;
    if (!strcmp(n, "color")) return A_COLOR;
    if (!strncmp(n, "font", 4)) return A_FONT;
    if (!strcmp(n, "text-align")) return A_ALIGN;
    if (!strcmp(n, "side")) return A_SIDE;
    if (!strcmp(n, "window-layout") || !strcmp(n, "gap") || !strcmp(n, "tile-ratio")) return A_LAYOUT;
    if (!strcmp(n, "tile")) return A_TILE;
    return 0;   /* not a property pane knows: parse_decl says so */
}

static int element_index(const char *name) {
    for (size_t i = 0; i < ARRAY_LEN(elements); i++) if (!strcmp(elements[i].name, name)) return (int)i;
    return -1;
}

/* The elements a rule styles: each selector's last compound's type name. -1 in
 * `out` for a selector that doesn't name one (".work", "*", ":hover"). Reports
 * type names pane has no element for. */
static int rule_subjects(struct parser *ps, const struct tok *t, int start, int end, int *out, int max) {
    int n = 0, s = start, depth = 0;
    for (int j = start; j <= end && n < max; j++) {
        if (j < end) {
            if (t[j].t == T_FUNC || t[j].t == T_LPAREN) depth++;
            if (t[j].t == T_RPAREN && depth > 0) depth--;
            if (depth > 0 || t[j].t != T_COMMA) continue;
        }
        /* one selector: t[s..j), without the spaces around it */
        int e = j;
        while (e > s && t[e - 1].t == T_WS) e--;
        int compound = s, d = 0;
        for (int i = s; i < e; i++) {
            if (t[i].t == T_FUNC || t[i].t == T_LPAREN) d++;
            if (t[i].t == T_RPAREN && d > 0) d--;
            if (d == 0 && (t[i].t == T_WS || (t[i].t == T_DELIM && t[i].s[0] == '>'))) compound = i + 1;
        }
        while (compound < e && t[compound].t == T_WS) compound++;
        int el = -1;
        if (compound < e && t[compound].t == T_IDENT) {
            el = element_index(t[compound].s);
            if (el < 0) {
                err(ps, t[compound].line, "pane has no element called", t[compound].s);
                el = -2;   /* reported: don't also check its properties */
            }
        }
        out[n++] = el;
        s = j + 1;
    }
    return n;
}

/* Report a declaration that does nothing on any element its rule styles. */
static void check_applies(struct parser *ps, const char *name, int line, const int *subj, int nsubj) {
    unsigned g = prop_group(name);
    if (!g) return;
#ifndef PANE_FREETYPE
    if (g == A_FONT && strcmp(name, "font-family") && !said_freetype) {
        err(ps, line, "pane was built without FreeType, so console font sizes only change in whole multiples:",
            name);
        said_freetype = true;
    }
#endif
    for (int i = 0; i < nsubj; i++) {
        if (subj[i] < 0 || (elements[subj[i]].props & g)) continue;
        char what[160];
        snprintf(what, sizeof what, "%s%s%s", elements[subj[i]].name, elements[subj[i]].why ? " " : "",
                 elements[subj[i]].why ? elements[subj[i]].why : "");
        char msg[64];
        snprintf(msg, sizeof msg, "%s has no effect on", name);
        err(ps, line, msg, what);
    }
}

static int parse_sheet(const char *text, const char *file, struct sheet *out, struct rewrite *rewrite) {
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
        int subj[16], nsubj = rule_subjects(&ps, t, pre_start[r], pre_end[r], subj, 16);
        struct strbuf sel = { 0 };
        if (rewrite) {
            sb_puts(&sel, "");
            emit_selector(&sel, t, pre_start[r], pre_end[r]);
            sb_puts(&rewrite->main, sel.s);
            sb_puts(&rewrite->main, " {\n");
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
                    /* libcss silently drops what it doesn't like, and accepts properties
                     * pane doesn't draw: check everything with pane's own parser */
                    struct rule scratch = { 0 };
                    parse_decl(&ps, rd->name, &v, &scratch, line);
                    free(scratch.decls);
                }
                check_applies(&ps, rd->name, line, subj, nsubj);
                if (rewrite) emit_decl(rewrite, sel.s, rd->name, &expanded, rd->important);
            }
            free(expanded.v);
        }
        if (rewrite) sb_puts(&rewrite->main, "}\n");
        free(sel.s);
    }
    free(pre_start);
    free(pre_end);
    free(ps.raw);
    free(ts.v);
    if (out) *out = sh;
    else free_sheet(&sh);
    return ps.errors;
}

struct css_rewritten css_rewrite_for_libcss(const char *text, const char *file) {
    struct rewrite rw = { 0 };
    sb_puts(&rw.main, "");
    parse_sheet(text, file, NULL, &rw);
    struct css_rewritten out = { rw.main.s, { 0 } };
    for (int i = 0; i < CSS_NEXT; i++) out.ext[i] = rw.ext[i].s;   /* NULL: nothing set it */
    return out;
}

void css_rewritten_free(struct css_rewritten *r) {
    free(r->main);
    for (int i = 0; i < CSS_NEXT; i++) free(r->ext[i]);
    memset(r, 0, sizeof *r);
}

/* -------------------------------------------------------------- computed style */

void style_init(struct style *s, const struct style *parent) {
    memset(s, 0, sizeof *s);
    /* initial values; color, font-* and text-align inherit */
    s->color = parent ? parent->color : (struct color){ 255, 255, 255, 255 };
    snprintf(s->font_family, sizeof s->font_family, "%s", parent ? parent->font_family : "Lat2-Terminus16");
    s->font_size = parent ? parent->font_size : 16;
    s->font_weight = parent ? parent->font_weight : 400;
    s->font_italic = parent ? parent->font_italic : false;
    s->text_align = parent ? parent->text_align : ALIGN_LEFT;
    for (int i = 0; i < 4; i++) s->border.w[i] = 3;   /* "medium", drawn only with a style */
    s->bg_pos_x = s->bg_pos_y = (struct length){ 0, true };
    s->bg_repeat_x = s->bg_repeat_y = true;
    s->width = s->height = -1;
    s->opacity = 1;
    s->side = TOP;
    s->tile_ratio = 0.55f;
    s->tile = true;
}

void style_finish(struct style *s) {
    for (int i = 0; i < 4; i++)
        if (!s->border_color_set[i]) s->border.c[i] = s->color;   /* currentColor */
}

static void apply_decl(struct style *s, const struct decl *d) {
    switch (d->prop) {
    case P_BG_COLOR: s->background = d->v.paint; break;
    case P_BG_IMAGE: s->background_image = d->v.paint; break;
    case P_BG_SIZE: s->bg_size_mode = d->v.size.mode; s->bg_size_w = d->v.size.w; s->bg_size_h = d->v.size.h; break;
    case P_BG_POS: s->bg_pos_x = d->v.pos.x; s->bg_pos_y = d->v.pos.y; break;
    case P_BG_REPEAT: s->bg_repeat_x = d->v.repeat.x; s->bg_repeat_y = d->v.repeat.y; break;
    case P_COLOR: s->color = d->v.color; break;
    case P_BORDER_WIDTH: s->border.w[d->idx] = MAX(d->v.len, 0); break;
    case P_BORDER_STYLE: s->border.style[d->idx] = (uint8_t)d->v.len; break;
    case P_BORDER_COLOR:
        s->border_color_set[d->idx] = d->v.paint.kind == PAINT_COLOR;
        s->border.c[d->idx] = d->v.paint.color;
        break;
    case P_RADIUS: s->radius[d->idx] = d->v.l; s->radius[d->idx].v = fmaxf(d->v.l.v, 0); break;
    case P_PADDING: s->padding[d->idx] = d->v.len; break;
    case P_MARGIN: s->margin[d->idx] = d->v.len; break;
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
    case P_FONT_SIZE: s->font_size = d->v.len; break;
    case P_FONT_WEIGHT: s->font_weight = d->v.len; break;
    case P_FONT_STYLE: s->font_italic = d->v.flag; break;
    case P_TEXT_ALIGN: s->text_align = d->v.len; break;
    case P_DISPLAY: s->hidden = d->v.flag; break;
    case P_FLOAT: s->float_left = d->v.flag; break;
    case P_TRANSITION:
        s->ntransitions = d->v.trans.n;
        memcpy(s->transitions, d->v.trans.t, sizeof s->transitions);
        break;
    case P_SIDE: s->side = d->v.len; break;
    case P_WINDOW_LAYOUT: s->window_layout = d->v.len; break;
    case P_GAP: s->gap = d->v.len; break;
    case P_TILE_RATIO: s->tile_ratio = d->v.num; break;
    case P_TILE: s->tile = d->v.flag; break;
    }
}

bool css_apply_property(struct style *s, const char *name, const char *value) {
    struct toks ts = { 0 };
    tokenize(value, &ts);
    struct parser ps = { .ts = &ts, .file = "(theme)", .quiet = true };
    struct vcur v = { ts.v, 0, ts.n - 1 };   /* without the EOF token */
    struct rule r = { 0 };
    bool ok = parse_decl(&ps, name, &v, &r, 0);
    for (int i = 0; i < r.ndecls; i++) apply_decl(s, &r.decls[i]);
    free(r.decls);
    free(ts.v);
    return ok;
}

struct radii style_radii(const struct style *s, int w, int h) {
    float m = MIN(w, h);
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = s->radius[i].pct ? s->radius[i].v / 100.0f * m : s->radius[i].v;
    return (struct radii){ r[0], r[1], r[2], r[3] };
}

struct border style_border(const struct style *s) {
    struct border b = s->border;
    for (int i = 0; i < 4; i++) if (b.style[i] == BORDER_NONE) b.w[i] = 0;
    return b;
}

bool style_has_radius(const struct style *s) {
    for (int i = 0; i < 4; i++) if (s->radius[i].v > 0) return true;
    return false;
}

void style_draw_box(struct canvas *c, struct rect r, struct radii rad, const struct style *s, float opacity) {
    if (s->hidden || opacity <= 0 || r.w <= 0 || r.h <= 0) return;
    if (s->shadow) {
        int sp = s->shadow_spread;
        struct rect sr = { r.x + s->shadow_x - sp, r.y + s->shadow_y - sp, r.w + 2 * sp, r.h + 2 * sp };
        struct radii srad = { rad.tl + sp, rad.tr + sp, rad.br + sp, rad.bl + sp };
        draw_shadow(c, sr, srad, s->shadow_blur, s->shadow_color, opacity, NULL);
    }
    struct border b = style_border(s);
    struct paint layers[2];
    int n = style_layers(s, layers);
    draw_box(c, r, rad, &b, layers, n, opacity, NULL);
}

int style_layers(const struct style *s, struct paint out[2]) {
    int n = 0;
    if (s->background.kind == PAINT_COLOR && s->background.color.a) out[n++] = s->background;
    if (s->background_image.kind != PAINT_NONE) {
        struct paint p = s->background_image;
        p.size_mode = s->bg_size_mode;
        p.size_w = s->bg_size_w;
        p.size_h = s->bg_size_h;
        p.pos_x = s->bg_pos_x;
        p.pos_y = s->bg_pos_y;
        p.repeat_x = s->bg_repeat_x;
        p.repeat_y = s->bg_repeat_y;
        out[n++] = p;
    }
    return n;
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
    css_messages_reset();
    css_window_classes_reset();
    css_set_base_dir(path);
    image_cache_clear();   /* pick up edited images too */
    int errors = parse_sheet(text, path, &sh, NULL);
    free(text);
    /* keep going with whatever parsed: like a browser, bad declarations are just skipped */
    activate(&sh);
    fprintf(stderr, "pane: loaded %s (%d rules%s)\n", path, sh.nrules, errors ? ", with errors" : "");
    return 0;
}

void css_load_string(const char *text, const char *name) {
    struct sheet sh;
    css_messages_reset();
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
    style_finish(s);
}

void css_elem_release(struct css_elem *e) { (void)e; }

const char *css_engine_name(void) { return "built-in"; }

#endif
