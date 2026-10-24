/* SPDX-License-Identifier: MIT
 * Character Map model: Unicode subsets, grid scrolling, selection, code point
 * to UTF-8 and the Windows Alt+0nnn keystroke. */
#include "charmap.h"
#include <stdio.h>
#include <string.h>

/* Blocks of the Unicode Character Database (Blocks.txt); the first block starts at the space,
 * as the Windows 98 Basic Latin subrange shows printable characters only. */
const struct charmap_subset charmap_subsets[] = {
    {"Basic Latin", 0x20, 0x7e},
    {"Latin-1 Supplement", 0xa0, 0xff},
    {"Latin Extended-A", 0x100, 0x17f},
    {"Latin Extended-B", 0x180, 0x24f},
    {"IPA Extensions", 0x250, 0x2af},
    {"Greek and Coptic", 0x370, 0x3ff},
    {"Cyrillic", 0x400, 0x4ff},
    {"General Punctuation", 0x2000, 0x206f},
    {"Currency Symbols", 0x20a0, 0x20cf},
    {"Letterlike Symbols", 0x2100, 0x214f},
    {"Number Forms", 0x2150, 0x218f},
    {"Arrows", 0x2190, 0x21ff},
    {"Mathematical Operators", 0x2200, 0x22ff},
    {"Box Drawing", 0x2500, 0x257f},
    {"Block Elements", 0x2580, 0x259f},
    {"Geometric Shapes", 0x25a0, 0x25ff},
    {"Miscellaneous Symbols", 0x2600, 0x26ff},
};
const unsigned charmap_subset_count = sizeof(charmap_subsets) / sizeof(charmap_subsets[0]);

size_t charmap_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); out[2] = 0; return 2; }
    if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd;
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); out[3] = 0;
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18 & 7)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); out[4] = 0;
    return 4;
}

int charmap_covered(const struct charmap_app *a, uint32_t cp)
{
    if (!a->face) return (cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff) || cp == 0x2022 || cp == 0x2026;
    unsigned glyph = 0;
    return !ciuki_font_lookup(a->face, cp, &glyph) && glyph != 0;
}

/* Windows 98 enters a character of the ANSI (code page 1252) set with Alt and 0 plus its decimal code. */
int charmap_keystroke(uint32_t cp, char *out, size_t cap)
{
    unsigned code;
    if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)) code = cp;
    else if (cp == 0x20ac) code = 128;
    else if (cp == 0x2026) code = 133;
    else if (cp == 0x2022) code = 149;
    else if (cp == 0x2122) code = 153;
    else { if (cap) out[0] = 0; return 0; }
    snprintf(out, cap, "Alt+0%03u", code);
    return 1;
}

unsigned charmap_total_rows(const struct charmap_app *a)
{
    const struct charmap_subset *s = &charmap_subsets[a->subset];
    return (s->last - s->first + 1 + CHARMAP_COLS - 1) / CHARMAP_COLS;
}
static void show_row(struct charmap_app *a, unsigned row)
{
    const struct charmap_subset *s = &charmap_subsets[a->subset];
    unsigned total = charmap_total_rows(a), first = row;
    if (total <= CHARMAP_ROWS) first = 0;
    else if (first > total - CHARMAP_ROWS) first = total - CHARMAP_ROWS;
    a->top = s->first + first * CHARMAP_COLS;
    struct du_widget *w = du_get(a->ui, CM_SCROLL);
    if (w) { w->value = (int)first; w->total = (int)total; w->page = CHARMAP_ROWS; }
}
void charmap_select(struct charmap_app *a, uint32_t cp)
{
    const struct charmap_subset *s = &charmap_subsets[a->subset];
    if (cp < s->first) cp = s->first;
    if (cp > s->last) cp = s->last;
    a->selected = cp;
    unsigned row = (cp - s->first) / CHARMAP_COLS, top_row = (a->top - s->first) / CHARMAP_COLS;
    if (row < top_row) show_row(a, row);
    else if (row >= top_row + CHARMAP_ROWS) show_row(a, row - CHARMAP_ROWS + 1);
    else show_row(a, top_row);
}
void charmap_set_subset(struct charmap_app *a, unsigned subset)
{
    if (subset >= charmap_subset_count) subset = 0;
    a->subset = subset; a->top = charmap_subsets[subset].first;
    a->selected = a->top;
    show_row(a, 0);
}
int charmap_goto(struct charmap_app *a, uint32_t cp)
{
    if (cp >= charmap_subsets[a->subset].first && cp <= charmap_subsets[a->subset].last) { charmap_select(a, cp); return 0; }
    for (unsigned i = 0; i < charmap_subset_count; i++)
        if (cp >= charmap_subsets[i].first && cp <= charmap_subsets[i].last) {
            a->subset = i; a->top = charmap_subsets[i].first; charmap_select(a, cp);
            return 0;
        }
    return -1;
}
void charmap_scroll(struct charmap_app *a, int rows)
{
    long row = (long)((a->top - charmap_subsets[a->subset].first) / CHARMAP_COLS) + rows;
    show_row(a, row < 0 ? 0 : (unsigned)row);
}
