/* WordPad document model, RTF, plain text and layout host tests (G28).
 * SPDX-License-Identifier: MIT */
#include "../../../apps/wordpad/rich.h"
#include "../../../apps/wordpad/layout.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fake text: every character advances size_hp * 0.5 pixels (a 10 pt character is 10 pixels wide). */
static int fw(void *ctx, unsigned family, unsigned flags, unsigned size, const char *s, size_t n)
{
    (void)ctx; (void)family; (void)flags;
    int chars = 0;
    for (size_t i = 0; i < n; i++) chars += ((unsigned char)s[i] & 0xc0) != 0x80;
    return chars * (int)size * 32;
}
static void fv(void *ctx, unsigned family, unsigned flags, unsigned size, int *a, int *d)
{
    (void)ctx; (void)family; (void)flags;
    int px = (int)size * 2 / 3;
    *a = px * 3 / 4; *d = px / 4 + 1;
}
static const struct wp_text_ops OPS = {NULL, fw, fv};

static void check(const struct wp_doc *d)
{
    size_t sum = 0;
    for (unsigned i = 0; i < d->nruns; i++) {
        assert(d->runs[i].len);
        if (i) assert(!wp_cattr_eq(&d->runs[i - 1].a, &d->runs[i].a));
        assert(d->runs[i].a.font < d->nfonts && d->runs[i].a.color < d->ncolors);
        sum += d->runs[i].len;
    }
    assert(sum == d->len);
    assert((d->nruns == 0) == (d->len == 0));
    sum = 0;
    assert(d->npara >= 1);
    unsigned lf = 0;
    for (size_t i = 0; i < d->len; i++) lf += d->text[i] == '\n';
    assert(lf + 1 == d->npara);
    for (unsigned i = 0; i < d->npara; i++) {
        sum += d->paras[i].len;
        if (i + 1 < d->npara) assert(d->text[sum - 1] == '\n');
    }
    assert(sum == d->len);
    assert(d->text[d->len] == 0);
}
static void insert(struct wp_doc *d, size_t at, const char *s)
{ assert(!wp_insert(d, at, s, strlen(s), NULL)); check(d); }
static int same(const struct wp_doc *a, const struct wp_doc *b)
{
    if (a->len != b->len || memcmp(a->text, b->text, a->len) || a->nruns != b->nruns || a->npara != b->npara) return 0;
    for (unsigned i = 0; i < a->nruns; i++) {
        if (a->runs[i].len != b->runs[i].len) return 0;
        const struct wp_cattr *x = &a->runs[i].a, *y = &b->runs[i].a;
        if (x->flags != y->flags || x->size != y->size) return 0;
        if (strcmp(a->fonts[x->font].name, b->fonts[y->font].name) || a->fonts[x->font].family != b->fonts[y->font].family) return 0;
        if (a->colors[x->color] != b->colors[y->color]) return 0;
    }
    for (unsigned i = 0; i < a->npara; i++)
        if (a->paras[i].len != b->paras[i].len || !wp_pattr_eq(&a->paras[i].a, &b->paras[i].a)) return 0;
    return 1;
}

