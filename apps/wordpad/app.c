/* SPDX-License-Identifier: MIT
 * WordPad application logic. Layout of the file: helpers, editing core,
 * view and painting, find, files, dialogs, commands, events, lifecycle. */
#include "app.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../desktop/tz_env.h"

enum { K_N = 0x31, K_O = 0x18, K_S = 0x1f, K_P = 0x19, K_Z = 0x2c, K_Y = 0x15, K_X = 0x2d, K_C = 0x2e, K_V = 0x2f,
       K_A = 0x1e, K_F = 0x21, K_H = 0x23, K_B = 0x30, K_I = 0x17, K_U = 0x16, K_L = 0x26, K_E = 0x12, K_R = 0x13, K_F3 = 0x3d };
#define MENUBAR_H 20
#define TOOLBAR_H 30
#define FORMATBAR_H 28
#define RULER_H 20
#define STATUS_H 20
#define EDIT_H 22
#define SB_W 16
#define PAD 8
#define NO_PARA UINT_MAX
#define DOUBLE_NS 500000000ull
#define TEXT_FILTER "*.rtf;*.txt"

static const char *const yes_no_cancel[] = {"&Yes", "&No", "Cancel"};
static const char *const yes_no[] = {"&Yes", "&No"};
static const char *const color_names[] = {"Automatic", "Black", "Maroon", "Green", "Olive", "Navy", "Purple", "Teal", "Gray",
                                          "Silver", "Red", "Lime", "Yellow", "Blue", "Fuchsia", "Aqua", "White"};
static const unsigned sizes_hp[] = {16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 72, 96, 144};   /* 8..72 pt */
static const int zoom_steps[] = {25, 35, 50, 70, 100, 140};

static const struct du_menu_item file_template[] = {
    {"&New", M_NEW, K_N, DU_CTRL, 0}, {"&Open...", M_OPEN, K_O, DU_CTRL, 0}, {"&Save", M_SAVE, K_S, DU_CTRL, 0},
    {"Save &As...", M_SAVEAS, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Print...", M_PRINT, K_P, DU_CTRL, DU_ITEM_DISABLED}, {"Print Pre&view", M_PREVIEW, 0, 0, 0},
    {"Page Set&up...", M_PAGESETUP, 0, 0, 0}, {"E&xit", M_EXIT, 0, 0, 0}};
static const struct du_menu_item edit_template[] = {
    {"&Undo", M_UNDO, K_Z, DU_CTRL, 0}, {"&Redo", M_REDO, K_Y, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Cu&t", M_CUT, K_X, DU_CTRL, 0}, {"&Copy", M_COPY, K_C, DU_CTRL, 0}, {"&Paste", M_PASTE, K_V, DU_CTRL, 0},
    {"C&lear", M_CLEAR, DU_KEY_DELETE, 0, 0}, {"Select &All", M_SELALL, K_A, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Find...", M_FIND, K_F, DU_CTRL, 0}, {"Find &Next", M_FINDNEXT, K_F3, 0, 0}, {"R&eplace...", M_REPLACE, K_H, DU_CTRL, 0}};
static const struct du_menu_item view_template[] = {
    {"&Toolbar", M_TOOLBAR, 0, 0, 0}, {"&Format Bar", M_FORMATBAR, 0, 0, 0}, {"&Ruler", M_RULER, 0, 0, 0}, {"&Status Bar", M_STATUS, 0, 0, 0}};
static const struct du_menu_item insert_template[] = {{"&Date and Time...", M_DATETIME, 0, 0, 0}};
static const struct du_menu_item format_template[] = {
    {"&Font...", M_FONT, 0, 0, 0}, {"&Bullet Style", M_BULLET, 0, 0, 0}, {"&Paragraph...", M_PARAGRAPH, 0, 0, 0},
    {"&Tabs...", M_TABS, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}};
static const struct du_menu_item help_template[] = {
    {"&Help Topics", M_HELP, DU_KEY_F1, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About WordPad", M_ABOUT, 0, 0, 0}};
/* Positions inside the menus (wp_refresh updates their flags). */
enum { E_UNDO = 0, E_REDO, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR };

/* ---- helpers --------------------------------------------------------------- */

static struct du_widget *W(struct wp_app *a, int id) { return du_get(a->ui, id); }
static void set_flag(struct du_menu_item *item, unsigned flag, int on) { if (on) item->flags |= flag; else item->flags &= ~flag; }
static const char *document_name(const struct wp_app *a) { return a->named ? cn_basename(a->path) : "Document"; }
static void box(struct wp_app *a, int id, const char *title, int icon, const char *const *buttons, unsigned count,
                int def, int cancel, const char *fmt, ...)
{
    char text[DU_DIALOG_TEXT]; va_list ap;
    va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    du_message(a->ui, id, title, text, icon, buttons, count, def, cancel);
}
static void shorten(char *out, size_t size, const char *name, size_t max)
{
    snprintf(out, size, "%s", name);
    if (strlen(name) > max && max >= 3 && max < size) strcpy(out + max - 3, "...");
}
static void failure(struct wp_app *a, const char *what, const char *name, int error)
{
    char shown[48]; shorten(shown, sizeof(shown), name, 44);
    if (error < 0) error = -error;
    box(a, D_INFO, "WordPad", DU_DLG_ICON_ERROR, NULL, 0, 0, 0, "%s\n%s.\n---\n%s (error %d)", what, shown, strerror(error), error);
}
uint32_t wp_digest(const struct du_ui *ui)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)ui->canvas.width * (size_t)ui->canvas.height; i++)
        for (unsigned s = 0; s < 32; s += 8) h = (h ^ ((ui->canvas.pixels[i] >> s) & 255u)) * 16777619u;
    return h;
}
size_t wp_cursor(const struct wp_app *a) { return a->cursor; }
static const struct wp_text_ops *tops(const struct wp_app *a) { return &a->draw->text; }
static void sel_range(const struct wp_app *a, size_t *lo, size_t *hi)
{ *lo = a->cursor < a->anchor ? a->cursor : a->anchor; *hi = a->cursor < a->anchor ? a->anchor : a->cursor; }
static int has_selection(const struct wp_app *a) { return a->cursor != a->anchor; }

/* ---- layout and scrolling ---------------------------------------------------------- */

static void relayout(struct wp_app *a)
{
    int width = a->doc_w > 16 ? a->doc_w : 16;
    unsigned from = a->dirty_para == NO_PARA ? a->lay.pcount : a->dirty_para;
    int e = wp_layout_build(&a->lay, &a->doc, tops(a), width, from);
    if (e) snprintf(a->note, sizeof(a->note), "Not enough memory to lay out the document");
    a->dirty_para = NO_PARA;
    struct du_widget *sb = W(a, W_SCROLL);
    if (sb) {
        sb->total = a->lay.height + 8; sb->page = a->doc_h;
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        if (a->top > max) a->top = max;
        if (a->top < 0) a->top = 0;
        sb->value = a->top;
    }
}
static void mark_dirty(struct wp_app *a, size_t at)
{
    unsigned p = wp_para_at(&a->doc, at, NULL, NULL);
    if (p < a->dirty_para) a->dirty_para = p;
}
static void caret_xy(struct wp_app *a, size_t pos, int *x, int *y, int *h)
{ wp_layout_caret(&a->lay, &a->doc, tops(a), pos, x, y, h); }
static void reveal(struct wp_app *a)
{
    if (a->preview) return;
    int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
    if (y < a->top) a->top = y;
    else if (y + h > a->top + a->doc_h) a->top = y + h - a->doc_h;
    if (a->top < 0) a->top = 0;
    struct du_widget *sb = W(a, W_SCROLL);
    if (sb) {
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        if (a->top > max) a->top = max;
        sb->value = a->top;
    }
}

/* ---- format bar state --------------------------------------------------------------- */

static void format_state(struct wp_app *a)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    wp_char_summary(&a->doc, lo, hi, &a->cmix);
    wp_para_summary(&a->doc, lo, hi, &a->pmix);
    if (!has_selection(a) && a->pending_valid) {
        a->cmix.font = a->pending.font; a->cmix.size = a->pending.size; a->cmix.color = a->pending.color;
        a->cmix.bold = !!(a->pending.flags & WP_BOLD); a->cmix.italic = !!(a->pending.flags & WP_ITALIC);
        a->cmix.underline = !!(a->pending.flags & WP_UNDERLINE); a->cmix.strike = !!(a->pending.flags & WP_STRIKE);
    }
    if (a->cmix.font >= 0 && (unsigned)a->cmix.font < a->doc.nfonts) snprintf(a->font_label, sizeof(a->font_label), "%s", a->doc.fonts[a->cmix.font].name);
    else a->font_label[0] = 0;
    if (a->cmix.size > 0) snprintf(a->size_label, sizeof(a->size_label), "%d%s", a->cmix.size / 2, a->cmix.size & 1 ? ".5" : "");
    else a->size_label[0] = 0;
    struct du_widget *f = W(a, W_FONTBTN), *s = W(a, W_SIZEBTN);
    if (f && strcmp(f->text, a->font_label)) { du_set_text(f, a->font_label); du_invalidate(a->ui, f->box); }
    if (s && strcmp(s->text, a->size_label)) { du_set_text(s, a->size_label); du_invalidate(a->ui, s->box); }
}

void wp_refresh(struct wp_app *a)
{
    format_state(a);
    size_t lo, hi; sel_range(a, &lo, &hi);
    int line = wp_layout_line_of(&a->lay, a->cursor), col = 0;
    if (a->lay.n) for (size_t i = a->lay.lines[line].start; i < a->cursor && i < a->doc.len; i++) col += ((unsigned char)a->doc.text[i] & 0xc0) != 0x80;
    char text[200];
    if (a->preview) snprintf(text, sizeof(text), "Page %d of %u", a->preview_page + 1, a->pages.count);
    else snprintf(text, sizeof(text), "%s%s%s%s\tLn %d, Col %d", document_name(a), a->modified ? " *" : "", a->note[0] ? "   " : "", a->note, line + 1, col + 1);
    struct du_widget *st = W(a, W_STATUS);
    if (st && strcmp(st->text, text)) { du_set_text(st, text); du_invalidate_widget(a->ui, W_STATUS); }
    int sel = has_selection(a);
    set_flag(&a->edit_items[E_UNDO], DU_ITEM_DISABLED, !(a->undo_valid && !a->undo_is_redo));
    set_flag(&a->edit_items[E_REDO], DU_ITEM_DISABLED, !(a->undo_valid && a->undo_is_redo));
    set_flag(&a->edit_items[E_CUT], DU_ITEM_DISABLED, !sel);
    set_flag(&a->edit_items[E_COPY], DU_ITEM_DISABLED, !sel);
    set_flag(&a->edit_items[E_CLEAR], DU_ITEM_DISABLED, !sel);
    set_flag(&a->view_items[0], DU_ITEM_CHECKED, a->show_toolbar);
    set_flag(&a->view_items[1], DU_ITEM_CHECKED, a->show_format);
    set_flag(&a->view_items[2], DU_ITEM_CHECKED, a->show_ruler);
    set_flag(&a->view_items[3], DU_ITEM_CHECKED, a->show_status);
    set_flag(&a->format_items[1], DU_ITEM_CHECKED, a->pmix.bullet == 1);
    struct { int id, disabled; } tool[] = {{M_CUT, !sel}, {M_COPY, !sel}, {M_UNDO, !(a->undo_valid && !a->undo_is_redo)}};
    for (unsigned i = 0; i < 3; i++) {
        struct du_widget *w = W(a, tool[i].id);
        if (w && w->disabled != tool[i].disabled) { w->disabled = (uint8_t)tool[i].disabled; du_invalidate(a->ui, w->box); }
    }
    du_invalidate(a->ui, (struct du_rect){0, a->doc_box.y - RULER_H - FORMATBAR_H, a->ui->canvas.width, FORMATBAR_H});
}

/* ---- editing core -------------------------------------------------------------------- */

static void invalidate_doc(struct wp_app *a)
{
    du_invalidate(a->ui, a->doc_box);
    du_invalidate(a->ui, a->ruler_box);
}
static void changed(struct wp_app *a)
{
    a->modified = 1; a->note[0] = 0; a->want_valid = 0;
    relayout(a); reveal(a); invalidate_doc(a); wp_refresh(a);
}
static void moved(struct wp_app *a)
{
    reveal(a); invalidate_doc(a); wp_refresh(a);
}
/* One undo level: a snapshot before each command; typing and deleting runs share one. */
static void edit_begin(struct wp_app *a, enum wp_edit kind)
{
    if (kind != WP_EDIT_OTHER && a->last_edit == kind && a->cursor == a->last_edit_end && !has_selection(a) && a->undo_valid && !a->undo_is_redo) return;
    if (wp_copy(&a->undo, &a->doc)) { a->undo_valid = 0; return; }
    a->undo_cursor = a->cursor; a->undo_anchor = a->anchor; a->undo_valid = 1; a->undo_is_redo = 0;
}
static void edit_end(struct wp_app *a, enum wp_edit kind) { a->last_edit = kind; a->last_edit_end = a->cursor; }
static void set_cursor(struct wp_app *a, size_t pos, int extend, int keep_want)
{
    if (pos > a->doc.len) pos = a->doc.len;
    a->cursor = pos;
    if (!extend) a->anchor = pos;
    a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    if (!keep_want) a->want_valid = 0;
    moved(a);
}
static int memory_note(struct wp_app *a, int e)
{
    if (e == -EFBIG) snprintf(a->note, sizeof(a->note), "The document is at its size limit");
    else if (e) snprintf(a->note, sizeof(a->note), "Not enough memory");
    wp_refresh(a);
    return e;
}
static struct wp_cattr typing_attr(const struct wp_app *a)
{ return a->pending_valid ? a->pending : wp_attr_at(&a->doc, a->cursor); }

/* Replaces the selection with text (or removes it). */
static int replace_selection(struct wp_app *a, const char *s, size_t n, enum wp_edit kind)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (a->doc.len - (hi - lo) + n > WP_LIMIT) return memory_note(a, -EFBIG);
    struct wp_cattr attr = typing_attr(a);
    edit_begin(a, kind);
    mark_dirty(a, lo);
    int e = 0;
    if (hi > lo) e = wp_delete(&a->doc, lo, hi);
    if (!e && n) e = wp_insert(&a->doc, lo, s, n, &attr);
    if (e) { memory_note(a, e); a->cursor = a->anchor = lo; changed(a); return e; }
    a->cursor = a->anchor = lo + n; a->pending_valid = 0;
    edit_end(a, kind);
    changed(a);
    return 0;
}
static void delete_range(struct wp_app *a, size_t lo, size_t hi, enum wp_edit kind)
{
    if (hi <= lo) return;
    edit_begin(a, kind);
    mark_dirty(a, lo);
    int e = wp_delete(&a->doc, lo, hi);
    if (e) { memory_note(a, e); return; }
    a->cursor = a->anchor = lo; a->pending_valid = 0;
    edit_end(a, kind);
    changed(a);
}
static void insert_utf8(struct wp_app *a, uint32_t cp)
{
    char b[4]; unsigned n = wp_utf8_put(b, cp);
    replace_selection(a, b, n, WP_EDIT_TYPE);
}
static void apply_chars(struct wp_app *a, const struct wp_cstyle *st)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (lo == hi) {   /* a bare caret: the format of the next typed text */
        struct wp_cattr p = typing_attr(a);
        if (st->font >= 0) p.font = (uint8_t)st->font;
        if (st->size) p.size = (uint16_t)st->size;
        if (st->color >= 0) p.color = (uint8_t)st->color;
        p.flags = (uint8_t)((p.flags | st->set) & ~st->clear);
        a->pending = p; a->pending_valid = 1;
        wp_refresh(a); return;
    }
    edit_begin(a, WP_EDIT_OTHER);
    mark_dirty(a, lo);
    int e = wp_style_chars(&a->doc, lo, hi, st);
    if (e) { memory_note(a, e); return; }
    a->last_edit = WP_EDIT_OTHER;
    changed(a);
}
static void apply_paras(struct wp_app *a, const struct wp_pstyle *st)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    edit_begin(a, WP_EDIT_OTHER);
    mark_dirty(a, lo);
    wp_style_paras(&a->doc, lo, hi, st);
    a->last_edit = WP_EDIT_OTHER;
    changed(a);
}
static void toggle_flag(struct wp_app *a, unsigned flag, int state)
{
    struct wp_cstyle st = {-1, 0, -1, 0, 0};
    if (state == 1) st.clear = flag; else st.set = flag;
    apply_chars(a, &st);
}
static void set_align(struct wp_app *a, unsigned align)
{
    struct wp_pstyle st = {WP_P_ALIGN, wp_default_pattr()};
    st.v.align = (uint8_t)align;
    apply_paras(a, &st);
}
static void toggle_bullet(struct wp_app *a)
{
    struct wp_pstyle st = {WP_P_BULLET | WP_P_LEFT | WP_P_FIRST, wp_default_pattr()};
    if (a->pmix.bullet != 1) { st.v.bullet = 1; st.v.left = 360; st.v.first = -360; }
    apply_paras(a, &st);
}

