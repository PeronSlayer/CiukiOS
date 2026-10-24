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

struct wp_cattr wp_default_cattr(void) { return (struct wp_cattr){0, 0, 0, 0, WP_SIZE_DEFAULT, 0}; }
struct wp_pattr wp_default_pattr(void) { struct wp_pattr p; memset(&p, 0, sizeof(p)); return p; }
int wp_cattr_eq(const struct wp_cattr *a, const struct wp_cattr *b)
{ return !a->pic && !b->pic && a->font == b->font && a->flags == b->flags && a->color == b->color && a->size == b->size; }
int wp_pattr_eq(const struct wp_pattr *a, const struct wp_pattr *b)
{
    if (a->align != b->align || a->bullet != b->bullet || a->ntabs != b->ntabs || a->left != b->left ||
        a->first != b->first || a->right != b->right) return 0;
    for (unsigned i = 0; i < a->ntabs; i++) if (a->tabs[i] != b->tabs[i]) return 0;
    return 1;
}

static void pics_compact(struct wp_doc *d);
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
static void pics_release(struct wp_doc *d)
{
    for (unsigned i = 0; i < d->npics; i++) wp_pic_unref(d->pics[i]);
    free(d->pics);
    d->pics = NULL; d->npics = d->cpics = 0; d->pic_bytes = 0;
}
void wp_free(struct wp_doc *d)
{
    pics_release(d);
    free(d->text); free(d->runs); free(d->paras);
    memset(d, 0, sizeof(*d));
}
int wp_reset(struct wp_doc *d)
{
    uint64_t revision = d->revision + 1;
    pics_release(d);
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
    n.pics = NULL; n.cpics = 0;
    if (src->npics) {
        n.pics = malloc(src->npics * sizeof(*n.pics));
        if (!n.pics) { free(n.text); free(n.runs); free(n.paras); return -ENOMEM; }
        for (unsigned i = 0; i < src->npics; i++) { n.pics[i] = src->pics[i]; wp_pic_ref(src->pics[i]); }
        n.cpics = src->npics;
    }
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
        if ((cp < 32 && cp != '\n' && cp != '\t' && cp != WP_SOFT_BREAK) || cp == 127 || cp == 0xfffc) continue;   /* U+FFFC is reserved for pictures */
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
/* The text attribute: a picture character's own format without the picture, so that typed text never becomes a picture. */
struct wp_cattr wp_attr_of(const struct wp_doc *d, size_t at)
{
    if (!d->nruns) return wp_default_cattr();
    unsigned i = run_find(d, at, NULL);
    struct wp_cattr a = d->runs[i < d->nruns ? i : d->nruns - 1].a;
    a.pic = 0;
    return a;
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
        if (st->mask & WP_P_ALIGN) a->align = v->align > WP_JUSTIFY ? WP_LEFT : v->align;
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
    for (unsigned i = 0; i < n.nruns; i++) {   /* the slice shares the pictures it uses */
        unsigned id = n.runs[i].a.pic;
        if (!id) continue;
        struct wp_pic *p = id <= src->npics ? src->pics[id - 1] : NULL;
        int at = -1;
        if (p) { wp_pic_ref(p); at = wp_pic_attach(&n, p); }
        if (at < 0) { wp_free(&n); return at < 0 && p ? at : -EINVAL; }
        n.runs[i].a.pic = (uint16_t)at;
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
    size_t need = 0;
    for (unsigned i = 0; i < sl->npics; i++) need += (size_t)sl->pics[i]->len + sl->pics[i]->objlen;
    if (d->npics + sl->npics > WP_PICS_MAX || d->pic_bytes + need > WP_PIC_BYTES) { pics_compact(d); if (d->npics + sl->npics > WP_PICS_MAX || d->pic_bytes + need > WP_PIC_BYTES) return -EFBIG; }
    unsigned p_at = wp_para_at(d, at, NULL, NULL);
    size_t pos = at, run_at = 0;
    for (unsigned i = 0; i < sl->nruns; i++) {
        struct wp_cattr a = sl->runs[i].a;
        if (a.pic) {
            struct wp_pic *pic = a.pic <= sl->npics ? sl->pics[a.pic - 1] : NULL;
            int id = -1;
            if (pic) { wp_pic_ref(pic); id = wp_pic_attach(d, pic); }
            if (id < 0) a.pic = 0;   /* cannot happen: the room was checked */
            else a.pic = (uint16_t)id;
        }
        a.font = (uint8_t)(a.font < sl->nfonts ? fmap[a.font] : 0);
        a.color = (uint8_t)(a.color < sl->ncolors ? cmap[a.color] : 0);
        r = wp_insert(d, pos, sl->text + run_at, sl->runs[i].len, &a);
        if (r) return r;   /* cannot happen: everything was reserved */
        pos += sl->runs[i].len; run_at += sl->runs[i].len;
    }
    for (unsigned i = 0; i < k; i++) d->paras[p_at + i].a = sl->paras[i].a;
    return 0;
}

/* ---- pictures ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[1] << 8 | p[0]; }

unsigned wp_pic_sniff(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    *w = *h = 0;
    if (n >= 24 && !memcmp(d, png, 8) && !memcmp(d + 12, "IHDR", 4)) { *w = be32(d + 16); *h = be32(d + 20); return WP_PIC_PNG; }
    if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xff) { i++; continue; }
            unsigned m = d[i + 1];
            if (m == 0xff) { i++; continue; }
            if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01 || m == 0) { i += 2; continue; }
            if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) { *h = (uint32_t)d[i + 5] << 8 | d[i + 6]; *w = (uint32_t)d[i + 7] << 8 | d[i + 8]; return WP_PIC_JPEG; }
            i += 2 + ((size_t)d[i + 2] << 8 | d[i + 3]);
        }
        return WP_PIC_JPEG;
    }
    if (n >= 16) {   /* a DIB: the BITMAPINFOHEADER (or the older core header) */
        uint32_t size = le32(d);
        if (size == 12) { *w = le16(d + 4); *h = le16(d + 6); return WP_PIC_DIB; }
        if (size >= 40 && size <= 256 && n >= 40) {
            int32_t hh = (int32_t)le32(d + 8);
            *w = le32(d + 4); *h = hh < 0 ? (uint32_t)-hh : (uint32_t)hh;
            if (le16(d + 12) == 1 && *w && *w < 65536 && *h && *h < 65536) return WP_PIC_DIB;
            *w = *h = 0;
        }
    }
    return 0;
}
uint8_t *wp_bmp_to_dib(const uint8_t *f, size_t n, size_t *out)
{
    if (n < 14 + 12 || f[0] != 'B' || f[1] != 'M') return NULL;
    uint8_t *o = malloc(n - 14);
    if (!o) return NULL;
    memcpy(o, f + 14, n - 14); *out = n - 14;
    return o;
}
/* A BMP file from a DIB: the 14-byte file header with the pixel offset computed from the header, palette and masks. */
uint8_t *wp_dib_to_bmp(const uint8_t *dib, size_t n, size_t *out)
{
    if (n < 12) return NULL;
    uint32_t size = le32(dib), bits, used = 0, comp = 0;
    if (size == 12) bits = le16(dib + 10);
    else if (size >= 40 && n >= 40) { bits = le16(dib + 14); comp = le32(dib + 16); used = le32(dib + 32); }
    else return NULL;
    size_t pal = 0;
    if (bits && bits <= 8) pal = (size_t)(used && used <= (1u << bits) ? used : (1u << bits)) * (size == 12 ? 3 : 4);
    if (size == 40 && (comp == 3 || comp == 6)) pal += comp == 3 ? 12 : 16;
    size_t off = 14 + size + pal;
    if (off > n + 14) return NULL;
    uint8_t *o = malloc(n + 14);
    if (!o) return NULL;
    o[0] = 'B'; o[1] = 'M';
    uint32_t total = (uint32_t)(n + 14);
    for (unsigned i = 0; i < 4; i++) { o[2 + i] = (uint8_t)(total >> (8 * i)); o[10 + i] = (uint8_t)((uint32_t)off >> (8 * i)); o[6 + i] = 0; }
    memcpy(o + 14, dib, n); *out = n + 14;
    return o;
}

