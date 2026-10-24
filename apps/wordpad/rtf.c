/* SPDX-License-Identifier: MIT
 * WordPad RTF and plain text readers and writers (rich.h).
 *
 * Source: Microsoft, "Rich Text Format (RTF) Specification" 1.6 (Word 97/2000
 * control words) and 1.9.1 (Word 2007; the last version). Supported: the
 * header (\ansi \ansicpg \deff), the font and colour tables, \f \fs \cf
 * \b \i \ul \strike \plain, \pard \par \line \tab \ql \qc \qr \li \ri \fi
 * \tx, the character escapes (\'hh, \uN with \ucN, \bullet, quotes and dashes,
 * \~ \- \_) and the bullet forms WordPad and Word write (\pntext with \pn
 * \pnlvlblt, \listtext). Pictures and objects are skipped and counted in the
 * report; unknown control words and destinations are ignored as the
 * specification requires. */
#include "rich.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- reader ------------------------------------------------------------------ */

enum { DEST_BODY, DEST_SKIP, DEST_FONT, DEST_COLOR, DEST_LIST, DEST_PN };
#define DEPTH 64
#define FONT_NUMBERS 256
#define LIST_TEXT 48

struct gstate {
    int dest, uc, fnum, cf, fs;
    unsigned flags;
    struct wp_pattr p;
};
struct rd {
    struct wp_doc *doc; struct wp_report *rep;
    const unsigned char *s; size_t n, i;
    struct gstate st[DEPTH]; int depth, over;
    int fmap[FONT_NUMBERS], cmap[FONT_NUMBERS], deff;
    char fname[40]; size_t flen; int fnum, ffam;
    int cred, cgreen, cblue, chave; unsigned ccount;
    char lbuf[LIST_TEXT]; size_t llen; int ltab;
    int pn_parent, skip, star, last_par, err;
    uint32_t hi;                 /* a high surrogate waiting for its low half */
};

static struct gstate *cur(struct rd *r) { return &r->st[r->depth]; }