/* ---- model ---------------------------------------------------------------------- */
static void test_model(void)
{
    struct wp_doc d; assert(!wp_init(&d)); check(&d);
    assert(d.len == 0 && d.npara == 1 && d.nfonts == 3 && !strcmp(d.fonts[0].name, "Arial"));
    insert(&d, 0, "Hello world");
    assert(d.nruns == 1 && d.paras[0].len == 11);
    struct wp_cstyle bold = {-1, 0, -1, WP_BOLD, 0};
    assert(!wp_style_chars(&d, 0, 5, &bold)); check(&d);
    assert(d.nruns == 2 && (d.runs[0].a.flags & WP_BOLD) && d.runs[0].len == 5);
    struct wp_cmix m; wp_char_summary(&d, 0, 11, &m);
    assert(m.bold == -1 && m.size == 20 && m.font == 0 && m.italic == 0);
    wp_char_summary(&d, 0, 5, &m); assert(m.bold == 1);
    wp_char_summary(&d, 8, 8, &m); assert(m.bold == 0);          /* the caret takes the previous character's format */
    wp_char_summary(&d, 5, 5, &m); assert(m.bold == 1);
    /* typing inside the bold run extends it; at its end it takes the same format */
    insert(&d, 3, "X"); assert(d.runs[0].len == 6 && d.nruns == 2);
    struct wp_cstyle plain = {-1, 0, -1, 0, WP_BOLD};
    assert(!wp_style_chars(&d, 0, 12, &plain)); check(&d); assert(d.nruns == 1);   /* runs merge again */
    /* paragraphs */
    insert(&d, 6, "\nSecond\nThird");
    assert(d.npara == 3);
    struct wp_pstyle center = {WP_P_ALIGN, {WP_CENTER, 0, 0, 0, 0, 0, 0, {0}}};
    assert(!wp_style_paras(&d, 8, 9, &center));
    assert(d.paras[1].a.align == WP_CENTER && d.paras[0].a.align == WP_LEFT && d.paras[2].a.align == WP_LEFT);
    insert(&d, 9, "\n");   /* splitting copies the attributes */
    assert(d.npara == 4 && d.paras[1].a.align == WP_CENTER && d.paras[2].a.align == WP_CENTER);
    struct wp_pmix pm; wp_para_summary(&d, 0, d.len, &pm); assert(pm.align == -1);
    wp_para_summary(&d, 8, 11, &pm); assert(pm.align == WP_CENTER);
    /* a selection ending at the start of a paragraph does not include it */
    size_t s1, e1; wp_para_at(&d, 8, &s1, &e1);
    wp_para_summary(&d, 0, s1, &pm); assert(pm.align == WP_LEFT);
    wp_para_summary(&d, 0, e1 + 1, &pm); assert(pm.align == -1);
    /* deleting a paragraph mark keeps the attributes of the surviving mark (the following paragraph's) */
    size_t nl = 0; for (size_t i = 0; i < d.len; i++) if (d.text[i] == '\n') { nl = i; break; }
    assert(!wp_delete(&d, nl, nl + 1)); check(&d);
    assert(d.npara == 3 && d.paras[0].a.align == WP_CENTER);
    /* bounds */
    assert(wp_insert(&d, d.len + 1, "x", 1, NULL) == -EINVAL);
    assert(wp_delete(&d, 5, 2) == -EINVAL && wp_delete(&d, 0, d.len + 1) == -EINVAL);
    assert(!wp_delete(&d, 0, d.len)); check(&d); assert(d.len == 0 && d.npara == 1);
    /* fonts and colours */
    int f = wp_font_add(&d, "Verdana", WP_SANS); assert(f == 3 && wp_font_find(&d, "VERDANA") == 3);
    assert(wp_font_add(&d, "verdana", WP_SANS) == 3);
    assert(wp_family_for_name("Courier New") == WP_MONO && wp_family_for_name("Georgia") == WP_SERIF &&
           wp_family_for_name("Microsoft Sans Serif") == WP_SANS && wp_family_for_name("Tahoma") == WP_SANS &&
           wp_family_for_name("Times New Roman") == WP_SERIF && wp_family_for_name("Lucida Console") == WP_MONO);
    int c = wp_color_add(&d, 0x123456); assert(c == 17 && wp_color_find(&d, 0x123456) == 17 && wp_color_find(&d, 0) == 1);
    for (unsigned i = 0; i < 40; i++) wp_color_add(&d, 0x010101 * (i + 20));
    assert(d.ncolors == WP_COLORS);
    wp_free(&d);
}

static void test_slices(void)
{
    struct wp_doc a, s, b; assert(!wp_init(&a) && !wp_init(&s) && !wp_init(&b));
    insert(&a, 0, "one two\nthree four\nfive");
    struct wp_cstyle it = {1, 24, 3, WP_ITALIC, 0};
    assert(!wp_style_chars(&a, 4, 7, &it)); check(&a);
    struct wp_pstyle right = {WP_P_ALIGN | WP_P_BULLET, {WP_RIGHT, 1, 0, 0, 0, 0, 0, {0}}};
    assert(!wp_style_paras(&a, 9, 12, &right));
    assert(!wp_slice(&s, &a, 4, 14)); check(&s);
    assert(s.len == 10 && s.npara == 2 && !memcmp(s.text, "two\nthree ", 10));
    assert(s.runs[0].a.flags & WP_ITALIC && s.paras[1].a.align == WP_RIGHT);
    /* paste it into another document that has no such font: tables are remapped */
    struct wp_cstyle fnt = {-1, 0, -1, 0, 0};
    (void)fnt;
    insert(&b, 0, "AB");
    assert(!wp_insert_slice(&b, 1, &s)); check(&b);
    assert(b.len == 12 && !memcmp(b.text, "Atwo\nthree B", 12) && b.npara == 2);
    assert(b.paras[0].a.align == WP_LEFT);          /* the first paragraph's mark is the slice's first mark */
    struct wp_cattr at = wp_attr_of(&b, 1); assert((at.flags & WP_ITALIC) && at.size == 24);
    assert(!strcmp(b.fonts[at.font].name, "Times New Roman") && b.colors[at.color] == a.colors[3]);
    /* slice of an empty range and of a whole paragraph structure */
    assert(!wp_slice(&s, &a, 3, 3) && s.len == 0 && s.npara == 1);
    assert(!wp_slice(&s, &a, 0, a.len)); check(&s); assert(same(&s, &a));
    wp_free(&a); wp_free(&s); wp_free(&b);
}

static void test_limits(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    size_t chunk = 1u << 20; char *big = malloc(chunk);
    memset(big, 'a', chunk); for (size_t i = 79; i < chunk; i += 80) big[i] = '\n';
    for (unsigned i = 0; i < 8; i++) assert(!wp_insert(&d, d.len, big, chunk, NULL));
    check(&d);
    assert(d.len == WP_LIMIT);
    assert(wp_insert(&d, 5, "x", 1, NULL) == -EFBIG); check(&d);
    assert(!wp_delete(&d, 1000, 5000000)); check(&d);
    assert(!wp_insert(&d, 10, "x", 1, NULL)); check(&d);
    free(big); wp_free(&d);
}

