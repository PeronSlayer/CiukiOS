/* SPDX-License-Identifier: MIT
 * WordPad layout: see layout.h. All positions are integers; x is tracked in
 * 1/64 pixel so that words measured in 26.6 pixels add up without drift. */
#include "layout.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TAB_DEFAULT 720   /* twips: half an inch, as WordPad */

const struct wp_paper wp_papers[] = {{"Letter", 12240, 15840}, {"Legal", 12240, 20160}, {"A4", 11906, 16838}, {"A5", 8392, 11906}};
const unsigned wp_paper_count = sizeof(wp_papers) / sizeof(*wp_papers);
struct wp_page_setup wp_default_page_setup(int a4)
{
    const struct wp_paper *p = &wp_papers[a4 ? 2 : 0];
    return (struct wp_page_setup){p->w, p->h, 1080, 1080, 1080, 1080, 0};
}
int wp_twips_px(int t) { return (int)(((int64_t)t * 96 + (t < 0 ? -720 : 720)) / 1440); }
static int64_t px64(int32_t twips) { return (int64_t)twips * 64 / 15; }
unsigned wp_size_px64(unsigned hp) { return hp * 128u / 3u; }

void wp_layout_init(struct wp_layout *l) { memset(l, 0, sizeof(*l)); }
void wp_layout_free(struct wp_layout *l) { free(l->lines); free(l->pfirst); free(l->py); memset(l, 0, sizeof(*l)); }

/* ---- atoms: words, runs of spaces, tabs and soft breaks within one run ----- */

struct cursor { const struct wp_doc *d; size_t pos; unsigned ri; size_t roff; };
enum { A_WORD, A_SPACE, A_TAB, A_BREAK };
struct atom { size_t start, end; int kind; struct wp_cattr a; };

static void seek(struct cursor *c, size_t pos)
{
    size_t s = 0; unsigned i = 0;
    while (i < c->d->nruns && pos >= s + c->d->runs[i].len) { s += c->d->runs[i].len; i++; }
    c->pos = pos; c->ri = i; c->roff = pos - s;
}
static struct wp_cattr cur_attr(const struct cursor *c)
{
    if (!c->d->nruns) return wp_default_cattr();
    return c->d->runs[c->ri < c->d->nruns ? c->ri : c->d->nruns - 1].a;
}
static void advance(struct cursor *c, size_t n)
{
    c->pos += n; c->roff += n;
    while (c->ri < c->d->nruns && c->roff >= c->d->runs[c->ri].len) { c->roff -= c->d->runs[c->ri].len; c->ri++; }
}
/* The next atom before `limit`, or 0. */
static int next_atom(struct cursor *c, size_t limit, struct atom *a)
{
    if (c->pos >= limit) return 0;
    const char *t = c->d->text;
    size_t run_left = c->ri < c->d->nruns ? c->d->runs[c->ri].len - c->roff : limit - c->pos;
    size_t max = limit - c->pos < run_left ? limit - c->pos : run_left, n = 1;
    char ch = t[c->pos];
    a->start = c->pos; a->a = cur_attr(c);
    if (ch == '\t') a->kind = A_TAB;
    else if (ch == WP_SOFT_BREAK) a->kind = A_BREAK;
    else if (ch == ' ') { a->kind = A_SPACE; while (n < max && t[c->pos + n] == ' ') n++; }
    else { a->kind = A_WORD; while (n < max && t[c->pos + n] != ' ' && t[c->pos + n] != '\t' && t[c->pos + n] != WP_SOFT_BREAK) n++; }
    a->end = c->pos + n;
    advance(c, n);
    return 1;
}
/* Advance of text[s,e) in 1/64 pixel. Long words are measured in chunks so that the host's 32-bit advance cannot overflow. */
static int64_t measure(const struct wp_text_ops *o, const struct wp_doc *d, const struct wp_cattr *a, size_t s, size_t e)
{
    unsigned fam = a->font < d->nfonts ? d->fonts[a->font].family : WP_SANS;
    int64_t total = 0;
    while (s < e) {
        size_t n = e - s > 1024 ? 1024 : e - s;
        while (s + n < e && ((unsigned char)d->text[s + n] & 0xc0) == 0x80) n++;   /* never split a character */
        total += o->width(o->ctx, fam, a->flags, a->size, d->text + s, n);
        s += n;
    }
    return total;
}
static void vmet(const struct wp_text_ops *o, const struct wp_doc *d, const struct wp_cattr *a, int *as, int *ds)
{
    unsigned fam = a->font < d->nfonts ? d->fonts[a->font].family : WP_SANS;
    o->vmetrics(o->ctx, fam, a->flags, a->size, as, ds);
}
/* Next tab stop after x (1/64 px) for the paragraph: custom stops first, then every half inch. */
static int64_t tab_stop(const struct wp_pattr *p, int64_t x)
{
    for (unsigned i = 0; i < p->ntabs; i++) if (px64(p->tabs[i]) > x) return px64(p->tabs[i]);
    int64_t step = px64(TAB_DEFAULT), start = p->ntabs ? px64(p->tabs[p->ntabs - 1]) : 0;
    if (x < start) x = start;
    return (x / step + 1) * step;
}
static int px_of(int64_t x64) { return (int)((x64 + 32) >> 6); }