static void undo_swap(struct wp_app *a)
{
    if (!a->undo_valid) return;
    struct wp_doc t = a->doc; a->doc = a->undo; a->undo = t;
    size_t c = a->cursor, n = a->anchor;
    a->cursor = a->undo_cursor > a->doc.len ? a->doc.len : a->undo_cursor;
    a->anchor = a->undo_anchor > a->doc.len ? a->doc.len : a->undo_anchor;
    a->undo_cursor = c; a->undo_anchor = n;
    a->undo_is_redo = !a->undo_is_redo; a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    a->dirty_para = 0; a->lay.pcount = 0;
    changed(a);
}

/* ---- movement -------------------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == WP_SOFT_BREAK; }
static size_t word_left(const struct wp_app *a, size_t at)
{
    while (at > 0 && is_space(a->doc.text[at - 1])) at--;
    while (at > 0 && !is_space(a->doc.text[at - 1])) at--;
    return at;
}
static size_t word_right(const struct wp_app *a, size_t at)
{
    while (at < a->doc.len && !is_space(a->doc.text[at])) at++;
    while (at < a->doc.len && is_space(a->doc.text[at]) && a->doc.text[at] != '\n') at++;
    return at;
}
static size_t line_end_pos(const struct wp_app *a, unsigned line)
{
    const struct wp_line *l = &a->lay.lines[line];
    size_t e = l->end;
    if ((l->flags & WP_LINE_WRAPPED) && e > l->start && a->doc.text[e - 1] == ' ') e--;
    return e;
}
static void vertical(struct wp_app *a, int dir, int pages, int extend)
{
    if (!a->lay.n) return;
    if (!a->want_valid) { int x, y, h; caret_xy(a, a->cursor, &x, &y, &h); a->want_x = x; a->want_valid = 1; }
    unsigned line = wp_layout_line_of(&a->lay, a->cursor);
    int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
    int ty = pages ? y + dir * (a->doc_h > h ? a->doc_h - h : h) : (dir < 0 ? y - 1 : y + h + 1);
    size_t pos;
    if (!pages && dir < 0 && line == 0) pos = 0;
    else if (!pages && dir > 0 && line + 1 >= a->lay.n) pos = a->doc.len;
    else {
        if (ty < 0) ty = 0;
        pos = wp_layout_hit(&a->lay, &a->doc, tops(a), a->want_x, ty);
        if (pages && dir > 0 && ty >= a->lay.height) pos = a->doc.len;
    }
    if (pages) { a->top += dir * (a->doc_h > h ? a->doc_h - h : h); if (a->top < 0) a->top = 0; }
    set_cursor(a, pos, extend, 1);
}

/* ---- clipboard ---------------------------------------------------------------------------- */

static int copy_selection(struct wp_app *a)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (lo == hi) return 0;
    struct wp_doc slice;
    if (wp_init(&slice)) return -ENOMEM;
    int e = wp_slice(&slice, &a->doc, lo, hi);
    char *plain = NULL; size_t n = 0;
    if (!e) e = wp_text_write(&slice, &plain, &n, 0);
    if (!e && du_clipboard_set_n(plain, n)) e = -ENOMEM;
    if (e) { free(plain); wp_free(&slice); return e; }
    wp_free(&a->clip); a->clip = slice; a->clip_valid = 1;
    free(a->clip_plain); a->clip_plain = plain;
    return 0;
}
static void paste(struct wp_app *a)
{
    const char *text = du_clipboard_get(); size_t n = strlen(text);
    if (!n) return;
    if (a->clip_valid && a->clip_plain && !strcmp(text, a->clip_plain)) {   /* our own copy: keep its formatting */
        size_t lo, hi; sel_range(a, &lo, &hi);
        if (a->doc.len - (hi - lo) + a->clip.len > WP_LIMIT) { memory_note(a, -EFBIG); return; }
        edit_begin(a, WP_EDIT_OTHER);
        mark_dirty(a, lo);
        int e = hi > lo ? wp_delete(&a->doc, lo, hi) : 0;
        if (!e) e = wp_insert_slice(&a->doc, lo, &a->clip);
        if (e) { memory_note(a, e); changed(a); return; }
        a->cursor = a->anchor = lo + a->clip.len; a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
        changed(a);
        return;
    }
    char *clean; size_t len;
    if (wp_sanitize(text, n, &clean, &len)) { memory_note(a, -ENOMEM); return; }
    replace_selection(a, clean, len, WP_EDIT_OTHER);
    free(clean);
}

/* ---- painting ------------------------------------------------------------------------------ */

struct paint_ctx {
    struct wp_app *a; struct du_canvas *c; struct du_rect box;
    int ox, oy;                   /* canvas position of the layout origin */
    int num, den;                 /* scale (preview) */
    int sel_x0, sel_x1, selected; /* selection span on this line, layout coordinates */
    int base; int line_h;
};
static uint32_t piece_colour(const struct wp_doc *d, const struct wp_cattr *a)
{ return a->color < d->ncolors ? d->colors[a->color] : 0; }
static void draw_text(struct paint_ctx *p, const struct wp_piece *pc, struct du_canvas *c, uint32_t rgb)
{
    const struct wp_doc *d = &p->a->doc;
    unsigned fam = pc->a.font < d->nfonts ? d->fonts[pc->a.font].family : WP_SANS;
    unsigned em = wp_size_px64(pc->a.size) * (unsigned)p->num / (unsigned)p->den;
    if (em < 64) em = 64;
    p->a->draw->draw(p->a->draw->text.ctx, c, fam, pc->a.flags, em, p->ox + pc->x * p->num / p->den, p->oy + p->base * p->num / p->den,
                     d->text + pc->start, pc->end - pc->start, rgb);
}
static int paint_piece(void *ctx, const struct wp_piece *pc)
{
    struct paint_ctx *p = ctx;
    struct du_canvas *c = p->c;
    int x = p->ox + pc->x * p->num / p->den, w = pc->w * p->num / p->den;
    if (x + w < p->box.x || x > p->box.x + p->box.w) return 0;
    uint32_t rgb = piece_colour(&p->a->doc, &pc->a);
    if (pc->kind == WP_PIECE_TEXT) draw_text(p, pc, c, rgb);
    int asc, desc; tops(p->a)->vmetrics(tops(p->a)->ctx, WP_SANS, 0, pc->a.size, &asc, &desc);
    int by = p->oy + p->base * p->num / p->den;
    if (pc->a.flags & WP_UNDERLINE) du_fill(c, (struct du_rect){x, by + 1, w, 1}, rgb);
    if (pc->a.flags & WP_STRIKE) du_fill(c, (struct du_rect){x, by - asc * p->num / (3 * p->den), w, 1}, rgb);
    if (p->selected && p->sel_x1 > p->sel_x0) {   /* the selected part again, in the highlight colours */
        int sx0 = p->ox + p->sel_x0 * p->num / p->den, sx1 = p->ox + p->sel_x1 * p->num / p->den;
        struct du_canvas inv = *c;
        inv.clip = du_intersection(c->clip, (struct du_rect){sx0, p->oy, sx1 - sx0, p->line_h * p->num / p->den});
        if (inv.clip.w > 0 && inv.clip.h > 0) {
            if (pc->kind == WP_PIECE_TEXT) draw_text(p, pc, &inv, DU_SELECT_INK);
            if (pc->a.flags & WP_UNDERLINE) du_fill(&inv, (struct du_rect){x, by + 1, w, 1}, DU_SELECT_INK);
        }
    }
    return 0;
}
/* Draws line `i` of layout l; ox/oy locate its origin. */
static void paint_line(struct wp_app *a, struct du_canvas *c, struct du_rect box, const struct wp_layout *l, unsigned i,
                       int ox, int oy, int num, int den, size_t lo, size_t hi)
{
    const struct wp_line *ln = &l->lines[i];
    struct paint_ctx p = {a, c, box, ox, oy + ln->y * num / den, num, den, 0, 0, 0, ln->base, ln->h};
    if (lo < hi && lo <= ln->end && hi > ln->start) {
        int x0, x1, y, h;
        if (lo <= ln->start) x0 = ln->shift + ln->indent; else { wp_layout_caret(l, &a->doc, tops(a), lo, &x0, &y, &h); }
        if (hi >= ln->end) { wp_layout_caret(l, &a->doc, tops(a), ln->end, &x1, &y, &h); if (hi > ln->end) x1 += 6; }
        else wp_layout_caret(l, &a->doc, tops(a), hi, &x1, &y, &h);
        if (x1 > x0) {
            p.selected = 1; p.sel_x0 = x0; p.sel_x1 = x1;
            du_fill(c, du_intersection(c->clip, (struct du_rect){ox + x0 * num / den, p.oy, (x1 - x0) * num / den, ln->h * num / den}), DU_SELECT);
        }
    }
    if (ln->flags & WP_LINE_BULLET) {
        struct wp_cattr at = wp_attr_of(&a->doc, ln->start);
        int bx = ox + wp_layout_bullet_x(&a->doc, ln) * num / den + 3 * num / den, by = p.oy + (ln->base - ln->h / 3) * num / den, r = 2 * num / den;
        uint32_t rgb = piece_colour(&a->doc, &at);
        for (int yy = -r; yy <= r; yy++) for (int xx = -r; xx <= r; xx++) if (xx * xx + yy * yy <= r * r + 1)
            du_fill(c, du_intersection(c->clip, (struct du_rect){bx + xx, by + yy, 1, 1}), rgb);
    }
    wp_layout_pieces(l, &a->doc, tops(a), i, paint_piece, &p);
}