static void test_sanitize(void)
{
    char *o; size_t n;
    assert(!wp_sanitize("a\r\nb\rc\x01\x7f\td", 10, &o, &n));
    assert(n == 7 && !memcmp(o, "a\nb\nc\td", 7)); free(o);
    assert(!wp_sanitize("ok\xc3\xa8\xff\xc3", 6, &o, &n));
    assert(!strcmp(o, "ok\xc3\xa8\xef\xbf\xbd\xef\xbf\xbd")); free(o);
    assert(!wp_sanitize("\xed\xa0\x80", 3, &o, &n) && n == 9); free(o);   /* a surrogate is not UTF-8 */
}

/* ---- RTF ------------------------------------------------------------------------- */
static int read_rtf(struct wp_doc *d, const char *s, struct wp_report *rep)
{ return wp_rtf_read(d, s, strlen(s), rep); }

static const char *WORDPAD_SAMPLE =
    "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033{\\fonttbl{\\f0\\fswiss\\fcharset0 Arial;}{\\f1\\froman\\fcharset0 Times New Roman;}"
    "{\\f2\\fmodern\\fcharset0 Courier New;}{\\f3\\fnil\\fcharset2 Symbol;}}\r\n"
    "{\\colortbl ;\\red255\\green0\\blue0;\\red0\\green0\\blue255;}\r\n"
    "\\viewkind4\\uc1\\pard\\qc\\f1\\fs32 Title\\par\r\n"
    "\\pard\\f0\\fs20 Plain \\b bold\\b0  \\i italic\\i0  \\ul under\\ulnone  \\cf1 red\\cf0 .\\par\r\n"
    "\\pard{\\pntext\\f3\\'B7\\tab}{\\*\\pn\\pnlvlblt\\pnf3\\pnindent0{\\pntxtb\\'B7}}\\fi-720\\li720 first bullet\\par\r\n"
    "{\\pntext\\f3\\'B7\\tab}second bullet\\par\r\n"
    "\\pard\\li360\\ri720\\tx1440\\tx2880 a\\tab b\\tab c\\line next\\par\r\n"
    "\\pard\\f2 caf\\'e9 \\u8364? \\'80 \\u-10179?\\u-8704? done\\par\r\n"
    "}";

static void test_rtf_read(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_report rep;
    assert(!read_rtf(&d, WORDPAD_SAMPLE, &rep)); check(&d);
    assert(rep.skipped_pictures == 0 && rep.skipped_objects == 0 && rep.unknown_charset == 0);
    assert(d.npara == 6 && !memcmp(d.text, "Title\nPlain bold italic under red.\n", 35));
    assert(d.paras[0].a.align == WP_CENTER && d.paras[1].a.align == WP_LEFT);
    struct wp_cattr a = wp_attr_of(&d, 0);
    assert(!strcmp(d.fonts[a.font].name, "Times New Roman") && d.fonts[a.font].family == WP_SERIF && a.size == 32);
    a = wp_attr_of(&d, 6); assert(!strcmp(d.fonts[a.font].name, "Arial") && a.size == 20 && !a.flags);
    a = wp_attr_of(&d, 12); assert((a.flags & WP_BOLD) && d.text[12] == 'b');
    size_t red = strstr(d.text, "red") - d.text;
    a = wp_attr_of(&d, red); assert(d.colors[a.color] == 0xff0000);
    a = wp_attr_of(&d, red + 3); assert(a.color == 0);
    size_t under = strstr(d.text, "under") - d.text; assert(wp_attr_of(&d, under).flags & WP_UNDERLINE);
    /* both bullet forms */
    assert(d.paras[2].a.bullet && d.paras[2].a.left == 720 && d.paras[2].a.first == -720);
    assert(!memcmp(d.text + (strstr(d.text, "first") - d.text), "first bullet\n", 13));
    assert(d.paras[3].a.bullet && d.paras[3].a.left == 720 && d.paras[3].a.first == -720);   /* paragraph properties persist until \pard */
    assert(!strstr(d.text, "\xc2\xb7") && !strstr(d.text, "\t" "second"));
    /* indents, tab stops and soft break */
    assert(d.paras[4].a.left == 360 && d.paras[4].a.right == 720 && d.paras[4].a.ntabs == 2 && d.paras[4].a.tabs[1] == 2880);
    assert(strstr(d.text, "a\tb\tc\vnext\n"));
    /* escapes: cp1252 e-acute, euro through \u and \'80, a supplementary character through two surrogates */
    assert(strstr(d.text, "caf\xc3\xa9 \xe2\x82\xac \xe2\x82\xac \xf0\x9f\x98\x80 done"));   /* \u-10179?\u-8704? is the pair D83D DE00: U+1F600 */
    /* the last \par does not leave an empty paragraph */
    assert(d.text[d.len - 1] == 'e' && d.npara == 6);
    wp_free(&d);
}

