/* SPDX-License-Identifier: MIT
 * Multi-line text area for libdeskui (f2-32). See textarea.h. Integer only. */
#include "textarea.h"
#include "internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


/* ---- characters --------------------------------------------------------- */

/* Byte length of the character at i: a valid UTF-8 sequence or one byte. */
static size_t ch_len(const char *s, size_t n, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    size_t need = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if (!need || i + need >= n) return 1;
    unsigned char c1 = (unsigned char)s[i + 1];
    unsigned char lo = 0x80, hi = 0xbf;
    if (c == 0xe0) lo = 0xa0; else if (c == 0xed) hi = 0x9f; else if (c == 0xf0) lo = 0x90; else if (c == 0xf4) hi = 0x8f;
    if (c1 < lo || c1 > hi) return 1;
    for (size_t k = 2; k <= need; k++) if (((unsigned char)s[i + k] & 0xc0) != 0x80) return 1;
    return need + 1;
}
static size_t prev_char(const char *s, size_t n, size_t p)
{
    if (!p) return 0;
    size_t q = p - 1;
    while (q > 0 && p - q < 4 && ((unsigned char)s[q] & 0xc0) == 0x80) q--;
    return ch_len(s, n, q) == p - q ? q : p - 1;
}
static size_t next_of(const struct du_textarea *ta, size_t p) { return p >= ta->len ? ta->len : p + ch_len(ta->text, ta->len, p); }
static size_t prev_of(const struct du_textarea *ta, size_t p) { return prev_char(ta->text, ta->len, p); }
/* Move p back to the start of the character containing it. */
static size_t snap(const struct du_textarea *ta, size_t p)
{
    if (p >= ta->len) return ta->len;
    size_t q = p;
    while (q > 0 && p - q < 3 && ((unsigned char)ta->text[q] & 0xc0) == 0x80) q--;
    return q < p && q + ch_len(ta->text, ta->len, q) > p ? q : p;
}
static int cclass(unsigned char c)
{
    if (c == ' ' || c == '\t' || c == '\n') return 0;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80) return 1;
    return 2;
}
static size_t count_lf(const char *s, size_t n)
{ size_t k = 0; for (size_t i = 0; i < n; i++) k += s[i] == '\n'; return k; }

/* ---- lifecycle ---------------------------------------------------------- */

int du_ta_init(struct du_textarea *ta, size_t cap)
{
    memset(ta, 0, sizeof(*ta));
    if (!cap || cap > (1u << 24)) return -1;
    ta->cap = cap; ta->vl_cap = (unsigned)cap + 2;
    ta->text = calloc(cap + 1, 1); ta->undo = calloc(cap + 1, 1);
    ta->vl = calloc(ta->vl_cap, sizeof(uint32_t));
    if (!ta->text || !ta->undo || !ta->vl) { du_ta_free(ta); return -1; }
    ta->vl_dirty = 1; ta->cols = 80; ta->rows = 24; ta->undo_kind = DU_TA_EDIT_OTHER;
    return 0;
}
void du_ta_free(struct du_textarea *ta)
{ free(ta->text); free(ta->undo); free(ta->vl); memset(ta, 0, sizeof(*ta)); }
static void clamp_scroll(struct du_textarea *);
void du_ta_set_view(struct du_textarea *ta, unsigned cols, unsigned rows)
{ ta->cols = cols ? cols : 1; ta->rows = rows ? rows : 1; clamp_scroll(ta); }

/* ---- layout ------------------------------------------------------------- */

static unsigned tab_adv(unsigned col) { return DU_TA_TAB - col % DU_TA_TAB; }
static unsigned cols_between(const struct du_textarea *ta, size_t from, size_t to)
{
    unsigned col = 0;
    for (size_t p = from; p < to;) {
        if (ta->text[p] == '\t') { col += tab_adv(col); p++; }
        else if (ta->text[p] == '\n') p++;
        else { col++; p += ch_len(ta->text, ta->len, p); }
    }
    return col;
}
/* Visual line starts. Wrapping breaks after the last blank that fits and
 * hard-breaks words longer than the line; blanks hang past the edge. */