/* The character attributes of the current group, resolved against the document tables. */
static struct wp_cattr attr_of(struct rd *r)
{
    const struct gstate *g = cur(r);
    int f = g->fnum >= 0 ? g->fnum : r->deff;
    struct wp_cattr a = {0, 0, 0, 0, 0};
    a.font = (uint8_t)(f >= 0 && f < FONT_NUMBERS && r->fmap[f] >= 0 ? r->fmap[f] : 0);
    a.color = (uint8_t)(g->cf >= 0 && g->cf < FONT_NUMBERS && r->cmap[g->cf] >= 0 ? r->cmap[g->cf] : 0);
    a.flags = (uint8_t)g->flags;
    a.size = (uint16_t)(g->fs < WP_SIZE_MIN ? WP_SIZE_MIN : g->fs > WP_SIZE_MAX ? WP_SIZE_MAX : g->fs);
    return a;
}
static void body(struct rd *r, const char *s, size_t n)
{
    if (r->err || !n) return;
    struct wp_cattr a = attr_of(r);
    r->doc->paras[r->doc->npara - 1].a = cur(r)->p;
    int e = wp_append(r->doc, s, n, &a);
    if (e) r->err = e;
    r->last_par = 0;
}
static void emit_cp(struct rd *r, uint32_t cp);
/* \uN: UTF-16 code units; a surrogate pair is one character, a lone half is U+FFFD. */
static void emit_u(struct rd *r, int v)
{
    uint32_t cp = (uint32_t)(v < 0 ? v + 65536 : v) & 0xffff;
    if (cp >= 0xd800 && cp < 0xdc00) {
        if (r->hi) { r->hi = 0; emit_cp(r, 0xfffd); }
        r->hi = cp;
        return;
    }
    if (cp >= 0xdc00 && cp < 0xe000) {
        if (r->hi) { cp = 0x10000 + ((r->hi - 0xd800) << 10) + (cp - 0xdc00); r->hi = 0; }
        else cp = 0xfffd;
    }
    emit_cp(r, cp);
}
static void emit_cp(struct rd *r, uint32_t cp)
{
    char b[4]; unsigned n;
    if (r->skip > 0) { r->skip--; return; }
    if (r->hi) { r->hi = 0; emit_cp(r, 0xfffd); }
    if ((cp < 32 && cp != '\t') || cp == 127) return;   /* control characters never reach the text */
    int d = cur(r)->dest;
    if (d == DEST_FONT) {
        if (cp == ';') {
            struct wp_doc *doc = r->doc;
            while (r->flen && r->fname[r->flen - 1] == ' ') r->flen--;
            r->fname[r->flen] = 0;
            if (r->fnum >= 0 && r->fnum < FONT_NUMBERS) {
                int idx = -1;
                if (r->flen) {
                    unsigned fam = wp_family_for_name(r->fname);
                    if (fam == WP_SANS) fam = r->ffam == 1 ? WP_SERIF : r->ffam == 2 ? WP_MONO : WP_SANS;
                    idx = wp_font_add(doc, r->fname, fam);
                    if (idx < 0) idx = (int)fam;
                } else idx = r->ffam == 1 ? WP_SERIF : r->ffam == 2 ? WP_MONO : -1;
                r->fmap[r->fnum] = idx;
            }
            r->flen = 0; r->ffam = 0; r->fnum = -1;
        } else if (cp >= 32 && cp < 127 && r->flen < sizeof(r->fname) - 1) r->fname[r->flen++] = (char)cp;
        return;
    }
    if (d == DEST_COLOR) {
        if (cp != ';') return;
        if (r->ccount < FONT_NUMBERS) {
            uint32_t rgb = (uint32_t)(r->cred & 255) << 16 | (uint32_t)(r->cgreen & 255) << 8 | (uint32_t)(r->cblue & 255);
            r->cmap[r->ccount] = r->chave ? wp_color_add(r->doc, rgb) : 0;
            r->ccount++;
        }
        r->cred = r->cgreen = r->cblue = r->chave = 0;
        return;
    }
    if (d == DEST_SKIP || d == DEST_PN) return;
    n = wp_utf8_put(b, cp);
    if (d == DEST_LIST) { if (r->llen + n < sizeof(r->lbuf)) { memcpy(r->lbuf + r->llen, b, n); r->llen += n; } return; }
    body(r, b, n);
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static int is_bullet(const char *s, size_t n)
{
    uint32_t cp = 0;
    if (!n) return 0;
    if ((unsigned char)s[0] < 0x80) cp = (unsigned char)s[0];
    else if (n >= 2 && ((unsigned char)s[0] & 0xe0) == 0xc0) cp = ((unsigned)(s[0] & 31) << 6) | (s[1] & 63);
    else if (n >= 3 && ((unsigned char)s[0] & 0xf0) == 0xe0) cp = ((unsigned)(s[0] & 15) << 12) | ((unsigned)(s[1] & 63) << 6) | (s[2] & 63);
    return cp == 0xb7 || cp == 0x2022 || cp == 0xf0b7 || cp == '*' || cp == 0x25cf || cp == 0x25aa;
}
static void make_bullet(struct wp_pattr *p)
{
    p->bullet = 1;
    if (!p->left && !p->first) { p->left = 360; p->first = -360; }
}

static void destination(struct rd *r, const char *w)
{
    static const char *const skip[] = {"stylesheet", "info", "header", "headerl", "headerr", "headerf", "footer", "footerl",
        "footerr", "footerf", "footnote", "listtable", "listoverridetable", "rsidtbl", "generator", "themedata",
        "colorschememapping", "latentstyles", "datastore", "xmlnstbl", "pgptbl", "revtbl", "userprops", "nonshppict",
        "upr", "fldinst", "sp", "shp", "shpgrp", "pnseclvl", "fontemb", "filetbl", "private", "xe", "tc", "txe", "comment"};
    struct gstate *g = cur(r);
    if (!strcmp(w, "fonttbl")) { g->dest = DEST_FONT; r->flen = 0; r->fnum = -1; r->ffam = 0; return; }
    if (!strcmp(w, "colortbl")) { g->dest = DEST_COLOR; r->cred = r->cgreen = r->cblue = r->chave = 0; r->ccount = 0; return; }
    if (!strcmp(w, "pntext") || !strcmp(w, "listtext")) { g->dest = DEST_LIST; r->llen = 0; r->ltab = 0; return; }
    if (!strcmp(w, "pn")) { g->dest = DEST_PN; r->pn_parent = r->depth - 1; return; }
    if (!strcmp(w, "pict")) { r->rep->skipped_pictures++; g->dest = DEST_SKIP; return; }
    if (!strcmp(w, "object") || !strcmp(w, "shp")) { r->rep->skipped_objects++; g->dest = DEST_SKIP; return; }
    for (unsigned i = 0; i < sizeof(skip) / sizeof(*skip); i++) if (!strcmp(w, skip[i])) { g->dest = DEST_SKIP; return; }
}

static void word(struct rd *r, const char *w, int has, int v)
{
    struct gstate *g = cur(r);
    int on = !has || v != 0;
    if (r->star) {
        r->star = 0;
        if (!strcmp(w, "pn")) { destination(r, w); return; }
        if (g->dest != DEST_SKIP) g->dest = DEST_SKIP;   /* ignorable destination the reader does not know */
        return;
    }
    if (g->dest == DEST_PN) {
        if (!strcmp(w, "pnlvlblt") && r->pn_parent >= 0) make_bullet(&r->st[r->pn_parent].p);
        return;
    }
    if (g->dest == DEST_FONT) {
        if (!strcmp(w, "f")) { r->fnum = v; r->flen = 0; }
        else if (!strcmp(w, "froman")) r->ffam = 1;
        else if (!strcmp(w, "fmodern")) r->ffam = 2;
        else if (!strcmp(w, "fswiss") || !strcmp(w, "fnil") || !strcmp(w, "fscript") || !strcmp(w, "fdecor") ||
                 !strcmp(w, "ftech") || !strcmp(w, "fbidi")) r->ffam = 0;
        return;
    }
    if (g->dest == DEST_COLOR) {
        if (!strcmp(w, "red")) { r->cred = v; r->chave = 1; }
        else if (!strcmp(w, "green")) { r->cgreen = v; r->chave = 1; }
        else if (!strcmp(w, "blue")) { r->cblue = v; r->chave = 1; }
        return;
    }
    if (g->dest == DEST_LIST) {
        if (!strcmp(w, "tab")) r->ltab = 1;
        else if (!strcmp(w, "bullet")) emit_cp(r, 0x2022);
        else if (!strcmp(w, "u")) { emit_u(r, v); r->skip = g->uc; }
        return;
    }
    if (!strcmp(w, "u")) {
        if (g->dest == DEST_BODY && has) { emit_u(r, v); r->skip = g->uc; }
        return;
    }
    if (g->dest == DEST_SKIP) { destination(r, w); return; }   /* a nested pict or object still counts */
    if (!strcmp(w, "uc")) { g->uc = v < 0 ? 0 : v > 8 ? 8 : v; return; }
    if (!strcmp(w, "par") || !strcmp(w, "row") || !strcmp(w, "sect")) {
        if (r->err) return;
        r->doc->paras[r->doc->npara - 1].a = g->p;
        struct wp_cattr a = attr_of(r);
        int e = wp_append(r->doc, "\n", 1, &a);
        if (e) r->err = e;
        r->last_par = 1;
    }
    else if (!strcmp(w, "line")) { body(r, "\v", 1); }
    else if (!strcmp(w, "tab") || !strcmp(w, "cell")) body(r, "\t", 1);
    else if (!strcmp(w, "pard")) g->p = wp_default_pattr();
    else if (!strcmp(w, "plain")) { g->fnum = -1; g->fs = 24; g->flags = 0; g->cf = 0; }
    else if (!strcmp(w, "b")) g->flags = on ? g->flags | WP_BOLD : g->flags & ~(unsigned)WP_BOLD;
    else if (!strcmp(w, "i")) g->flags = on ? g->flags | WP_ITALIC : g->flags & ~(unsigned)WP_ITALIC;
    else if (!strcmp(w, "strike")) g->flags = on ? g->flags | WP_STRIKE : g->flags & ~(unsigned)WP_STRIKE;
    else if (!strcmp(w, "ul") || !strcmp(w, "uld") || !strcmp(w, "uldb") || !strcmp(w, "ulw") || !strcmp(w, "ulth") ||
             !strcmp(w, "ulwave") || !strcmp(w, "uldash") || !strcmp(w, "uldashd") || !strcmp(w, "uldashdd") || !strcmp(w, "ulhwave"))
        g->flags = on ? g->flags | WP_UNDERLINE : g->flags & ~(unsigned)WP_UNDERLINE;
    else if (!strcmp(w, "ulnone")) g->flags &= ~(unsigned)WP_UNDERLINE;
    else if (!strcmp(w, "f")) g->fnum = has ? v : -1;
    else if (!strcmp(w, "fs")) g->fs = has ? v : 24;
    else if (!strcmp(w, "cf")) g->cf = has && v >= 0 ? v : 0;
    else if (!strcmp(w, "ql") || !strcmp(w, "qj")) g->p.align = WP_LEFT;
    else if (!strcmp(w, "qc")) g->p.align = WP_CENTER;
    else if (!strcmp(w, "qr")) g->p.align = WP_RIGHT;
    else if (!strcmp(w, "li") || !strcmp(w, "lin")) g->p.left = v < 0 ? 0 : v > WP_INDENT_MAX ? WP_INDENT_MAX : v;
    else if (!strcmp(w, "ri") || !strcmp(w, "rin")) g->p.right = v < 0 ? 0 : v > WP_INDENT_MAX ? WP_INDENT_MAX : v;
    else if (!strcmp(w, "fi")) g->p.first = v < -WP_INDENT_MAX ? -WP_INDENT_MAX : v > WP_INDENT_MAX ? WP_INDENT_MAX : v;
    else if (!strcmp(w, "tx")) {
        if (has && v > 0 && v <= WP_INDENT_MAX && g->p.ntabs < WP_TABS && (!g->p.ntabs || v > g->p.tabs[g->p.ntabs - 1]))
            g->p.tabs[g->p.ntabs++] = v;
    }
    else if (!strcmp(w, "deff")) r->deff = has ? v : 0;
    else if (!strcmp(w, "ansicpg")) { if (has && v != 1252) r->rep->unknown_charset++; }
    else if (!strcmp(w, "mac") || !strcmp(w, "pc") || !strcmp(w, "pca")) r->rep->unknown_charset++;
    else if (!strcmp(w, "bullet")) body(r, "\xe2\x80\xa2", 3);
    else if (!strcmp(w, "endash")) body(r, "\xe2\x80\x93", 3);
    else if (!strcmp(w, "emdash")) body(r, "\xe2\x80\x94", 3);
    else if (!strcmp(w, "lquote")) body(r, "\xe2\x80\x98", 3);
    else if (!strcmp(w, "rquote")) body(r, "\xe2\x80\x99", 3);
    else if (!strcmp(w, "ldblquote")) body(r, "\xe2\x80\x9c", 3);
    else if (!strcmp(w, "rdblquote")) body(r, "\xe2\x80\x9d", 3);
    else if (!strcmp(w, "enspace") || !strcmp(w, "emspace") || !strcmp(w, "qmspace")) body(r, " ", 1);
    else destination(r, w);
}

static void close_group(struct rd *r)
{
    struct gstate done = *cur(r);
    r->depth--;
    r->skip = 0;
    if (done.dest == DEST_LIST) {
        struct gstate *g = cur(r);
        if (is_bullet(r->lbuf, r->llen)) make_bullet(&g->p);
        else {
            if (r->llen) body(r, r->lbuf, r->llen);
            if (r->ltab) body(r, "\t", 1);
        }
        r->llen = 0; r->ltab = 0;
    }
}

int wp_is_rtf(const char *buf, size_t n) { return n >= 5 && !memcmp(buf, "{\\rtf", 5); }

int wp_rtf_read(struct wp_doc *doc, const char *buf, size_t n, struct wp_report *rep)
{
    struct wp_report none;
    if (!rep) rep = &none;
    memset(rep, 0, sizeof(*rep));
    if (!wp_is_rtf(buf, n)) return -EINVAL;
    struct rd *r = calloc(1, sizeof(*r));
    if (!r) return -ENOMEM;
    struct wp_doc work;
    int e = wp_init(&work);
    if (e) { free(r); return e; }
    r->doc = &work; r->rep = rep; r->s = (const unsigned char *)buf; r->n = n; r->pn_parent = -1; r->deff = 0;
    for (int i = 0; i < FONT_NUMBERS; i++) r->fmap[i] = r->cmap[i] = -1;
    r->st[0] = (struct gstate){DEST_BODY, 1, -1, 0, 24, 0, wp_default_pattr()};
    while (r->i < r->n && !r->err) {
        unsigned c = r->s[r->i++];
        if (c == '{') {
            r->skip = 0;
            if (r->depth >= DEPTH - 1) { r->over++; continue; }
            r->st[r->depth + 1] = r->st[r->depth]; r->depth++;
        } else if (c == '}') {
            r->skip = 0;
            if (r->over) { r->over--; continue; }
            if (r->depth) close_group(r);
        } else if (c == '\\') {
            if (r->i >= r->n) break;
            c = r->s[r->i++];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                char w[34]; size_t k = 0; int has = 0, neg = 0; long v = 0;
                w[k++] = (char)c;
                while (r->i < r->n && ((r->s[r->i] | 32) >= 'a' && (r->s[r->i] | 32) <= 'z')) {
                    if (k < sizeof(w) - 1) w[k++] = (char)r->s[r->i];
                    r->i++;
                }
                w[k] = 0;
                if (r->i < r->n && r->s[r->i] == '-') { neg = 1; r->i++; }
                while (r->i < r->n && r->s[r->i] >= '0' && r->s[r->i] <= '9') {
                    has = 1; if (v < (1L << 24)) v = v * 10 + (r->s[r->i] - '0');
                    r->i++;
                }
                if (neg && !has) neg = 0;
                if (neg) v = -v;
                if (r->i < r->n && r->s[r->i] == ' ') r->i++;
                if (!strcmp(w, "bin") && has && v > 0) {
                    size_t skip = (size_t)v < r->n - r->i ? (size_t)v : r->n - r->i;
                    r->i += skip;
                    continue;
                }
                word(r, w, has, (int)v);
            } else if (c == '\'') {
                int hi = r->i < r->n ? hexval(r->s[r->i]) : -1, lo = r->i + 1 < r->n ? hexval(r->s[r->i + 1]) : -1;
                if (hi >= 0 && lo >= 0) { r->i += 2; emit_cp(r, wp_cp1252((unsigned)(hi * 16 + lo))); }
            } else if (c == '\\' || c == '{' || c == '}') emit_cp(r, c);
            else if (c == '~') emit_cp(r, 0xa0);
            else if (c == '_') emit_cp(r, '-');
            else if (c == '*') r->star = 1;
            else if (c == '\r' || c == '\n') {
                if (c == '\r' && r->i < r->n && r->s[r->i] == '\n') r->i++;
                if (cur(r)->dest == DEST_BODY) word(r, "par", 0, 0);
            }
        } else if (c == '\r' || c == '\n') {
        } else if (c >= 0x80) emit_cp(r, wp_cp1252(c));
        else if (c >= 32 || c == '\t') emit_cp(r, c);
    }
    e = r->err;
    if (!e && r->last_par && work.len && work.text[work.len - 1] == '\n') e = wp_delete(&work, work.len - 1, work.len);
    free(r);
    if (e) { wp_free(&work); return e; }
    wp_free(doc); *doc = work;
    return 0;
}