static void test_rtf_edge(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_report rep;
    assert(wp_rtf_read(&d, "plain", 5, &rep) == -EINVAL && wp_rtf_read(&d, "{\\rt", 4, &rep) == -EINVAL);
    assert(!read_rtf(&d, "{\\rtf1}", &rep) && d.len == 0 && d.npara == 1);
    assert(!read_rtf(&d, "{\\rtf1 abc", &rep) && !strcmp(d.text, "abc"));              /* unterminated */
    assert(!read_rtf(&d, "{\\rtf1 a\\par}", &rep) && !strcmp(d.text, "a") && d.npara == 1);
    assert(!read_rtf(&d, "{\\rtf1 a\\par\\par}", &rep) && !strcmp(d.text, "a\n") && d.npara == 2);
    assert(!read_rtf(&d, "{\\rtf1 a\\par b}", &rep) && !strcmp(d.text, "a\nb"));
    assert(!read_rtf(&d, "{\\rtf1 a\\\nb}", &rep) && !strcmp(d.text, "a\nb"));         /* backslash newline is \par */
    assert(!read_rtf(&d, "{\\rtf1 a\r\nb \\{x\\} \\\\ \\~ \\_ \\-z}", &rep) && !strcmp(d.text, "ab {x} \\ \xc2\xa0 - z"));
    assert(!read_rtf(&d, "{\\rtf1 \\bullet\\endash\\emdash\\lquote\\rquote\\ldblquote\\rdblquote}", &rep) &&
           !strcmp(d.text, "\xe2\x80\xa2\xe2\x80\x93\xe2\x80\x94\xe2\x80\x98\xe2\x80\x99\xe2\x80\x9c\xe2\x80\x9d"));
    /* pictures and objects are skipped and counted; unknown and ignorable destinations vanish */
    assert(!read_rtf(&d, "{\\rtf1 a{\\pict\\pngblip\\bin3 {}\\par}b{\\object{\\*\\objdata 00ff}}c{\\*\\unknown stuff}{\\info{\\author X}}{\\stylesheet{\\s1 Normal;}}d}", &rep));
    assert(!strcmp(d.text, "abcd") && rep.skipped_pictures == 1 && rep.skipped_objects == 1);
    assert(!read_rtf(&d, "{\\rtf1\\ansicpg1250 x}", &rep) && rep.unknown_charset == 1);
    /* \uc0 and \uc2 fallbacks */
    assert(!read_rtf(&d, "{\\rtf1\\uc0\\u233 x\\uc2\\u233 yyz}", &rep) && !strcmp(d.text, "\xc3\xa9x\xc3\xa9z"));
    assert(!read_rtf(&d, "{\\rtf1\\u233\\'e9z{\\u233 q}r}", &rep));
    /* groups restore character and paragraph state */
    assert(!read_rtf(&d, "{\\rtf1 a{\\b b\\qc}c\\par}", &rep) && !(wp_attr_of(&d, 0).flags & WP_BOLD) && (wp_attr_of(&d, 1).flags & WP_BOLD) && !(wp_attr_of(&d, 2).flags & WP_BOLD));
    /* sizes and indents are clamped; huge parameters do not overflow */
    assert(!read_rtf(&d, "{\\rtf1\\fs99999\\li999999999 a\\fs0 b\\fi-99999999999 c\\par}", &rep));
    assert(wp_attr_of(&d, 0).size == WP_SIZE_MAX && wp_attr_of(&d, 1).size == WP_SIZE_MIN && d.paras[0].a.left == WP_INDENT_MAX);
    check(&d);
    /* an unknown font number and colour index fall back to the defaults */
    assert(!read_rtf(&d, "{\\rtf1\\f99\\cf77 a}", &rep) && wp_attr_of(&d, 0).font == 0 && wp_attr_of(&d, 0).color == 0);
    /* font table forms: nested without braces, family classes decide for unknown names, same name shares one slot */
    assert(!read_rtf(&d, "{\\rtf1{\\fonttbl\\f0\\fswiss Foo;\\f1\\froman Bar;\\f2\\fmodern Baz;\\f3\\fswiss Foo;}\\f1 x\\f2 y\\f3 z}", &rep));
    assert(d.fonts[wp_attr_of(&d, 0).font].family == WP_SERIF && d.fonts[wp_attr_of(&d, 1).font].family == WP_MONO);
    assert(wp_attr_of(&d, 2).font == 3 && d.nfonts == 6);
    /* deep nesting, many groups, a flood of closing braces */
    {
        size_t n = 5000, k = 0; char *deep = malloc(n * 2 + 64);
        k += (size_t)sprintf(deep, "{\\rtf1 ");
        for (size_t i = 0; i < n; i++) deep[k++] = '{';
        k += (size_t)sprintf(deep + k, "x");
        for (size_t i = 0; i < n; i++) deep[k++] = '}';
        deep[k++] = '}'; deep[k++] = '}'; deep[k] = 0;
        assert(!read_rtf(&d, deep, &rep) && !strcmp(d.text, "x"));
        free(deep);
    }
    wp_free(&d);
}