static void layout(struct du_textarea *ta)
{
    unsigned width = ta->wrap ? ta->cols : 0;
    if (!ta->vl_dirty && ta->vl_cols == width) return;
    ta->vl_dirty = 0; ta->vl_cols = width; ta->vl_count = 0; ta->maxcol = 0;
    ta->vl[ta->vl_count++] = 0;
    size_t p = 0;
    for (;;) {
        size_t e = p;
        while (e < ta->len && ta->text[e] != '\n') e++;
        size_t vstart = p, brk = p; int have = 0; unsigned col = 0;
        for (size_t q = p; q < e;) {
            unsigned char c = (unsigned char)ta->text[q];
            if (c == ' ' || c == '\t') { col += c == '\t' ? tab_adv(col) : 1; q++; brk = q; have = 1; continue; }
            size_t n = ch_len(ta->text, ta->len, q);
            if (width && col >= width) {
                size_t at = have && brk > vstart ? brk : q;
                if (col > ta->maxcol) ta->maxcol = col;
                ta->vl[ta->vl_count++] = (uint32_t)at; vstart = at; have = 0;
                col = at == q ? 0 : cols_between(ta, at, q);
            }
            col++; q += n;
        }
        if (col > ta->maxcol) ta->maxcol = col;
        if (e >= ta->len) break;
        p = e + 1; ta->vl[ta->vl_count++] = (uint32_t)p;
    }
}
static unsigned vl_index(const struct du_textarea *ta, size_t off)
{
    unsigned lo = 0, hi = ta->vl_count;
    while (hi - lo > 1) { unsigned mid = (lo + hi) / 2; if (ta->vl[mid] <= off) lo = mid; else hi = mid; }
    return lo;
}
static size_t vl_end(const struct du_textarea *ta, unsigned i)
{
    if (i + 1 >= ta->vl_count) return ta->len;
    size_t e = ta->vl[i + 1];
    return ta->text[e - 1] == '\n' ? e - 1 : e;
}
static int vl_soft(const struct du_textarea *ta, unsigned i)
{ return i + 1 < ta->vl_count && ta->text[ta->vl[i + 1] - 1] != '\n'; }
static size_t vl_limit(const struct du_textarea *ta, unsigned i)
{ size_t e = vl_end(ta, i); return vl_soft(ta, i) ? prev_of(ta, e) : e; }
static void locate(struct du_textarea *ta, size_t off, int affinity, unsigned *vi, unsigned *col)
{
    layout(ta);
    unsigned i = vl_index(ta, off);
    if (affinity && i > 0 && off == ta->vl[i] && ta->text[off - 1] != '\n') i--;
    *vi = i; *col = cols_between(ta, ta->vl[i], off);
}
static size_t offset_at(const struct du_textarea *ta, unsigned vi, unsigned col)
{
    size_t p = ta->vl[vi], lim = vl_limit(ta, vi); unsigned c = 0;
    while (p < lim) {
        unsigned w = ta->text[p] == '\t' ? tab_adv(c) : 1;
        size_t n = ta->text[p] == '\t' ? 1 : ch_len(ta->text, ta->len, p);
        if (c + w > col) break;
        c += w; p += n;
    }
    return p > lim ? lim : p;
}
unsigned du_ta_visual_lines(struct du_textarea *ta) { layout(ta); return ta->vl_count; }
unsigned du_ta_visual_line_of(struct du_textarea *ta, size_t at, unsigned *col)
{ unsigned vi, c; locate(ta, at > ta->len ? ta->len : at, 0, &vi, &c); if (col) *col = c; return vi; }

static void clamp_scroll(struct du_textarea *ta)
{
    layout(ta);
    unsigned rows = ta->rows ? ta->rows : 1, max_top = ta->vl_count > rows ? ta->vl_count - rows : 0;
    if (ta->top > max_top) ta->top = max_top;
    if (ta->wrap) ta->left = 0;
    else {
        unsigned cols = ta->cols ? ta->cols : 1, extent = ta->maxcol + 1, max_left = extent > cols ? extent - cols : 0;
        if (ta->left > max_left) ta->left = max_left;
    }
}
void du_ta_reveal(struct du_textarea *ta)
{
    unsigned vi, col; locate(ta, ta->cursor, ta->affinity, &vi, &col);
    unsigned rows = ta->rows ? ta->rows : 1, cols = ta->cols ? ta->cols : 1;
    if (vi < ta->top) ta->top = vi; else if (vi >= ta->top + rows) ta->top = vi - rows + 1;
    if (!ta->wrap) { if (col < ta->left) ta->left = col; else if (col >= ta->left + cols) ta->left = col - cols + 1; }
    clamp_scroll(ta);
}
void du_ta_set_wrap(struct du_textarea *ta, int wrap)
{ ta->wrap = wrap != 0; ta->vl_dirty = 1; ta->left = 0; du_ta_reveal(ta); }

/* ---- editing ------------------------------------------------------------ */

int du_ta_has_selection(const struct du_textarea *ta) { return ta->cursor != ta->anchor; }
void du_ta_selection(const struct du_textarea *ta, size_t *a, size_t *b)
{ *a = ta->cursor < ta->anchor ? ta->cursor : ta->anchor; *b = ta->cursor < ta->anchor ? ta->anchor : ta->cursor; }
int du_ta_can_undo(const struct du_textarea *ta) { return ta->undo_valid; }

