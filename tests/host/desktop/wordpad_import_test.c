/* WordPad import host tests (G28b): the OLE2 reader, Word 6 and Word 97 .doc files and Write .wri files.
 *
 *   wordpad_import_test <fixture folder> <repo fixture folder>
 *
 * The folder holds the documents built by tests/host/desktop/wordpad_fixtures.py; the repository folder holds
 * wordpad-lo97.doc, a Word 97 document written by LibreOffice from wordpad-lo97.rtf. Every file is also damaged
 * (flipped bytes, cut short) under ASAN/UBSAN: a damaged file is refused or read, never a crash.
 * SPDX-License-Identifier: MIT */
#include "rich.h"
#include "ole.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *dir_fx, *dir_repo;
static uint8_t *slurp(const char *folder, const char *name, size_t *n)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", folder, name);
    FILE *f = fopen(path, "rb"); assert(f);
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1); assert(b && fread(b, 1, (size_t)len, f) == (size_t)len); fclose(f);
    *n = (size_t)len; return b;
}
static int load_word(struct wp_doc *d, const char *folder, const char *name, struct wp_report *rep)
{
    size_t n; uint8_t *b = slurp(folder, name, &n);
    wp_init(d);
    int e = wp_is_write((const char *)b, n) ? wp_write_read(d, b, n, rep) : wp_word_read(d, b, n, rep);
    free(b);
    return e;
}
static int text_is(const struct wp_doc *d, const char *s) { return d->len == strlen(s) && !memcmp(d->text, s, d->len); }
static struct wp_cattr attr_at(const struct wp_doc *d, size_t at) { return wp_attr_of(d, at); }
static size_t find(const struct wp_doc *d, const char *s) { const char *p = strstr(d->text, s); assert(p); return (size_t)(p - d->text); }
static unsigned color_of(const struct wp_doc *d, size_t at) { return d->colors[attr_at(d, at).color]; }
static const char *font_of(const struct wp_doc *d, size_t at) { return d->fonts[attr_at(d, at).font].name; }

static void test_ole(void)
{
    /* the small Word 97 document: mini stream (1Table < 4096) and regular stream (WordDocument) */
    size_t n; uint8_t *b = slurp(dir_fx, "w97-complex.doc", &n);
    struct wp_ole o; assert(wp_is_ole(b, n) && !wp_ole_open(&o, b, n));
    uint8_t *s; size_t sl;
    assert(!wp_ole_read(&o, "WordDocument", &s, &sl) && sl >= 2048 && s[0] == 0xec && s[1] == 0xa5); free(s);
    assert(!wp_ole_read(&o, "1table", &s, &sl) && sl > 100); free(s);   /* the name compares without case */
    assert(wp_ole_read(&o, "Nonesuch", &s, &sl) == -ENOENT);
    wp_ole_close(&o);
    /* not compound files, truncated headers and loops */
    assert(wp_ole_open(&o, (const uint8_t *)"not an ole file", 15) == -EINVAL);
    assert(wp_ole_open(&o, b, 100) == -EINVAL);
    uint8_t *c = malloc(n); memcpy(c, b, n);
    c[0x1e] = 7;   /* a sector shift Word never writes */
    assert(wp_ole_open(&o, c, n) == -EINVAL);
    memcpy(c, b, n); c[0x30] = 0xff; c[0x31] = 0xff; c[0x32] = 0xff; c[0x33] = 0x7f;   /* the directory starts outside the file */
    assert(wp_ole_open(&o, c, n) == -EINVAL);
    free(c); free(b);
    /* a real file from LibreOffice */
    b = slurp(dir_repo, "wordpad-lo97.doc", &n);
    assert(!wp_ole_open(&o, b, n)); assert(!wp_ole_read(&o, "WordDocument", &s, &sl)); free(s); assert(!wp_ole_read(&o, "Data", &s, &sl)); free(s);
    wp_ole_close(&o); free(b);
}