static void test_rtf_round_trip(void)
{
    struct wp_doc d, e; assert(!wp_init(&d) && !wp_init(&e));
    struct wp_report rep;
    assert(!read_rtf(&d, WORDPAD_SAMPLE, &rep));
    char *out; size_t n;
    assert(!wp_rtf_write(&d, &out, &n));
    assert(wp_is_rtf(out, n) && out[n - 1] == '\n');
    assert(!wp_rtf_read(&e, out, n, &rep)); check(&e);
    assert(same(&d, &e));
    char *again; size_t n2;
    assert(!wp_rtf_write(&e, &again, &n2) && n == n2 && !memcmp(out, again, n));    /* stable */
    free(out); free(again);
    /* empty documents, an empty last paragraph, characters outside Latin-1, long lines */
    struct wp_doc m; assert(!wp_init(&m));
    assert(!wp_rtf_write(&m, &out, &n) && !wp_rtf_read(&e, out, n, &rep) && e.len == 0 && e.npara == 1); free(out);
    insert(&m, 0, "x\n\n");
    assert(!wp_rtf_write(&m, &out, &n) && !wp_rtf_read(&e, out, n, &rep) && same(&m, &e)); free(out);
    char *text = malloc(2000 * 4 + 64), *p = text;
    for (unsigned i = 0; i < 2000; i++) p += wp_utf8_put(p, i % 7 == 0 ? 0x4e2d : i % 5 == 0 ? 0xe8 : i % 3 == 0 ? 0x1f600 : i % 11 == 0 ? '\\' : i % 13 == 0 ? '{' : 'a' + i % 26);
    *p = 0;
    assert(!wp_reset(&m)); assert(!wp_insert(&m, 0, text, strlen(text), NULL));
    assert(!wp_insert(&m, 5, "\t\v}", 3, NULL));
    assert(!wp_rtf_write(&m, &out, &n));
    for (size_t i = 0, col = 0; i < n; i++) { col = out[i] == '\n' ? 0 : col + 1; assert(col < 400); }   /* lines stay short */
    assert(!wp_rtf_read(&e, out, n, &rep)); check(&e);
    assert(e.len == m.len && !memcmp(e.text, m.text, m.len));
    free(out); free(text);
    /* every attribute and paragraph form */
    assert(!wp_reset(&m)); insert(&m, 0, "Heading\nbody text here\nlast");
    int f = wp_font_add(&m, "Lucida Console", WP_MONO), c = wp_color_add(&m, 0x336699);
    struct wp_cstyle st = {f, 36, c, WP_BOLD | WP_ITALIC | WP_UNDERLINE | WP_STRIKE, 0};
    assert(!wp_style_chars(&m, 2, 12, &st));
    struct wp_pstyle ps = {WP_P_ALIGN | WP_P_BULLET | WP_P_LEFT | WP_P_FIRST | WP_P_RIGHT | WP_P_TABS, {WP_RIGHT, 1, 2, 0, 900, -300, 100, {500, 1500, 3000}}};
    ps.v.ntabs = 3; assert(!wp_style_paras(&m, 9, 12, &ps));
    assert(!wp_rtf_write(&m, &out, &n) && !wp_rtf_read(&e, out, n, &rep)); check(&e);
    assert(same(&m, &e)); free(out);
    wp_free(&d); wp_free(&e); wp_free(&m);
}

/* Mutating valid files must never crash, hang or break the model's invariants. */
static void test_rtf_fuzz(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_report rep;
    size_t base = strlen(WORDPAD_SAMPLE);
    unsigned seed = 12345;
    char *buf = malloc(base + 64);
    for (unsigned it = 0; it < 4000; it++) {
        memcpy(buf, WORDPAD_SAMPLE, base);
        size_t n = base;
        unsigned edits = 1 + it % 6;
        for (unsigned e = 0; e < edits; e++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % n;
            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) % 4) {
            case 0: buf[at] = (char)(seed >> 3); break;
            case 1: n = at + 1; break;                                   /* truncate */
            case 2: if (n + 8 < base + 64) { memmove(buf + at + 4, buf + at, n - at); memcpy(buf + at, "\\bin9", 4); n += 4; } break;
            default: buf[at] = "{}\\;'-0 "[(seed >> 5) & 7]; break;
            }
        }
        int r = wp_rtf_read(&d, buf, n, &rep);
        assert(r == 0 || r == -EINVAL);
        check(&d);
        if (!r) {
            char *out; size_t m;
            assert(!wp_rtf_write(&d, &out, &m));
            struct wp_doc e; assert(!wp_init(&e));
            assert(!wp_rtf_read(&e, out, m, &rep)); check(&e);
            if (e.len != d.len) { fprintf(stderr, "mismatch %zu %zu\n---\n", d.len, e.len); fwrite(buf, 1, n, stderr); fprintf(stderr, "\n---\n"); fwrite(out, 1, m, stderr); fprintf(stderr, "\n--- d '%s' e '%s'\n", d.text, e.text); }
            assert(e.len == d.len);
            wp_free(&e); free(out);
        }
    }
    free(buf); wp_free(&d);
}