/* ---- layout ------------------------------------------------------------------ */

static int add_line(struct wp_layout *l, const struct wp_line *line)
{
    if (l->n == l->cap) {
        unsigned c = l->cap ? l->cap * 2 : 64;
        struct wp_line *q = realloc(l->lines, c * sizeof(*q));
        if (!q) return -ENOMEM;
        l->lines = q; l->cap = c;
    }
    l->lines[l->n++] = *line;
    return 0;
}

struct para_state {
    struct wp_layout *l; const struct wp_doc *d; const struct wp_text_ops *o; const struct wp_pattr *pa;
    unsigned para; int64_t limit, indent_first, indent_rest;
    size_t ls; int line_no; int64_t x, last, origin; int content, la, ld;
    int y;
};
static int64_t line_origin(const struct para_state *s)
{ return s->line_no == 0 ? s->indent_first : s->indent_rest; }
static void start_line(struct para_state *s, size_t at)
{
    s->ls = at; s->origin = line_origin(s); s->x = s->origin; s->last = s->origin; s->content = 0; s->la = s->ld = 0;
}
static int flush_line(struct para_state *s, size_t end, unsigned flags, const struct wp_cattr *mark_attr)
{
    struct wp_line ln;
    int la = s->la, ld = s->ld;
    if (!la && !ld) vmet(s->o, s->d, mark_attr, &la, &ld);
    if (la + ld < 1) { la = 1; ld = 0; }
    int64_t width = s->last - s->origin, avail = s->limit - s->origin;
    if (width < 0) width = 0;
    if (avail < 0) avail = 0;
    int shift = 0;
    if (width < avail) shift = s->pa->align == WP_CENTER ? px_of((avail - width) / 2) : s->pa->align == WP_RIGHT ? px_of(avail - width) : 0;
    ln = (struct wp_line){s->ls, end, s->y, la + ld, la, px_of(s->origin), shift, px_of(width), s->para,
                          flags | (s->line_no == 0 ? WP_LINE_FIRST : 0) | (s->line_no == 0 && s->pa->bullet ? WP_LINE_BULLET : 0)};
    int e = add_line(s->l, &ln);
    s->y += la + ld; s->line_no++;
    return e;
}
static void note(struct para_state *s, const struct wp_cattr *a)
{
    int as, ds; vmet(s->o, s->d, a, &as, &ds);
    if (as > s->la) s->la = as;
    if (ds > s->ld) s->ld = ds;
}