static void undo_point(struct du_textarea *ta, int kind)
{
    int same = kind != DU_TA_EDIT_OTHER && ta->undo_valid && ta->undo_kind == kind &&
               ta->cursor == ta->edit_cursor && ta->cursor == ta->anchor;
    if (same) return;
    memcpy(ta->undo, ta->text, ta->len + 1);
    ta->undo_len = ta->len; ta->undo_cursor = ta->cursor; ta->undo_anchor = ta->anchor;
    ta->undo_valid = 1; ta->undo_kind = kind;
}
/* Any caret movement or selection change ends the current typing/delete group. */
static void break_group(struct du_textarea *ta) { ta->undo_kind = DU_TA_EDIT_OTHER; }
static void touched(struct du_textarea *ta)
{ ta->revision++; ta->edit_cursor = ta->cursor; ta->modified = 1; ta->vl_dirty = 1; ta->want_valid = 0; ta->affinity = 0; du_ta_reveal(ta); }

/* `s` must not point into the buffer. */
static int replace_range(struct du_textarea *ta, size_t a, size_t b, const char *s, size_t n, int kind)
{
    if (ta->readonly) return -1;
    if (ta->len - (b - a) + n > ta->cap) { ta->limit_hit = 1; return -1; }
    if (a == b && !n) return 0;
    undo_point(ta, kind);
    ta->lf = (unsigned)(ta->lf - count_lf(ta->text + a, b - a) + count_lf(s, n));
    memmove(ta->text + a + n, ta->text + b, ta->len - b + 1);
    memcpy(ta->text + a, s, n);
    ta->len = ta->len - (b - a) + n;
    ta->cursor = ta->anchor = a + n;
    touched(ta);
    return 0;
}
int du_ta_set_text(struct du_textarea *ta, const char *s, size_t n)
{
    if (n > ta->cap) return -1;
    ta->revision++;
    memcpy(ta->text, s, n); ta->text[n] = 0; ta->len = n; ta->lf = (unsigned)count_lf(s, n);
    ta->cursor = ta->anchor = 0; ta->top = ta->left = 0; ta->undo_valid = 0; ta->undo_kind = DU_TA_EDIT_OTHER;
    ta->modified = 0; ta->limit_hit = 0; ta->vl_dirty = 1; ta->want_valid = 0; ta->affinity = 0; ta->edit_cursor = 0;
    memset(&ta->selection, 0, sizeof(ta->selection)); ta->drag = DU_TA_DRAG_NONE;
    return 0;
}
int du_ta_insert(struct du_textarea *ta, const char *s, size_t n, enum du_ta_edit kind)
{ size_t a, b; du_ta_selection(ta, &a, &b); return replace_range(ta, a, b, s, n, kind); }
int du_ta_delete_selection(struct du_textarea *ta)
{ size_t a, b; du_ta_selection(ta, &a, &b); return a == b ? 0 : replace_range(ta, a, b, "", 0, DU_TA_EDIT_OTHER); }
int du_ta_backspace(struct du_textarea *ta)
{
    if (du_ta_has_selection(ta)) return du_ta_delete_selection(ta);
    if (!ta->cursor) return 0;
    return replace_range(ta, prev_of(ta, ta->cursor), ta->cursor, "", 0, DU_TA_EDIT_BACK);
}
int du_ta_delete(struct du_textarea *ta)
{
    if (du_ta_has_selection(ta)) return du_ta_delete_selection(ta);
    if (ta->cursor >= ta->len) return 0;
    return replace_range(ta, ta->cursor, next_of(ta, ta->cursor), "", 0, DU_TA_EDIT_FORWARD);
}
int du_ta_undo(struct du_textarea *ta)
{
    if (!ta->undo_valid || ta->readonly) return -1;
    char *t = ta->text; ta->text = ta->undo; ta->undo = t;
    size_t l = ta->len; ta->len = ta->undo_len; ta->undo_len = l;
    size_t c = ta->cursor, a = ta->anchor;
    ta->cursor = ta->undo_cursor; ta->anchor = ta->undo_anchor; ta->undo_cursor = c; ta->undo_anchor = a;
    ta->lf = (unsigned)count_lf(ta->text, ta->len);
    ta->undo_kind = DU_TA_EDIT_OTHER;
    touched(ta);
    return 0;
}

/* ---- selection and movement --------------------------------------------- */