static void test_text(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    assert(!wp_text_read(&d, "one\r\ntwo\rthree\nfour", 19) && !strcmp(d.text, "one\ntwo\nthree\nfour") && d.npara == 4); check(&d);
    assert(!wp_text_read(&d, "\xef\xbb\xbf" "caf\xc3\xa9", 8) && !strcmp(d.text, "caf\xc3\xa9"));
    assert(!wp_text_read(&d, "caf\xe9 \x80\x81", 8) && !strcmp(d.text, "caf\xc3\xa9 \xe2\x82\xac\xef\xbf\xbd"));  /* Windows-1252, 0x81 undefined */
    assert(!wp_text_read(&d, "", 0) && d.len == 0 && d.npara == 1);
    assert(!wp_text_read(&d, "a\x01\x00" "b", 4) && !strcmp(d.text, "ab") && d.len == 2);
    char *o; size_t n;
    assert(!wp_text_read(&d, "a\nb\n", 4));
    assert(!wp_text_write(&d, &o, &n, 1) && n == 6 && !memcmp(o, "a\r\nb\r\n", 6)); free(o);
    assert(!wp_text_write(&d, &o, &n, 0) && n == 4); free(o);
    insert(&d, 1, "\v"); assert(!wp_text_write(&d, &o, &n, 0) && !memcmp(o, "a\n\nb\n", 5)); free(o);
    wp_free(&d);
}

/* ---- layout ------------------------------------------------------------------------ */
static int count_pieces(void *ctx, const struct wp_piece *p) { (void)p; ++*(int *)ctx; return 0; }
static int gxs[16], gn;
static int collect(void *c, const struct wp_piece *p) { (void)c; gxs[gn++] = p->x; return 0; }
static void test_layout(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_layout l; wp_layout_init(&l);
    /* 10 pt characters are 10 px: a 100 px column holds ten */
    insert(&d, 0, "aaaa bbbb cccc dddd");
    assert(!wp_layout_build(&l, &d, &OPS, 100, 0));
    assert(l.n == 2 && l.lines[0].start == 0 && l.lines[0].end == 10 && l.lines[1].start == 10 && l.lines[1].end == 19);
    assert(l.lines[0].flags & WP_LINE_WRAPPED && l.lines[1].flags & WP_LINE_PARA_END && l.lines[0].flags & WP_LINE_FIRST);
    assert(l.lines[0].h == l.lines[0].base + 4 && l.lines[1].y == l.lines[0].h && l.height == 2 * l.lines[0].h);
    assert(l.lines[0].width == 90);   /* "aaaa bbbb" without the trailing space */
    /* a word wider than the line breaks between characters */
    assert(!wp_reset(&d)); insert(&d, 0, "xxxxxxxxxxxxxxxxxxxxxxxxx yy");
    assert(!wp_layout_build(&l, &d, &OPS, 100, 0));
    assert(l.n == 3 && l.lines[0].end == 10 && l.lines[1].end == 20 && l.lines[2].start == 20);
    /* narrow column: at least one character per line, no endless loop */
    assert(!wp_layout_build(&l, &d, &OPS, 8, 0) && l.n >= 20);
    /* alignment, indents */
    assert(!wp_reset(&d)); insert(&d, 0, "abcd\nabcd\nabcd");
    struct wp_pstyle right = {WP_P_ALIGN, {WP_RIGHT, 0, 0, 0, 0, 0, 0, {0}}}, ctr = {WP_P_ALIGN, {WP_CENTER, 0, 0, 0, 0, 0, 0, {0}}};
    assert(!wp_style_paras(&d, 5, 6, &right) && !wp_style_paras(&d, 10, 11, &ctr));
    struct wp_pstyle ind = {WP_P_LEFT | WP_P_RIGHT, {0, 0, 0, 0, 750, 0, 1500, {0}}};   /* 50 px left, 100 px right */
    assert(!wp_style_paras(&d, 0, 1, &ind));
    assert(!wp_layout_build(&l, &d, &OPS, 300, 0) && l.n == 3);
    assert(l.lines[0].indent == 50 && l.lines[0].shift == 0);
    assert(l.lines[1].shift == 300 - 40 && l.lines[2].shift == (300 - 40) / 2);
    /* hanging bullet */
    struct wp_pstyle bul = {WP_P_BULLET | WP_P_LEFT | WP_P_FIRST, {0, 1, 0, 0, 720, -360, 0, {0}}};
    assert(!wp_style_paras(&d, 10, 11, &bul));
    assert(!wp_layout_build(&l, &d, &OPS, 300, 0));
    assert((l.lines[2].flags & WP_LINE_BULLET) && l.lines[2].indent == 48 && wp_layout_bullet_x(&d, &l.lines[2]) == 24);
    /* tabs: default stops every 48 px, custom stops first */
    assert(!wp_reset(&d)); insert(&d, 0, "a\tb\tc");
    assert(!wp_layout_build(&l, &d, &OPS, 400, 0));
    gn = 0; wp_layout_pieces(&l, &d, &OPS, 0, collect, NULL);
    assert(gn == 5 && gxs[0] == 0 && gxs[1] == 10 && gxs[2] == 48 && gxs[3] == 58 && gxs[4] == 96);
    struct wp_pstyle tabs = {WP_P_TABS, {0, 0, 2, 0, 0, 0, 0, {300, 600}}};    /* 20 px, 40 px */
    assert(!wp_style_paras(&d, 0, 1, &tabs) && !wp_layout_build(&l, &d, &OPS, 400, 0));
    gn = 0; wp_layout_pieces(&l, &d, &OPS, 0, collect, NULL);
    assert(gn == 5 && gxs[2] == 20 && gxs[4] == 40);
    /* soft breaks and empty paragraphs */
    assert(!wp_reset(&d)); insert(&d, 0, "ab\vcd\n\nef");
    assert(!wp_layout_build(&l, &d, &OPS, 400, 0));
    assert(l.n == 4 && l.lines[0].end == 2 && l.lines[1].start == 3 && l.lines[2].start == l.lines[2].end && l.lines[2].h > 0);
    /* mixed sizes: the line is as tall as its tallest run, the baseline shared */
    assert(!wp_reset(&d)); insert(&d, 0, "small BIG small");
    struct wp_cstyle big = {-1, 72, -1, 0, 0};
    assert(!wp_style_chars(&d, 6, 9, &big));
    assert(!wp_layout_build(&l, &d, &OPS, 1000, 0) && l.n == 1);
    int a72, d72; fv(NULL, 0, 0, 72, &a72, &d72);
    assert(l.lines[0].base == a72 && l.lines[0].h == a72 + d72);
    {
        int n = 0; wp_layout_pieces(&l, &d, &OPS, 0, count_pieces, &n);
        assert(n == 5);     /* word, space, word (new run), space (own run), ... pieces follow the atoms */
    }
    wp_layout_free(&l); wp_free(&d);
}