static int layout_para(struct para_state *s, struct cursor *c, size_t ps, size_t pe)
{
    const struct wp_pattr *pa = s->pa;
    int64_t left = px64(pa->left), first = pa->bullet ? left : left + px64(pa->first);
    if (first < 0) first = 0;
    s->indent_first = first; s->indent_rest = left;
    s->line_no = 0; start_line(s, ps);
    struct wp_cattr last_attr = cur_attr(c);
    struct atom held; int have = 0;
    struct atom *grp = NULL; unsigned gcap = 0;
    int err = 0;
    for (;;) {
        struct atom a;
        if (have) { a = held; have = 0; }
        else if (!next_atom(c, pe, &a)) break;
        last_attr = a.a;
        if (a.kind == A_BREAK) {
            note(s, &a.a);
            err = flush_line(s, a.start, 0, &a.a);
            if (err) break;
            start_line(s, a.end);
            continue;
        }
        if (a.kind == A_TAB) {
            int64_t stop = tab_stop(pa, s->x);
            note(s, &a.a);
            s->x = stop; s->last = stop; s->content = 1;
            continue;
        }
        if (a.kind == A_SPACE) {
            s->x += measure(s->o, s->d, &a.a, a.start, a.end);
            note(s, &a.a);
            continue;
        }
        /* a word: every adjacent word atom (run changes inside it) */
        unsigned gn = 0; int64_t gw = 0;
        for (;;) {
            if (gn == gcap) {
                unsigned nc = gcap ? gcap * 2 : 16;
                struct atom *q = realloc(grp, nc * sizeof(*q));
                if (!q) { err = -ENOMEM; break; }
                grp = q; gcap = nc;
            }
            grp[gn++] = a; gw += measure(s->o, s->d, &a.a, a.start, a.end);
            if (!next_atom(c, pe, &a)) break;
            if (a.kind != A_WORD) { held = a; have = 1; break; }
        }
        if (err) break;
        if (s->x + gw > s->limit && s->content) {
            err = flush_line(s, grp[0].start, WP_LINE_WRAPPED, &grp[0].a);
            if (err) break;
            start_line(s, grp[0].start);
        }
        if (s->x + gw <= s->limit || !gn) {
            s->x += gw; s->last = s->x; s->content = 1;
            for (unsigned i = 0; i < gn; i++) note(s, &grp[i].a);
            continue;
        }
        /* wider than an empty line: break between characters, at least one per line */
        for (unsigned i = 0; i < gn && !err; i++) {
            size_t at = grp[i].start;
            while (at < grp[i].end) {
                size_t nx = at + 1;
                while (nx < grp[i].end && ((unsigned char)s->d->text[nx] & 0xc0) == 0x80) nx++;
                int64_t cw = measure(s->o, s->d, &grp[i].a, at, nx);
                if (s->x + cw > s->limit && s->content) {
                    err = flush_line(s, at, WP_LINE_WRAPPED, &grp[i].a);
                    if (err) break;
                    start_line(s, at);
                }
                s->x += cw; s->last = s->x; s->content = 1; note(s, &grp[i].a);
                at = nx;
            }
        }
    }
    free(grp);
    if (err) return err;
    return flush_line(s, pe, WP_LINE_PARA_END, &last_attr);
}

int wp_layout_build(struct wp_layout *l, const struct wp_doc *d, const struct wp_text_ops *o, int width, unsigned first_para)
{
    if (width < 8) width = 8;
    if (width != l->width || first_para > l->pcount) first_para = 0;
    if (first_para >= d->npara) first_para = d->npara ? d->npara - 1 : 0;
    if (l->pcap < d->npara + 1) {
        unsigned nc = l->pcap ? l->pcap : 64;
        while (nc < d->npara + 1) nc *= 2;
        unsigned *pf = realloc(l->pfirst, nc * sizeof(*pf));
        if (!pf) return -ENOMEM;
        l->pfirst = pf;
        int *py = realloc(l->py, nc * sizeof(*py));
        if (!py) return -ENOMEM;
        l->py = py; l->pcap = nc;
    }
    l->width = width;
    unsigned keep = 0; int y = 0;
    if (first_para && first_para == l->pcount) { keep = l->n; y = l->height; }
    else if (first_para) { keep = l->pfirst[first_para]; y = l->py[first_para]; }
    l->n = keep;
    size_t ps = 0;
    for (unsigned p = 0; p < first_para; p++) ps += d->paras[p].len;
    struct cursor c = {d, 0, 0, 0}; seek(&c, ps);
    struct para_state s = {0};
    s.l = l; s.d = d; s.o = o; s.y = y;
    for (unsigned p = first_para; p < d->npara; p++) {
        size_t plen = d->paras[p].len, pe = p + 1 < d->npara ? ps + plen - 1 : ps + plen;
        l->pfirst[p] = l->n; l->py[p] = s.y;
        s.pa = &d->paras[p].a; s.para = p;
        s.limit = (int64_t)width * 64 - px64(s.pa->right);
        if (s.limit < 64) s.limit = 64;
        int e = layout_para(&s, &c, ps, pe);
        if (e) { l->pcount = p; return e; }
        if (p + 1 < d->npara) advance(&c, 1);
        ps += plen;
    }
    l->pcount = d->npara;
    l->height = s.y;
    return 0;
}