void du_ta_select(struct du_textarea *ta, size_t anchor, size_t cursor)
{
    break_group(ta);
    ta->anchor = snap(ta, anchor); ta->cursor = snap(ta, cursor);
    ta->want_valid = 0; ta->affinity = 0; du_ta_reveal(ta);
}
void du_ta_select_all(struct du_textarea *ta) { du_ta_select(ta, 0, ta->len); }
void du_ta_select_word(struct du_textarea *ta, size_t at)
{
    break_group(ta);
    size_t a, b; du_text_unit(ta->text, at, 2, &a, &b);
    ta->anchor = a; ta->cursor = b; ta->want_valid = 0; ta->affinity = 0; du_ta_reveal(ta);
}
void du_ta_move(struct du_textarea *ta, enum du_ta_motion m, int extend)
{
    layout(ta); break_group(ta);
    size_t c = ta->cursor, a, b; int vertical = 0, aff = 0;
    unsigned vi, col, rows = ta->rows ? ta->rows : 1;
    du_ta_selection(ta, &a, &b);
    locate(ta, c, ta->affinity, &vi, &col);
    switch (m) {
    case DU_TA_LEFT: c = !extend && a != b ? a : prev_of(ta, c); break;
    case DU_TA_RIGHT: c = !extend && a != b ? b : next_of(ta, c); break;
    case DU_TA_UP: case DU_TA_DOWN: case DU_TA_PAGE_UP: case DU_TA_PAGE_DOWN: {
        if (!ta->want_valid) { ta->want_col = col; ta->want_valid = 1; }
        vertical = 1; unsigned target = vi;
        if (m == DU_TA_UP) target = vi ? vi - 1 : 0;
        else if (m == DU_TA_DOWN) target = vi + 1 < ta->vl_count ? vi + 1 : vi;
        else if (m == DU_TA_PAGE_UP) { target = vi > rows ? vi - rows : 0; ta->top = ta->top > rows ? ta->top - rows : 0; }
        else { target = vi + rows < ta->vl_count ? vi + rows : ta->vl_count - 1; ta->top += rows; }
        if (target == vi && (m == DU_TA_UP || m == DU_TA_PAGE_UP)) c = a != b && !extend ? a : c;
        else if (target == vi && (m == DU_TA_DOWN || m == DU_TA_PAGE_DOWN)) c = a != b && !extend ? b : c;
        else c = offset_at(ta, target, ta->want_col);
        break;
    }
    case DU_TA_HOME: c = ta->vl[vi]; break;
    case DU_TA_END:
        if (vl_soft(ta, vi)) { c = ta->vl[vi + 1]; aff = 1; } else c = vl_end(ta, vi);
        break;
    case DU_TA_DOC_START: c = 0; break;
    case DU_TA_DOC_END: c = ta->len; break;
    case DU_TA_WORD_RIGHT: {
        int cls = c < ta->len ? cclass((unsigned char)ta->text[c]) : 0;
        if (cls) while (c < ta->len && cclass((unsigned char)ta->text[c]) == cls) c = next_of(ta, c);
        while (c < ta->len && cclass((unsigned char)ta->text[c]) == 0) c = next_of(ta, c);
        break;
    }
    case DU_TA_WORD_LEFT: {
        while (c > 0 && cclass((unsigned char)ta->text[prev_of(ta, c)]) == 0) c = prev_of(ta, c);
        if (c > 0) { int cls = cclass((unsigned char)ta->text[prev_of(ta, c)]); while (c > 0 && cclass((unsigned char)ta->text[prev_of(ta, c)]) == cls) c = prev_of(ta, c); }
        break;
    }
    }
    ta->cursor = c; if (!extend) ta->anchor = c;
    ta->affinity = aff; if (!vertical) ta->want_valid = 0;
    du_ta_reveal(ta);
}
void du_ta_line_col(const struct du_textarea *ta, size_t at, unsigned *line, unsigned *col)
{
    if (at > ta->len) at = ta->len;
    unsigned l = 1; size_t s = 0;
    for (size_t i = 0; i < at; i++) if (ta->text[i] == '\n') { l++; s = i + 1; }
    unsigned c = 1;
    for (size_t p = s; p < at; p += ch_len(ta->text, ta->len, p)) c++;
    if (line) *line = l;
    if (col) *col = c;
}
unsigned du_ta_lines(const struct du_textarea *ta) { return ta->lf + 1; }
int du_ta_goto_line(struct du_textarea *ta, unsigned line)
{
    if (!line || line > ta->lf + 1) return -1;
    break_group(ta);
    size_t s = 0;
    for (unsigned n = 1; n < line; n++) { while (ta->text[s] != '\n') s++; s++; }
    ta->cursor = ta->anchor = s; ta->want_valid = 0; ta->affinity = 0; du_ta_reveal(ta);
    return 0;
}

/* ---- search ------------------------------------------------------------- */