static void paint_doc(struct wp_app *a, struct du_canvas *c)
{
    struct du_rect b = a->doc_box;
    struct du_canvas cv = *c; cv.clip = du_intersection(c->clip, b);
    if (cv.clip.w <= 0 || cv.clip.h <= 0) return;
    du_fill(&cv, b, 0xffffffu);
    size_t lo, hi; sel_range(a, &lo, &hi);
    int ox = b.x + PAD, oy = b.y - a->top;
    unsigned first = wp_layout_line_at_y(&a->lay, a->top);
    for (unsigned i = first; i < a->lay.n && a->lay.lines[i].y - a->top < b.h; i++)
        paint_line(a, &cv, b, &a->lay, i, ox, oy, 1, 1, lo, hi);
    if (lo == hi && a->caret_on && a->ui->focused_window && du_focused(a->ui) == W_DOC && !a->ui->dialog.kind) {
        int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
        du_fill(&cv, (struct du_rect){ox + x, oy + y, 1, h}, 0x000000u);
    }
}

static void paint_ruler(struct wp_app *a, struct du_canvas *c)
{
    struct du_rect r = a->ruler_box;
    if (!a->show_ruler || a->preview || r.w <= 0) return;
    struct du_canvas cv = *c; cv.clip = du_intersection(c->clip, r);
    if (cv.clip.w <= 0 || cv.clip.h <= 0) return;
    du_fill(&cv, r, DU_FACE);
    int x0 = r.x + PAD, width = a->doc_w;
    du_fill(&cv, (struct du_rect){x0, r.y + 4, width, r.h - 8}, 0xffffffu);
    du_outline(&cv, (struct du_rect){x0, r.y + 4, width, r.h - 8}, DU_SHADOW);
    for (int eighth = 0; eighth * 12 <= width; eighth++) {   /* 96 dpi: an inch is 96 px, an eighth 12 */
        int x = x0 + eighth * 12, tall = eighth % 8 == 0 ? 6 : eighth % 4 == 0 ? 4 : 2;
        if (eighth % 8 == 0 && eighth) { char n[8]; snprintf(n, sizeof(n), "%d", eighth / 8); du_text(&cv, x - 2, r.y + 5, n, DU_INK); }
        else du_fill(&cv, (struct du_rect){x, r.y + r.h / 2 - tall / 2, 1, tall}, DU_INK);
    }
    size_t lo, hi; sel_range(a, &lo, &hi);
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    int left = wp_twips_px(pa.left), first = wp_twips_px(pa.left + pa.first), right = wp_twips_px(pa.right);
    du_triangle(&cv, x0 + first, r.y + 3, 1, 3, DU_INK);               /* first-line indent */
    du_triangle(&cv, x0 + left, r.y + r.h - 4, 0, 3, DU_INK);          /* hanging indent */
    du_fill(&cv, (struct du_rect){x0 + width - right - 1, r.y + 5, 1, r.h - 10}, DU_INK);
}

static void paint_preview(struct wp_app *a, struct du_canvas *c)
{
    struct du_rect b = a->doc_box;
    struct du_canvas cv = *c; cv.clip = du_intersection(c->clip, b);
    if (cv.clip.w <= 0 || cv.clip.h <= 0) return;
    du_fill(&cv, b, DU_SHADOW);
    if (!a->preview_valid || !a->pages.count) return;
    int pw = a->page.landscape ? a->page.paper_h : a->page.paper_w, ph = a->page.landscape ? a->page.paper_w : a->page.paper_h;
    int num = a->preview_zoom, den = 100;
    int w = wp_twips_px(pw) * num / den, h = wp_twips_px(ph) * num / den;
    int px = b.x + (b.w > w + 16 ? (b.w - w) / 2 : 8), py = b.y + 8 - a->top;
    du_fill(&cv, (struct du_rect){px + 4, py + 4, w, h}, DU_DARK);
    du_fill(&cv, (struct du_rect){px, py, w, h}, 0xffffffu);
    du_outline(&cv, (struct du_rect){px, py, w, h}, 0x000000u);
    unsigned first = a->pages.first_line[a->preview_page];
    unsigned last = (unsigned)a->preview_page + 1 < a->pages.count ? a->pages.first_line[a->preview_page + 1] : a->plo.n;
    int base_y = a->plo.lines[first].y;
    int mx = wp_twips_px(a->page.left) * num / den, my = wp_twips_px(a->page.top) * num / den;
    struct du_canvas page = cv; page.clip = du_intersection(cv.clip, (struct du_rect){px, py, w, h});
    for (unsigned i = first; i < last; i++) {
        struct wp_line shifted = a->plo.lines[i];
        (void)shifted;
        paint_line(a, &page, b, &a->plo, i, px + mx, py + my - base_y * num / den, num, den, 0, 0);
    }
}

static void paint_hook(struct du_ui *u, void *user)
{
    struct wp_app *a = user;
    struct du_canvas *c = &u->canvas;
    if (a->preview) paint_preview(a, c); else paint_doc(a, c);
    paint_ruler(a, c);
    /* Pressed look of the toggled format buttons. */
    if (a->preview || !a->show_format) return;
    struct { int id, on; } tog[] = {{M_BOLD, a->cmix.bold == 1}, {M_ITALIC, a->cmix.italic == 1}, {M_UNDERLINE, a->cmix.underline == 1},
        {M_LEFT, a->pmix.align == WP_LEFT}, {M_CENTER, a->pmix.align == WP_CENTER}, {M_RIGHT, a->pmix.align == WP_RIGHT}, {M_BULLET, a->pmix.bullet == 1}};
    for (unsigned i = 0; i < sizeof(tog) / sizeof(*tog); i++) {
        struct du_widget *w = W(a, tog[i].id);
        if (w && tog[i].on && !w->hidden) du_edge(c, w->box, DU_SUNKEN_THIN);
    }
}

/* ---- layout of the window ----------------------------------------------------------------------- */

static const struct { int id; const char *icon; } toolbar_buttons[] = {
    {M_NEW, "action:new"}, {M_OPEN, "action:open"}, {M_SAVE, "action:save"}, {W_SEP1, NULL}, {M_PRINT, "action:print"},
    {M_PREVIEW, "action:print-preview"}, {W_SEP2, NULL}, {M_FIND, "action:find"}, {W_SEP3, NULL}, {M_CUT, "action:cut"},
    {M_COPY, "action:copy"}, {M_PASTE, "action:paste"}, {M_UNDO, "action:undo"}, {W_SEP4, NULL}, {M_DATETIME, "program:clock"}};
static const uint32_t *icon(const char *object, unsigned size) { return du_icon_argb(du_icon_name(object), size); }

static void preview_fit(struct wp_app *a);
void wp_layout_ui(struct wp_app *a)
{
    struct du_ui *u = a->ui;
    int w = u->canvas.width, h = u->canvas.height, y = MENUBAR_H + 1;
    int preview = a->preview;
    int bar_h = a->find_mode == 2 ? 58 : a->find_mode == 1 ? 30 : 0, status_h = a->show_status ? STATUS_H : 0;
    struct du_widget *tb = W(a, W_TOOLBAR);
    int show_tb = a->show_toolbar && !preview;
    tb->hidden = !show_tb; tb->box = (struct du_rect){0, y, w, TOOLBAR_H};
    int x = 6;
    for (unsigned i = 0; i < sizeof(toolbar_buttons) / sizeof(*toolbar_buttons); i++) {
        struct du_widget *b = W(a, toolbar_buttons[i].id);
        b->hidden = !show_tb;
        if (!toolbar_buttons[i].icon) { b->box = (struct du_rect){x + 3, y + 4, 2, TOOLBAR_H - 8}; x += 8; }
        else { b->box = (struct du_rect){x, y + 2, 28, TOOLBAR_H - 4}; x += 29; }
    }
    if (show_tb) y += TOOLBAR_H;
    struct du_widget *fb = W(a, W_FORMATBAR);
    int show_fb = a->show_format && !preview;
    fb->hidden = !show_fb; fb->box = (struct du_rect){0, y, w, FORMATBAR_H};
    static const struct { int id, width; } fbar[] = {{W_FONTBTN, 150}, {W_SIZEBTN, 46}, {W_COLORBTN, 24}, {W_SEP1 + 10, 0}, {M_BOLD, 26},
        {M_ITALIC, 26}, {M_UNDERLINE, 26}, {W_SEP1 + 11, 0}, {M_LEFT, 26}, {M_CENTER, 26}, {M_RIGHT, 26}, {M_BULLET, 26}};
    x = 6;
    for (unsigned i = 0; i < sizeof(fbar) / sizeof(*fbar); i++) {
        struct du_widget *b = W(a, fbar[i].id);
        if (!b) { x += 8; continue; }
        b->hidden = !show_fb;
        b->box = (struct du_rect){x, y + 2, fbar[i].width, FORMATBAR_H - 4}; x += fbar[i].width + 3;
        if (b->kind == DU_TOOLBUTTON) b->box.h = FORMATBAR_H - 4;
    }
    if (show_fb) y += FORMATBAR_H;
    a->ruler_box = (struct du_rect){0, y, w, a->show_ruler && !preview ? RULER_H : 0};
    if (a->show_ruler && !preview) y += RULER_H;
    struct du_widget *pv = W(a, W_PVBAR);
    pv->hidden = !preview;
    if (preview) {
        pv->box = (struct du_rect){0, y, w, TOOLBAR_H};
        static const int ids[] = {M_PV_PREV, M_PV_NEXT, M_PV_ZOOMOUT, M_PV_ZOOMIN, M_PV_CLOSE};
        static const int widths[] = {90, 90, 90, 90, 80};
        int px = 6;
        for (unsigned i = 0; i < 5; i++) {
            struct du_widget *b = W(a, ids[i]);
            b->hidden = 0; b->box = (struct du_rect){px, y + 3, widths[i], TOOLBAR_H - 6}; px += widths[i] + 4;
        }
        y += TOOLBAR_H;
    } else {
        static const int ids[] = {M_PV_PREV, M_PV_NEXT, M_PV_ZOOMOUT, M_PV_ZOOMIN, M_PV_CLOSE};
        for (unsigned i = 0; i < 5; i++) W(a, ids[i])->hidden = 1;
    }
    int bar_y = h - status_h - bar_h;
    a->doc_box = (struct du_rect){0, y, w - SB_W, bar_y - y};
    if (a->doc_box.h < 16) a->doc_box.h = 16;
    a->doc_w = a->doc_box.w - 2 * PAD; a->doc_h = a->doc_box.h;
    struct du_widget *d = W(a, W_DOC);
    d->box = a->doc_box;
    struct du_widget *sb = W(a, W_SCROLL);
    sb->box = (struct du_rect){w - SB_W, y, SB_W, a->doc_box.h};
    struct du_widget *st = W(a, W_STATUS);
    st->hidden = !a->show_status; st->box = (struct du_rect){0, h - STATUS_H, w, STATUS_H};
    st->panels[0] = w > 220 ? w - 130 : w / 2; st->panels[1] = 0;
    int fy = bar_y + 4;
    static const int find_ids[] = {W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE};
    static const int replace_ids[] = {W_RLABEL, W_REPL, W_RONE, W_RALL};
    W(a, W_FLABEL)->box = (struct du_rect){6, fy, 84, EDIT_H}; W(a, W_FIND)->box = (struct du_rect){92, fy, 150, EDIT_H};
    W(a, W_FNEXT)->box = (struct du_rect){248, fy, 76, EDIT_H}; W(a, W_FDIR)->box = (struct du_rect){328, fy, 54, EDIT_H};
    W(a, W_FCASE)->box = (struct du_rect){386, fy, 108, EDIT_H}; W(a, W_FCLOSE)->box = (struct du_rect){498, fy, 24, EDIT_H};
    fy += EDIT_H + 2;
    W(a, W_RLABEL)->box = (struct du_rect){6, fy, 84, EDIT_H}; W(a, W_REPL)->box = (struct du_rect){92, fy, 150, EDIT_H};
    W(a, W_RONE)->box = (struct du_rect){248, fy, 76, EDIT_H}; W(a, W_RALL)->box = (struct du_rect){328, fy, 90, EDIT_H};
    for (unsigned i = 0; i < 6; i++) W(a, find_ids[i])->hidden = !a->find_mode || preview;
    for (unsigned i = 0; i < 4; i++) W(a, replace_ids[i])->hidden = a->find_mode != 2 || preview;
    if (preview) preview_fit(a);
    a->dirty_para = a->lay.width == a->doc_w ? a->dirty_para : 0;
    if (!preview) relayout(a);
    u->dirty = 1;
}
static void resize_layout(struct du_ui *u, void *user) { (void)u; wp_layout_ui(user); reveal(user); }

/* ---- print preview --------------------------------------------------------------------------------- */