/* Caret and hit testing agree with each other on every position of a wrapped, formatted text. */
static void test_layout_hit(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_layout l; wp_layout_init(&l);
    insert(&d, 0, "The quick brown fox jumps over the lazy dog.\tTabbed text \xc3\xa8 and\vsoft break\nSecond paragraph with some more words to wrap around.\n\nLast");
    struct wp_cstyle b = {-1, 28, -1, WP_BOLD, 0};
    assert(!wp_style_chars(&d, 10, 25, &b));
    struct wp_pstyle ps = {WP_P_ALIGN, {WP_CENTER, 0, 0, 0, 0, 0, 0, {0}}};
    assert(!wp_style_paras(&d, 80, 81, &ps));
    for (int width = 40; width <= 400; width += 37) {
        assert(!wp_layout_build(&l, &d, &OPS, width, 0));
        for (size_t pos = 0; pos <= d.len; pos = wp_next(&d, pos)) {
            int x, y, h; wp_layout_caret(&l, &d, &OPS, pos, &x, &y, &h);
            assert(h > 0 && y >= 0 && y < l.height + h);
            size_t back = wp_layout_hit(&l, &d, &OPS, x, y + h / 2);
            /* wrapped lines place the caret before trailing spaces: the same visual place */
            if (back != pos) {
                int x2, y2, h2; wp_layout_caret(&l, &d, &OPS, back, &x2, &y2, &h2);
                if (!(x2 == x && y2 == y)) { printf("width %d pos %zu -> %zu (x %d y %d / x %d y %d)\n", width, pos, back, x, y, x2, y2); assert(0); }
            }
            if (pos == d.len) break;
        }
    }
    wp_layout_free(&l); wp_free(&d);
}

static void test_layout_incremental(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    struct wp_layout inc, full; wp_layout_init(&inc); wp_layout_init(&full);
    for (unsigned i = 0; i < 300; i++) {
        char line[96]; snprintf(line, sizeof(line), "Paragraph %u has a few words that should wrap more than once in a narrow column.\n", i);
        assert(!wp_append(&d, line, strlen(line), &(struct wp_cattr){0, (uint8_t)(i % 3 ? 0 : WP_BOLD), 0, 0, 20}));
    }
    check(&d);
    assert(!wp_layout_build(&inc, &d, &OPS, 200, 0));
    unsigned seed = 7;
    for (unsigned it = 0; it < 200; it++) {
        seed = seed * 1103515245u + 12345u;
        size_t at = (seed >> 8) % (d.len + 1);
        at = (at / 1) * 1; while (at < d.len && ((unsigned char)d.text[at] & 0xc0) == 0x80) at++;
        unsigned para = wp_para_at(&d, at, NULL, NULL);
        switch ((seed >> 20) % 3) {
        case 0: assert(!wp_insert(&d, at, "inserted words here\nand a new paragraph", 38, NULL)); break;
        case 1: { size_t to = at + (seed >> 12) % 400; if (to > d.len) to = d.len; assert(!wp_delete(&d, at, to)); break; }
        default: { struct wp_cstyle s = {-1, 36, -1, WP_ITALIC, 0}; size_t to = at + 50 > d.len ? d.len : at + 50; assert(!wp_style_chars(&d, at, to, &s)); break; }
        }
        check(&d);
        assert(!wp_layout_build(&inc, &d, &OPS, 200, para));
        assert(!wp_layout_build(&full, &d, &OPS, 200, 0));
        assert(inc.n == full.n && inc.height == full.height);
        for (unsigned i = 0; i < full.n; i++)
            assert(inc.lines[i].start == full.lines[i].start && inc.lines[i].end == full.lines[i].end && inc.lines[i].y == full.lines[i].y);
    }
    wp_layout_free(&inc); wp_layout_free(&full); wp_free(&d);
}