struct wp_pic *wp_pic_new(unsigned kind, const void *data, size_t len)
{
    struct wp_pic *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->refs = 1; p->kind = (uint8_t)kind; p->scalex = p->scaley = 100;
    if (len) {
        p->data = malloc(len);
        if (!p->data) { free(p); return NULL; }
        memcpy(p->data, data, len);
        p->len = (uint32_t)len;
        uint32_t w, h;
        if (kind == WP_PIC_PNG || kind == WP_PIC_JPEG || kind == WP_PIC_DIB) { wp_pic_sniff(p->data, len, &w, &h); if (w <= 65535 && h <= 65535) { p->pw = w; p->ph = h; } }   /* an absurd header is no size */
    }
    return p;
}
void wp_pic_ref(struct wp_pic *p) { if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED); }
void wp_pic_unref(struct wp_pic *p)
{
    if (!p || __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(p->data); free(p->objdata); free(p->px); free(p);
}
void wp_pic_size(const struct wp_pic *p, int *w, int *h)
{
    int64_t gw = p->goalw, gh = p->goalh;
    if (gw <= 0 || gh <= 0) {
        if (p->pw && p->ph) { gw = (int64_t)p->pw * 15; gh = (int64_t)p->ph * 15; }
        else gw = gh = 1440;
    }
    int64_t sx = p->scalex ? p->scalex : 100, sy = p->scaley ? p->scaley : 100;
    gw = gw * sx / 100; gh = gh * sy / 100;
    if (gw < 15) gw = 15; if (gh < 15) gh = 15;
    if (gw > WP_INDENT_MAX) gw = WP_INDENT_MAX; if (gh > WP_INDENT_MAX) gh = WP_INDENT_MAX;
    *w = (int)gw; *h = (int)gh;
}

static void pics_compact(struct wp_doc *d)
{
    if (!d->npics) return;
    unsigned old = d->npics;
    unsigned *map = calloc(old + 1, sizeof(*map));
    if (!map) return;
    for (unsigned i = 0; i < d->nruns; i++) if (d->runs[i].a.pic && d->runs[i].a.pic <= old) map[d->runs[i].a.pic] = 1;
    unsigned n = 0; size_t bytes = 0;
    for (unsigned i = 1; i <= old; i++) {
        struct wp_pic *p = d->pics[i - 1];
        if (map[i]) { map[i] = ++n; d->pics[n - 1] = p; bytes += (size_t)p->len + p->objlen; }
        else wp_pic_unref(p);
    }
    d->npics = n; d->pic_bytes = bytes;
    for (unsigned i = 0; i < d->nruns; i++) if (d->runs[i].a.pic) d->runs[i].a.pic = (uint16_t)(d->runs[i].a.pic <= old ? map[d->runs[i].a.pic] : 0);
    free(map);
}
int wp_pic_attach(struct wp_doc *d, struct wp_pic *p)
{
    for (unsigned i = 0; i < d->npics; i++) if (d->pics[i] == p) { wp_pic_unref(p); return (int)i + 1; }
    size_t sz = (size_t)p->len + p->objlen;
    if (d->npics >= WP_PICS_MAX || d->pic_bytes + sz > WP_PIC_BYTES) {
        pics_compact(d);
        if (d->npics >= WP_PICS_MAX || d->pic_bytes + sz > WP_PIC_BYTES) { wp_pic_unref(p); return -EFBIG; }
    }
    if (d->npics == d->cpics) {
        void *q = d->pics; unsigned c = d->cpics;
        if (grow(&q, &c, (size_t)d->npics + 1, sizeof(*d->pics))) { wp_pic_unref(p); return -ENOMEM; }
        d->pics = q; d->cpics = c;
    }
    d->pics[d->npics++] = p; d->pic_bytes += sz;
    return (int)d->npics;
}
int wp_insert_pic(struct wp_doc *d, size_t at, struct wp_pic *p, const struct wp_cattr *attr)
{
    if (at > d->len) { wp_pic_unref(p); return -EINVAL; }
    struct wp_cattr a = attr ? *attr : wp_attr_at(d, at);
    int id = wp_pic_attach(d, p);
    if (id < 0) return id;
    a.pic = (uint16_t)id;
    return wp_insert(d, at, WP_PIC_MARK, WP_PIC_MARK_LEN, &a);
}
struct wp_pic *wp_pic_at(const struct wp_doc *d, size_t at)
{
    if (at + WP_PIC_MARK_LEN > d->len || memcmp(d->text + at, WP_PIC_MARK, WP_PIC_MARK_LEN)) return NULL;
    size_t s;
    unsigned i = run_find(d, at, &s);
    if (i >= d->nruns) return NULL;
    unsigned id = d->runs[i].a.pic;
    return id && id <= d->npics ? d->pics[id - 1] : NULL;
}
unsigned wp_pic_count_used(const struct wp_doc *d)
{
    unsigned n = 0;
    for (unsigned i = 0; i < d->nruns; i++) if (d->runs[i].a.pic) n += d->runs[i].len / WP_PIC_MARK_LEN;
    return n;
}