static void preview_build(struct wp_app *a)
{
    wp_pages_free(&a->pages);
    a->preview_valid = !wp_paginate(&a->pages, &a->plo, &a->doc, tops(a), &a->page);
    if (a->preview_page >= (int)a->pages.count) a->preview_page = (int)a->pages.count - 1;
    if (a->preview_page < 0) a->preview_page = 0;
}
static void preview_scroll_range(struct wp_app *a)
{
    struct du_widget *sb = W(a, W_SCROLL);
    int ph = a->page.landscape ? a->page.paper_w : a->page.paper_h;
    int h = wp_twips_px(ph) * a->preview_zoom / 100 + 16;
    sb->total = h; sb->page = a->doc_h;
    int max = h > a->doc_h ? h - a->doc_h : 0;
    if (a->top > max) a->top = max;
    sb->value = a->top;
}
static void preview_fit(struct wp_app *a)
{
    int pw = a->page.landscape ? a->page.paper_h : a->page.paper_w;
    int wpx = wp_twips_px(pw);
    int fit_w = (a->doc_box.w - 16) * 100 / (wpx ? wpx : 1);
    if (a->preview_zoom > fit_w) { int z = zoom_steps[0]; for (unsigned i = 0; i < sizeof(zoom_steps) / sizeof(*zoom_steps); i++) if (zoom_steps[i] <= fit_w) z = zoom_steps[i]; a->preview_zoom = z; }
    preview_scroll_range(a);
}
static void preview_open(struct wp_app *a)
{
    a->preview = 1; a->preview_page = 0; a->top = 0;
    int ph = a->page.landscape ? a->page.paper_w : a->page.paper_h;
    int fit_h = (a->doc_h - 16) * 100 / (wp_twips_px(ph) ? wp_twips_px(ph) : 1);
    a->preview_zoom = zoom_steps[0];
    for (unsigned i = 0; i < sizeof(zoom_steps) / sizeof(*zoom_steps); i++) if (zoom_steps[i] <= fit_h) a->preview_zoom = zoom_steps[i];
    preview_build(a);
    wp_layout_ui(a);
    wp_refresh(a);
    du_focus(a->ui, W_DOC);
    du_invalidate(a->ui, (struct du_rect){0, 0, a->ui->canvas.width, a->ui->canvas.height});
}
static void preview_close(struct wp_app *a)
{
    a->preview = 0; a->top = 0; wp_pages_free(&a->pages); a->preview_valid = 0;
    wp_layout_free(&a->plo);
    a->lay.pcount = 0; a->dirty_para = 0;
    wp_layout_ui(a); reveal(a); wp_refresh(a);
    du_focus(a->ui, W_DOC);
    du_invalidate(a->ui, (struct du_rect){0, 0, a->ui->canvas.width, a->ui->canvas.height});
}
static void preview_step(struct wp_app *a, int dir)
{
    int p = a->preview_page + dir;
    if (p < 0 || p >= (int)a->pages.count) return;
    a->preview_page = p; a->top = 0; preview_scroll_range(a); wp_refresh(a); invalidate_doc(a);
}
static void preview_zoom_by(struct wp_app *a, int dir)
{
    int at = 0, n = (int)(sizeof(zoom_steps) / sizeof(*zoom_steps));
    for (int i = 0; i < n; i++) if (zoom_steps[i] <= a->preview_zoom) at = i;
    int pw = a->page.landscape ? a->page.paper_h : a->page.paper_w;
    int fit_w = (a->doc_box.w - 16) * 100 / (wp_twips_px(pw) ? wp_twips_px(pw) : 1);
    int next = at + dir;
    if (next < 0 || next >= n || zoom_steps[next] > fit_w) return;
    a->preview_zoom = zoom_steps[next]; a->top = 0; preview_scroll_range(a); invalidate_doc(a);
}

/* ---- find ------------------------------------------------------------------------------------------- */

static void select_all_text(struct wp_app *a, int id)
{ struct du_widget *w = W(a, id); w->anchor = 0; w->cursor = strlen(w->text); w->offset = 0; du_invalidate(a->ui, w->box); }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static long find_text(const struct wp_doc *d, const char *needle, size_t n, size_t from, int match_case, int backward)
{
    if (!n || n > d->len) return -1;
    if (backward) {
        for (size_t i = from < d->len - n ? from : d->len - n + 1; i-- > 0;) {
            size_t k = 0;
            while (k < n && (match_case ? d->text[i + k] == needle[k] : lower(d->text[i + k]) == lower(needle[k]))) k++;
            if (k == n) return (long)i;
        }
        return -1;
    }
    for (size_t i = from; i + n <= d->len; i++) {
        size_t k = 0;
        while (k < n && (match_case ? d->text[i + k] == needle[k] : lower(d->text[i + k]) == lower(needle[k]))) k++;
        if (k == n) return (long)i;
    }
    return -1;
}
static void show_find(struct wp_app *a, int mode)
{
    a->find_mode = mode;
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (hi > lo && hi - lo < 200 && !memchr(a->doc.text + lo, '\n', hi - lo)) {
        char needle[208]; memcpy(needle, a->doc.text + lo, hi - lo); needle[hi - lo] = 0;
        du_set_text(W(a, W_FIND), needle);
    }
    wp_layout_ui(a);
    du_focus(a->ui, W_FIND); select_all_text(a, W_FIND);
}
static void hide_find(struct wp_app *a) { a->find_mode = 0; wp_layout_ui(a); du_focus(a->ui, W_DOC); wp_refresh(a); }
static int find_next(struct wp_app *a)
{
    const char *needle = W(a, W_FIND)->text; size_t n = strlen(needle);
    if (!n) { show_find(a, a->find_mode ? a->find_mode : 1); return -1; }
    size_t lo, hi; sel_range(a, &lo, &hi);
    long at = find_text(&a->doc, needle, n, a->find_up ? lo : hi, a->match_case, a->find_up);
    if (at < 0) {
        char shown[48]; shorten(shown, sizeof(shown), needle, 40);
        box(a, D_INFO, "WordPad", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "Finished searching the document.\n\"%s\" was not found.", shown);
        return -1;
    }
    a->anchor = (size_t)at; a->cursor = (size_t)at + n; a->last_edit = WP_EDIT_OTHER;
    moved(a);
    return 0;
}
static void replace_one(struct wp_app *a)
{
    const char *needle = W(a, W_FIND)->text, *repl = W(a, W_REPL)->text;
    size_t n = strlen(needle), lo, hi; sel_range(a, &lo, &hi);
    int equal = n && hi - lo == n;
    for (size_t i = 0; equal && i < n; i++) equal = a->match_case ? a->doc.text[lo + i] == needle[i] : lower(a->doc.text[lo + i]) == lower(needle[i]);
    if (equal) replace_selection(a, repl, strlen(repl), WP_EDIT_OTHER);
    find_next(a);
}
static void replace_all(struct wp_app *a)
{
    const char *needle = W(a, W_FIND)->text, *repl = W(a, W_REPL)->text;
    size_t n = strlen(needle), r = strlen(repl);
    if (!n) { show_find(a, 2); return; }
    long count = 0, at = find_text(&a->doc, needle, n, 0, a->match_case, 0);
    if (at < 0) { box(a, D_INFO, "WordPad", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "Cannot find \"%.40s\"", needle); return; }
    edit_begin(a, WP_EDIT_OTHER);
    mark_dirty(a, (size_t)at);
    size_t from = 0;
    while (at >= 0) {
        if (a->doc.len - n + r > WP_LIMIT) { memory_note(a, -EFBIG); break; }
        struct wp_cattr attr = wp_attr_of(&a->doc, (size_t)at);
        if (wp_delete(&a->doc, (size_t)at, (size_t)at + n) || wp_insert(&a->doc, (size_t)at, repl, r, &attr)) { memory_note(a, -ENOMEM); break; }
        count++; from = (size_t)at + r;
        at = find_text(&a->doc, needle, n, from, a->match_case, 0);
    }
    a->cursor = a->anchor = from > a->doc.len ? a->doc.len : from; a->last_edit = WP_EDIT_OTHER;
    changed(a);
    snprintf(a->note, sizeof(a->note), "Replaced %ld occurrence%s", count, count == 1 ? "" : "s");
    wp_refresh(a);
}

/* ---- files ---------------------------------------------------------------------------------------------- */

static int is_binary_office(const char *raw, size_t n)
{
    static const unsigned char ole[] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
    if (n >= 8 && !memcmp(raw, ole, 8)) return 1;                                   /* Word 6/97 .doc */
    if (n >= 2 && (unsigned char)raw[0] == 0x31 && (unsigned char)raw[1] == 0xbe) return 1;   /* Write .wri */
    if (n >= 2 && (unsigned char)raw[0] == 0x32 && (unsigned char)raw[1] == 0xbe) return 1;
    return 0;
}
static void reset_document(struct wp_app *a)
{
    a->undo_valid = 0; a->undo_is_redo = 0; a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    a->cursor = a->anchor = 0; a->top = 0; a->note[0] = 0; a->modified = 0;
    a->dirty_para = 0; a->lay.pcount = 0;
}
static void new_document(struct wp_app *a)
{
    wp_reset(&a->doc);
    reset_document(a);
    a->path[0] = 0; a->named = 0; a->format = WP_FMT_RTF; a->crlf = 1;
    relayout(a); reveal(a); invalidate_doc(a); wp_refresh(a);
}
int wp_open_bytes(struct wp_app *a, const char *path, const char *raw, size_t n)
{
    struct wp_doc fresh; struct wp_report rep;
    if (is_binary_office(raw, n)) return -ENOTSUP;
    if (wp_init(&fresh)) return -ENOMEM;
    int rtf = wp_is_rtf(raw, n);
    int e = rtf ? wp_rtf_read(&fresh, raw, n, &rep) : wp_text_read(&fresh, raw, n);
    if (e) { wp_free(&fresh); return e; }
    wp_free(&a->doc); a->doc = fresh;
    reset_document(a);
    snprintf(a->path, sizeof(a->path), "%s", path); a->named = 1; a->format = rtf ? WP_FMT_RTF : WP_FMT_TEXT;
    a->crlf = 1;
    for (size_t i = 0; i < n && !rtf; i++) if (raw[i] == '\n') { a->crlf = i > 0 && raw[i - 1] == '\r'; break; }
    if (a->recent) a->recent(a->path);
    relayout(a); reveal(a); invalidate_doc(a); wp_refresh(a);
    if (rtf && (rep.skipped_pictures || rep.skipped_objects))
        box(a, D_NOTICE, "WordPad", DU_DLG_ICON_WARNING, NULL, 0, 0, 0,
            "%u picture%s and %u embedded object%s cannot be shown by this version of WordPad.\nThey are left out of the document and will not be kept if you save it.",
            rep.skipped_pictures, rep.skipped_pictures == 1 ? "" : "s", rep.skipped_objects, rep.skipped_objects == 1 ? "" : "s");
    else if (rtf && rep.unknown_charset)
        box(a, D_NOTICE, "WordPad", DU_DLG_ICON_WARNING, NULL, 0, 0, 0, "This document uses a code page other than Windows-1252.\nAccented characters may look different.");
    return 0;
}
static void load_failure(struct wp_app *a, const char *path, int r)
{
    if (r == -ENOTSUP) failure(a, "Word for Windows (.doc) and Write (.wri) documents cannot be opened yet. Save the file as Rich Text Format or text first:", cn_basename(path), EINVAL);
    else if (r == -EFBIG) box(a, D_INFO, "WordPad", DU_DLG_ICON_ERROR, NULL, 0, 0, 0, "The file is larger than 16 MiB and cannot be opened:\n%s", cn_basename(path));
    else failure(a, "The file could not be opened:", cn_basename(path), r);
}
static int load_file(struct wp_app *a, const char *path)
{
    size_t cap = (size_t)WP_FILE_MAX + 1, n = 0;
    char *raw = malloc(cap);
    int r = raw ? a->fs->load(a->fs->ctx, path, raw, cap, &n) : -ENOMEM;
    if (!r && n > WP_FILE_MAX) r = -EFBIG;
    if (!r) r = wp_open_bytes(a, path, raw, n);
    free(raw);
    return r;
}
static int save_file(struct wp_app *a, const char *path, int format)
{
    char *data = NULL; size_t n = 0;
    int e = format == WP_FMT_RTF ? wp_rtf_write(&a->doc, &data, &n) : wp_text_write(&a->doc, &data, &n, a->crlf);
    if (!e) e = a->fs->save(a->fs->ctx, path, data, n);
    free(data);
    if (!e) {
        if (path != a->path) snprintf(a->path, sizeof(a->path), "%s", path);
        a->named = 1; a->modified = 0; a->format = format; a->note[0] = 0;
        if (a->recent) a->recent(a->path);
    }
    return e;
}
static int has_formatting(const struct wp_doc *d)
{
    struct wp_cattr def = wp_default_cattr();
    for (unsigned i = 0; i < d->nruns; i++) if (!wp_cattr_eq(&d->runs[i].a, &def)) return 1;
    struct wp_pattr pd = wp_default_pattr();
    for (unsigned i = 0; i < d->npara; i++) if (!wp_pattr_eq(&d->paras[i].a, &pd)) return 1;
    return 0;
}