static void test_word97_basic(void)
{
    struct wp_doc d; struct wp_report r;
    assert(!load_word(&d, dir_fx, "w97-basic.doc", &r) && r.version == 97 && !r.reason);
    assert(text_is(&d, "Title\nPlain bold ital under strike red blue sans big strong CAPS\nJustified paragraph\nvisible text\vline two\ttab\nlast"));
    assert(d.npara == 5);
    /* the Heading 1 style: bold, 16 pt, centred, from the style sheet */
    assert(attr_at(&d, 0).flags == WP_BOLD && attr_at(&d, 0).size == 32 && d.paras[0].a.align == WP_CENTER);
    assert(!strcmp(font_of(&d, 0), "Times New Roman"));   /* the default font is the first of the table */
    size_t at = find(&d, "bold");
    assert(attr_at(&d, at).flags == WP_BOLD && attr_at(&d, at - 1).flags == 0 && attr_at(&d, at + 4).flags == 0);
    assert(attr_at(&d, find(&d, "ital")).flags == WP_ITALIC && attr_at(&d, find(&d, "under")).flags == WP_UNDERLINE && attr_at(&d, find(&d, "strike")).flags == WP_STRIKE);
    assert(color_of(&d, find(&d, "red")) == 0xff0000 && color_of(&d, find(&d, "blue")) == 0x0000ff && color_of(&d, find(&d, "Plain")) == 0);
    assert(!strcmp(font_of(&d, find(&d, "sans")), "Arial") && attr_at(&d, find(&d, "sans")).size == 20);
    assert(attr_at(&d, find(&d, "big")).size == 40);
    assert(attr_at(&d, find(&d, "strong")).flags == WP_BOLD);   /* a character style */
    /* the paragraph: justified, indents, tab stops */
    size_t ps; wp_para_at(&d, find(&d, "Justified"), &ps, NULL);
    struct wp_pattr p = wp_pattr_at(&d, ps);
    assert(p.align == WP_JUSTIFY && p.left == 720 && p.first == -360 && p.right == 360 && p.ntabs == 2 && p.tabs[0] == 1440 && p.tabs[1] == 2880);
    /* hidden text is left out, a soft break stays */
    assert(!strstr(d.text, "SECRET") && strstr(d.text, "visible text\vline"));
    wp_free(&d);
}

static void test_word97_other(void)
{
    struct wp_doc d; struct wp_report r;
    /* tables: cells are tab separated, a row is a paragraph; fields show their result */
    assert(!load_word(&d, dir_fx, "w97-table-field.doc", &r) && text_is(&d, "A\tB\t\nBefore shown after")); wp_free(&d);
    /* lists: a bullet and numbers with the level's indents */
    assert(!load_word(&d, dir_fx, "w97-lists.doc", &r) && text_is(&d, "bullet one\n1.\tnumber one\n2.\tnumber two\nplain"));
    assert(d.paras[0].a.bullet && d.paras[0].a.left == 720 && d.paras[0].a.first == -360 && !d.paras[1].a.bullet && d.paras[1].a.left == 720);
    assert(d.paras[1].a.ntabs == 1 && d.paras[1].a.tabs[0] == 720 && !d.paras[3].a.bullet && !d.paras[3].a.left);
    wp_free(&d);
    /* compressed and UTF-16 pieces; a surrogate pair is one character */
    assert(!load_word(&d, dir_fx, "w97-unicode.doc", &r) && text_is(&d, "abc\ncaf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80")); wp_free(&d);
    /* fast-saved: pieces out of order, the first with a grpprl (bold) */
    assert(!load_word(&d, dir_fx, "w97-complex.doc", &r) && text_is(&d, "Hello \nWorld"));
    assert(attr_at(&d, 0).flags == WP_BOLD && attr_at(&d, 6).flags == WP_BOLD && attr_at(&d, 7).flags == 0); wp_free(&d);
    /* pictures through the Data stream: a PNG BLIP and a DIB BLIP, both kept with their bytes */
    assert(!load_word(&d, dir_fx, "w97-picture.doc", &r) && r.kept_pictures == 1 && d.len == 7 + 3 + 6);
    struct wp_pic *pic = wp_pic_at(&d, 7);
    assert(pic && pic->kind == WP_PIC_PNG && pic->pw == 16 && pic->ph == 8 && pic->goalw == 1440 && pic->goalh == 720 && pic->scalex == 100);
    assert(pic->len > 40 && !memcmp(pic->data, "\x89PNG", 4));
    wp_free(&d);
    assert(!load_word(&d, dir_fx, "w97-dib.doc", &r)); pic = wp_pic_at(&d, 7);
    assert(pic && pic->kind == WP_PIC_DIB && pic->pw == 4 && pic->ph == 2); wp_free(&d);
    /* structures that refer to themselves (a character style naming itself, a list level setting its own list) end, with the text kept */
    assert(!load_word(&d, dir_fx, "w97-loops.doc", &r) && text_is(&d, "looped\nitem") && attr_at(&d, 0).flags == WP_BOLD); wp_free(&d);
    /* refusals with a reason */
    assert(load_word(&d, dir_fx, "w97-encrypted.doc", &r) == -ENOTSUP && strstr(r.reason, "password")); wp_free(&d);
    assert(load_word(&d, dir_fx, "w97-old.doc", &r) == -ENOTSUP && strstr(r.reason, "before Word 6")); wp_free(&d);
}