/* ---- writer -------------------------------------------------------------------- */

struct sb { char *p; size_t n, cap; int err; unsigned col; };
static void put(struct sb *b, const char *s, size_t n)
{
    if (b->err) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { b->err = -ENOMEM; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->col += (unsigned)n;
    b->p[b->n] = 0;
}
static void puts_(struct sb *b, const char *s) { put(b, s, strlen(s)); }
static void putf(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(struct sb *b, const char *fmt, ...)
{
    char t[96]; __builtin_va_list ap;
    __builtin_va_start(ap, fmt); int k = vsnprintf(t, sizeof(t), fmt, ap); __builtin_va_end(ap);
    if (k > 0) put(b, t, (size_t)(k < (int)sizeof(t) ? k : (int)sizeof(t) - 1));
}
static void token(struct sb *b) { if (b->col >= 200) { put(b, "\n", 1); b->col = 0; } }

static int to_cp1252(uint32_t cp)
{
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff)) return (int)cp;
    for (unsigned c = 0x80; c < 0xa0; c++) if (wp_cp1252(c) == cp && cp) return (int)c;
    return -1;
}
static void put_text(struct sb *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i];
        uint32_t cp; unsigned k = 1;
        if (c < 0x80) cp = c;
        else {
            k = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
            if (i + k > n) { k = 1; cp = 0xfffd; }
            else {
                cp = c & (0xffu >> (k + 1));
                for (unsigned j = 1; j < k; j++) cp = cp << 6 | ((unsigned char)s[i + j] & 63);
            }
        }
        i += k;
        token(b);
        if (cp == '\t') puts_(b, "\\tab ");
        else if (cp == WP_SOFT_BREAK) puts_(b, "\\line ");
        else if (cp == '\\' || cp == '{' || cp == '}') { char t[2] = {'\\', (char)cp}; put(b, t, 2); }
        else if (cp >= 32 && cp < 127) { char t = (char)cp; put(b, &t, 1); }
        else if (cp < 32) continue;
        else {
            int m = to_cp1252(cp);
            if (m >= 0) putf(b, "\\'%02x", (unsigned)m);
            else if (cp < 0x10000) putf(b, "\\u%d?", (int)(cp > 32767 ? (int)cp - 65536 : (int)cp));
            else {
                uint32_t v = cp - 0x10000, hi = 0xd800 + (v >> 10), lo = 0xdc00 + (v & 0x3ff);
                putf(b, "\\u%d?\\u%d?", (int)hi - 65536, (int)lo - 65536);
            }
        }
    }
}
static void clean_name(char *out, const char *name)
{
    size_t k = 0;
    for (; name[k] && k < 31; k++) {
        unsigned char c = (unsigned char)name[k];
        out[k] = c < 32 || c > 126 || c == ';' || c == '{' || c == '}' || c == '\\' ? ' ' : (char)c;
    }
    out[k] = 0;
}
/* Words that turn `from` into `to`, then one delimiter space when any was written. */
static void delta(struct sb *b, struct wp_cattr *from, const struct wp_cattr *to)
{
    unsigned before = (unsigned)b->n;
    if (from->font != to->font) { token(b); putf(b, "\\f%u", to->font); }
    if (from->size != to->size) { token(b); putf(b, "\\fs%u", to->size); }
    if (from->color != to->color) { token(b); putf(b, "\\cf%u", to->color); }
    static const struct { unsigned bit; const char *on, *off; } flags[] = {
        {WP_BOLD, "\\b", "\\b0"}, {WP_ITALIC, "\\i", "\\i0"}, {WP_UNDERLINE, "\\ul", "\\ulnone"}, {WP_STRIKE, "\\strike", "\\strike0"}};
    for (unsigned i = 0; i < 4; i++)
        if ((from->flags ^ to->flags) & flags[i].bit) { token(b); puts_(b, to->flags & flags[i].bit ? flags[i].on : flags[i].off); }
    if (b->n != before) put(b, " ", 1);
    *from = *to;
}