static void file_dialog(struct wp_app *a, int save, const char *name);
static void abandon(struct wp_app *a)
{
    enum wp_after after = a->after; a->after = WP_AFTER_NONE;
    if (after == WP_AFTER_EXIT && a->declined) a->declined(a->host);
}
static void proceed(struct wp_app *a)
{
    enum wp_after after = a->after; a->after = WP_AFTER_NONE;
    if (after == WP_AFTER_NEW) new_document(a);
    else if (after == WP_AFTER_OPEN) file_dialog(a, 0, "");
    else if (after == WP_AFTER_EXIT) a->quit(a->host);
    else if (after == WP_AFTER_DROP) wp_open_path(a, a->dropped);
}
static void ask_save_changes(struct wp_app *a, enum wp_after after)
{
    char name[28]; shorten(name, sizeof(name), document_name(a), 24);
    a->after = after;
    box(a, D_CHANGES, "WordPad", DU_DLG_ICON_QUESTION, yes_no_cancel, 3, 0, 2, "Save changes to %s?", name);
}
static void guard(struct wp_app *a, enum wp_after after)
{
    if (a->modified) ask_save_changes(a, after);
    else { a->after = after; proceed(a); }
}
int wp_close_request(struct wp_app *a)
{
    if (a->modified) { ask_save_changes(a, WP_AFTER_EXIT); return 0; }
    return 1;
}
static void command_save(struct wp_app *a)
{
    if (!a->named) { file_dialog(a, 1, ""); return; }
    if (a->format == WP_FMT_TEXT && has_formatting(&a->doc)) {
        snprintf(a->pending_path, sizeof(a->pending_path), "%s", a->path);
        box(a, D_TEXTWARN, "Save", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1,
            "You are about to save the document in a Text-Only format, which will remove all formatting.\nAre you sure you want to do this?");
        return;
    }
    int r = save_file(a, a->path, a->format);
    if (r) failure(a, "The file could not be saved:", document_name(a), r);
    wp_refresh(a);
}

/* File dialog filter: a list of patterns separated by ';' (the default "*.rtf;*.txt", or what was typed). */
static int match_any(const char *filters, const char *name)
{
    char one[64];
    for (const char *p = filters; *p;) {
        const char *e = strchr(p, ';'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof(one)) { memcpy(one, p, n); one[n] = 0; if (cn_match(one, name)) return 1; }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
struct collect { struct du_file_entry *out; unsigned capacity, count; const char *pattern; };
static int collect_entry(const char *name, const struct cn_stat *st, void *arg)
{
    struct collect *c = arg;
    if (c->count >= c->capacity || strlen(name) >= sizeof(c->out->name)) return 0;
    if (!st->dir && !match_any(c->pattern, name)) return 0;
    struct du_file_entry *f = &c->out[c->count++];
    snprintf(f->name, sizeof(f->name), "%s", name); f->directory = st->dir != 0; f->size = st->size;
    return 0;
}
static int lister(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count)
{
    struct wp_app *a = ctx; struct collect c = {out, capacity, 0, a->filter};
    int r = a->fs->list(a->fs->ctx, path, collect_entry, &c);
    *count = c.count;
    return r;
}
static void folder_of(char *out, size_t size, const char *path)
{
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == out) out[1] = 0; else if (slash) *slash = 0;
}
static void file_dialog(struct wp_app *a, int save, const char *name)
{
    static char folder[CN_PATH];
    snprintf(folder, sizeof(folder), "%s", a->dir[0] ? a->dir : "/");
    if (a->named && a->path[0]) {
        folder_of(folder, sizeof(folder), a->path);
        if (save && !name[0]) name = cn_basename(a->path);
    }
    int r = du_file_dialog(a->ui, save ? D_SAVE : D_OPEN, save, save ? "Save As" : "Open", folder, name, lister, a);
    if (r) { failure(a, "The folder could not be opened:", folder, r); abandon(a); }
}
static int ends_with_text_ext(const char *name)
{
    size_t n = strlen(name);
    return n >= 5 && name[n - 4] == '.' && (name[n - 3] | 32) == 't' && (name[n - 2] | 32) == 'x' && (name[n - 1] | 32) == 't';
}
static void save_as_target(struct wp_app *a, const char *target)
{
    int format = ends_with_text_ext(cn_basename(target)) ? WP_FMT_TEXT : WP_FMT_RTF;
    int r = save_file(a, target, format);
    if (r) { failure(a, "The file could not be saved:", cn_basename(target), r); abandon(a); return; }
    static char folder[CN_PATH]; folder_of(folder, sizeof(folder), target);
    snprintf(a->dir, sizeof(a->dir), "%s", folder);
    proceed(a);
}
static void file_chosen(struct wp_app *a, int save, const char *chosen)
{
    static char target[CN_PATH], with[CN_PATH + 8], folder[CN_PATH];
    struct cn_stat st;
    if (cn_resolve(target, "/", chosen)) { failure(a, "That file name is not valid:", cn_basename(chosen), EINVAL); abandon(a); return; }
    folder_of(folder, sizeof(folder), target);
    snprintf(a->dir, sizeof(a->dir), "%s", folder);
    const char *base = cn_basename(target);
    if (cn_has_wildcard(base) || strchr(base, ';')) {   /* a pattern in the name box is a filter */
        snprintf(a->filter, sizeof(a->filter), "%s", base);
        file_dialog(a, save, "");
        return;
    }
    int r = a->fs->stat(a->fs->ctx, target, &st);
    int default_extension = !cn_has_extension(base);
    strcpy(a->filter, TEXT_FILTER);
    if (!save) {
        if (r == -ENOENT && default_extension) {
            snprintf(with, sizeof(with), "%s.rtf", target);
            if (!cn_resolve(target, "/", with)) r = a->fs->stat(a->fs->ctx, target, &st);
        }
        if (!r && st.dir) r = -EISDIR;
        if (r) { failure(a, r == -ENOENT ? "The file was not found:" : "The file could not be opened:", cn_basename(target), r); abandon(a); return; }
        r = load_file(a, target);
        if (r) { load_failure(a, target, r); abandon(a); return; }
        a->after = WP_AFTER_NONE;
        return;
    }
    if (default_extension) {
        snprintf(with, sizeof(with), "%s.rtf", target);
        if (!cn_resolve(target, "/", with)) r = a->fs->stat(a->fs->ctx, target, &st);
    }
    if (!r && st.dir) { failure(a, "That name is a folder:", cn_basename(target), EISDIR); abandon(a); return; }
    snprintf(a->pending_path, sizeof(a->pending_path), "%s", target);
    if (!r) {
        char shown[40]; shorten(shown, sizeof(shown), cn_basename(target), 36);
        box(a, D_REPLACE, "Save As", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1, "%s already exists.\n\nDo you want to replace it?", shown);
        return;
    }
    if (ends_with_text_ext(cn_basename(target)) && has_formatting(&a->doc)) {
        box(a, D_TEXTWARN, "Save", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1,
            "You are about to save the document in a Text-Only format, which will remove all formatting.\nAre you sure you want to do this?");
        return;
    }
    save_as_target(a, target);
}
int wp_open_path(struct wp_app *a, const char *path)
{
    static char target[CN_PATH]; struct cn_stat st;
    int r = cn_resolve(target, "/", path);
    if (r) { failure(a, "The file could not be opened:", path, r); return r; }
    r = a->fs->stat(a->fs->ctx, target, &st);
    if (r == -ENOENT) { failure(a, "The file was not found:", cn_basename(target), -r); return r; }
    if (!r && st.dir) r = -EISDIR;
    if (!r) r = load_file(a, target);
    if (r) load_failure(a, target, r);
    wp_refresh(a);
    return r;
}

/* ---- dialogs ----------------------------------------------------------------------------------------------- */

static int parse_inches(const char *s, int *twips)
{
    long whole = 0, frac = 0, scale = 1; int digits = 0;
    while (*s == ' ') s++;
    for (; *s >= '0' && *s <= '9'; s++) { whole = whole * 10 + (*s - '0'); digits++; if (whole > 100) return -1; }
    if (*s == '.' || *s == ',') {
        s++;
        for (; *s >= '0' && *s <= '9'; s++) { digits++; if (scale < 1000) { frac = frac * 10 + (*s - '0'); scale *= 10; } }
    }
    while (*s == ' ') s++;
    if (*s == '"') s++;
    while (*s == ' ') s++;
    if (*s || !digits) return -1;
    long t = whole * 1440 + (frac * 1440 + scale / 2) / scale;
    if (t > WP_INDENT_MAX) return -1;
    *twips = (int)t;
    return 0;
}
static void format_inches(char *out, size_t cap, int twips)
{
    int whole = twips / 1440, frac = (twips % 1440 * 100 + 720) / 1440;
    if (frac >= 100) { whole++; frac -= 100; }
    snprintf(out, cap, "%d.%02d", whole, frac);
}

static void sheet_open(struct wp_app *a, int id, const char *title, int w, int h, const char *page, int (*accept)(struct du_ui *, int, void *),
                       void (*paint)(struct du_ui *, void *))
{
    a->sheet_page = (struct du_item){.text = {page}};
    du_property_sheet(a->ui, id, title, w, h, &a->sheet_page, 1);
    struct du_dialog *d = &a->ui->dialog;
    d->accept = accept; d->accept_ctx = a; d->paint = paint; d->sheet_ctx = a;
    d->items[2].disabled = 1;   /* Apply is unused */
}
static void label_at(struct du_ui *u, int slot, const char *text, int dx, int dy)
{
    struct du_rect r = u->dialog.items[slot].box;
    du_text(&u->canvas, r.x + dx, r.y + dy, text, DU_INK);
}

/* Paragraph: three indents in inches and the alignment. */
static int paragraph_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    int v[3];
    for (unsigned i = 0; i < 3; i++)
        if (parse_inches(u->dialog.items[4 + i].text, &v[i])) { snprintf(u->dialog.text, sizeof(u->dialog.text), "Enter an indent in inches, for example 0.5."); return 1; }
    struct wp_pstyle st = {WP_P_ALIGN | WP_P_LEFT | WP_P_FIRST | WP_P_RIGHT, wp_default_pattr()};
    st.v.left = v[0]; st.v.right = v[1]; st.v.first = v[2] - v[0];
    st.v.align = u->dialog.items[7].value ? WP_LEFT : u->dialog.items[8].value ? WP_CENTER : WP_RIGHT;
    du_dialog_close(u);   /* the result event follows; apply first while the values are valid */
    apply_paras(a, &st);
    return 0;
}
static void paragraph_paint(struct du_ui *u, void *ctx)
{
    (void)ctx;
    label_at(u, 4, "Left indent (inches):", 0, -16); label_at(u, 5, "Right indent (inches):", 0, -16);
    label_at(u, 6, "First line (inches):", 0, -16); label_at(u, 7, "Alignment:", 0, -18);
}
static void paragraph_dialog(struct wp_app *a)
{
    sheet_open(a, D_PARAGRAPH, "Paragraph", 340, 290, "Indentation", paragraph_accept, paragraph_paint);
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    char b[3][24];
    format_inches(b[0], sizeof(b[0]), pa.left); format_inches(b[1], sizeof(b[1]), pa.right); format_inches(b[2], sizeof(b[2]), pa.left + pa.first);
    for (unsigned i = 0; i < 3; i++)
        du_property_control(a->ui, 4 + i, DU_EDIT, 300 + (int)i, (struct du_rect){24, 52 + (int)i * 46, 130, 22}, b[i]);
    static const char *const names[] = {"&Left", "&Center", "&Right"};
    for (unsigned i = 0; i < 3; i++) {
        struct du_widget *w = du_property_control(a->ui, 7 + i, DU_RADIO, 310 + (int)i, (struct du_rect){24 + (int)i * 80, 186, 76, 22}, names[i]);
        w->total = 77; w->value = pa.align == i;
    }
}
/* Tabs: stop positions in inches. */
static int tabs_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct wp_pstyle st = {WP_P_TABS, wp_default_pattr()};
    const char *s = u->dialog.items[4].text;
    while (*s) {
        char one[24]; size_t n = 0;
        while (*s == ' ' || *s == ';' || *s == ',') s++;
        while (*s && *s != ' ' && *s != ';' && *s != ',' && n < sizeof(one) - 1) one[n++] = *s++;
        one[n] = 0;
        if (!n) break;
        int t;
        if (parse_inches(one, &t) || !t) { snprintf(u->dialog.text, sizeof(u->dialog.text), "Tab stops are positions in inches, for example 0.5 1.5 3."); return 1; }
        if (st.v.ntabs >= WP_TABS) break;
        int at = (int)st.v.ntabs;
        while (at > 0 && st.v.tabs[at - 1] > t) { st.v.tabs[at] = st.v.tabs[at - 1]; at--; }
        if (at > 0 && st.v.tabs[at - 1] == t) continue;
        st.v.tabs[at] = t; st.v.ntabs++;
    }
    du_dialog_close(u);
    apply_paras(a, &st);
    return 0;
}
static void tabs_paint(struct du_ui *u, void *ctx)
{
    (void)ctx;
    label_at(u, 4, "Tab stop positions (inches, separated by spaces):", 0, -16);
    label_at(u, 4, "Leave empty to use the default stops every half inch.", 0, 30);
}
static void tabs_dialog(struct wp_app *a)
{
    sheet_open(a, D_TABS, "Tabs", 380, 200, "Tab stops", tabs_accept, tabs_paint);
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    char text[160] = ""; size_t k = 0;
    for (unsigned i = 0; i < pa.ntabs && k + 12 < sizeof(text); i++) {
        char one[24]; format_inches(one, sizeof(one), pa.tabs[i]);
        k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", i ? " " : "", one);
    }
    du_property_control(a->ui, 4, DU_EDIT, 320, (struct du_rect){24, 60, 320, 22}, text);
}