static void test_pages(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    for (unsigned i = 0; i < 200; i++) { char s[64]; snprintf(s, sizeof(s), "Line number %u of the page test\n", i); assert(!wp_append(&d, s, strlen(s), &(struct wp_cattr){0, 0, 0, 0, 20})); }
    struct wp_layout l; wp_layout_init(&l); struct wp_pages pg = {0};
    struct wp_page_setup a4 = wp_default_page_setup(1), us = wp_default_page_setup(0);
    assert(a4.paper_w == 11906 && us.paper_h == 15840 && a4.left == 1080);
    assert(!wp_paginate(&pg, &l, &d, &OPS, &a4));
    int page_h = wp_twips_px(a4.paper_h - a4.top - a4.bottom);
    assert(pg.count > 2 && pg.first_line[0] == 0);
    for (unsigned p = 0; p < pg.count; p++) {
        unsigned from = pg.first_line[p], to = p + 1 < pg.count ? pg.first_line[p + 1] : l.n;
        assert(to > from);
        assert(l.lines[to - 1].y + l.lines[to - 1].h - l.lines[from].y <= page_h);
        if (p + 1 < pg.count) assert(l.lines[to].y + l.lines[to].h - l.lines[from].y > page_h);
    }
    unsigned a4_pages = pg.count; wp_pages_free(&pg);
    struct wp_page_setup land = us; land.landscape = 1;
    assert(!wp_paginate(&pg, &l, &d, &OPS, &land) && pg.count > a4_pages / 2); wp_pages_free(&pg);
    assert(l.width == wp_twips_px(land.paper_h - land.left - land.right));
    wp_layout_free(&l); wp_free(&d);
}

static void test_large(void)
{
    /* 3 MiB of text: loading, layout and an edit stay linear and within the 8 MiB limit */
    struct wp_doc d; assert(!wp_init(&d));
    size_t chunk = 3u << 20; char *t = malloc(chunk + 1);
    for (size_t i = 0; i < chunk; i++) t[i] = i % 61 == 60 ? '\n' : i % 7 == 6 ? ' ' : 'a' + i % 26;
    t[chunk] = 0;
    assert(!wp_text_read(&d, t, chunk)); check(&d);
    struct wp_layout l; wp_layout_init(&l);
    assert(!wp_layout_build(&l, &d, &OPS, 600, 0));
    assert(l.n >= d.npara);
    assert(!wp_insert(&d, d.len / 2, "middle", 6, NULL)); check(&d);
    unsigned p = wp_para_at(&d, d.len / 2, NULL, NULL);
    assert(!wp_layout_build(&l, &d, &OPS, 600, p));
    free(t); wp_layout_free(&l); wp_free(&d);
}

/* Random editing keeps the model's invariants and matches a naive reference of the text. */
static void test_random_edits(void)
{
    struct wp_doc d; assert(!wp_init(&d));
    char *ref = calloc(1, 1 << 20); size_t rn = 0;
    unsigned seed = 99;
    static const char *const pieces[] = {"ab", "\n", "x y z", "\n\n", "\xc3\xa8", "\t", "long piece of text\nwith a break", "\v"};
    for (unsigned it = 0; it < 6000; it++) {
        seed = seed * 1103515245u + 12345u;
        unsigned op = (seed >> 16) % 4;
        size_t at = rn ? (seed >> 4) % (rn + 1) : 0;
        while (at < rn && ((unsigned char)ref[at] & 0xc0) == 0x80) at++;
        if (op < 2 || !rn) {
            const char *s = pieces[(seed >> 9) % 8]; size_t n = strlen(s);
            struct wp_cattr a = {(uint8_t)((seed >> 3) % 3), (uint8_t)((seed >> 5) & 15), (uint8_t)((seed >> 7) % 4), 0, (uint16_t)(20 + 2 * ((seed >> 11) % 4))};
            assert(!wp_insert(&d, at, s, n, &a));
            memmove(ref + at + n, ref + at, rn - at); memcpy(ref + at, s, n); rn += n;
        } else if (op == 2) {
            size_t to = at + (seed >> 12) % 20; if (to > rn) to = rn;
            while (to < rn && ((unsigned char)ref[to] & 0xc0) == 0x80) to++;
            assert(!wp_delete(&d, at, to));
            memmove(ref + at, ref + to, rn - to); rn -= to - at;
        } else {
            size_t to = at + (seed >> 12) % 40; if (to > rn) to = rn;
            while (to < rn && ((unsigned char)ref[to] & 0xc0) == 0x80) to++;
            struct wp_cstyle s = {(int)((seed >> 4) % 3), (int)(20 + (seed >> 6) % 20), (int)((seed >> 8) % 4), (seed >> 10) & 15, (seed >> 14) & 15};
            assert(!wp_style_chars(&d, at, to, &s));
        }
        if (it % 50 == 0) { check(&d); assert(d.len == rn && !memcmp(d.text, ref, rn)); }
    }
    check(&d); assert(d.len == rn && !memcmp(d.text, ref, rn));
    free(ref); wp_free(&d);
}

int main(void)
{
    test_model(); test_slices(); test_limits(); test_sanitize();
    test_rtf_read(); test_rtf_edge(); test_rtf_round_trip(); test_rtf_fuzz(); test_text();
    test_layout(); test_layout_hit(); test_layout_incremental(); test_pages(); test_large(); test_random_edits();
    puts("PASS wordpad model");
    return 0;
}