int wp_rtf_write(const struct wp_doc *d, char **out, size_t *len)
{
    struct sb b = {0};
    unsigned symbol = d->nfonts;
    puts_(&b, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033\n{\\fonttbl");
    for (unsigned i = 0; i < d->nfonts; i++) {
        char name[32]; clean_name(name, d->fonts[i].name);
        putf(&b, "{\\f%u\\%s\\fcharset0 %s;}", i, d->fonts[i].family == WP_SERIF ? "froman" : d->fonts[i].family == WP_MONO ? "fmodern" : "fswiss", name);
    }
    putf(&b, "{\\f%u\\fnil\\fcharset2 Symbol;}}\n{\\colortbl ;", symbol);
    for (unsigned i = 1; i < d->ncolors; i++)
        putf(&b, "\\red%u\\green%u\\blue%u;", (d->colors[i] >> 16) & 255, (d->colors[i] >> 8) & 255, d->colors[i] & 255);
    puts_(&b, "}\n\\viewkind4\\uc1 ");
    struct wp_cattr now = {0, 0, 0, 0, 24};
    unsigned ri = 0; size_t roff = 0, at = 0;
    for (unsigned p = 0; p < d->npara; p++) {
        const struct wp_pattr *a = &d->paras[p].a;
        size_t plen = d->paras[p].len, tl = p + 1 < d->npara ? plen - 1 : plen;
        puts_(&b, "\\pard");
        if (a->bullet) putf(&b, "{\\pntext\\f%u\\'B7\\tab}{\\*\\pn\\pnlvlblt\\pnf%u\\pnindent0{\\pntxtb\\'B7}}", symbol, symbol);
        if (a->align == WP_CENTER) puts_(&b, "\\qc"); else if (a->align == WP_RIGHT) puts_(&b, "\\qr");
        if (a->first) putf(&b, "\\fi%d", (int)a->first);
        if (a->left) putf(&b, "\\li%d", (int)a->left);
        if (a->right) putf(&b, "\\ri%d", (int)a->right);
        for (unsigned t = 0; t < a->ntabs; t++) putf(&b, "\\tx%d", (int)a->tabs[t]);
        put(&b, " ", 1);
        size_t left = tl;
        while (left) {
            while (ri < d->nruns && roff == d->runs[ri].len) { ri++; roff = 0; }
            if (ri >= d->nruns) break;
            size_t take = d->runs[ri].len - roff;
            if (take > left) take = left;
            delta(&b, &now, &d->runs[ri].a);
            put_text(&b, d->text + at, take);
            at += take; roff += take; left -= take;
        }
        if (p + 1 < d->npara) {   /* the paragraph mark carries a character format too */
            while (ri < d->nruns && roff == d->runs[ri].len) { ri++; roff = 0; }
            if (ri < d->nruns) { delta(&b, &now, &d->runs[ri].a); roff++; }
            at++;
        }
        puts_(&b, "\\par\n"); b.col = 0;
    }
    puts_(&b, "}\n");
    if (b.err) { free(b.p); return b.err; }
    *out = b.p; *len = b.n;
    return 0;
}

/* ---- plain text ---------------------------------------------------------------- */

int wp_text_read(struct wp_doc *d, const char *buf, size_t n)
{
    size_t skip = n >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3) ? 3 : 0;
    const unsigned char *s = (const unsigned char *)buf + skip;
    size_t m = n - skip;
    /* Valid UTF-8 is read as such; anything else is Windows-1252, as Notepad and WordPad do for ANSI files. */
    int utf8 = 1;
    for (size_t i = 0; i < m && utf8;) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        unsigned k = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
        if (!k || i + k > m) { utf8 = 0; break; }
        for (unsigned j = 1; j < k; j++) if ((s[i + j] & 0xc0) != 0x80) utf8 = 0;
        i += k;
    }
    char *text; size_t len;
    int e;
    if (utf8) e = wp_sanitize((const char *)s, m, &text, &len);
    else {
        size_t need = 0; char one[4];
        for (size_t i = 0; i < m; i++) { uint32_t cp = s[i] < 0x80 ? s[i] : wp_cp1252(s[i]); need += wp_utf8_put(one, cp ? cp : (s[i] ? 0xfffd : 0)); }
        char *conv = malloc(need + 1);
        if (!conv) return -ENOMEM;
        size_t w = 0;
        for (size_t i = 0; i < m; i++) { uint32_t cp = s[i] < 0x80 ? s[i] : wp_cp1252(s[i]); w += wp_utf8_put(conv + w, cp ? cp : (s[i] ? 0xfffd : 0)); }
        e = wp_sanitize(conv, w, &text, &len);
        free(conv);
    }
    if (e) return e;
    if (len > WP_LIMIT) { free(text); return -EFBIG; }
    struct wp_doc work;
    e = wp_init(&work);
    if (!e) { struct wp_cattr a = wp_default_cattr(); e = wp_append(&work, text, len, &a); }
    free(text);
    if (e) { wp_free(&work); return e; }
    wp_free(d); *d = work;   /* moved, not copied: a large document is never held twice */
    return 0;
}

int wp_text_write(const struct wp_doc *d, char **out, size_t *len, int crlf)
{
    size_t extra = 0;
    if (crlf) for (size_t i = 0; i < d->len; i++) extra += d->text[i] == '\n' || d->text[i] == WP_SOFT_BREAK;
    char *o = malloc(d->len + extra + 1);
    if (!o) return -ENOMEM;
    size_t w = 0;
    for (size_t i = 0; i < d->len; i++) {
        char c = d->text[i];
        if (c == WP_SOFT_BREAK) c = '\n';
        if (c == '\n' && crlf) o[w++] = '\r';
        o[w++] = c;
    }
    o[w] = 0; *out = o; *len = w;
    return 0;
}