/* Font: family, style, size, colour and the effects, with a sample. */
static void build_font_lists(struct wp_app *a)
{
    for (unsigned i = 0; i < a->doc.nfonts; i++) a->font_items[i] = (struct du_item){.text = {a->doc.fonts[i].name}};
    static const char *const styles[] = {"Regular", "Italic", "Bold", "Bold Italic"};
    for (unsigned i = 0; i < 4; i++) a->style_items[i] = (struct du_item){.text = {styles[i]}};
    for (unsigned i = 0; i < sizeof(sizes_hp) / sizeof(*sizes_hp); i++) {
        snprintf(a->date_text[0], sizeof(a->date_text[0]), "%u", sizes_hp[i] / 2);
    }
    for (unsigned i = 0; i < a->doc.ncolors; i++) {
        if (i < sizeof(color_names) / sizeof(*color_names)) snprintf(a->color_names[i], sizeof(a->color_names[i]), "%s", color_names[i]);
        else snprintf(a->color_names[i], sizeof(a->color_names[i]), "#%06X", (unsigned)a->doc.colors[i]);
        a->color_items[i] = (struct du_item){.text = {a->color_names[i]}};
    }
}
static char size_names[16][6];
static int font_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct du_dialog *d = &u->dialog;
    unsigned fam = d->items[4].active, sty = d->items[5].active, siz = d->items[6].active, col = d->items[7].active;
    struct wp_cstyle st = {(int)fam, 0, (int)col, 0, 0};
    if (siz < sizeof(sizes_hp) / sizeof(*sizes_hp)) st.size = (int)sizes_hp[siz];
    st.set = ((sty & 2 ? WP_BOLD : 0) | (sty & 1 ? WP_ITALIC : 0) | (d->items[9].value ? WP_UNDERLINE : 0) | (d->items[8].value ? WP_STRIKE : 0));
    st.clear = (unsigned)(WP_BOLD | WP_ITALIC | WP_UNDERLINE | WP_STRIKE) & ~st.set;
    du_dialog_close(u);
    apply_chars(a, &st);
    return 0;
}
static void font_changed(struct du_ui *u, const struct du_event *e, void *ctx)
{ (void)e; (void)ctx; du_invalidate(u, u->dialog.box); }
static void font_paint(struct du_ui *u, void *ctx)
{
    struct wp_app *a = ctx; struct du_dialog *d = &u->dialog;
    label_at(u, 4, "Font:", 0, -16); label_at(u, 5, "Font style:", 0, -16); label_at(u, 6, "Size:", 0, -16); label_at(u, 7, "Color:", 0, -16);
    struct du_rect r = d->items[8].box;
    struct du_rect sample = {d->box.x + 24, r.y + 30, d->box.w - 48, 52};
    du_well(&u->canvas, sample, 0xffffffu);
    unsigned fam = d->items[4].active, sty = d->items[5].active, siz = d->items[6].active, col = d->items[7].active;
    if (fam >= a->doc.nfonts || siz >= sizeof(sizes_hp) / sizeof(*sizes_hp) || col >= a->doc.ncolors) return;
    unsigned flags = (sty & 2 ? WP_BOLD : 0) | (sty & 1 ? WP_ITALIC : 0);
    unsigned hp = sizes_hp[siz] > 48 ? 48 : sizes_hp[siz];
    struct du_canvas cv = u->canvas; cv.clip = du_intersection(u->canvas.clip, (struct du_rect){sample.x + 2, sample.y + 2, sample.w - 4, sample.h - 4});
    int asc, desc; tops(a)->vmetrics(tops(a)->ctx, a->doc.fonts[fam].family, flags, hp, &asc, &desc);
    int base = sample.y + sample.h / 2 + (asc - desc) / 2, x = sample.x + 12;
    uint32_t rgb = a->doc.colors[col];
    const char *text = "AaBbYyZz";
    a->draw->draw(a->draw->text.ctx, &cv, a->doc.fonts[fam].family, flags, wp_size_px64(hp), x, base, text, strlen(text), rgb);
    int w = tops(a)->width(tops(a)->ctx, a->doc.fonts[fam].family, flags, hp, text, strlen(text)) >> 6;
    if (d->items[9].value) du_fill(&cv, (struct du_rect){x, base + 1, w, 1}, rgb);
    if (d->items[8].value) du_fill(&cv, (struct du_rect){x, base - asc / 3, w, 1}, rgb);
}
static void font_dialog(struct wp_app *a)
{
    build_font_lists(a);
    for (unsigned i = 0; i < sizeof(sizes_hp) / sizeof(*sizes_hp); i++) {
        snprintf(size_names[i], sizeof(size_names[i]), "%u", sizes_hp[i] / 2);
        a->size_items[i] = (struct du_item){.text = {size_names[i]}};
    }
    sheet_open(a, D_FONT, "Font", 540, 360, "Font", font_accept, font_paint);
    struct du_dialog *d = &a->ui->dialog; d->changed = font_changed;
    static const char *const titles[] = {"Font", "Font style", "Size", "Color"};
    static const int xs[] = {24, 200, 316, 382}, widths[] = {170, 110, 62, 118};
    const struct du_item *items[] = {a->font_items, a->style_items, a->size_items, a->color_items};
    unsigned counts[] = {a->doc.nfonts, 4, sizeof(sizes_hp) / sizeof(*sizes_hp), a->doc.ncolors};
    struct wp_cmix m = a->cmix;
    unsigned actives[4];
    actives[0] = m.font >= 0 ? (unsigned)m.font : 0;
    actives[1] = (m.bold == 1 ? 2u : 0u) | (m.italic == 1 ? 1u : 0u);
    actives[2] = 2;
    for (unsigned i = 0; i < sizeof(sizes_hp) / sizeof(*sizes_hp); i++) if ((int)sizes_hp[i] == m.size) actives[2] = i;
    actives[3] = m.color >= 0 ? (unsigned)m.color : 0;
    for (unsigned i = 0; i < 4; i++) {
        struct du_widget *w = du_property_control(a->ui, 4 + i, DU_LISTVIEW, 330 + (int)i, (struct du_rect){xs[i], 52, widths[i], 140}, "");
        w->items = items[i]; w->count = counts[i]; w->view = DU_DETAILS; w->column_count = 1;
        w->columns[0] = (struct du_column){titles[i], widths[i] - 18, 0};
        w->active = actives[i]; w->inactive_selection = 1; w->selection_active = 1;
        du_list_ensure(w, w->active);
    }
    struct du_widget *s = du_property_control(a->ui, 8, DU_CHECKBOX, 340, (struct du_rect){24, 204, 100, 22}, "Stri&keout");
    s->value = m.strike == 1;
    struct du_widget *ul = du_property_control(a->ui, 9, DU_CHECKBOX, 341, (struct du_rect){140, 204, 100, 22}, "&Underline");
    ul->value = m.underline == 1;
}

/* Page Setup: paper, orientation and margins in inches. */
static int page_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct du_dialog *d = &u->dialog;
    int m[4];
    for (unsigned i = 0; i < 4; i++)
        if (parse_inches(d->items[6 + i].text, &m[i])) { snprintf(d->text, sizeof(d->text), "Enter the margins in inches, for example 0.75."); return 1; }
    unsigned paper = d->items[4].active;
    if (paper >= wp_paper_count) paper = 0;
    struct wp_page_setup s = {wp_papers[paper].w, wp_papers[paper].h, m[0], m[1], m[2], m[3], d->items[5].value != 0};
    int pw = s.landscape ? s.paper_h : s.paper_w, ph = s.landscape ? s.paper_w : s.paper_h;
    if (s.left + s.right >= pw - 720 || s.top + s.bottom >= ph - 720) { snprintf(d->text, sizeof(d->text), "The margins leave no room for text on the page."); return 1; }
    a->page = s;
    du_dialog_close(u);
    a->preview_valid = 0;
    return 0;
}
static void page_paint(struct du_ui *u, void *ctx)
{
    (void)ctx;
    label_at(u, 4, "Paper:", 0, -16);
    static const char *const names[] = {"Left:", "Right:", "Top:", "Bottom:"};
    for (unsigned i = 0; i < 4; i++) label_at(u, 6 + (int)i, names[i], -52, 4);
    label_at(u, 6, "Margins (inches)", -52, -20);
}
static void page_dialog(struct wp_app *a)
{
    sheet_open(a, D_PAGESETUP, "Page Setup", 400, 320, "Page", page_accept, page_paint);
    for (unsigned i = 0; i < wp_paper_count; i++) { snprintf(a->color_names[i], sizeof(a->color_names[i]), "%s", wp_papers[i].name); a->color_items[i] = (struct du_item){.text = {a->color_names[i]}}; }
    struct du_widget *l = du_property_control(a->ui, 4, DU_LISTVIEW, 350, (struct du_rect){24, 56, 130, 100}, "");
    l->items = a->color_items; l->count = wp_paper_count; l->view = DU_DETAILS; l->column_count = 1; l->columns[0] = (struct du_column){"Paper", 112, 0};
    l->inactive_selection = 1; l->selection_active = 1; l->active = 0;
    for (unsigned i = 0; i < wp_paper_count; i++) if (wp_papers[i].w == a->page.paper_w && wp_papers[i].h == a->page.paper_h) l->active = i;
    struct du_widget *o = du_property_control(a->ui, 5, DU_CHECKBOX, 351, (struct du_rect){24, 168, 130, 22}, "&Landscape");
    o->value = a->page.landscape;
    int v[4] = {a->page.left, a->page.right, a->page.top, a->page.bottom};
    for (unsigned i = 0; i < 4; i++) {
        char b[24]; format_inches(b, sizeof(b), v[i]);
        du_property_control(a->ui, 6 + i, DU_EDIT, 352 + (int)i, (struct du_rect){260, 56 + (int)i * 30, 90, 22}, b);
    }
}

