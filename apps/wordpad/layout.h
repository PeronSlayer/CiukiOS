/* SPDX-License-Identifier: MIT
 * WordPad layout: word wrap, tab stops, indents, alignment and bullets for a
 * document at 96 dpi (one pixel is 15 twips; one half-point is 2/3 pixel).
 * Text measuring and drawing are supplied by the host (fonts.c in the
 * application, a fixed-advance fake in the host tests), so the same code
 * lays out the screen and a printed page. Positions inside a line are
 * recomputed from the same atoms the layout used, so drawing, the caret and
 * hit testing agree with the line breaks. */
#ifndef CIUKI_WORDPAD_LAYOUT_H
#define CIUKI_WORDPAD_LAYOUT_H
#include "rich.h"

struct wp_text_ops {
    void *ctx;
    /* Advance of the UTF-8 text in 26.6 pixels. */
    int (*width)(void *, unsigned family, unsigned flags, unsigned size_hp, const char *s, size_t n);
    /* Ascent and descent in pixels (positive). */
    void (*vmetrics)(void *, unsigned family, unsigned flags, unsigned size_hp, int *ascent, int *descent);
};

enum { WP_LINE_FIRST = 1, WP_LINE_PARA_END = 2, WP_LINE_WRAPPED = 4, WP_LINE_BULLET = 8 };
struct wp_line {
    size_t start, end;      /* text range; end excludes the break character ("\n" or U+000B) */
    int y, h, base;         /* top, height and baseline offset in pixels */
    int indent, shift;      /* text origin before alignment, alignment shift (pixels) */
    int width;              /* used width from the origin, trailing spaces excluded */
    unsigned para, flags;
};
struct wp_layout {
    struct wp_line *lines; unsigned n, cap;
    unsigned *pfirst; int *py; unsigned pcap, pcount;    /* first line and top of each paragraph */
    int width, height;
};
void wp_layout_init(struct wp_layout *);
void wp_layout_free(struct wp_layout *);
/* Lays out paragraphs from `first_para` on (earlier ones are kept; pass 0 after a width change).
 * 0, or -ENOMEM with the layout unchanged beyond the first paragraph being rebuilt. */
int wp_layout_build(struct wp_layout *, const struct wp_doc *, const struct wp_text_ops *, int width, unsigned first_para);

unsigned wp_layout_line_of(const struct wp_layout *, size_t pos);       /* the line the caret at pos is on */
unsigned wp_layout_line_at_y(const struct wp_layout *, int y);          /* line covering y (clamped) */
/* Caret position in layout coordinates: x, line top and height. */
void wp_layout_caret(const struct wp_layout *, const struct wp_doc *, const struct wp_text_ops *, size_t pos, int *x, int *y, int *h);
/* Character boundary nearest to the point (layout coordinates). */
size_t wp_layout_hit(const struct wp_layout *, const struct wp_doc *, const struct wp_text_ops *, int x, int y);

enum { WP_PIECE_TEXT, WP_PIECE_TAB };
struct wp_piece { size_t start, end; int x, w; struct wp_cattr a; int kind; };
/* The drawable pieces of a line in order (text clipped to the line; a tab has no text). A nonzero return stops. */
typedef int (*wp_piece_fn)(void *, const struct wp_piece *);
void wp_layout_pieces(const struct wp_layout *, const struct wp_doc *, const struct wp_text_ops *, unsigned line, wp_piece_fn, void *);
/* Bullet position of a line flagged WP_LINE_BULLET (pixels from the left margin). */
int wp_layout_bullet_x(const struct wp_doc *, const struct wp_line *);
int wp_twips_px(int twips);   /* rounded to the nearest pixel */
unsigned wp_size_px64(unsigned half_points);   /* em size in 26.6 pixels at 96 dpi */

/* ---- pages (print preview and printing) ---------------------------------- */
struct wp_page_setup {
    int paper_w, paper_h;                         /* twips */
    int left, right, top, bottom;                 /* margins, twips */
    int landscape;
};
struct wp_paper { const char *name; int w, h; };  /* twips */
extern const struct wp_paper wp_papers[];
extern const unsigned wp_paper_count;
struct wp_page_setup wp_default_page_setup(int a4);
struct wp_pages { unsigned *first_line; unsigned count; };   /* first line of each page; count >= 1 */
/* Lays the document out at the printable width of `setup` and breaks it into pages. `layout` receives the page layout. */
int wp_paginate(struct wp_pages *, struct wp_layout *layout, const struct wp_doc *, const struct wp_text_ops *, const struct wp_page_setup *);
void wp_pages_free(struct wp_pages *);
#endif
