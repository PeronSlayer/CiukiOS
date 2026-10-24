/* SPDX-License-Identifier: MIT
 * WordPad document model: see rich.h. */
#include "rich.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t standard_colors[] = {
    0x000000, 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0x808080,
    0xc0c0c0, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};

/* ---- defaults and small helpers ------------------------------------------ */

struct wp_cattr wp_default_cattr(void) { return (struct wp_cattr){0, 0, 0, 0, WP_SIZE_DEFAULT}; }
struct wp_pattr wp_default_pattr(void) { struct wp_pattr p; memset(&p, 0, sizeof(p)); return p; }
int wp_cattr_eq(const struct wp_cattr *a, const struct wp_cattr *b)
{ return a->font == b->font && a->flags == b->flags && a->color == b->color && a->size == b->size; }
int wp_pattr_eq(const struct wp_pattr *a, const struct wp_pattr *b)
{
    if (a->align != b->align || a->bullet != b->bullet || a->ntabs != b->ntabs || a->left != b->left ||
        a->first != b->first || a->right != b->right) return 0;
    for (unsigned i = 0; i < a->ntabs; i++) if (a->tabs[i] != b->tabs[i]) return 0;
    return 1;
}

static int grow(void **p, unsigned *cap, size_t need, size_t item)
{
    if (need <= *cap) return 0;
    size_t n = *cap ? *cap : 16;
    while (n < need) n *= 2;
    if (n > UINT32_MAX / item) return -ENOMEM;
    void *q = realloc(*p, n * item);
    if (!q) return -ENOMEM;
    *p = q; *cap = (unsigned)n;
    return 0;
}
/* Capacity for the worst case of an operation, before it changes anything. */
static int reserve(struct wp_doc *d, size_t text, unsigned runs, unsigned paras)
{
    if (text > WP_LIMIT - d->len) return -EFBIG;
    size_t need = d->len + text;
    if (need + 1 > d->cap + 1 || !d->text) {
        size_t n = d->cap ? d->cap : 256;
        while (n < need) n *= 2;
        if (n > WP_LIMIT) n = WP_LIMIT;
        if (n < need) n = need;
        char *q = realloc(d->text, n + 1);
        if (!q) return -ENOMEM;
        d->text = q; d->cap = n; d->text[d->len] = 0;
    }
    unsigned cr = d->cruns, cp = d->cpara;
    void *r = d->runs, *p = d->paras;
    if (grow(&r, &cr, (size_t)d->nruns + runs, sizeof(*d->runs))) return -ENOMEM;
    d->runs = r; d->cruns = cr;
    if (grow(&p, &cp, (size_t)d->npara + paras, sizeof(*d->paras))) return -ENOMEM;
    d->paras = p; d->cpara = cp;
    return 0;
}