/* ---- queries ------------------------------------------------------------------ */

unsigned wp_layout_line_of(const struct wp_layout *l, size_t pos)
{
    unsigned lo = 0, hi = l->n;
    if (!l->n) return 0;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (l->lines[mid].start <= pos) lo = mid; else hi = mid;
    }
    return lo;
}
unsigned wp_layout_line_at_y(const struct wp_layout *l, int y)
{
    unsigned lo = 0, hi = l->n;
    if (!l->n) return 0;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (l->lines[mid].y <= y) lo = mid; else hi = mid;
    }
    return lo;
}
int wp_layout_bullet_x(const struct wp_doc *d, const struct wp_line *ln)
{
    const struct wp_pattr *pa = &d->paras[ln->para].a;
    int x = px_of(px64(pa->left) + px64(pa->first));
    return x < 0 ? 0 : x;
}

void wp_layout_pieces(const struct wp_layout *l, const struct wp_doc *d, const struct wp_text_ops *o, unsigned line, wp_piece_fn fn, void *ctx)
{
    if (line >= l->n) return;
    const struct wp_line *ln = &l->lines[line];
    const struct wp_pattr *pa = &d->paras[ln->para].a;
    struct cursor c = {d, 0, 0, 0}; seek(&c, ln->start);
    /* The origin is the exact (unrounded) indent, as in layout_para. */
    int64_t left = px64(pa->left), first = pa->bullet ? left : left + px64(pa->first);
    if (first < 0) first = 0;
    int64_t x = (ln->flags & WP_LINE_FIRST) ? first : left;
    struct atom a;
    while (next_atom(&c, ln->end, &a)) {
        struct wp_piece p = {a.start, a.end, 0, 0, a.a, WP_PIECE_TEXT};
        if (a.kind == A_BREAK) continue;
        if (a.kind == A_TAB) {
            int64_t stop = tab_stop(pa, x);
            p.kind = WP_PIECE_TAB; p.x = ln->shift + px_of(x); p.w = px_of(stop) - px_of(x);
            x = stop;
        } else {
            int64_t w = measure(o, d, &a.a, a.start, a.end);
            p.x = ln->shift + px_of(x); p.w = px_of(x + w) - px_of(x);
            x += w;
        }
        if (fn(ctx, &p)) return;
    }
}

struct caret_walk { const struct wp_doc *d; const struct wp_text_ops *o; size_t pos; int x, found, end_x; };
static int caret_piece(void *ctx, const struct wp_piece *p)
{
    struct caret_walk *w = ctx;
    if (p->start > w->pos) return 1;
    w->end_x = p->x + p->w;
    if (p->kind == WP_PIECE_TAB) {
        if (w->pos == p->start) { w->x = p->x; w->found = 1; return 1; }
        w->x = p->x + p->w; w->found = 1;
        return 0;
    }
    if (w->pos <= p->end) {
        w->x = p->x + (w->pos == p->start ? 0 : px_of(measure(w->o, w->d, &p->a, p->start, w->pos)));
        w->found = 1;
        if (w->pos < p->end) return 1;
    }
    return 0;
}
void wp_layout_caret(const struct wp_layout *l, const struct wp_doc *d, const struct wp_text_ops *o, size_t pos, int *x, int *y, int *h)
{
    unsigned line = wp_layout_line_of(l, pos);
    if (!l->n) { *x = 0; *y = 0; *h = 16; return; }
    const struct wp_line *ln = &l->lines[line];
    struct caret_walk w = {d, o, pos, ln->shift + ln->indent, 0, ln->shift + ln->indent};
    wp_layout_pieces(l, d, o, line, caret_piece, &w);
    /* A caret past the last piece (or on an empty line) sits at the end of the text. */
    *x = w.found ? w.x : w.end_x; *y = ln->y; *h = ln->h;
}