static unsigned char fold(unsigned char c, int match_case) { return !match_case && c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }
static int match_in(const char *buf, size_t blen, size_t i, const char *needle, size_t n, int match_case)
{
    if (i + n > blen) return 0;
    if (i && ((unsigned char)buf[i] & 0xc0) == 0x80) return 0; /* inside a character */
    for (size_t k = 0; k < n; k++)
        if (fold((unsigned char)buf[i + k], match_case) != fold((unsigned char)needle[k], match_case)) return 0;
    return 1;
}
long du_ta_find(const struct du_textarea *ta, const char *needle, size_t n, size_t from, int match_case, int backward)
{
    if (!n || n > ta->len) return -1;
    if (!backward) {
        for (size_t i = from; i + n <= ta->len; i++) if (match_in(ta->text, ta->len, i, needle, n, match_case)) return (long)i;
        return -1;
    }
    if (from < n) return -1;
    size_t i = from - n;
    if (i + n > ta->len) i = ta->len - n;
    for (;;) {
        if (match_in(ta->text, ta->len, i, needle, n, match_case)) return (long)i;
        if (!i) return -1;
        i--;
    }
}
int du_ta_find_select(struct du_textarea *ta, const char *needle, size_t n, int match_case, int backward)
{
    size_t a, b; du_ta_selection(ta, &a, &b);
    long at = du_ta_find(ta, needle, n, backward ? a : b, match_case, backward);
    if (at < 0) return -1;
    break_group(ta);
    ta->anchor = backward ? (size_t)at + n : (size_t)at; ta->cursor = backward ? (size_t)at : (size_t)at + n;
    ta->want_valid = 0; ta->affinity = 0; du_ta_reveal(ta);
    return 0;
}
int du_ta_selection_equals(const struct du_textarea *ta, const char *needle, size_t n, int match_case)
{
    size_t a, b; du_ta_selection(ta, &a, &b);
    return n && b - a == n && match_in(ta->text, ta->len, a, needle, n, match_case);
}
long du_ta_replace_all(struct du_textarea *ta, const char *needle, size_t n, const char *repl, size_t r, int match_case)
{
    if (!n || ta->readonly) return 0;
    size_t count = 0;
    for (size_t i = 0; i + n <= ta->len;) { if (match_in(ta->text, ta->len, i, needle, n, match_case)) { count++; i += n; } else i++; }
    if (!count) return 0;
    size_t result = ta->len - count * n + count * r;
    if (result > ta->cap) { ta->limit_hit = 1; return -1; }
    undo_point(ta, DU_TA_EDIT_OTHER); /* the snapshot now holds the old text: use it as the source */
    const char *old = ta->undo; size_t old_len = ta->undo_len, w = 0, last = 0;
    for (size_t i = 0; i < old_len;) {
        if (match_in(old, old_len, i, needle, n, match_case)) { memcpy(ta->text + w, repl, r); w += r; i += n; last = w; }
        else ta->text[w++] = old[i++];
    }
    ta->text[w] = 0; ta->len = w; ta->lf = (unsigned)count_lf(ta->text, w);
    ta->cursor = ta->anchor = last;
    touched(ta);
    return (long)count;
}

/* ---- pointer ------------------------------------------------------------ */

struct geo { struct du_rect body, text, vbar, hbar; };
static void geometry(const struct du_textarea *ta, struct du_rect box, struct geo *g)
{
    int hb = ta->wrap ? 0 : DU_TA_BAR;
    g->body = (struct du_rect){box.x + 2, box.y + 2, box.w - 4, box.h - 4};
    g->text = (struct du_rect){g->body.x, g->body.y, g->body.w - DU_TA_BAR, g->body.h - hb};
    g->vbar = (struct du_rect){g->body.x + g->body.w - DU_TA_BAR, g->body.y, DU_TA_BAR, g->body.h - hb};
    g->hbar = (struct du_rect){g->body.x, g->body.y + g->body.h - DU_TA_BAR, g->text.w, hb};
    if (ta->line_numbers) { g->text.x += 42; g->text.w -= 42; }
}
struct du_rect du_ta_caret(struct du_textarea *ta, struct du_rect box)
{
    struct geo g; geometry(ta,box,&g); unsigned line,col;
    locate(ta,ta->cursor,ta->affinity,&line,&col);
    if (line<ta->top || line>=ta->top+ta->rows || col<ta->left || col>ta->left+ta->cols) return (struct du_rect){0};
    return du_intersection(g.text,(struct du_rect){g.text.x+DU_TA_PAD+((int)col-(int)ta->left)*DU_TA_CELL_W-1,
        g.text.y+DU_TA_PAD+((int)line-(int)ta->top)*DU_TA_CELL_H+1,1,DU_TA_CELL_H-2});
}
void du_ta_fit(struct du_textarea *ta, struct du_rect box)
{
    struct geo g; geometry(ta, box, &g);
    int cols = (g.text.w - 2 * DU_TA_PAD) / DU_TA_CELL_W, rows = (g.text.h - 2 * DU_TA_PAD) / DU_TA_CELL_H;
    du_ta_set_view(ta, cols > 0 ? (unsigned)cols : 1, rows > 0 ? (unsigned)rows : 1);
}
static struct du_sb vscroll(struct du_textarea *ta, const struct geo *g)
{ layout(ta); return (struct du_sb){g->vbar, 1, (int)ta->top, (int)ta->vl_count, (int)ta->rows}; }
int du_ta_wheel(struct du_textarea *ta, struct du_rect box, int detents)
{
    du_ta_fit(ta,box);
    unsigned next=(unsigned)du_wheel_value((int)ta->top,detents,(int)ta->vl_count,(int)ta->rows);
    int changed=next!=ta->top;
    ta->top=next;
    return changed;
}
static struct du_sb hscroll(const struct du_textarea *ta, const struct geo *g)
{ return (struct du_sb){g->hbar, 0, (int)ta->left, (int)ta->maxcol + 1, (int)ta->cols}; }