int wp_init(struct wp_doc *d)
{
    memset(d, 0, sizeof(*d));
    return wp_reset(d);
}
void wp_free(struct wp_doc *d)
{
    free(d->text); free(d->runs); free(d->paras);
    memset(d, 0, sizeof(*d));
}
int wp_reset(struct wp_doc *d)
{
    uint64_t revision = d->revision + 1;
    d->len = 0; d->nruns = 0; d->npara = 0;
    if (reserve(d, 0, 0, 1)) return -ENOMEM;
    d->text[0] = 0;
    d->paras[0] = (struct wp_para){0, wp_default_pattr()};
    d->npara = 1;
    memset(d->fonts, 0, sizeof(d->fonts));
    strcpy(d->fonts[0].name, "Arial"); d->fonts[0].family = WP_SANS;
    strcpy(d->fonts[1].name, "Times New Roman"); d->fonts[1].family = WP_SERIF;
    strcpy(d->fonts[2].name, "Courier New"); d->fonts[2].family = WP_MONO;
    d->nfonts = 3;
    for (unsigned i = 0; i < sizeof(standard_colors) / sizeof(*standard_colors); i++) d->colors[i] = standard_colors[i];
    d->ncolors = (unsigned)(sizeof(standard_colors) / sizeof(*standard_colors));
    d->revision = revision;
    return 0;
}
int wp_copy(struct wp_doc *dst, const struct wp_doc *src)
{
    struct wp_doc n = *src;
    n.text = malloc(src->cap + 1); n.runs = malloc((src->nruns ? src->nruns : 1) * sizeof(*src->runs));
    n.paras = malloc((src->npara ? src->npara : 1) * sizeof(*src->paras));
    if (!n.text || !n.runs || !n.paras) { free(n.text); free(n.runs); free(n.paras); return -ENOMEM; }
    memcpy(n.text, src->text, src->len + 1);
    if (src->nruns) memcpy(n.runs, src->runs, src->nruns * sizeof(*src->runs));
    memcpy(n.paras, src->paras, src->npara * sizeof(*src->paras));
    n.cruns = src->nruns ? src->nruns : 1; n.cpara = src->npara ? src->npara : 1;
    wp_free(dst);
    *dst = n;
    return 0;
}

/* ---- fonts and colours ---------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int icontains(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        for (; i < n; i++) {
            unsigned x = (unsigned char)s[i];
            if (!x) return 0;
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != (unsigned char)needle[i]) break;
        }
        if (i == n) return 1;
    }
    return 0;
}
unsigned wp_family_for_name(const char *name)
{
    static const char *const mono[] = {"courier", "consolas", "mono", "lucida console", "fixed", "terminal", "typewriter", "menlo"};
    static const char *const serif[] = {"times", "georgia", "cambria", "garamond", "palatino", "book antiqua", "century",
                                        "roman", "minion", "bookman"};
    if (icontains(name, "sans")) return WP_SANS;
    for (unsigned i = 0; i < sizeof(mono) / sizeof(*mono); i++) if (icontains(name, mono[i])) return WP_MONO;
    for (unsigned i = 0; i < sizeof(serif) / sizeof(*serif); i++) if (icontains(name, serif[i])) return WP_SERIF;
    if (icontains(name, "serif")) return WP_SERIF;
    return WP_SANS;
}
int wp_font_find(const struct wp_doc *d, const char *name)
{
    for (unsigned i = 0; i < d->nfonts; i++) if (ieq(d->fonts[i].name, name)) return (int)i;
    return -1;
}
int wp_font_add(struct wp_doc *d, const char *name, unsigned family)
{
    int at = wp_font_find(d, name);
    if (at >= 0) return at;
    if (d->nfonts >= WP_FONTS || !name[0]) return -1;
    struct wp_font *f = &d->fonts[d->nfonts];
    memset(f, 0, sizeof(*f));
    size_t n = strlen(name);
    if (n > sizeof(f->name) - 1) n = sizeof(f->name) - 1;
    memcpy(f->name, name, n);
    f->family = (uint8_t)(family < WP_FAMILIES ? family : WP_SANS);
    return (int)d->nfonts++;
}
int wp_color_find(const struct wp_doc *d, uint32_t rgb)
{
    for (unsigned i = 1; i < d->ncolors; i++) if (d->colors[i] == (rgb & 0xffffff)) return (int)i;
    return rgb ? -1 : 0;
}
static unsigned dist(uint32_t a, uint32_t b)
{
    unsigned s = 0;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        int x = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
        s += (unsigned)(x * x);
    }
    return s;
}
int wp_color_add(struct wp_doc *d, uint32_t rgb)
{
    rgb &= 0xffffff;
    int at = wp_color_find(d, rgb);
    if (at >= 0) return at;
    if (d->ncolors < WP_COLORS) { d->colors[d->ncolors] = rgb; return (int)d->ncolors++; }
    unsigned best = 1, bd = dist(d->colors[1], rgb);
    for (unsigned i = 2; i < d->ncolors; i++) if (dist(d->colors[i], rgb) < bd) { bd = dist(d->colors[i], rgb); best = i; }
    return (int)best;
}

/* ---- UTF-8 ----------------------------------------------------------------- */