static void test_word6(void)
{
    struct wp_doc d; struct wp_report r;
    assert(!load_word(&d, dir_fx, "w6-basic.doc", &r) && r.version == 6);
    assert(text_is(&d, "Title\nPlain bold ital under red sans big\nHanging paragraph\nbullet\nlast"));
    assert(attr_at(&d, 0).flags == WP_BOLD && attr_at(&d, 0).size == 32 && d.paras[0].a.align == WP_CENTER);
    assert(attr_at(&d, find(&d, "bold")).flags == WP_BOLD && attr_at(&d, find(&d, "ital")).flags == WP_ITALIC && attr_at(&d, find(&d, "under")).flags == WP_UNDERLINE);
    assert(color_of(&d, find(&d, "red")) == 0xff0000 && !strcmp(font_of(&d, find(&d, "sans")), "Arial") && attr_at(&d, find(&d, "big")).size == 40);
    struct wp_pattr p = wp_pattr_at(&d, find(&d, "Hanging"));
    assert(p.align == WP_RIGHT && p.left == 720 && p.first == -360 && p.right == 360 && p.ntabs == 1 && p.tabs[0] == 1440);
    assert(wp_pattr_at(&d, find(&d, "bullet")).bullet);   /* the ANLD of Word 6 */
    wp_free(&d);
    /* a metafile picture in the main stream */
    assert(!load_word(&d, dir_fx, "w6-picture.doc", &r) && r.kept_pictures == 1);
    struct wp_pic *pic = wp_pic_at(&d, 4);
    assert(pic && pic->kind == WP_PIC_WMF && pic->len == 36 && pic->goalw == 1440 && pic->goalh == 720); wp_free(&d);
    /* fast-saved: the Clx is in the main stream and the second piece is italic */
    assert(!load_word(&d, dir_fx, "w6-complex.doc", &r) && text_is(&d, "Hello \nWorld") && attr_at(&d, 8).flags == WP_ITALIC && attr_at(&d, 0).flags == 0);
    wp_free(&d);
}

static void test_write(void)
{
    struct wp_doc d; struct wp_report r;
    assert(!load_word(&d, dir_fx, "write-basic.wri", &r) && r.version == 3);
    assert(text_is(&d, "Title\nPlain bold italic sans under\nIndented\ttab\nlast"));
    assert(attr_at(&d, 0).flags == WP_BOLD && attr_at(&d, 0).size == 36 && d.paras[0].a.align == WP_CENTER && !strcmp(font_of(&d, 0), "Times New Roman"));
    assert(attr_at(&d, find(&d, "bold")).flags == WP_BOLD && attr_at(&d, find(&d, "italic")).flags == WP_ITALIC && attr_at(&d, find(&d, "under")).flags == WP_UNDERLINE);
    assert(!strcmp(font_of(&d, find(&d, "sans")), "Arial") && attr_at(&d, find(&d, "sans")).size == 20 && attr_at(&d, find(&d, "Plain")).size == 24);
    struct wp_pattr p = wp_pattr_at(&d, find(&d, "Indented"));
    assert(p.align == WP_JUSTIFY && p.left == 720 && p.right == 360 && p.first == -360 && p.ntabs == 2 && p.tabs[0] == 1440 && p.tabs[1] == 2880);
    wp_free(&d);
    /* a monochrome bitmap becomes a DIB and a metafile is kept */
    assert(!load_word(&d, dir_fx, "write-picture.wri", &r) && r.kept_pictures == 2 && wp_pic_count_used(&d) == 2);
    struct wp_pic *bm = wp_pic_at(&d, 7), *wmf = wp_pic_at(&d, 7 + 4);
    assert(bm && bm->kind == WP_PIC_DIB && bm->pw == 10 && bm->ph == 2 && bm->goalw == 1440 && bm->goalh == 720);
    assert(bm->len == 40 + 8 + 8 && bm->data[40 + 4 * 0] == 0 && bm->data[40 + 4 + 2] == 255);   /* black, then white */
    assert(wmf && wmf->kind == WP_PIC_WMF && wmf->len == 36);
    wp_free(&d);
    /* header paragraphs are not shown */
    assert(!load_word(&d, dir_fx, "write-header.wri", &r) && text_is(&d, "body") && r.ignored_parts == 1); wp_free(&d);
    /* the signature checks */
    assert(!wp_is_write("short", 5));
    char junk[256]; memset(junk, 0, sizeof(junk)); junk[0] = 0x31; junk[1] = (char)0xbe;
    assert(!wp_is_write(junk, sizeof(junk)));   /* wTool missing */
    junk[4] = 0; junk[5] = (char)0xab; assert(wp_is_write(junk, sizeof(junk)));
    wp_init(&d); assert(wp_write_read(&d, (const uint8_t *)junk, sizeof(junk), &r) == -EINVAL && strstr(r.reason, "Word document")); wp_free(&d);
}