size_t du_ta_hit(struct du_textarea *ta, struct du_rect box, int x, int y)
{
    struct geo g; geometry(ta, box, &g);
    layout(ta);
    int row = y - (g.text.y + DU_TA_PAD), vi;
    if (row < 0) { unsigned back = (unsigned)((-row + DU_TA_CELL_H - 1) / DU_TA_CELL_H); vi = ta->top > back ? (int)(ta->top - back) : 0; }
    else vi = (int)ta->top + row / DU_TA_CELL_H;
    if (vi >= (int)ta->vl_count) vi = (int)ta->vl_count - 1;
    int cx = x - (g.text.x + DU_TA_PAD) + DU_TA_CELL_W / 2;
    int col = cx < 0 ? 0 : cx / DU_TA_CELL_W;
    return offset_at(ta, (unsigned)vi, (unsigned)col + ta->left);
}
static void reveal_pointer(struct du_textarea *ta)
{
    size_t cursor=ta->cursor;
    /* A line includes its newline. Keep its last selected character visible
     * instead of scrolling a one-row view to the next, unselected line. */
    if(ta->selection.clicks>1 && ta->cursor>ta->anchor)ta->cursor=prev_of(ta,ta->cursor);
    du_ta_reveal(ta); ta->cursor=cursor;
}
int du_ta_mouse_down(struct du_textarea *ta, struct du_rect box, int x, int y, unsigned modifiers, uint64_t ns)
{
    struct geo g; geometry(ta, box, &g);
    du_ta_fit(ta, box); layout(ta); clamp_scroll(ta);
    struct du_sb v = vscroll(ta, &g), h = hscroll(ta, &g);
    int pv = du_sb_part(&v, x, y), ph = g.hbar.h ? du_sb_part(&h, x, y) : 0;
    if (pv || ph) {
        const struct du_sb *s = pv ? &v : &h;
        int part = pv ? pv : ph;
        if (part == 3) { ta->drag = pv ? DU_TA_DRAG_VTHUMB : DU_TA_DRAG_HTHUMB; ta->grab = du_sb_grab(s, x, y); }
        else if (pv) ta->top = (unsigned)du_sb_apply(s, part);
        else ta->left = (unsigned)du_sb_apply(s, part);
        return 1;
    }
    if (!du_contains(g.text, x, y)) return 0;
    size_t off = du_ta_hit(ta, box, x, y);
    break_group(ta);
    du_select_click(&ta->selection, ta->text, off, (modifiers & DU_SHIFT) != 0, ns, x, y, &ta->anchor, &ta->cursor);
    ta->want_valid = 0; ta->affinity = 0; ta->drag = DU_TA_DRAG_SELECT;
    reveal_pointer(ta);
    return 1;
}
int du_ta_mouse_move(struct du_textarea *ta, struct du_rect box, int x, int y)
{
    if (ta->drag == DU_TA_DRAG_NONE) return 0;
    struct geo g; geometry(ta, box, &g);
    if (ta->drag == DU_TA_DRAG_VTHUMB) { struct du_sb v = vscroll(ta, &g); ta->top = (unsigned)du_sb_drag(&v, x, y, ta->grab); return 1; }
    if (ta->drag == DU_TA_DRAG_HTHUMB) { struct du_sb h = hscroll(ta, &g); ta->left = (unsigned)du_sb_drag(&h, x, y, ta->grab); return 1; }
    break_group(ta);
    du_select_drag(&ta->selection, ta->text, du_ta_hit(ta, box, x, y), &ta->anchor, &ta->cursor);
    ta->want_valid = 0; ta->affinity = 0;
    reveal_pointer(ta);
    return 1;
}
void du_ta_mouse_up(struct du_textarea *ta) { ta->drag = DU_TA_DRAG_NONE; }