unsigned wp_utf8_put(char *s, uint32_t ch)
{
    if (ch < 0x80) { s[0] = (char)ch; return 1; }
    if (ch < 0x800) { s[0] = (char)(0xc0 | (ch >> 6)); s[1] = (char)(0x80 | (ch & 63)); return 2; }
    if (ch < 0x10000) { s[0] = (char)(0xe0 | (ch >> 12)); s[1] = (char)(0x80 | ((ch >> 6) & 63)); s[2] = (char)(0x80 | (ch & 63)); return 3; }
    s[0] = (char)(0xf0 | (ch >> 18)); s[1] = (char)(0x80 | ((ch >> 12) & 63));
    s[2] = (char)(0x80 | ((ch >> 6) & 63)); s[3] = (char)(0x80 | (ch & 63));
    return 4;
}
/* Length of the valid UTF-8 sequence at s (at most n bytes), or 0. */
static unsigned utf8_seq(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    unsigned len = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
    if (c >= 0xf5 || !len || len > n) return 0;
    uint32_t v = c & (0xffu >> (len + 1));
    for (unsigned i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        v = v << 6 | (s[i] & 63);
    }
    if ((len == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (len == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
    *cp = v;
    return len;
}
size_t wp_prev(const struct wp_doc *d, size_t at)
{
    if (at > d->len) at = d->len;
    if (!at) return 0;
    at--;
    while (at && ((unsigned char)d->text[at] & 0xc0) == 0x80) at--;
    return at;
}
size_t wp_next(const struct wp_doc *d, size_t at)
{
    if (at >= d->len) return d->len;
    at++;
    while (at < d->len && ((unsigned char)d->text[at] & 0xc0) == 0x80) at++;
    return at;
}
uint32_t wp_cp1252(unsigned b)
{
    static const uint16_t t[32] = {0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
                                   0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178};
    return b >= 0x80 && b < 0xa0 ? t[b - 0x80] : b;
}
/* Writes the sanitised text to out (NULL: only counts) and returns its length. */
static size_t sanitize_into(const char *in, size_t n, char *out)
{
    size_t w = 0;
    char tmp[4];
    for (size_t i = 0; i < n;) {
        uint32_t cp; unsigned k = utf8_seq((const unsigned char *)in + i, n - i, &cp);
        if (!k) { if (out) memcpy(out + w, "\xef\xbf\xbd", 3); w += 3; i++; continue; }
        i += k;
        if (cp == '\r') { if (i < n && in[i] == '\n') i++; cp = '\n'; }
        if ((cp < 32 && cp != '\n' && cp != '\t' && cp != WP_SOFT_BREAK) || cp == 127) continue;
        unsigned m = wp_utf8_put(tmp, cp);
        if (out) memcpy(out + w, tmp, m);
        w += m;
    }
    return w;
}
int wp_sanitize(const char *in, size_t n, char **out, size_t *len)
{
    size_t w = sanitize_into(in, n, NULL);
    char *o = malloc(w + 1);
    if (!o) return -ENOMEM;
    sanitize_into(in, n, o);
    o[w] = 0; *out = o; *len = w;
    return 0;
}

/* ---- locating --------------------------------------------------------------- */

unsigned wp_para_at(const struct wp_doc *d, size_t at, size_t *start, size_t *end)
{
    size_t s = 0;
    for (unsigned i = 0; i < d->npara; i++) {
        size_t e = s + d->paras[i].len;
        if (i == d->npara - 1 || at < e) {
            if (start) *start = s;
            if (end) *end = i == d->npara - 1 ? e : e - 1;
            return i;
        }
        s = e;
    }
    return 0;
}
/* Index of the run holding byte `at` (nruns when at == len) and its start. */
static unsigned run_find(const struct wp_doc *d, size_t at, size_t *start)
{
    size_t s = 0;
    for (unsigned i = 0; i < d->nruns; i++) {
        if (at < s + d->runs[i].len) { if (start) *start = s; return i; }
        s += d->runs[i].len;
    }
    if (start) *start = s;
    return d->nruns;
}
struct wp_cattr wp_attr_of(const struct wp_doc *d, size_t at)
{
    if (!d->nruns) return wp_default_cattr();
    unsigned i = run_find(d, at, NULL);
    return d->runs[i < d->nruns ? i : d->nruns - 1].a;
}
struct wp_cattr wp_attr_at(const struct wp_doc *d, size_t at)
{
    if (!d->nruns) return wp_default_cattr();
    return wp_attr_of(d, at ? at - 1 : 0);
}
struct wp_pattr wp_pattr_at(const struct wp_doc *d, size_t at)
{ return d->paras[wp_para_at(d, at, NULL, NULL)].a; }

/* ---- runs -------------------------------------------------------------------- */

/* Makes `at` a run boundary and returns the index of the run starting there
 * (nruns at the end). Needs one spare slot (reserved by the caller). */
static unsigned run_split(struct wp_doc *d, size_t at)
{
    size_t s;
    unsigned i = run_find(d, at, &s);
    if (i == d->nruns || s == at) return i;
    memmove(d->runs + i + 1, d->runs + i, (d->nruns - i) * sizeof(*d->runs));
    d->runs[i + 1] = d->runs[i];
    d->runs[i].len = (uint32_t)(at - s);
    d->runs[i + 1].len -= d->runs[i].len;
    d->nruns++;
    return i + 1;
}
/* Joins equal neighbours inside runs [from-1, to] and drops empty runs. */
static void run_merge(struct wp_doc *d, unsigned from, unsigned to)
{
    unsigned i = from ? from - 1 : 0;
    if (to >= d->nruns) to = d->nruns ? d->nruns - 1 : 0;
    while (i < d->nruns && i <= to + 1) {
        if (!d->runs[i].len) {
            memmove(d->runs + i, d->runs + i + 1, (d->nruns - i - 1) * sizeof(*d->runs));
            d->nruns--; if (to) to--;
        } else if (i + 1 < d->nruns && wp_cattr_eq(&d->runs[i].a, &d->runs[i + 1].a)) {
            d->runs[i].len += d->runs[i + 1].len;
            memmove(d->runs + i + 1, d->runs + i + 2, (d->nruns - i - 2) * sizeof(*d->runs));
            d->nruns--; if (to) to--;
        } else i++;
    }
}

/* ---- editing ------------------------------------------------------------------ */

int wp_insert(struct wp_doc *d, size_t at, const char *s, size_t n, const struct wp_cattr *attr)
{
    if (at > d->len) return -EINVAL;
    if (!n) return 0;
    unsigned k = 0;
    for (size_t i = 0; i < n; i++) k += s[i] == '\n';
    int r = reserve(d, n, 2, k);
    if (r) return r;
    struct wp_cattr a = attr ? *attr : wp_attr_at(d, at);
    /* paragraphs */
    size_t ps; unsigned p = wp_para_at(d, at, &ps, NULL);
    struct wp_para old = d->paras[p];
    size_t off = at - ps;
    if (!k) d->paras[p].len += (uint32_t)n;
    else {
        memmove(d->paras + p + 1 + k, d->paras + p + 1, (d->npara - p - 1) * sizeof(*d->paras));
        const char *seg = s; unsigned idx = 0;
        for (size_t i = 0; i < n; i++) {
            if (s[i] != '\n') continue;
            size_t seglen = (size_t)(s + i - seg);
            d->paras[p + idx] = (struct wp_para){(uint32_t)((idx ? 0 : off) + seglen + 1), old.a};
            idx++; seg = s + i + 1;
        }
        d->paras[p + k] = (struct wp_para){(uint32_t)((size_t)(s + n - seg) + (old.len - off)), old.a};
        d->npara += k;
    }
    /* text */
    memmove(d->text + at + n, d->text + at, d->len - at);
    memcpy(d->text + at, s, n);
    d->len += n; d->text[d->len] = 0;
    /* runs */
    unsigned b = run_split(d, at);
    memmove(d->runs + b + 1, d->runs + b, (d->nruns - b) * sizeof(*d->runs));
    d->runs[b] = (struct wp_run){(uint32_t)n, a};
    d->nruns++;
    run_merge(d, b, b);
    d->revision++;
    return 0;
}

int wp_append(struct wp_doc *d, const char *s, size_t n, const struct wp_cattr *attr)
{
    if (!n) return 0;
    unsigned k = 0;
    for (size_t i = 0; i < n; i++) k += s[i] == '\n';
    int r = reserve(d, n, 1, k);
    if (r) return r;
    memcpy(d->text + d->len, s, n);
    d->len += n; d->text[d->len] = 0;
    const char *seg = s;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n') continue;
        d->paras[d->npara - 1].len += (uint32_t)(s + i + 1 - seg);
        d->paras[d->npara] = (struct wp_para){0, d->paras[d->npara - 1].a};
        d->npara++; seg = s + i + 1;
    }
    d->paras[d->npara - 1].len += (uint32_t)(s + n - seg);
    if (d->nruns && wp_cattr_eq(&d->runs[d->nruns - 1].a, attr)) d->runs[d->nruns - 1].len += (uint32_t)n;
    else d->runs[d->nruns++] = (struct wp_run){(uint32_t)n, *attr};
    d->revision++;
    return 0;
}

int wp_delete(struct wp_doc *d, size_t from, size_t to)
{
    if (from > to || to > d->len) return -EINVAL;
    if (from == to) return 0;
    int r = reserve(d, 0, 2, 0);
    if (r) return r;
    size_t s0, s1, e1; unsigned p0 = wp_para_at(d, from, &s0, NULL), p1 = wp_para_at(d, to, &s1, NULL);
    e1 = s1 + d->paras[p1].len;
    d->paras[p1].len = (uint32_t)((from - s0) + (e1 - to));
    if (p0 != p1) {
        memmove(d->paras + p0, d->paras + p1, (d->npara - p1) * sizeof(*d->paras));
        d->npara -= p1 - p0;
    }
    unsigned bf = run_split(d, from), bt = run_split(d, to);
    memmove(d->runs + bf, d->runs + bt, (d->nruns - bt) * sizeof(*d->runs));
    d->nruns -= bt - bf;
    run_merge(d, bf, bf);
    memmove(d->text + from, d->text + to, d->len - to);
    d->len -= to - from; d->text[d->len] = 0;
    d->revision++;
    return 0;
}

int wp_style_chars(struct wp_doc *d, size_t from, size_t to, const struct wp_cstyle *c)
{
    if (from > to || to > d->len) return -EINVAL;
    if (from == to) return 0;
    int r = reserve(d, 0, 2, 0);
    if (r) return r;
    unsigned bf = run_split(d, from), bt = run_split(d, to);
    for (unsigned i = bf; i < bt; i++) {
        struct wp_cattr *a = &d->runs[i].a;
        if (c->font >= 0 && (unsigned)c->font < d->nfonts) a->font = (uint8_t)c->font;
        if (c->size) a->size = (uint16_t)(c->size < WP_SIZE_MIN ? WP_SIZE_MIN : c->size > WP_SIZE_MAX ? WP_SIZE_MAX : c->size);
        if (c->color >= 0 && (unsigned)c->color < d->ncolors) a->color = (uint8_t)c->color;
        a->flags = (uint8_t)((a->flags | c->set) & ~c->clear);
    }
    run_merge(d, bf, bt);
    d->revision++;
    return 0;
}

static void para_range(const struct wp_doc *d, size_t from, size_t to, unsigned *p0, unsigned *p1)
{
    size_t s1;
    *p0 = wp_para_at(d, from, NULL, NULL);
    *p1 = wp_para_at(d, to, &s1, NULL);
    if (to > from && to == s1 && *p1 > *p0) (*p1)--;
}
int wp_style_paras(struct wp_doc *d, size_t from, size_t to, const struct wp_pstyle *st)
{
    if (from > to || to > d->len) return -EINVAL;
    unsigned p0, p1; para_range(d, from, to, &p0, &p1);
    for (unsigned i = p0; i <= p1; i++) {
        struct wp_pattr *a = &d->paras[i].a;
        const struct wp_pattr *v = &st->v;
        if (st->mask & WP_P_ALIGN) a->align = v->align > WP_RIGHT ? WP_LEFT : v->align;
        if (st->mask & WP_P_BULLET) a->bullet = v->bullet != 0;
        if (st->mask & WP_P_LEFT) a->left = v->left < 0 ? 0 : v->left > WP_INDENT_MAX ? WP_INDENT_MAX : v->left;
        if (st->mask & WP_P_FIRST) a->first = v->first < -WP_INDENT_MAX ? -WP_INDENT_MAX : v->first > WP_INDENT_MAX ? WP_INDENT_MAX : v->first;
        if (st->mask & WP_P_RIGHT) a->right = v->right < 0 ? 0 : v->right > WP_INDENT_MAX ? WP_INDENT_MAX : v->right;
        if (st->mask & WP_P_TABS) {
            a->ntabs = 0;
            for (unsigned k = 0; k < v->ntabs && k < WP_TABS; k++) {
                int32_t t = v->tabs[k];
                if (t <= 0 || t > WP_INDENT_MAX || (a->ntabs && t <= a->tabs[a->ntabs - 1])) continue;
                a->tabs[a->ntabs++] = t;
            }
        }
        /* The left margin always keeps the text of a hanging first line on the page. */
        if (a->left + a->first < 0) a->first = -a->left;
    }
    d->revision++;
    return 0;
}

void wp_char_summary(const struct wp_doc *d, size_t from, size_t to, struct wp_cmix *m)
{
    struct wp_cattr a;
    if (from >= to || !d->nruns) {
        a = from == to ? wp_attr_at(d, from) : wp_default_cattr();
        *m = (struct wp_cmix){a.font, a.size, a.color, !!(a.flags & WP_BOLD), !!(a.flags & WP_ITALIC),
                              !!(a.flags & WP_UNDERLINE), !!(a.flags & WP_STRIKE)};
        return;
    }
    size_t s; unsigned i = run_find(d, from, &s);
    a = d->runs[i].a;
    *m = (struct wp_cmix){a.font, a.size, a.color, !!(a.flags & WP_BOLD), !!(a.flags & WP_ITALIC),
                          !!(a.flags & WP_UNDERLINE), !!(a.flags & WP_STRIKE)};
    for (s += d->runs[i].len, i++; s < to && i < d->nruns; s += d->runs[i].len, i++) {
        const struct wp_cattr *b = &d->runs[i].a;
        if (m->font != b->font) m->font = -1;
        if (m->size != b->size) m->size = -1;
        if (m->color != b->color) m->color = -1;
        if (m->bold != !!(b->flags & WP_BOLD)) m->bold = -1;
        if (m->italic != !!(b->flags & WP_ITALIC)) m->italic = -1;
        if (m->underline != !!(b->flags & WP_UNDERLINE)) m->underline = -1;
        if (m->strike != !!(b->flags & WP_STRIKE)) m->strike = -1;
    }
}
void wp_para_summary(const struct wp_doc *d, size_t from, size_t to, struct wp_pmix *m)
{
    unsigned p0, p1; para_range(d, from, to, &p0, &p1);
    m->align = d->paras[p0].a.align; m->bullet = d->paras[p0].a.bullet;
    for (unsigned i = p0 + 1; i <= p1; i++) {
        if (m->align != d->paras[i].a.align) m->align = -1;
        if (m->bullet != d->paras[i].a.bullet) m->bullet = -1;
    }
}

/* ---- slices -------------------------------------------------------------------- */

int wp_slice(struct wp_doc *dst, const struct wp_doc *src, size_t from, size_t to)
{
    if (from > to || to > src->len) return -EINVAL;
    struct wp_doc n;
    if (wp_init(&n)) return -ENOMEM;
    memcpy(n.fonts, src->fonts, sizeof(n.fonts)); n.nfonts = src->nfonts;
    memcpy(n.colors, src->colors, sizeof(n.colors)); n.ncolors = src->ncolors;
    unsigned p0 = wp_para_at(src, from, NULL, NULL), p1 = wp_para_at(src, to, NULL, NULL);
    int r = reserve(&n, to - from, (unsigned)src->nruns + 2, p1 - p0 + 1);
    if (r) { wp_free(&n); return r; }
    memcpy(n.text, src->text + from, to - from); n.len = to - from; n.text[n.len] = 0;
    size_t s = 0;
    for (unsigned i = 0; i < p0; i++) s += src->paras[i].len;
    n.npara = 0;
    for (unsigned i = p0; i <= p1; i++) {
        size_t e = s + src->paras[i].len, lo = s > from ? s : from, hi = e < to ? e : to;
        n.paras[n.npara++] = (struct wp_para){(uint32_t)(hi > lo ? hi - lo : 0), src->paras[i].a};
        s = e;
    }
    n.nruns = 0;
    size_t rs = 0;
    for (unsigned i = 0; i < src->nruns && rs < to; i++) {
        size_t re = rs + src->runs[i].len, lo = rs > from ? rs : from, hi = re < to ? re : to;
        if (hi > lo) n.runs[n.nruns++] = (struct wp_run){(uint32_t)(hi - lo), src->runs[i].a};
        rs = re;
    }
    wp_free(dst); *dst = n;
    return 0;
}

int wp_insert_slice(struct wp_doc *d, size_t at, const struct wp_doc *sl)
{
    if (at > d->len) return -EINVAL;
    if (!sl->len) return 0;
    unsigned k = sl->npara - 1;
    int r = reserve(d, sl->len, sl->nruns * 2 + 2, k);
    if (r) return r;
    int fmap[WP_FONTS]; int cmap[WP_COLORS];
    for (unsigned i = 0; i < sl->nfonts; i++) {
        int f = wp_font_add(d, sl->fonts[i].name, sl->fonts[i].family);
        fmap[i] = f >= 0 ? f : (int)sl->fonts[i].family;   /* the table is full: the family's built-in face */
    }
    for (unsigned i = 0; i < sl->ncolors; i++) cmap[i] = i ? wp_color_add(d, sl->colors[i]) : 0;
    unsigned p_at = wp_para_at(d, at, NULL, NULL);
    size_t pos = at, run_at = 0;
    for (unsigned i = 0; i < sl->nruns; i++) {
        struct wp_cattr a = sl->runs[i].a;
        a.font = (uint8_t)(a.font < sl->nfonts ? fmap[a.font] : 0);
        a.color = (uint8_t)(a.color < sl->ncolors ? cmap[a.color] : 0);
        r = wp_insert(d, pos, sl->text + run_at, sl->runs[i].len, &a);
        if (r) return r;   /* cannot happen: everything was reserved */
        pos += sl->runs[i].len; run_at += sl->runs[i].len;
    }
    for (unsigned i = 0; i < k; i++) d->paras[p_at + i].a = sl->paras[i].a;
    return 0;
}