struct hit_walk { const struct wp_doc *d; const struct wp_text_ops *o; int x; size_t pos; int done; };
static int hit_piece(void *ctx, const struct wp_piece *p)
{
    struct hit_walk *w = ctx;
    if (w->x < p->x) { w->pos = p->start; w->done = 1; return 1; }
    if (w->x >= p->x + p->w) { w->pos = p->end; return 0; }
    if (p->kind == WP_PIECE_TAB) { w->pos = w->x - p->x < p->w / 2 ? p->start : p->end; w->done = 1; return 1; }
    size_t at = p->start, best = p->start;
    int prev = 0;
    while (at < p->end) {
        size_t nx = at + 1;
        while (nx < p->end && ((unsigned char)w->d->text[nx] & 0xc0) == 0x80) nx++;
        int cw = px_of(measure(w->o, w->d, &p->a, p->start, nx));
        if (w->x < p->x + (prev + cw + 1) / 2) { best = at; w->pos = best; w->done = 1; return 1; }
        prev = cw; at = nx; best = nx;
    }
    w->pos = best; w->done = 1;
    return 1;
}
size_t wp_layout_hit(const struct wp_layout *l, const struct wp_doc *d, const struct wp_text_ops *o, int x, int y)
{
    if (!l->n) return 0;
    unsigned line = wp_layout_line_at_y(l, y);
    const struct wp_line *ln = &l->lines[line];
    struct hit_walk w = {d, o, x, ln->start, 0};
    wp_layout_pieces(l, d, o, line, hit_piece, &w);
    size_t pos = w.pos;
    if (pos > ln->end) pos = ln->end;
    if (!w.done && (ln->flags & WP_LINE_WRAPPED))
        while (pos > ln->start && d->text[pos - 1] == ' ') pos--;
    if (pos < ln->start) pos = ln->start;
    return pos;
}

/* ---- pages ----------------------------------------------------------------------- */

void wp_pages_free(struct wp_pages *p) { free(p->first_line); memset(p, 0, sizeof(*p)); }
int wp_paginate(struct wp_pages *pages, struct wp_layout *l, const struct wp_doc *d, const struct wp_text_ops *o, const struct wp_page_setup *s)
{
    int pw = s->landscape ? s->paper_h : s->paper_w, ph = s->landscape ? s->paper_w : s->paper_h;
    int width = wp_twips_px(pw - s->left - s->right), height = wp_twips_px(ph - s->top - s->bottom);
    if (width < 8) width = 8;
    if (height < 16) height = 16;
    int r = wp_layout_build(l, d, o, width, 0);
    if (r) return r;
    unsigned cap = 8, count = 1;
    unsigned *first = malloc(cap * sizeof(*first));
    if (!first) return -ENOMEM;
    first[0] = 0;
    int top = 0;
    for (unsigned i = 0; i < l->n; i++) {
        if (i > first[count - 1] && l->lines[i].y + l->lines[i].h - top > height) {
            if (count == cap) {
                unsigned *q = realloc(first, cap * 2 * sizeof(*q));
                if (!q) { free(first); return -ENOMEM; }
                first = q; cap *= 2;
            }
            first[count++] = i; top = l->lines[i].y;
        }
    }
    pages->first_line = first; pages->count = count;
    return 0;
}