/* ---- keyboard ----------------------------------------------------------- */

/* Editing keys; printable characters arrive through du_ta_text (the toolkit
 * derives them from KEY events with Shift/Caps Lock, as for edit widgets). */
int du_ta_key(struct du_textarea *ta, unsigned code, unsigned modifiers)
{
    int shift = (modifiers & DU_SHIFT) != 0, ctrl = (modifiers & DU_CTRL) != 0;
    if (modifiers & DU_ALT) return 0;
    ta->selection.ns = 0;
    if (ctrl) {
        if (code == 0x1e) { du_ta_select_all(ta); return 1; }
        if (code == 0x2e || code == 0x2d) {
            size_t a, b; du_ta_selection(ta, &a, &b);
            if (a != b && !du_clipboard_set_n(ta->text + a, b - a) && code == 0x2d)
                du_ta_delete_selection(ta);
            return 1;
        }
        if (code == 0x2f) {
            const char *s = du_clipboard_get();
            du_ta_insert(ta, s, strlen(s), DU_TA_EDIT_OTHER); return 1;
        }
    }
    switch (code) {
    case DU_KEY_LEFT: du_ta_move(ta, ctrl ? DU_TA_WORD_LEFT : DU_TA_LEFT, shift); return 1;
    case DU_KEY_RIGHT: du_ta_move(ta, ctrl ? DU_TA_WORD_RIGHT : DU_TA_RIGHT, shift); return 1;
    case DU_KEY_UP: du_ta_move(ta, DU_TA_UP, shift); return 1;
    case DU_KEY_DOWN: du_ta_move(ta, DU_TA_DOWN, shift); return 1;
    case DU_KEY_HOME: du_ta_move(ta, ctrl ? DU_TA_DOC_START : DU_TA_HOME, shift); return 1;
    case DU_KEY_END: du_ta_move(ta, ctrl ? DU_TA_DOC_END : DU_TA_END, shift); return 1;
    case DU_KEY_PGUP: du_ta_move(ta, DU_TA_PAGE_UP, shift); return 1;
    case DU_KEY_PGDN: du_ta_move(ta, DU_TA_PAGE_DOWN, shift); return 1;
    case DU_KEY_BACKSPACE: du_ta_backspace(ta); return 1;
    case DU_KEY_DELETE: du_ta_delete(ta); return 1;
    case DU_KEY_ENTER: case 0x11c: if (!ctrl) du_ta_insert(ta, "\n", 1, DU_TA_EDIT_TYPE); return !ctrl;
    case DU_KEY_TAB: if (!ctrl) du_ta_insert(ta, "\t", 1, DU_TA_EDIT_TYPE); return !ctrl;
    default: return 0;
    }
}
int du_ta_text(struct du_textarea *ta, unsigned ch)
{
    if (ch < 32 || ch == 127 || ch > 0x10ffffu) return 0;
    ta->selection.ns = 0;
    char s[4]; size_t n;
    if (ch < 0x80) { s[0] = (char)ch; n = 1; }
    else if (ch < 0x800) { s[0] = (char)(0xc0 | (ch >> 6)); s[1] = (char)(0x80 | (ch & 63)); n = 2; }
    else if (ch < 0x10000) { s[0] = (char)(0xe0 | (ch >> 12)); s[1] = (char)(0x80 | ((ch >> 6) & 63)); s[2] = (char)(0x80 | (ch & 63)); n = 3; }
    else { s[0] = (char)(0xf0 | (ch >> 18)); s[1] = (char)(0x80 | ((ch >> 12) & 63)); s[2] = (char)(0x80 | ((ch >> 6) & 63)); s[3] = (char)(0x80 | (ch & 63)); n = 4; }
    du_ta_insert(ta, s, n, DU_TA_EDIT_TYPE);
    return 1;
}

/* ---- drawing ------------------------------------------------------------ */

/* Latin-1 and the face's extra glyphs come from the monospace face; anything
 * else (control characters, other scripts) is a hollow placeholder box. */