/* Date and time: one list of formats. */
static void date_formats(struct wp_app *a)
{
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    time_t t = a->now(a->host);
    struct tm tm; if (!desk_localtime(&t, &tm)) memset(&tm, 0, sizeof(tm));
    a->tm = tm;
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(a->date_text[0], sizeof(a->date_text[0]), "%d/%d/%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
    snprintf(a->date_text[1], sizeof(a->date_text[1]), "%s, %s %d, %d", days[tm.tm_wday % 7], months[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
    snprintf(a->date_text[2], sizeof(a->date_text[2]), "%d %s %d", tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900);
    snprintf(a->date_text[3], sizeof(a->date_text[3]), "%s %d, %d", months[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
    snprintf(a->date_text[4], sizeof(a->date_text[4]), "%d:%02d %s", h12, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM");
    snprintf(a->date_text[5], sizeof(a->date_text[5]), "%d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(a->date_text[6], sizeof(a->date_text[6]), "%d:%02d %s %d/%d/%d", h12, tm.tm_min, tm.tm_hour >= 12 ? "PM" : "AM", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
    for (unsigned i = 0; i < 7; i++) a->date_items[i] = (struct du_item){.text = {a->date_text[i]}};
}
static int date_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    unsigned at = u->dialog.items[4].active;
    if (at >= 7) return 0;
    char text[48]; snprintf(text, sizeof(text), "%s", a->date_text[at]);
    du_dialog_close(u);
    replace_selection(a, text, strlen(text), WP_EDIT_OTHER);
    return 0;
}
static void date_paint(struct du_ui *u, void *ctx) { (void)ctx; label_at(u, 4, "Available formats:", 0, -16); }
static void date_dialog(struct wp_app *a)
{
    date_formats(a);
    sheet_open(a, D_DATETIME, "Date and Time", 340, 260, "Date and Time", date_accept, date_paint);
    struct du_widget *l = du_property_control(a->ui, 4, DU_LISTVIEW, 360, (struct du_rect){24, 56, 270, 140}, "");
    l->items = a->date_items; l->count = 7; l->view = DU_DETAILS; l->column_count = 1; l->columns[0] = (struct du_column){"Format", 252, 0};
    l->inactive_selection = 1; l->selection_active = 1; l->active = 0;
}

/* ---- popups ------------------------------------------------------------------------------------------------------- */

static void popup_fonts(struct wp_app *a)
{
    struct du_widget *b = W(a, W_FONTBTN);
    for (unsigned i = 0; i < a->doc.nfonts; i++)
        a->list_popup[i] = (struct du_menu_item){a->doc.fonts[i].name, P_FONT + (int)i, 0, 0, a->cmix.font == (int)i ? DU_ITEM_CHECKED : 0};
    du_popup(a->ui, a->list_popup, a->doc.nfonts, b->box.x, b->box.y + b->box.h, W_FONTBTN);
}
static void popup_sizes(struct wp_app *a)
{
    struct du_widget *b = W(a, W_SIZEBTN);
    for (unsigned i = 0; i < sizeof(sizes_hp) / sizeof(*sizes_hp); i++) {
        snprintf(size_names[i], sizeof(size_names[i]), "%u", sizes_hp[i] / 2);
        a->list_popup[i] = (struct du_menu_item){size_names[i], P_SIZE + (int)i, 0, 0, a->cmix.size == (int)sizes_hp[i] ? DU_ITEM_CHECKED : 0};
    }
    du_popup(a->ui, a->list_popup, sizeof(sizes_hp) / sizeof(*sizes_hp), b->box.x, b->box.y + b->box.h, W_SIZEBTN);
}
static void popup_colors(struct wp_app *a)
{
    struct du_widget *b = W(a, W_COLORBTN);
    for (unsigned i = 0; i < a->doc.ncolors; i++) {
        if (i < sizeof(color_names) / sizeof(*color_names)) snprintf(a->color_names[i], sizeof(a->color_names[i]), "%s", color_names[i]);
        else snprintf(a->color_names[i], sizeof(a->color_names[i]), "#%06X", (unsigned)a->doc.colors[i]);
        a->list_popup[i] = (struct du_menu_item){a->color_names[i], P_COLOR + (int)i, 0, 0, a->cmix.color == (int)i ? DU_ITEM_CHECKED : 0};
    }
    du_popup(a->ui, a->list_popup, a->doc.ncolors, b->box.x, b->box.y + b->box.h, W_COLORBTN);
}

/* ---- commands ----------------------------------------------------------------------------------------------------- */

static void help_box(struct wp_app *a)
{
    box(a, D_INFO, "WordPad Help", DU_DLG_ICON_INFO, NULL, 0, 0, 0,
        "File: Ctrl+N New, Ctrl+O Open, Ctrl+S Save, Print Preview, Page Setup\n"
        "Edit: Ctrl+Z Undo, Ctrl+Y Redo, Ctrl+X/C/V, Ctrl+A, Ctrl+F Find, F3 Next, Ctrl+H Replace\n"
        "Format: Ctrl+B Bold, Ctrl+I Italic, Ctrl+U Underline,\n"
        "Ctrl+L Left, Ctrl+E Center, Ctrl+R Right, Bullet Style, Paragraph, Tabs\n"
        "---\nFiles: Rich Text Format (.rtf) and text (.txt) up to 16 MiB.\n"
        "Word (.doc) and Write (.wri) files and printing are not available yet.");
}
static void command(struct wp_app *a, int id)
{
    if (a->preview) {
        switch (id) {
        case M_PV_NEXT: preview_step(a, 1); return;
        case M_PV_PREV: preview_step(a, -1); return;
        case M_PV_ZOOMIN: preview_zoom_by(a, 1); return;
        case M_PV_ZOOMOUT: preview_zoom_by(a, -1); return;
        case M_PV_CLOSE: case M_PREVIEW: preview_close(a); return;
        case M_EXIT: guard(a, WP_AFTER_EXIT); return;
        default: return;
        }
    }
    if (id >= P_COLOR) { struct wp_cstyle st = {-1, 0, id - P_COLOR, 0, 0}; apply_chars(a, &st); return; }
    if (id >= P_SIZE) { unsigned i = (unsigned)(id - P_SIZE); if (i < sizeof(sizes_hp) / sizeof(*sizes_hp)) { struct wp_cstyle st = {-1, (int)sizes_hp[i], -1, 0, 0}; apply_chars(a, &st); } return; }
    if (id >= P_FONT) { struct wp_cstyle st = {id - P_FONT, 0, -1, 0, 0}; apply_chars(a, &st); return; }
    switch (id) {
    case M_NEW: guard(a, WP_AFTER_NEW); break;
    case M_OPEN: guard(a, WP_AFTER_OPEN); break;
    case M_SAVE: command_save(a); break;
    case M_SAVEAS: file_dialog(a, 1, ""); break;
    case M_PREVIEW: preview_open(a); break;
    case M_PAGESETUP: page_dialog(a); break;
    case M_EXIT: guard(a, WP_AFTER_EXIT); break;
    case M_UNDO: if (a->undo_valid && !a->undo_is_redo) undo_swap(a); break;
    case M_REDO: if (a->undo_valid && a->undo_is_redo) undo_swap(a); break;
    case M_CUT: if (has_selection(a) && !copy_selection(a)) { size_t lo, hi; sel_range(a, &lo, &hi); delete_range(a, lo, hi, WP_EDIT_OTHER); } break;
    case M_COPY: if (has_selection(a)) copy_selection(a); wp_refresh(a); break;
    case M_PASTE: paste(a); break;
    case M_CLEAR: { size_t lo, hi; sel_range(a, &lo, &hi); delete_range(a, lo, hi, WP_EDIT_OTHER); break; }
    case M_SELALL: a->anchor = 0; a->cursor = a->doc.len; a->pending_valid = 0; moved(a); break;
    case M_FIND: show_find(a, 1); break;
    case M_FINDNEXT: find_next(a); break;
    case M_REPLACE: show_find(a, 2); break;
    case M_TOOLBAR: a->show_toolbar = !a->show_toolbar; wp_layout_ui(a); wp_refresh(a); break;
    case M_FORMATBAR: a->show_format = !a->show_format; wp_layout_ui(a); wp_refresh(a); break;
    case M_RULER: a->show_ruler = !a->show_ruler; wp_layout_ui(a); wp_refresh(a); break;
    case M_STATUS: a->show_status = !a->show_status; wp_layout_ui(a); wp_refresh(a); break;
    case M_DATETIME: date_dialog(a); break;
    case M_FONT: font_dialog(a); break;
    case M_BULLET: toggle_bullet(a); break;
    case M_PARAGRAPH: paragraph_dialog(a); break;
    case M_TABS: tabs_dialog(a); break;
    case M_BOLD: toggle_flag(a, WP_BOLD, a->cmix.bold); break;
    case M_ITALIC: toggle_flag(a, WP_ITALIC, a->cmix.italic); break;
    case M_UNDERLINE: toggle_flag(a, WP_UNDERLINE, a->cmix.underline); break;
    case M_STRIKE: toggle_flag(a, WP_STRIKE, a->cmix.strike); break;
    case M_LEFT: set_align(a, WP_LEFT); break;
    case M_CENTER: set_align(a, WP_CENTER); break;
    case M_RIGHT: set_align(a, WP_RIGHT); break;
    case M_FONTMENU: case W_FONTBTN: popup_fonts(a); break;
    case M_SIZEMENU: case W_SIZEBTN: popup_sizes(a); break;
    case M_COLORMENU: case W_COLORBTN: popup_colors(a); break;
    case M_HELP: help_box(a); break;
    case M_ABOUT:
        box(a, D_INFO, "About WordPad", DU_DLG_ICON_INFO, NULL, 0, 0, 0,
            "CiukiOS WordPad\nClassic computing, one woof at a time.\n---\nDedicated to Ciuki.\nFonts: Liberation (SIL OFL 1.1).\n"
            "Tango 0.8.90 icons: Public Domain.\nlibdeskui and WordPad: MIT licence.");
        break;
    case W_FNEXT: find_next(a); break;
    case W_FDIR: a->find_up = !a->find_up; du_set_text(W(a, W_FDIR), a->find_up ? "&Up" : "&Down"); break;
    case W_FCASE: a->match_case = !a->match_case; du_set_text(W(a, W_FCASE), a->match_case ? "[x] Match &case" : "[ ] Match &case"); break;
    case W_FCLOSE: hide_find(a); break;
    case W_RONE: replace_one(a); break;
    case W_RALL: replace_all(a); break;
    default: break;
    }
}

static void dialog_result(struct wp_app *a, int id, int index, const char *text)
{
    switch (id) {
    case D_CHANGES:
        if (index == 0) {
            if (!a->named) { file_dialog(a, 1, ""); return; }
            if (a->format == WP_FMT_TEXT && has_formatting(&a->doc)) {
                snprintf(a->pending_path, sizeof(a->pending_path), "%s", a->path);
                box(a, D_TEXTWARN, "Save", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1,
                    "You are about to save the document in a Text-Only format, which will remove all formatting.\nAre you sure you want to do this?");
                return;
            }
            int r = save_file(a, a->path, a->format);
            if (r) { abandon(a); failure(a, "The file could not be saved:", document_name(a), r); }
            else proceed(a);
        } else if (index == 1) proceed(a);
        else abandon(a);
        break;
    case D_OPEN: case D_SAVE:
        if (index == 0) file_chosen(a, id == D_SAVE, text);
        else { strcpy(a->filter, TEXT_FILTER); abandon(a); }
        break;
    case D_REPLACE:
        if (index == 0) {
            if (ends_with_text_ext(cn_basename(a->pending_path)) && has_formatting(&a->doc))
                box(a, D_TEXTWARN, "Save", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1,
                    "You are about to save the document in a Text-Only format, which will remove all formatting.\nAre you sure you want to do this?");
            else save_as_target(a, a->pending_path);
        } else file_dialog(a, 1, cn_basename(a->pending_path));
        break;
    case D_TEXTWARN:
        if (index == 0) {
            if (a->pending_path[0] && strcmp(a->pending_path, a->path)) save_as_target(a, a->pending_path);
            else {
                int r = save_file(a, a->path, WP_FMT_TEXT);
                if (r) { abandon(a); failure(a, "The file could not be saved:", document_name(a), r); }
                else proceed(a);
            }
        } else abandon(a);
        break;
    default: break;
    }
}

/* ---- keyboard and pointer ------------------------------------------------------------------------------------------- */

static void handle_key(struct wp_app *a, const struct du_event *e)
{
    int shift = (e->modifiers & DU_SHIFT) != 0, ctrl = (e->modifiers & DU_CTRL) != 0;
    if (a->preview) {
        if (e->key == DU_KEY_ESC) preview_close(a);
        else if (e->key == DU_KEY_PGDN || e->key == DU_KEY_DOWN || e->key == DU_KEY_RIGHT) preview_step(a, 1);
        else if (e->key == DU_KEY_PGUP || e->key == DU_KEY_UP || e->key == DU_KEY_LEFT) preview_step(a, -1);
        return;
    }
    if (e->modifiers & DU_ALT) return;
    if (ctrl && !shift) {   /* WordPad's format shortcuts have no menu entry */
        switch (e->key) {
        case K_B: command(a, M_BOLD); return;
        case K_I: command(a, M_ITALIC); return;
        case K_U: command(a, M_UNDERLINE); return;
        case K_L: command(a, M_LEFT); return;
        case K_E: command(a, M_CENTER); return;
        case K_R: command(a, M_RIGHT); return;
        default: break;
        }
    }
    switch (e->key) {
    case DU_KEY_LEFT:
        if (!shift && has_selection(a)) { size_t lo, hi; sel_range(a, &lo, &hi); set_cursor(a, lo, 0, 0); }
        else set_cursor(a, ctrl ? word_left(a, a->cursor) : wp_prev(&a->doc, a->cursor), shift, 0);
        return;
    case DU_KEY_RIGHT:
        if (!shift && has_selection(a)) { size_t lo, hi; sel_range(a, &lo, &hi); set_cursor(a, hi, 0, 0); }
        else set_cursor(a, ctrl ? word_right(a, a->cursor) : wp_next(&a->doc, a->cursor), shift, 0);
        return;
    case DU_KEY_UP: vertical(a, -1, 0, shift); return;
    case DU_KEY_DOWN: vertical(a, 1, 0, shift); return;
    case DU_KEY_PGUP: vertical(a, -1, 1, shift); return;
    case DU_KEY_PGDN: vertical(a, 1, 1, shift); return;
    case DU_KEY_HOME:
        if (ctrl) set_cursor(a, 0, shift, 0);
        else if (a->lay.n) set_cursor(a, a->lay.lines[wp_layout_line_of(&a->lay, a->cursor)].start, shift, 0);
        return;
    case DU_KEY_END:
        if (ctrl) set_cursor(a, a->doc.len, shift, 0);
        else if (a->lay.n) set_cursor(a, line_end_pos(a, wp_layout_line_of(&a->lay, a->cursor)), shift, 0);
        return;
    case DU_KEY_ESC: if (a->find_mode) hide_find(a); return;
    case DU_KEY_BACKSPACE: case DU_KEY_DELETE: {
        size_t lo, hi; sel_range(a, &lo, &hi);
        if (lo != hi) { delete_range(a, lo, hi, WP_EDIT_OTHER); return; }
        if (e->key == DU_KEY_BACKSPACE) {
            size_t ps; wp_para_at(&a->doc, a->cursor, &ps, NULL);
            if (a->cursor == ps && wp_pattr_at(&a->doc, a->cursor).bullet) { toggle_bullet(a); return; }
            if (a->cursor) delete_range(a, ctrl ? word_left(a, a->cursor) : wp_prev(&a->doc, a->cursor), a->cursor, WP_EDIT_BACK);
        } else if (a->cursor < a->doc.len) delete_range(a, a->cursor, ctrl ? word_right(a, a->cursor) : wp_next(&a->doc, a->cursor), WP_EDIT_FORWARD);
        return;
    }
    case DU_KEY_ENTER: {
        size_t ps, pe; wp_para_at(&a->doc, a->cursor, &ps, &pe);
        if (!has_selection(a) && ps == pe && wp_pattr_at(&a->doc, a->cursor).bullet) { toggle_bullet(a); return; }   /* Enter on an empty bullet ends the list */
        replace_selection(a, "\n", 1, WP_EDIT_OTHER);
        return;
    }
    case DU_KEY_TAB: if (!ctrl && !shift) replace_selection(a, "\t", 1, WP_EDIT_TYPE); return;
    default: break;
    }
    if (e->ch >= 32 && e->ch != 127 && !ctrl) insert_utf8(a, e->ch);
}

static size_t doc_hit(struct wp_app *a, int x, int y)
{ return wp_layout_hit(&a->lay, &a->doc, tops(a), x - a->doc_box.x - PAD, y - a->doc_box.y + a->top); }
static void word_bounds(const struct wp_app *a, size_t at, size_t *lo, size_t *hi)
{
    size_t s = at, e = at;
    if (at < a->doc.len && is_space(a->doc.text[at])) {
        while (s > 0 && a->doc.text[s - 1] == ' ') s--;
        while (e < a->doc.len && a->doc.text[e] == ' ') e++;
        if (a->doc.text[at] != ' ') { s = at; e = at + 1; }
    } else {
        while (s > 0 && !is_space(a->doc.text[s - 1])) s--;
        while (e < a->doc.len && !is_space(a->doc.text[e])) e++;
        while (e < a->doc.len && a->doc.text[e] == ' ') e++;   /* a word takes its trailing spaces, as WordPad */
    }
    *lo = s; *hi = e;
}
static void handle_pointer(struct wp_app *a, const struct du_event *e)
{
    if (a->preview) return;
    if (e->index == DU_POINTER_WHEEL) {
        struct du_widget *sb = W(a, W_SCROLL);
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        a->top -= e->x * 3 * 13;
        if (a->top < 0) a->top = 0;
        if (a->top > max) a->top = max;
        sb->value = a->top; invalidate_doc(a); return;
    }
    if (e->index == DU_POINTER_DOWN && e->key == 1) {
        size_t pos = doc_hit(a, e->x, e->y);
        int extend = (e->modifiers & DU_SHIFT) != 0;
        uint64_t now = a->ui->now;
        if (now - a->click_ns < DOUBLE_NS && abs(e->x - a->click_x) < 5 && abs(e->y - a->click_y) < 5 && a->clicks < 3) a->clicks++; else a->clicks = 1;
        a->click_ns = now; a->click_x = e->x; a->click_y = e->y;
        a->dragging = 1;
        if (a->clicks == 2) { word_bounds(a, pos, &a->drag_lo, &a->drag_hi); a->anchor = a->drag_lo; a->cursor = a->drag_hi; }
        else if (a->clicks == 3) { wp_para_at(&a->doc, pos, &a->drag_lo, &a->drag_hi); if (a->drag_hi < a->doc.len) a->drag_hi++; a->anchor = a->drag_lo; a->cursor = a->drag_hi; }
        else { a->drag_lo = a->drag_hi = pos; if (!extend) a->anchor = pos; a->cursor = pos; }
        a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER; a->want_valid = 0;
        moved(a);
    } else if (e->index == DU_POINTER_DOWN && e->key == 2) {
        size_t pos = doc_hit(a, e->x, e->y), lo, hi; sel_range(a, &lo, &hi);
        if (pos < lo || pos > hi) set_cursor(a, pos, 0, 0);
    } else if (e->index == DU_POINTER_UP && e->key == 2) {   /* the menu opens on release: the canvas has let go of the pointer by then */
        wp_refresh(a);
        static const struct { const char *label; int id; } rows[] = {{"Cu&t", M_CUT}, {"&Copy", M_COPY}, {"&Paste", M_PASTE}, {"Select &All", M_SELALL},
                                                                     {"&Font...", M_FONT}, {"&Bullet Style", M_BULLET}, {"&Paragraph...", M_PARAGRAPH}};
        int sel = has_selection(a);
        for (unsigned i = 0; i < 7; i++) {
            unsigned disabled = (rows[i].id == M_CUT || rows[i].id == M_COPY) && !sel ? DU_ITEM_DISABLED : rows[i].id == M_PASTE && !du_clipboard_get()[0] ? DU_ITEM_DISABLED : 0;
            a->list_popup[i] = (struct du_menu_item){rows[i].label, rows[i].id, 0, 0, disabled | (rows[i].id == M_BULLET && a->pmix.bullet == 1 ? DU_ITEM_CHECKED : 0)};
        }
        du_popup(a->ui, a->list_popup, 7, e->x, e->y, W_DOC);
    } else if ((e->index == DU_POINTER_MOVE || e->index == DU_POINTER_UP) && a->dragging && e->key == 1) {
        if (e->y < a->doc_box.y) a->top -= 12; else if (e->y > a->doc_box.y + a->doc_box.h) a->top += 12;
        struct du_widget *sb = W(a, W_SCROLL);
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        if (a->top < 0) a->top = 0;
        if (a->top > max) a->top = max;
        sb->value = a->top;
        size_t pos = doc_hit(a, e->x, e->y);
        if (a->clicks >= 2) {
            size_t lo, hi;
            if (a->clicks == 2) word_bounds(a, pos, &lo, &hi); else { wp_para_at(&a->doc, pos, &lo, &hi); if (hi < a->doc.len) hi++; }
            if (lo < a->drag_lo) { a->anchor = a->drag_hi; a->cursor = lo; } else { a->anchor = a->drag_lo; a->cursor = hi; }
        } else a->cursor = pos;
        invalidate_doc(a); wp_refresh(a);
    }
    if (e->index == DU_POINTER_UP || e->index == DU_POINTER_CANCEL) a->dragging = 0;
}

void wp_event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct wp_app *a = ctx; int id = e->id;
    switch (e->type) {
    case DU_EV_CHANGE:
        if (id == W_SCROLL) { a->top = W(a, W_SCROLL)->value; invalidate_doc(a); }
        return;
    case DU_EV_POINTER: if (id == W_DOC) handle_pointer(a, e); return;
    case DU_EV_KEY:
        if (e->key == DU_KEY_ESC && a->find_mode && id >= W_FLABEL && id <= W_RALL) { hide_find(a); return; }
        if (id == W_DOC || id == 0 || a->preview) handle_key(a, e);
        return;
    case DU_EV_TICK:
        if (e->id == 1) { a->caret_on = !a->caret_on; if (!has_selection(a) && !a->preview) {
            int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
            du_invalidate(u, du_intersection(a->doc_box, (struct du_rect){a->doc_box.x + PAD + x - 1, a->doc_box.y - a->top + y, 3, h})); } }
        return;
    case DU_EV_COMMIT:
        if (id == W_FIND) find_next(a);
        else if (id == W_REPL) replace_one(a);
        return;
    case DU_EV_CANCEL: if (id == W_FIND || id == W_REPL) hide_find(a); return;
    case DU_EV_DIALOG: dialog_result(a, id, e->index, e->text); break;
    case DU_EV_DROPFILES:
        if (u->dialog.kind || du_drop_file(0, a->dropped, sizeof(a->dropped)) <= 0 || !a->dropped[0]) return;
        guard(a, WP_AFTER_DROP);
        break;
    case DU_EV_ACTIVATE:
        command(a, id);
        /* A toolbar, format bar or popup command hands the keyboard back to the document, as WordPad. */
        if (!u->dialog.kind && !(id >= W_FLABEL && id <= W_RALL) && id != M_FIND && id != M_REPLACE && !a->preview) du_focus(u, W_DOC);
        break;
    default: return;
    }
    wp_refresh(a);
}

/* ---- lifecycle ------------------------------------------------------------------------------------------------------------ */

static void add_tool(struct wp_app *a, int id, const char *object, unsigned size, int dropdown)
{
    struct du_widget *b = du_add(a->ui, DU_TOOLBUTTON, id, (struct du_rect){0}, "");
    if (!b) return;
    b->icon = icon(object, size); b->icon_size = (int)size; b->dropdown = dropdown;
    if (!b->icon) b->icon_size = 0;
}
int wp_attach(struct wp_app *a, struct du_ui *u)
{
    a->ui = u; u->event = wp_event; u->user = a; u->layout = resize_layout; u->paint = paint_hook;
    if (wp_init(&a->doc) || wp_init(&a->undo) || wp_init(&a->clip)) return -ENOMEM;
    wp_layout_init(&a->lay); wp_layout_init(&a->plo);
    a->crlf = 1; a->show_toolbar = a->show_format = a->show_ruler = a->show_status = 1; strcpy(a->dir, "/"); strcpy(a->filter, TEXT_FILTER);
    a->page = wp_default_page_setup(a->a4); a->dirty_para = NO_PARA; a->caret_on = 1;
    memcpy(a->file_items, file_template, sizeof(file_template)); memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->view_items, view_template, sizeof(view_template)); memcpy(a->insert_items, insert_template, sizeof(insert_template));
    memcpy(a->format_items, format_template, sizeof(format_template)); memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, sizeof(file_template) / sizeof(*file_template)};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, sizeof(edit_template) / sizeof(*edit_template)};
    a->menus[2] = (struct du_menu){"&View", a->view_items, sizeof(view_template) / sizeof(*view_template)};
    a->menus[3] = (struct du_menu){"&Insert", a->insert_items, sizeof(insert_template) / sizeof(*insert_template)};
    a->menus[4] = (struct du_menu){"F&ormat", a->format_items, 4};   /* the separator row stays unused */
    a->menus[5] = (struct du_menu){"&Help", a->help_items, sizeof(help_template) / sizeof(*help_template)};
    u->menus = a->menus; u->menu_count = 6;
    struct du_rect zero = {0, 0, 0, 0};
    du_add(u, DU_TOOLBAR, W_TOOLBAR, zero, "");
    for (unsigned i = 0; i < sizeof(toolbar_buttons) / sizeof(*toolbar_buttons); i++) {
        if (toolbar_buttons[i].icon) add_tool(a, toolbar_buttons[i].id, toolbar_buttons[i].icon, 22, 0);
        else du_add(u, DU_SEPARATOR, toolbar_buttons[i].id, zero, "");
    }
    du_add(u, DU_TOOLBAR, W_FORMATBAR, zero, "");
    struct du_widget *f = du_add(u, DU_BUTTON, W_FONTBTN, zero, ""); if (f) f->dropdown = 0;
    du_add(u, DU_BUTTON, W_SIZEBTN, zero, "");
    add_tool(a, W_COLORBTN, "preferences-desktop-theme", 22, 0);
    du_add(u, DU_SEPARATOR, W_SEP1 + 10, zero, ""); du_add(u, DU_SEPARATOR, W_SEP1 + 11, zero, "");
    add_tool(a, M_BOLD, "action:bold", 22, 0); add_tool(a, M_ITALIC, "action:italic", 22, 0); add_tool(a, M_UNDERLINE, "action:underline", 22, 0);
    add_tool(a, M_LEFT, "action:align-left", 22, 0); add_tool(a, M_CENTER, "action:align-center", 22, 0); add_tool(a, M_RIGHT, "action:align-right", 22, 0);
    add_tool(a, M_BULLET, "format-indent-more", 22, 0);
    du_add(u, DU_TOOLBAR, W_PVBAR, zero, "");
    static const struct { int id; const char *label; } pv[] = {{M_PV_PREV, "&Previous"}, {M_PV_NEXT, "&Next Page"}, {M_PV_ZOOMOUT, "Zoom &Out"}, {M_PV_ZOOMIN, "Zoom &In"}, {M_PV_CLOSE, "&Close"}};
    for (unsigned i = 0; i < 5; i++) du_add(u, DU_BUTTON, pv[i].id, zero, pv[i].label);
    struct du_widget *doc = du_add(u, DU_CANVAS, W_DOC, zero, "");
    if (!doc) return -ENOMEM;
    doc->accepts_tab = 1; doc->wheel_events = 1; doc->pointer_cursor = DESK_CURSOR_TEXT;
    struct du_widget *sb = du_add(u, DU_SCROLLBAR, W_SCROLL, zero, ""); if (sb) sb->vertical = 1;
    du_add(u, DU_STATUS, W_STATUS, zero, "");
    du_add(u, DU_LABEL, W_FLABEL, zero, "Find &what:"); du_add(u, DU_EDIT, W_FIND, zero, "");
    du_add(u, DU_BUTTON, W_FNEXT, zero, "Find &Next"); du_add(u, DU_BUTTON, W_FDIR, zero, "&Down");
    du_add(u, DU_BUTTON, W_FCASE, zero, "[ ] Match &case"); du_add(u, DU_BUTTON, W_FCLOSE, zero, "X");
    du_add(u, DU_LABEL, W_RLABEL, zero, "Replace &with:"); du_add(u, DU_EDIT, W_REPL, zero, "");
    du_add(u, DU_BUTTON, W_RONE, zero, "&Replace"); du_add(u, DU_BUTTON, W_RALL, zero, "Replace &All");
    du_timer_start(u, 1, 530000000ull, 1);
    W(a, M_PRINT)->disabled = 1;   /* printing arrives with the printing subsystem (G31) */
    wp_layout_ui(a);
    du_focus(u, W_DOC);
    relayout(a);
    wp_refresh(a);
    return 0;
}
void wp_detach(struct wp_app *a)
{
    wp_free(&a->doc); wp_free(&a->undo); wp_free(&a->clip); free(a->clip_plain); a->clip_plain = NULL;
    wp_layout_free(&a->lay); wp_layout_free(&a->plo); wp_pages_free(&a->pages);
}