static void test_libreoffice_file(void)
{
    /* a Word 97 file from another producer: styles, fonts, tabs, justification, a bullet text and a PNG picture */
    struct wp_doc d; struct wp_report r;
    assert(!load_word(&d, dir_repo, "wordpad-lo97.doc", &r) && r.version == 97);
    assert(strstr(d.text, "Title of the Test\n") == d.text);
    assert(attr_at(&d, 0).flags == WP_BOLD && attr_at(&d, 0).size == 36 && d.paras[0].a.align == WP_CENTER && !strcmp(font_of(&d, 0), "Arial"));
    assert(attr_at(&d, find(&d, "bold")).flags == WP_BOLD && attr_at(&d, find(&d, "italic")).flags == WP_ITALIC && attr_at(&d, find(&d, "underlined")).flags == WP_UNDERLINE);
    assert(attr_at(&d, find(&d, "struck")).flags == WP_STRIKE && color_of(&d, find(&d, "red")) == 0xff0000 && color_of(&d, find(&d, "blue courier")) == 0x0000ff);
    assert(!strcmp(font_of(&d, find(&d, "blue courier")), "Courier New"));
    struct wp_pattr j = wp_pattr_at(&d, find(&d, "This paragraph"));
    assert(j.align == WP_JUSTIFY && j.left == 720 && j.right == 360);
    struct wp_pattr t = wp_pattr_at(&d, find(&d, "one\ttwo"));
    assert(t.ntabs == 2 && t.tabs[0] == 1440 && t.tabs[1] == 2880);
    assert(strstr(d.text, "End \xc3\xa9 \xc3\xa0 \xe2\x80\x93" "done."));   /* Latin-1 and an en dash */
    assert(r.kept_pictures == 1 && wp_pic_count_used(&d) == 1);
    struct wp_pic *pic = NULL;
    for (size_t i = 0; i + 3 <= d.len && !pic; i++) pic = wp_pic_at(&d, i);
    assert(pic && pic->kind == WP_PIC_PNG && pic->pw == 16 && pic->ph == 8 && pic->goalw == 240 && pic->goalh == 120 && pic->scalex == 200 && pic->scaley == 200);
    int pw, ph; wp_pic_size(pic, &pw, &ph); assert(pw == 480 && ph == 240);   /* the displayed size: the goal at 200 % */
    wp_free(&d);
}

/* Damaged files: byte flips and truncation never crash and never leak a half-built document into the caller. */
static void damage(const char *folder, const char *name)
{
    size_t n; uint8_t *b = slurp(folder, name, &n);
    uint32_t seed = 12345u + (uint32_t)strlen(name);
    for (unsigned round = 0; round < 400; round++) {
        uint8_t *c = malloc(n); memcpy(c, b, n);
        size_t len = n;
        seed = seed * 1664525u + 1013904223u;
        unsigned flips = 1 + (seed >> 28) % 6;
        for (unsigned k = 0; k < flips; k++) {
            seed = seed * 1664525u + 1013904223u;
            size_t at = (seed >> 8) % (round % 3 == 0 && n > 3000 ? 3000 : n);   /* a third of the rounds hit the headers */
            seed = seed * 1664525u + 1013904223u;
            c[at] = (uint8_t)(seed >> 16);
        }
        if (round % 11 == 0) len = (seed >> 4) % n;
        struct wp_doc d; struct wp_report r; wp_init(&d);
        int e = wp_is_write((const char *)c, len) ? wp_write_read(&d, c, len, &r) : wp_word_read(&d, c, len, &r);
        assert(e == 0 || e == -EINVAL || e == -ENOTSUP || e == -EFBIG || e == -ENOMEM);
        if (!e) { assert(d.npara >= 1); size_t sum = 0; for (unsigned i = 0; i < d.npara; i++) sum += d.paras[i].len; assert(sum == d.len); }
        wp_free(&d); free(c);
    }
    free(b);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    dir_fx = argv[1]; dir_repo = argv[2];
    test_ole(); test_word97_basic(); test_word97_other(); test_word6(); test_write(); test_libreoffice_file();
    static const char *const fixtures[] = {"w97-basic.doc", "w97-table-field.doc", "w97-lists.doc", "w97-unicode.doc", "w97-complex.doc",
        "w97-picture.doc", "w97-dib.doc", "w97-loops.doc", "w6-basic.doc", "w6-picture.doc", "w6-complex.doc", "write-basic.wri", "write-picture.wri", "write-header.wri"};
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(*fixtures); i++) damage(dir_fx, fixtures[i]);
    damage(dir_repo, "wordpad-lo97.doc");
    puts("PASS wordpad import");
    return 0;
}