static void draw_char(struct du_canvas *c, int x, int y, unsigned ch, uint32_t ink, int bold)
{
    if (desk_font_code_index(ch)>=0) {
        char s[4]; size_t n;
        if (ch < 0x80) { s[0] = (char)ch; n = 1; }
        else if (ch < 0x800) { s[0] = (char)(0xc0 | (ch >> 6)); s[1] = (char)(0x80 | (ch & 63)); n = 2; }
        else { s[0] = (char)(0xe0 | (ch >> 12)); s[1] = (char)(0x80 | ((ch >> 6) & 63)); s[2] = (char)(0x80 | (ch & 63)); n = 3; }
        s[n] = 0;
        du_text_font(c, bold ? desk_theme->font_mono_bold : desk_theme->font_mono, x, y, s, ink);
    } else du_outline(c, (struct du_rect){x + 1, y, DU_TA_CELL_W - 2, desk_font_cap(desk_theme->font_mono)}, ink);
}
void du_ta_draw(struct du_canvas *c, struct du_rect box, struct du_textarea *ta, int focused)
{
    if (!ta) return;
    struct geo g; geometry(ta, box, &g);
    du_ta_fit(ta, box); layout(ta); clamp_scroll(ta);
    du_well(c, box, DU_WINDOW);
    struct du_rect saved = c->clip;
    if (ta->line_numbers) {
        struct du_rect gutter = {g.body.x, g.body.y, 42, g.text.h};
        c->clip = du_intersection(saved, gutter);
        du_fill(c, gutter, DU_FACE);
        unsigned logical = 1;
        size_t before = ta->top < ta->vl_count ? ta->vl[ta->top] : ta->len;
        for (size_t p = 0; p < before; ++p) logical += ta->text[p] == '\n';
        for (unsigned row = 0; row < ta->rows && ta->top + row < ta->vl_count; ++row) {
            size_t p = ta->vl[ta->top + row];
            if (!p || ta->text[p-1] == '\n') {
                char label[16]; snprintf(label, sizeof(label), "%u", logical);
                du_text_font(c, desk_theme->font_mono, gutter.x + 38 - du_text_width_font(desk_theme->font_mono,label),
                             gutter.y + DU_TA_PAD + (int)row * DU_TA_CELL_H + 2, label, DU_GRAY);
            }
            size_t end = vl_end(ta,ta->top + row);
            if (end < ta->len && ta->text[end] == '\n') ++logical;
        }
    }
    c->clip = du_intersection(saved, g.text);
    size_t a, b; du_ta_selection(ta, &a, &b);
    int ox = g.text.x + DU_TA_PAD, oy = g.text.y + DU_TA_PAD, glyph_dy = (DU_TA_CELL_H - desk_font_cap(desk_theme->font_mono)) / 2 - 1;
    uint32_t sel_bg = focused ? DU_SELECT : DU_GRAY;
    for (unsigned n = 0; n < ta->rows && ta->top + n < ta->vl_count; n++) {
        unsigned vi = ta->top + n; int y = oy + (int)n * DU_TA_CELL_H;
        size_t p = ta->vl[vi], end = vl_end(ta, vi); unsigned col = 0;
        while (p < end && col < ta->left + ta->cols) {
            unsigned char ch = (unsigned char)ta->text[p];
            unsigned w = ch == '\t' ? tab_adv(col) : 1; size_t len = ch == '\t' ? 1 : ch_len(ta->text, ta->len, p);
            if (col + w > ta->left) {
                int x = ox + ((int)col - (int)ta->left) * DU_TA_CELL_W;
                int sel = p >= a && p < b;
                if (sel) du_fill(c, (struct du_rect){x, y, (int)w * DU_TA_CELL_W, DU_TA_CELL_H}, sel_bg);
                if (ch != '\t') {
                    size_t used; unsigned cp = len == 1 ? ch : desk_utf8_decode(ta->text + p, &used);
                    uint32_t ink = ta->colour ? ta->colour(ta->colour_ctx, p, DU_INK) : DU_INK;
                    draw_char(c, x, y + glyph_dy, cp, sel ? DU_SELECT_INK : ink, ta->bold);
                }
            }
            col += w; p += len;
        }
        if (end < ta->len && ta->text[end] == '\n' && a <= end && b > end && col >= ta->left && col < ta->left + ta->cols)
            du_fill(c, (struct du_rect){ox + ((int)col - (int)ta->left) * DU_TA_CELL_W, y, DU_TA_CELL_W, DU_TA_CELL_H}, sel_bg);
    }
    if (focused && a == b) {
        unsigned vi, col; locate(ta, ta->cursor, ta->affinity, &vi, &col);
        if (vi >= ta->top && vi < ta->top + ta->rows && col >= ta->left && col <= ta->left + ta->cols)
            du_vline(c, ox + ((int)col - (int)ta->left) * DU_TA_CELL_W - 1, oy + (int)(vi - ta->top) * DU_TA_CELL_H + 1, DU_TA_CELL_H - 2, DU_INK);
    }
    c->clip = du_intersection(saved, g.body);
    struct du_sb v = vscroll(ta, &g);
    du_sb_draw(c, &v, 0);
    if (g.hbar.h) {
        struct du_sb h = hscroll(ta, &g);
        du_sb_draw(c, &h, 0);
        du_fill(c, (struct du_rect){g.vbar.x, g.hbar.y, DU_TA_BAR, g.hbar.h}, DU_FACE);
    }
    c->clip = saved;
}
