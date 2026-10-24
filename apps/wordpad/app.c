/* SPDX-License-Identifier: MIT
 * WordPad application logic. Layout of the file: helpers, editing core,
 * view and painting, find, files, dialogs, commands, events, lifecycle. */
#include "app.h"
#include "../libdeskui/clipboard.h"
#include "../libdeskui/clipstore.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../desktop/tz_env.h"

enum { K_N = 0x31, K_O = 0x18, K_S = 0x1f, K_P = 0x19, K_Z = 0x2c, K_Y = 0x15, K_X = 0x2d, K_C = 0x2e, K_V = 0x2f,
       K_A = 0x1e, K_F = 0x21, K_H = 0x23, K_B = 0x30, K_I = 0x17, K_U = 0x16, K_L = 0x26, K_E = 0x12, K_R = 0x13, K_J = 0x24, K_F3 = 0x3d };
#define MENUBAR_H 20
#define TOOLBAR_H 30
#define FORMATBAR_H 28
#define RULER_H 20
#define STATUS_H 20
#define EDIT_H 22
#define SB_W 16
#define HSB_H 16
#define PAD 8
#define NO_PARA UINT_MAX
#define DOUBLE_NS 500000000ull
#define TEXT_FILTER "*.rtf;*.doc;*.wri;*.txt"
#define PIC_FILTER "*.bmp;*.png;*.jpg;*.jpeg;*.gif"

static const char *const yes_no_cancel[] = {"&Yes", "&No", "Cancel"};
static const char *const yes_no[] = {"&Yes", "&No"};
static const char *const color_names[] = {"Automatic", "Black", "Maroon", "Green", "Olive", "Navy", "Purple", "Teal", "Gray",
                                          "Silver", "Red", "Lime", "Yellow", "Blue", "Fuchsia", "Aqua", "White"};
static char size_names[16][6];
static const unsigned sizes_hp[] = {16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 72, 96, 144};   /* 8..72 pt */
static const int zoom_steps[] = {25, 35, 50, 70, 100, 140};

static const struct du_menu_item file_template[] = {
    {"&New", M_NEW, K_N, DU_CTRL, 0}, {"&Open...", M_OPEN, K_O, DU_CTRL, 0}, {"&Save", M_SAVE, K_S, DU_CTRL, 0},
    {"Save &As...", M_SAVEAS, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Print...", M_PRINT, K_P, DU_CTRL, 0}, {"Print Pre&view", M_PREVIEW, 0, 0, 0},
    {"Page Set&up...", M_PAGESETUP, 0, 0, 0}, {"E&xit", M_EXIT, 0, 0, 0}};
static const struct du_menu_item edit_template[] = {
    {"&Undo", M_UNDO, K_Z, DU_CTRL, 0}, {"&Redo", M_REDO, K_Y, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Cu&t", M_CUT, K_X, DU_CTRL, 0}, {"&Copy", M_COPY, K_C, DU_CTRL, 0}, {"&Paste", M_PASTE, K_V, DU_CTRL, 0},
    {"C&lear", M_CLEAR, DU_KEY_DELETE, 0, 0}, {"Select &All", M_SELALL, K_A, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Find...", M_FIND, K_F, DU_CTRL, 0}, {"Find &Next", M_FINDNEXT, K_F3, 0, 0}, {"R&eplace...", M_REPLACE, K_H, DU_CTRL, 0}};
static const struct du_menu_item view_template[] = {
    {"&Toolbar", M_TOOLBAR, 0, 0, 0}, {"&Format Bar", M_FORMATBAR, 0, 0, 0}, {"&Ruler", M_RULER, 0, 0, 0}, {"&Status Bar", M_STATUS, 0, 0, 0},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Options...", M_OPTIONS, 0, 0, 0}};
static const struct du_menu_item insert_template[] = {{"&Date and Time...", M_DATETIME, 0, 0, 0}, {"&Picture...", M_INSERTPIC, 0, 0, 0}, {"&Object...", M_INSERTOBJ, 0, 0, 0}};
static const struct du_menu_item format_template[] = {
    {"&Font...", M_FONT, 0, 0, 0}, {"&Bullet Style", M_BULLET, 0, 0, 0}, {"&Paragraph...", M_PARAGRAPH, 0, 0, 0},
    {"&Tabs...", M_TABS, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}};
static const struct du_menu_item help_template[] = {
    {"&Help Topics", M_HELP, DU_KEY_F1, 0, 0}, {"&What's This?", M_CONTEXTHELP, DU_KEY_F1, DU_SHIFT, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About WordPad", M_ABOUT, 0, 0, 0}};
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

static int page_width_px(const struct wp_app *a)
{
    int pw = a->page.landscape ? a->page.paper_h : a->page.paper_w;
    return wp_twips_px(pw - a->page.left - a->page.right);
}
/* The layout width: the window, or in "wrap to ruler" mode the printable width of the page. */
static int wrap_width(const struct wp_app *a)
{
    int w = a->doc_w > 16 ? a->doc_w : 16;
    if (a->wrap[a->format] == WP_WRAP_RULER) { int p = page_width_px(a); if (p > 16) w = p; }
    return w;
}
static void relayout(struct wp_app *a)
{
    int width = wrap_width(a);
    a->lay.nowrap = a->wrap[a->format] == WP_WRAP_NONE;
    unsigned from = a->dirty_para == NO_PARA ? a->lay.pcount : a->dirty_para;
    int e = wp_layout_build(&a->lay, &a->doc, tops(a), width, from);
    if (e) snprintf(a->note, sizeof(a->note), "Not enough memory to lay out the document");
    a->dirty_para = NO_PARA;
    struct du_widget *sb = W(a, W_SCROLL), *hb = W(a, W_HSCROLL);
    int need_h = !a->preview && a->lay.content_w + 2 * PAD > a->doc_box.w;
    if (need_h != a->hs_visible && !a->preview) { a->hs_visible = need_h; wp_layout_ui(a); return; }   /* the bars change the room of the document */
    if (sb) {
        sb->total = a->lay.height + 8; sb->page = a->doc_h;
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        if (a->top > max) a->top = max;
        if (a->top < 0) a->top = 0;
        sb->value = a->top;
    }
    if (hb) {
        hb->total = a->lay.content_w + 2 * PAD; hb->page = a->doc_box.w;
        int max = hb->total > hb->page ? hb->total - hb->page : 0;
        if (a->left > max) a->left = max;
        if (a->left < 0 || !a->hs_visible) a->left = 0;
        hb->value = a->left; hb->hidden = !a->hs_visible || a->preview;
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
    struct du_widget *sb = W(a, W_SCROLL), *hb = W(a, W_HSCROLL);
    if (sb) {
        int max = sb->total > sb->page ? sb->total - sb->page : 0;
        if (a->top > max) a->top = max;
        sb->value = a->top;
    }
    if (hb && a->hs_visible) {   /* keep the caret column in view when lines are wider than the window */
        if (PAD + x < a->left) a->left = PAD + x > 8 ? PAD + x - 8 : 0;
        else if (PAD + x + 2 > a->left + a->doc_box.w) a->left = PAD + x + 10 - a->doc_box.w;
        int max = hb->total > hb->page ? hb->total - hb->page : 0;
        if (a->left > max) a->left = max;
        if (a->left < 0) a->left = 0;
        hb->value = a->left;
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
    for (unsigned i = 0; i < a->doc.nfonts; i++) a->font_items[i] = (struct du_item){.text = {a->doc.fonts[i].name}};
    unsigned nsizes = sizeof(sizes_hp) / sizeof(*sizes_hp);
    for (unsigned i = 0; i < nsizes; i++) {
        snprintf(size_names[i], sizeof(size_names[i]), "%u", sizes_hp[i] / 2);
        a->size_items[i] = (struct du_item){.text = {size_names[i]}};
    }
    int size_index = -1;
    for (unsigned i = 0; i < nsizes; i++) if (a->cmix.size == (int)sizes_hp[i]) size_index = (int)i;
    /* A box the user is typing in keeps its text until Enter or Escape. */
    int f_busy = f && a->ui->focus >= 0 && a->ui->focus < (int)a->ui->count && &a->ui->widgets[a->ui->focus] == f;
    int s_busy = s && a->ui->focus >= 0 && a->ui->focus < (int)a->ui->count && &a->ui->widgets[a->ui->focus] == s;
    if (f) { f->items = a->font_items; f->count = a->doc.nfonts; f->value = a->cmix.font >= 0 && (unsigned)a->cmix.font < a->doc.nfonts ? a->cmix.font : -1; }
    if (s) { s->items = a->size_items; s->count = nsizes; s->value = size_index; }
    if (f && !f_busy && strcmp(f->text, a->font_label)) { du_set_text(f, a->font_label); du_invalidate(a->ui, f->box); }
    if (s && !s_busy && strcmp(s->text, a->size_label)) { du_set_text(s, a->size_label); du_invalidate(a->ui, s->box); }
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
    set_flag(&a->edit_items[E_UNDO], DU_ITEM_DISABLED, !a->nundo);
    set_flag(&a->edit_items[E_REDO], DU_ITEM_DISABLED, !a->nredo);
    set_flag(&a->edit_items[E_CUT], DU_ITEM_DISABLED, !sel);
    set_flag(&a->edit_items[E_COPY], DU_ITEM_DISABLED, !sel);
    set_flag(&a->edit_items[E_CLEAR], DU_ITEM_DISABLED, !sel);
    set_flag(&a->view_items[0], DU_ITEM_CHECKED, a->show_toolbar);
    set_flag(&a->view_items[1], DU_ITEM_CHECKED, a->show_format);
    set_flag(&a->view_items[2], DU_ITEM_CHECKED, a->show_ruler);
    set_flag(&a->view_items[3], DU_ITEM_CHECKED, a->show_status);
    set_flag(&a->format_items[1], DU_ITEM_CHECKED, a->pmix.bullet == 1);
    struct { int id, disabled; } tool[] = {{M_CUT, !sel}, {M_COPY, !sel}, {M_UNDO, !a->nundo}};
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
/* Multi-level undo and redo: a snapshot before each command (typing and deleting runs share one), at most WP_UNDO_LEVELS
 * of them and WP_UNDO_BYTES of text; the oldest levels are dropped first. Pictures are shared by reference. */
static size_t snap_bytes(const struct wp_doc *d)
{ return d->len + (size_t)d->nruns * sizeof(*d->runs) + (size_t)d->npara * sizeof(*d->paras) + 4096; }
static void snaps_clear(struct wp_snap **v, unsigned *n, unsigned *cap)
{
    for (unsigned i = 0; i < *n; i++) wp_free(&(*v)[i].doc);
    free(*v); *v = NULL; *n = *cap = 0;
}
static int snaps_push(struct wp_snap **v, unsigned *n, unsigned *cap, struct wp_snap *s)   /* takes the document of s */
{
    if (*n == *cap) {
        unsigned c = *cap ? *cap * 2 : 16;
        struct wp_snap *q = realloc(*v, c * sizeof(*q));
        if (!q) return -ENOMEM;
        *v = q; *cap = c;
    }
    (*v)[(*n)++] = *s;
    return 0;
}
static void undo_forget(struct wp_app *a)
{
    snaps_clear(&a->undo_s, &a->nundo, &a->cundo); snaps_clear(&a->redo_s, &a->nredo, &a->credo); a->undo_bytes = 0;
}
static void edit_begin(struct wp_app *a, enum wp_edit kind)
{
    if (kind != WP_EDIT_OTHER && a->last_edit == kind && a->cursor == a->last_edit_end && !has_selection(a) && a->nundo) return;
    size_t need = snap_bytes(&a->doc);
    struct wp_snap s; memset(&s, 0, sizeof(s));
    if (need > WP_UNDO_BYTES || wp_copy(&s.doc, &a->doc)) { undo_forget(a); return; }
    s.cursor = a->cursor; s.anchor = a->anchor;
    while (a->nundo && (a->nundo >= WP_UNDO_LEVELS || a->undo_bytes + need > WP_UNDO_BYTES)) {   /* drop the oldest level */
        a->undo_bytes -= snap_bytes(&a->undo_s[0].doc);
        wp_free(&a->undo_s[0].doc);
        memmove(a->undo_s, a->undo_s + 1, (a->nundo - 1) * sizeof(*a->undo_s));
        a->nundo--;
    }
    if (snaps_push(&a->undo_s, &a->nundo, &a->cundo, &s)) { wp_free(&s.doc); return; }
    a->undo_bytes += need;
    snaps_clear(&a->redo_s, &a->nredo, &a->credo);   /* a new edit ends the redo chain */
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

/* Takes one level from `from` and puts the current document on `to`: undo when from is the undo stack, redo the other way. */
static void undo_move(struct wp_app *a, struct wp_snap **from, unsigned *nfrom, struct wp_snap **to, unsigned *nto, unsigned *cto, int undoing)
{
    if (!*nfrom) return;
    struct wp_snap now = {a->doc, a->cursor, a->anchor};
    if (snaps_push(to, nto, cto, &now)) { memset(&now, 0, sizeof(now)); memory_note(a, -ENOMEM); return; }
    struct wp_snap top = (*from)[--*nfrom];
    if (undoing) a->undo_bytes -= snap_bytes(&top.doc); else a->undo_bytes += snap_bytes(&a->doc);
    a->doc = top.doc;
    a->cursor = top.cursor > a->doc.len ? a->doc.len : top.cursor;
    a->anchor = top.anchor > a->doc.len ? a->doc.len : top.anchor;
    a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    a->dirty_para = 0; a->lay.pcount = 0;
    changed(a);
}
static void undo_swap(struct wp_app *a, int undoing)
{
    if (undoing) undo_move(a, &a->undo_s, &a->nundo, &a->redo_s, &a->nredo, &a->credo, 1);
    else undo_move(a, &a->redo_s, &a->nredo, &a->undo_s, &a->nundo, &a->cundo, 0);
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

/* ---- pictures ------------------------------------------------------------------------------------------------ */

/* The decoded pixels of a picture, decoding on first use; NULL when it cannot be shown (a placeholder is drawn). */
static const struct wp_pic *pic_ready(struct wp_app *a, struct wp_pic *p)
{
    if (p->px_state == WP_PX_UNTRIED) {
        p->px_state = WP_PX_FAILED;
        uint32_t *px = NULL; unsigned w = 0, h = 0;
        if (a->images && a->images->decode && p->len && a->images->decode(p->kind, p->data, p->len, &px, &w, &h) == 0 && px) {
            p->px = px; p->pxw = w; p->pxh = h; p->px_state = WP_PX_OK;
            if (!p->pw || !p->ph) { p->pw = w; p->ph = h; }
        }
    }
    return p->px_state == WP_PX_OK ? p : NULL;
}

/* Inserts a picture (taking the caller's reference) in place of the selection. */
static int insert_picture(struct wp_app *a, struct wp_pic *p)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (a->doc.len - (hi - lo) + WP_PIC_MARK_LEN > WP_LIMIT) { wp_pic_unref(p); return memory_note(a, -EFBIG); }
    struct wp_cattr attr = typing_attr(a);
    edit_begin(a, WP_EDIT_OTHER);
    mark_dirty(a, lo);
    int e = hi > lo ? wp_delete(&a->doc, lo, hi) : 0;
    if (!e) e = wp_insert_pic(&a->doc, lo, p, &attr); else wp_pic_unref(p);
    if (e) { memory_note(a, e); a->cursor = a->anchor = lo; changed(a); return e; }
    a->cursor = a->anchor = lo + WP_PIC_MARK_LEN; a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    changed(a);
    return 0;
}

/* ---- clipboard ----------------------------------------------------------------------------------------------- */

#define RTF_FORMAT_NAME "Rich Text Format"

/* Publishes the selection on the system clipboard (G5) as Unicode text, Rich Text Format and, for a lone picture, a DIB.
 * Without a desktop clipboard (-ENOSYS) the text goes to the toolkit's own clipboard. */
static int clip_publish(struct wp_app *a, const char *plain, size_t n, const char *rtf, size_t rn, const struct wp_doc *slice)
{
    int e = du_clip_open();
    if (e == -ENOSYS) return du_clipboard_set_n(plain, n) ? -ENOMEM : 0;
    if (e) return e;
    void *u = NULL; uint32_t un = 0;
    e = clip_text_encode(plain, n, &u, &un);
    if (!e) e = du_clip_empty();
    if (!e) e = du_clip_put(DESK_CF_UNICODETEXT, u, un);
    free(u);
    int rtf_id = du_clip_register(RTF_FORMAT_NAME);
    if (!e && rtf_id > 0) e = du_clip_put((unsigned)rtf_id, rtf, (uint32_t)(rn + 1));   /* the writer terminates its output: the format is NUL-terminated */
    struct wp_pic *only = slice->len == WP_PIC_MARK_LEN ? wp_pic_at(slice, 0) : NULL;
    if (!e && only && pic_ready(a, only) && only->pxw <= 2048 && only->pxh <= 2048) {
        void *dib = NULL; uint32_t db = 0;
        if (!clip_dib_encode(only->px, only->pxw, only->pxw, only->pxh, &dib, &db)) { e = du_clip_put(DESK_CF_DIB, dib, db); free(dib); }
    }
    int closed = du_clip_close();
    return e ? e : closed;
}
static int copy_selection(struct wp_app *a)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (lo == hi) return 0;
    struct wp_doc slice;
    if (wp_init(&slice)) return -ENOMEM;
    int e = wp_slice(&slice, &a->doc, lo, hi);
    char *plain = NULL, *rtf = NULL; size_t n = 0, rn = 0;
    if (!e) e = wp_text_write(&slice, &plain, &n, 0);
    if (!e) e = wp_rtf_write(&slice, &rtf, &rn);
    if (!e) e = clip_publish(a, plain, n, rtf, rn, &slice);
    free(rtf);
    if (e) { free(plain); wp_free(&slice); return e; }
    wp_free(&a->clip); a->clip = slice; a->clip_valid = 1;   /* the private copy serves when the desktop has no clipboard */
    free(a->clip_plain); a->clip_plain = plain;
    return 0;
}
/* Replaces the selection with a rich slice (the document of a paste). */
static void paste_slice(struct wp_app *a, const struct wp_doc *slice)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    if (a->doc.len - (hi - lo) + slice->len > WP_LIMIT) { memory_note(a, -EFBIG); return; }
    edit_begin(a, WP_EDIT_OTHER);
    mark_dirty(a, lo);
    int e = hi > lo ? wp_delete(&a->doc, lo, hi) : 0;
    if (!e) e = wp_insert_slice(&a->doc, lo, slice);
    if (e) { memory_note(a, e); changed(a); return; }
    a->cursor = a->anchor = lo + slice->len; a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    changed(a);
}
static int clip_get_format(unsigned format, void **data, uint32_t *n)
{
    int e = du_clip_open();
    if (e) return e;
    e = du_clip_get(format, data, n);
    du_clip_close();
    return e;
}
/* Paste from the system clipboard: Rich Text Format first (formatting survives between programs), then text, then a picture. */
static int paste_system(struct wp_app *a)
{
    int rtf_id = du_clip_register(RTF_FORMAT_NAME);
    if (rtf_id <= 0) return 0;
    if (du_clip_available((unsigned)rtf_id) == 1) {
        void *d = NULL; uint32_t n = 0;
        if (!clip_get_format((unsigned)rtf_id, &d, &n) && d) {
            while (n && ((unsigned char *)d)[n - 1] == 0) n--;
            struct wp_doc sl;
            int ok = !wp_init(&sl) && !wp_rtf_read(&sl, d, n, NULL);
            if (ok) { if (sl.len) paste_slice(a, &sl); wp_free(&sl); free(d); return 1; }
            wp_free(&sl);
        }
        free(d);
    }
    if (du_clip_available(DESK_CF_UNICODETEXT) == 1 || du_clip_available(DESK_CF_TEXT) == 1) return 0;   /* the text path below */
    if (du_clip_available(DESK_CF_DIB) == 1) {
        void *d = NULL; uint32_t n = 0;
        if (!clip_get_format(DESK_CF_DIB, &d, &n) && d) {
            struct wp_pic *p = wp_pic_new(WP_PIC_DIB, d, n);
            free(d);
            if (p) { insert_picture(a, p); return 1; }
        }
    }
    return 0;
}
static const char *clipboard_text(void)
{
    const char *text = du_system_clipboard_text_get();
    return text ? text : du_clipboard_get();
}
static void paste(struct wp_app *a)
{
    if (paste_system(a)) return;
    const char *text = clipboard_text(); size_t n = strlen(text);
    if (!n) return;
    if (a->clip_valid && a->clip_plain && !strcmp(text, a->clip_plain)) { paste_slice(a, &a->clip); return; }   /* our own copy: keep its formatting */
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
/* A picture or object: its pixels scaled to the line (nearest neighbour), or a placeholder when they cannot be decoded. */
static void draw_picture(struct paint_ctx *p, const struct wp_piece *pc)
{
    struct wp_app *a = p->a; struct du_canvas *c = p->c;
    struct wp_pic *pic = wp_pic_at(&a->doc, pc->start);
    if (!pic) return;
    int pw, ph; wp_layout_pic_size(&a->doc, &pc->a, &pw, &ph);
    int w = pw * p->num / p->den, h = ph * p->num / p->den;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int x = p->ox + pc->x * p->num / p->den, y = p->oy + p->base * p->num / p->den - h;   /* the bottom sits on the baseline */
    struct du_rect r = du_intersection(c->clip, (struct du_rect){x, y, w, h});
    if (r.w <= 0 || r.h <= 0) return;
    const struct wp_pic *ok = pic_ready(a, pic);
    if (ok) {
        for (int yy = r.y; yy < r.y + r.h; yy++) {
            const uint32_t *row = ok->px + (size_t)(((int64_t)(yy - y) * ok->pxh) / h) * ok->pxw;
            uint32_t *dst = c->pixels + (size_t)yy * (size_t)c->width;
            for (int xx = r.x; xx < r.x + r.w; xx++) dst[xx] = row[((int64_t)(xx - x) * ok->pxw) / w];
        }
    } else {
        static const char *const kind[] = {"Object", "PNG", "JPEG", "Bitmap", "Metafile", "Metafile", "Picture"};
        du_fill(c, (struct du_rect){x, y, w, h}, 0xf0f0f0u);
        du_outline(c, (struct du_rect){x, y, w, h}, 0x808080u);
        char label[48]; snprintf(label, sizeof(label), "%s", pic->cls[0] ? pic->cls : kind[pic->kind < 7 ? pic->kind : 0]);
        if (w > 40 && h > 14) du_text(c, x + 4, y + 3, label, 0x606060u);
    }
    if (p->selected && p->sel_x0 <= pc->x && p->sel_x1 >= pc->x + pc->w) {
        du_outline(c, (struct du_rect){x - 1, y - 1, w + 2, h + 2}, DU_SELECT);
        du_outline(c, (struct du_rect){x - 2, y - 2, w + 4, h + 4}, DU_SELECT);
    }
}
static int paint_piece(void *ctx, const struct wp_piece *pc)
{
    struct paint_ctx *p = ctx;
    struct du_canvas *c = p->c;
    int x = p->ox + pc->x * p->num / p->den, w = pc->w * p->num / p->den;
    if (x + w < p->box.x || x > p->box.x + p->box.w) return 0;
    if (pc->kind == WP_PIECE_PIC) { draw_picture(p, pc); return 0; }
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
/* The right edge of what line i shows (pieces, with the trailing spaces of a wrapped line), in layout pixels. */
static int piece_right(void *ctx, const struct wp_piece *pc) { int *r = ctx; if (pc->x + pc->w > *r) *r = pc->x + pc->w; return 0; }
static int line_end_x(const struct wp_app *a, const struct wp_layout *l, unsigned i)
{
    int r = l->lines[i].shift + l->lines[i].indent;
    wp_layout_pieces(l, &a->doc, tops(a), i, piece_right, &r);
    return r;
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
        if (hi >= ln->end) { x1 = line_end_x(a, l, i); if (hi > ln->end) x1 += 6; }   /* not the caret at ln->end: a wrapped line's end is the next line's start */
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
    int ox = b.x + PAD - a->left, oy = b.y - a->top;
    unsigned first = wp_layout_line_at_y(&a->lay, a->top);
    for (unsigned i = first; i < a->lay.n && a->lay.lines[i].y - a->top < b.h; i++)
        paint_line(a, &cv, b, &a->lay, i, ox, oy, 1, 1, lo, hi);
    if (lo == hi && a->caret_on && a->ui->focused_window && du_focused(a->ui) == W_DOC && !a->ui->dialog.kind) {
        int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
        du_fill(&cv, (struct du_rect){ox + x, oy + y, 1, h}, 0x000000u);
    }
}

/* ---- measurement units (View > Options): the dialogs, the ruler and the status bar use the chosen one ------------------------ */

static const char *const unit_names[WP_UNITS] = {"inches", "centimeters", "points", "picas"};
static const char *const unit_short[WP_UNITS] = {"\"", " cm", " pt", " pi"};
/* Twips per unit as a fraction (a centimetre is 566.93 twips). */
static const int64_t unit_tw[WP_UNITS] = {1440, 56693, 20, 240}, unit_den[WP_UNITS] = {1, 100, 1, 1};

/* A number with an optional unit (", in, cm, mm, pt, pi, pc); a bare number is in `units`. */
static int parse_units(const char *s, int units, int *twips)
{
    long whole = 0, frac = 0, scale = 1; int digits = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9'; s++) { whole = whole * 10 + (*s - '0'); digits++; if (whole > 100000) return -1; }
    if (*s == '.' || *s == ',') {
        s++;
        for (; *s >= '0' && *s <= '9'; s++) { digits++; if (scale < 1000) { frac = frac * 10 + (*s - '0'); scale *= 10; } }
    }
    while (*s == ' ') s++;
    int u = units; int64_t mm = 0;
    if (*s == '"') { u = WP_UNIT_INCH; s++; }
    else if ((s[0] | 32) == 'i' && (s[1] | 32) == 'n') { u = WP_UNIT_INCH; s += 2; }
    else if ((s[0] | 32) == 'c' && (s[1] | 32) == 'm') { u = WP_UNIT_CM; s += 2; }
    else if ((s[0] | 32) == 'm' && (s[1] | 32) == 'm') { u = WP_UNIT_CM; mm = 1; s += 2; }
    else if ((s[0] | 32) == 'p' && (s[1] | 32) == 't') { u = WP_UNIT_POINT; s += 2; }
    else if ((s[0] | 32) == 'p' && ((s[1] | 32) == 'i' || (s[1] | 32) == 'c')) { u = WP_UNIT_PICA; s += 2; }
    while (*s == ' ') s++;
    if (*s || !digits) return -1;
    int64_t v1000 = (int64_t)whole * 1000 + (int64_t)(frac * 1000 / scale);   /* thousandths of the unit */
    int64_t den = 1000 * unit_den[u] * (mm ? 10 : 1), t = (v1000 * unit_tw[u] + den / 2) / den;   /* rounded to the nearest twip */
    if (neg) t = -t;
    if (t > WP_INDENT_MAX || t < -WP_INDENT_MAX) return -1;
    *twips = (int)t;
    return 0;
}
static void format_units(char *out, size_t cap, int twips, int units)
{
    int64_t t = twips < 0 ? -(int64_t)twips : twips;
    int64_t v100 = (t * unit_den[units] * 100 + unit_tw[units] / 2) / unit_tw[units];
    int whole = (int)(v100 / 100), frac = (int)(v100 % 100);
    char num[24];
    if (frac) snprintf(num, sizeof(num), "%d.%02d", whole, frac); else snprintf(num, sizeof(num), "%d", whole);
    size_t n = strlen(num);
    if (frac && num[n - 1] == '0') num[--n] = 0;
    snprintf(out, cap, "%s%s%s", twips < 0 ? "-" : "", num, unit_short[units]);
}
/* Snap of a ruler drag, in twips: 1/16 inch, 1 mm, 1 point, 1/4 pica. */
static int snap_twips(const struct wp_app *a, int t)
{
    static const int step[WP_UNITS] = {90, 57, 20, 60};
    int s = step[a->units], r = t < 0 ? -((-t + s / 2) / s) * s : ((t + s / 2) / s) * s;
    return r;
}
static int twips_of_px(int px) { return px * 15; }

/* ---- the ruler ------------------------------------------------------------------------------------------------------------ */

enum { R_NONE, R_FIRST, R_LEFT, R_RIGHT, R_TAB };
#define RULER_TOL 4
static int ruler_x0(const struct wp_app *a) { return a->ruler_box.x + PAD - a->left; }
static int ruler_width_twips(const struct wp_app *a) { return twips_of_px(a->lay.width ? a->lay.width : a->doc_w); }
static void paint_ruler(struct wp_app *a, struct du_canvas *c)
{
    struct du_rect r = a->ruler_box;
    if (!a->show_ruler || a->preview || r.w <= 0) return;
    struct du_canvas cv = *c; cv.clip = du_intersection(c->clip, r);
    if (cv.clip.w <= 0 || cv.clip.h <= 0) return;
    du_fill(&cv, r, DU_FACE);
    int x0 = ruler_x0(a), width = a->lay.width ? a->lay.width : a->doc_w;
    du_fill(&cv, (struct du_rect){x0, r.y + 4, width, r.h - 8}, 0xffffffu);
    du_outline(&cv, (struct du_rect){x0, r.y + 4, width, r.h - 8}, DU_SHADOW);
    /* ticks: eighths of an inch (also for points and picas, whose labels count 72 and 6 to the inch); half centimetres */
    int64_t step_n = a->units == WP_UNIT_CM ? 56693 : 180, step_d = a->units == WP_UNIT_CM ? 200 : 1;
    unsigned per = a->units == WP_UNIT_CM ? 2 : 8;
    for (unsigned k = 0;; k++) {
        int x = (int)((int64_t)k * step_n / step_d * 96 / 1440);
        if (x > width) break;
        int tall = k % per == 0 ? 6 : (per == 8 && k % 4 == 0) ? 4 : 2;
        if (k % per == 0 && k) {
            unsigned n = k / per;
            char num[12]; snprintf(num, sizeof(num), "%u", a->units == WP_UNIT_POINT ? n * 72 : a->units == WP_UNIT_PICA ? n * 6 : n);
            du_text(&cv, x0 + x - 2, r.y + 5, num, DU_INK);
        } else du_fill(&cv, (struct du_rect){x0 + x, r.y + r.h / 2 - tall / 2, 1, tall}, DU_INK);
    }
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    int left = wp_twips_px(pa.left), first = wp_twips_px(pa.left + pa.first), right = wp_twips_px(pa.right);
    if (a->rdrag.active) {
        int v = wp_twips_px(a->rdrag.pos);
        if (a->rdrag.kind == R_FIRST) first = v; else if (a->rdrag.kind == R_LEFT) { first += v - left; left = v; } else if (a->rdrag.kind == R_RIGHT) right = width - v;
    }
    for (unsigned t = 0; t < pa.ntabs; t++) {   /* a left tab stop: an L */
        int v = a->rdrag.active && a->rdrag.kind == R_TAB && a->rdrag.index == (int)t ? a->rdrag.pos : pa.tabs[t], x = x0 + wp_twips_px(v);
        du_fill(&cv, (struct du_rect){x, r.y + r.h - 11, 1, 7}, DU_INK);
        du_fill(&cv, (struct du_rect){x, r.y + r.h - 5, 5, 1}, DU_INK);
    }
    du_triangle(&cv, x0 + first, r.y + 3, 1, 3, DU_INK);               /* first-line indent */
    du_triangle(&cv, x0 + left, r.y + r.h - 4, 0, 3, DU_INK);          /* left indent */
    du_triangle(&cv, x0 + width - right, r.y + r.h - 4, 0, 3, DU_INK); /* right indent */
}

/* What lies under the pointer on the ruler: a marker (and the tab index) or nothing. */
static int ruler_hit(const struct wp_app *a, int x, int y, int *tab)
{
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    int x0 = ruler_x0(a), width = a->lay.width ? a->lay.width : a->doc_w, mid = a->ruler_box.y + a->ruler_box.h / 2;
    int left = x0 + wp_twips_px(pa.left), first = x0 + wp_twips_px(pa.left + pa.first), right = x0 + width - wp_twips_px(pa.right);
    for (unsigned t = 0; t < pa.ntabs; t++) if (abs(x - (x0 + wp_twips_px(pa.tabs[t]))) <= RULER_TOL && y >= mid - 2) { *tab = (int)t; return R_TAB; }
    if (y < mid && abs(x - first) <= RULER_TOL) return R_FIRST;
    if (y >= mid && abs(x - left) <= RULER_TOL) return R_LEFT;
    if (y >= mid && abs(x - right) <= RULER_TOL) return R_RIGHT;
    return R_NONE;
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

/* The B, I and U of the format bar: bold face, slanted regular face, underlined regular face. */
static void draw_style_letter(struct du_canvas *c, const struct du_widget *w, unsigned which, int down)
{
    static const char *const letters[3] = {"B", "I", "U"};
    int font = which == 0 ? DESK_FONT_BOLD : desk_theme->font_ui, off = down ? 1 : 0;
    int tw = du_text_width_font(font, letters[which]);
    int x = w->box.x + (w->box.w - tw) / 2 + off, y = w->box.y + (w->box.h - DU_GLYPH_H) / 2 + off;
    uint32_t ink = DU_INK;
    struct du_rect keep = c->clip;
    if (which == 1) {   /* slant: each 3-pixel band of rows moves one pixel right going up */
        int bands = (DU_GLYPH_H + 2) / 3;
        for (int k = 0; k < bands; k++) {
            struct du_rect band = {w->box.x, y + k * 3, w->box.w, 3};
            c->clip = du_intersection(keep, band);
            du_text_font(c, font, x + (bands - 1 - k) * 1 - bands / 2, y, letters[which], ink);
        }
        c->clip = keep;
    } else du_text_font(c, font, x, y, letters[which], ink);
    if (which == 2) du_fill(c, (struct du_rect){x, y + DU_GLYPH_H + 1, tw, 1}, ink);
}

static void paint_hook(struct du_ui *u, void *user)
{
    struct wp_app *a = user;
    struct du_canvas *c = &u->canvas;
    if (a->preview) paint_preview(a, c); else paint_doc(a, c);
    paint_ruler(a, c);
    /* Pressed look of the toggled format buttons. */
    if (a->preview || !a->show_format) return;
    struct du_rect keep_clip = c->clip;
    struct { int id, on; } tog[] = {{M_BOLD, a->cmix.bold == 1}, {M_ITALIC, a->cmix.italic == 1}, {M_UNDERLINE, a->cmix.underline == 1},
        {M_LEFT, a->pmix.align == WP_LEFT}, {M_CENTER, a->pmix.align == WP_CENTER}, {M_RIGHT, a->pmix.align == WP_RIGHT}, {M_BULLET, a->pmix.bullet == 1}};
    for (unsigned i = 0; i < sizeof(tog) / sizeof(*tog); i++) {
        struct du_widget *w = W(a, tog[i].id);
        if (!w || w->hidden) continue;
        c->clip = keep_clip;
        int letter = i < 3, down = w->pressed && w->hot && !w->disabled;
        if (tog[i].on || (letter && down)) {
            /* Win98 toggle: dithered face and a sunken edge while the format is on. */
            c->clip = du_intersection(c->clip, w->box);
            if (letter) for (int y = w->box.y + 1; y < w->box.y + w->box.h - 1; y++)
                for (int x = w->box.x + 1 + (y & 1); x < w->box.x + w->box.w - 1; x += 2) du_fill(c, (struct du_rect){x, y, 1, 1}, DU_LIGHT);
            du_edge(c, w->box, DU_SUNKEN_THIN);
        }
        if (letter) draw_style_letter(c, w, i, down || tog[i].on);
    }
    c->clip = keep_clip;
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
    static const struct { int id, width; } fbar[] = {{W_FONTBTN, 150}, {W_SIZEBTN, 52}, {W_COLORBTN, 24}, {W_SEP1 + 10, 0}, {M_BOLD, 26},
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
    struct du_widget *rl = W(a, W_RULER);
    rl->box = a->ruler_box; rl->hidden = !a->show_ruler || preview;
    if (a->show_ruler && !preview) y += RULER_H;
    struct du_widget *pv = W(a, W_PVBAR);
    pv->hidden = !preview;
    if (preview) {
        pv->box = (struct du_rect){0, y, w, TOOLBAR_H};
        static const int ids[] = {M_PV_PRINT, M_PV_PREV, M_PV_NEXT, M_PV_ZOOMOUT, M_PV_ZOOMIN, M_PV_CLOSE};
        static const int widths[] = {64, 76, 76, 76, 76, 66};
        int px = 6;
        for (unsigned i = 0; i < 6; i++) {
            struct du_widget *b = W(a, ids[i]);
            b->hidden = 0; b->box = (struct du_rect){px, y + 3, widths[i], TOOLBAR_H - 6}; px += widths[i] + 4;
        }
        y += TOOLBAR_H;
    } else {
        static const int ids[] = {M_PV_PRINT, M_PV_PREV, M_PV_NEXT, M_PV_ZOOMOUT, M_PV_ZOOMIN, M_PV_CLOSE};
        for (unsigned i = 0; i < 6; i++) W(a, ids[i])->hidden = 1;
    }
    int bar_y = h - status_h - bar_h;
    int hs = a->hs_visible && !preview ? HSB_H : 0;
    a->doc_box = (struct du_rect){0, y, w - SB_W, bar_y - y - hs};
    if (a->doc_box.h < 16) a->doc_box.h = 16;
    a->doc_w = a->doc_box.w - 2 * PAD; a->doc_h = a->doc_box.h;
    struct du_widget *d = W(a, W_DOC);
    d->box = a->doc_box;
    struct du_widget *sb = W(a, W_SCROLL);
    sb->box = (struct du_rect){w - SB_W, y, SB_W, a->doc_box.h};
    struct du_widget *hb = W(a, W_HSCROLL);
    hb->box = (struct du_rect){0, y + a->doc_box.h, w - SB_W, HSB_H}; hb->hidden = !hs;
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
    a->dirty_para = a->lay.width == wrap_width(a) ? a->dirty_para : 0;
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

static void reset_document(struct wp_app *a)
{
    undo_forget(a); a->pending_valid = 0; a->last_edit = WP_EDIT_OTHER;
    a->cursor = a->anchor = 0; a->top = a->left = 0; a->note[0] = 0; a->modified = 0;
    a->dirty_para = 0; a->lay.pcount = 0;
}
static void new_document(struct wp_app *a)
{
    wp_reset(&a->doc);
    reset_document(a);
    a->path[0] = 0; a->named = 0; a->format = WP_FMT_RTF; a->crlf = 1;
    relayout(a); reveal(a); invalidate_doc(a); wp_refresh(a);
}
/* PNG, JPEG, GIF and BMP files are not read as text. */
static int is_picture_file(const char *raw, size_t n)
{
    uint32_t w, h;
    if (wp_pic_sniff((const uint8_t *)raw, n, &w, &h) == WP_PIC_PNG || (n > 3 && (unsigned char)raw[0] == 0xff && (unsigned char)raw[1] == 0xd8 && (unsigned char)raw[2] == 0xff)) return 1;
    return (n >= 6 && !memcmp(raw, "GIF8", 4)) || (n >= 26 && raw[0] == 'B' && raw[1] == 'M' && raw[14] >= 12 && raw[14] <= 124 && !raw[15] && !raw[16] && !raw[17]);
}
static void open_notice(struct wp_app *a, int fmt, const struct wp_report *rep)
{
    char text[DU_DIALOG_TEXT]; size_t k = 0;
    text[0] = 0;
    if (rep->skipped_pictures || rep->skipped_objects)
        k += (size_t)snprintf(text + k, sizeof(text) - k, "%u picture%s and %u embedded object%s cannot be shown or kept by WordPad.\nThey are left out of the document.\n",
                              rep->skipped_pictures, rep->skipped_pictures == 1 ? "" : "s", rep->skipped_objects, rep->skipped_objects == 1 ? "" : "s");
    if (rep->unsupported_pictures && k + 120 < sizeof(text))
        k += (size_t)snprintf(text + k, sizeof(text) - k, "%u picture%s in a format WordPad cannot read.\n", rep->unsupported_pictures, rep->unsupported_pictures == 1 ? " is" : "s are");
    if (fmt == WP_FMT_RTF && rep->unknown_charset && k + 120 < sizeof(text))
        k += (size_t)snprintf(text + k, sizeof(text) - k, "This document uses a code page other than Windows-1252.\nAccented characters may look different.\n");
    if (rep->ignored_parts && k + 120 < sizeof(text))
        k += (size_t)snprintf(text + k, sizeof(text) - k, "Headers, footers, footnotes and text boxes are not shown.\n");
    if ((fmt == WP_FMT_WORD || fmt == WP_FMT_WRITE) && k + 120 < sizeof(text))
        k += (size_t)snprintf(text + k, sizeof(text) - k, "The document is saved as Rich Text Format.\n");
    if (!k) return;
    if (text[k - 1] == '\n') text[k - 1] = 0;
    box(a, D_NOTICE, "WordPad", DU_DLG_ICON_WARNING, NULL, 0, 0, 0, "%s", text);
}
int wp_open_bytes(struct wp_app *a, const char *path, const char *raw, size_t n)
{
    struct wp_doc fresh; struct wp_report rep;
    memset(&rep, 0, sizeof(rep));
    a->reason = NULL;
    if (wp_init(&fresh)) return -ENOMEM;
    int fmt = WP_FMT_TEXT, e;
    if (wp_is_rtf(raw, n)) { fmt = WP_FMT_RTF; e = wp_rtf_read(&fresh, raw, n, &rep); }
    else if (wp_is_word(raw, n)) { fmt = WP_FMT_WORD; e = wp_word_read(&fresh, (const uint8_t *)raw, n, &rep); }
    else if (wp_is_write(raw, n)) { fmt = WP_FMT_WRITE; e = wp_write_read(&fresh, (const uint8_t *)raw, n, &rep); }
    else if (is_picture_file(raw, n)) { e = -EINVAL; rep.reason = "This file is a picture, not a document. Use Insert > Picture to put it in a document."; }
    else e = wp_text_read(&fresh, raw, n);
    if (e) { wp_free(&fresh); a->reason = rep.reason; return e; }
    wp_free(&a->doc); a->doc = fresh;
    reset_document(a);
    snprintf(a->path, sizeof(a->path), "%s", path); a->named = 1; a->format = fmt;
    a->crlf = 1;
    for (size_t i = 0; i < n && fmt == WP_FMT_TEXT; i++) if (raw[i] == '\n') { a->crlf = i > 0 && raw[i - 1] == '\r'; break; }
    if (a->recent) a->recent(a->path);
    relayout(a); reveal(a); invalidate_doc(a); wp_refresh(a);
    open_notice(a, fmt, &rep);
    return 0;
}
static void load_failure(struct wp_app *a, const char *path, int r)
{
    if (r == -ENOTSUP || (r == -EINVAL && a->reason)) {
        char shown[48]; shorten(shown, sizeof(shown), cn_basename(path), 44);
        box(a, D_INFO, "WordPad", DU_DLG_ICON_ERROR, NULL, 0, 0, 0, "%s\n%s", a->reason ? a->reason : "This kind of file cannot be opened.", shown);
    }
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
    if (format != WP_FMT_TEXT) format = WP_FMT_RTF;   /* an imported Word or Write document is saved as Rich Text Format */
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
static int imported(const struct wp_app *a) { return a->format == WP_FMT_WORD || a->format == WP_FMT_WRITE; }
static void command_save(struct wp_app *a)
{
    if (!a->named || imported(a)) { file_dialog(a, 1, ""); return; }   /* a Word or Write document is saved under a new .rtf name */
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
        if (save && !name[0]) {
            name = cn_basename(a->path);
            if (imported(a)) {   /* the suggested name has the .rtf extension */
                snprintf(a->suggest, sizeof(a->suggest), "%s", name);
                char *dot = strrchr(a->suggest, '.');
                if (dot) strcpy(dot, ".rtf"); else strncat(a->suggest, ".rtf", sizeof(a->suggest) - strlen(a->suggest) - 1);
                name = a->suggest;
            }
        }
    }
    /* G28: WordPad appends .rtf and asks about replacing the resulting name itself (D_REPLACE), so the picker does
     * not prompt on the typed name (G16's du_file_dialog would). */
    struct du_file_options o = {.flags = save ? DU_FILE_SAVE : 0, .title = save ? "Save As" : "Open", .folder = folder,
                                .name = name, .lister = lister, .lister_ctx = a};
    int r = du_file_open(a->ui, save ? D_SAVE : D_OPEN, &o);
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
        if (parse_units(u->dialog.items[4 + i].text, a->units, &v[i]) || v[i] < (i == 2 ? -WP_INDENT_MAX : 0)) {
            snprintf(u->dialog.text, sizeof(u->dialog.text), "Enter an indent as a number, in %s unless you add a unit, for example 0.5.", unit_names[a->units]);
            return 1;
        }
    struct wp_pstyle st = {WP_P_ALIGN | WP_P_LEFT | WP_P_FIRST | WP_P_RIGHT, wp_default_pattr()};
    st.v.left = v[0]; st.v.right = v[1]; st.v.first = v[2] - v[0];
    st.v.align = u->dialog.items[7].value ? WP_LEFT : u->dialog.items[8].value ? WP_CENTER : u->dialog.items[9].value ? WP_RIGHT : WP_JUSTIFY;
    du_dialog_close(u);   /* the result event follows; apply first while the values are valid */
    apply_paras(a, &st);
    return 0;
}
static void paragraph_paint(struct du_ui *u, void *ctx)
{
    struct wp_app *a = ctx; char t[48];
    snprintf(t, sizeof(t), "Left indent (%s):", unit_names[a->units]); label_at(u, 4, t, 0, -16);
    snprintf(t, sizeof(t), "Right indent (%s):", unit_names[a->units]); label_at(u, 5, t, 0, -16);
    snprintf(t, sizeof(t), "First line (%s):", unit_names[a->units]); label_at(u, 6, t, 0, -16);
    label_at(u, 7, "Alignment:", 0, -18);
}
static void paragraph_dialog(struct wp_app *a)
{
    sheet_open(a, D_PARAGRAPH, "Paragraph", 340, 290, "Indentation", paragraph_accept, paragraph_paint);
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    char b[3][24];
    format_units(b[0], sizeof(b[0]), pa.left, a->units); format_units(b[1], sizeof(b[1]), pa.right, a->units); format_units(b[2], sizeof(b[2]), pa.left + pa.first, a->units);
    for (unsigned i = 0; i < 3; i++)
        du_property_control(a->ui, 4 + i, DU_EDIT, 300 + (int)i, (struct du_rect){24, 52 + (int)i * 46, 130, 22}, b[i]);
    static const char *const names[] = {"&Left", "&Center", "&Right", "&Justify"};
    for (unsigned i = 0; i < 4; i++) {
        struct du_widget *w = du_property_control(a->ui, 7 + i, DU_RADIO, 310 + (int)i, (struct du_rect){24 + (int)i * 76, 186, 72, 22}, names[i]);
        w->total = 73; w->value = pa.align == i;
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
        if (parse_units(one, a->units, &t) || t <= 0) { snprintf(u->dialog.text, sizeof(u->dialog.text), "Tab stops are positions in %s, for example 0.5 1.5 3.", unit_names[a->units]); return 1; }
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
    struct wp_app *a = ctx; char t[80];
    snprintf(t, sizeof(t), "Tab stop positions (%s, separated by spaces):", unit_names[a->units]); label_at(u, 4, t, 0, -16);
    label_at(u, 4, "Leave empty to use the default stops every half inch.", 0, 30);
}
static void tabs_dialog(struct wp_app *a)
{
    sheet_open(a, D_TABS, "Tabs", 380, 200, "Tab stops", tabs_accept, tabs_paint);
    struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
    char text[160] = ""; size_t k = 0;
    for (unsigned i = 0; i < pa.ntabs && k + 12 < sizeof(text); i++) {
        char one[24]; format_units(one, sizeof(one), pa.tabs[i], a->units);
        k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", i ? " " : "", one);
    }
    du_property_control(a->ui, 4, DU_EDIT, 320, (struct du_rect){24, 60, 320, 22}, text);
}

/* The shared Font dialog paints its sample with WordPad's real font backend. */
static void font_sample(struct du_canvas *cv, struct du_rect r, const struct du_font_choice *c, void *ctx)
{
    struct wp_app *a = ctx;
    int at = wp_font_find(&a->doc, c->family);
    unsigned family = at >= 0 ? a->doc.fonts[at].family : WP_SANS;
    unsigned flags = (c->style & DU_FONT_BOLD ? WP_BOLD : 0) | (c->style & DU_FONT_ITALIC ? WP_ITALIC : 0);
    unsigned hp = c->points > 24 ? 48 : c->points * 2;
    int asc, desc; tops(a)->vmetrics(tops(a)->ctx, family, flags, hp, &asc, &desc);
    int base = r.y + r.h / 2 + (asc - desc) / 2, x = r.x + 8;
    const char *text = "AaBbYyZz";
    a->draw->draw(a->draw->text.ctx, cv, family, flags, wp_size_px64(hp), x, base, text, strlen(text), c->color);
    int width = tops(a)->width(tops(a)->ctx, family, flags, hp, text, strlen(text)) >> 6;
    if (c->underline) du_fill(cv, (struct du_rect){x, base + 1, width, 1}, c->color);
    if (c->strikeout) du_fill(cv, (struct du_rect){x, base - asc / 3, width, 1}, c->color);
}
static void font_dialog(struct wp_app *a)
{
    struct wp_cattr current = typing_attr(a);
    const char *faces[WP_FONTS];
    for (unsigned i = 0; i < a->doc.nfonts; i++) faces[i] = a->doc.fonts[i].name;
    struct du_font_choice c = {.points = current.size / 2,
        .style = (current.flags & WP_BOLD ? DU_FONT_BOLD : 0) | (current.flags & WP_ITALIC ? DU_FONT_ITALIC : 0),
        .underline = !!(current.flags & WP_UNDERLINE), .strikeout = !!(current.flags & WP_STRIKE), .color = a->doc.colors[current.color]};
    snprintf(c.family, sizeof(c.family), "%s", a->doc.fonts[current.font].name);
    if (du_font_dialog(a->ui, D_FONT, &c, faces, a->doc.nfonts, 0, font_sample, a))
        failure(a, "The Font dialog could not be opened:", "", ENOMEM);
    else du_font_size_limits(a->ui, WP_SIZE_MIN / 2, WP_SIZE_MAX / 2);
}

/* Page Setup: paper, orientation and margins in inches. */
static int page_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct du_dialog *d = &u->dialog;
    int m[4];
    for (unsigned i = 0; i < 4; i++)
        if (parse_units(d->items[6 + i].text, a->units, &m[i]) || m[i] < 0) { snprintf(d->text, sizeof(d->text), "Enter the margins in %s, for example 0.75.", unit_names[a->units]); return 1; }
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
    struct wp_app *a = ctx; char t[40];
    label_at(u, 4, "Paper:", 0, -16);
    static const char *const names[] = {"Left:", "Right:", "Top:", "Bottom:"};
    for (unsigned i = 0; i < 4; i++) label_at(u, 6 + (int)i, names[i], -52, 4);
    snprintf(t, sizeof(t), "Margins (%s)", unit_names[a->units]); label_at(u, 6, t, -52, -20);
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
        char b[24]; format_units(b, sizeof(b), v[i], a->units);
        du_property_control(a->ui, 6 + i, DU_EDIT, 352 + (int)i, (struct du_rect){260, 56 + (int)i * 30, 90, 22}, b);
    }
}

/* Date and time: one list of formats, built from the user's regional settings (G24: strftime follows setlocale and the
 * Control Panel's Regional Settings; WordPad opts in with setlocale in main). */
static size_t tfmt(struct wp_app *a, char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    size_t n = a->format_time ? a->format_time(out, cap, fmt, tm) : strftime(out, cap, fmt, tm);
    if (!n && cap) out[0] = 0;
    return n;
}
static void date_formats(struct wp_app *a)
{
    time_t t = a->now(a->host);
    struct tm tm; if (!desk_localtime(&t, &tm)) memset(&tm, 0, sizeof(tm));
    a->tm = tm;
    char c[64], x[48];
    tfmt(a, a->date_text[0], sizeof(a->date_text[0]), "%x", &tm);                  /* the short date picture */
    tfmt(a, c, sizeof(c), "%c", &tm); tfmt(a, x, sizeof(x), "%X", &tm);            /* the long date picture followed by the time */
    size_t cl = strlen(c), xl = strlen(x);
    if (xl && cl > xl + 1 && !strcmp(c + cl - xl, x)) { c[cl - xl - 1] = 0; snprintf(a->date_text[1], sizeof(a->date_text[1]), "%s", c); }
    else tfmt(a, a->date_text[1], sizeof(a->date_text[1]), "%A, %B %d, %Y", &tm);
    tfmt(a, a->date_text[2], sizeof(a->date_text[2]), "%d %B %Y", &tm);
    tfmt(a, a->date_text[3], sizeof(a->date_text[3]), "%B %d, %Y", &tm);
    snprintf(a->date_text[4], sizeof(a->date_text[4]), "%s", x);                  /* the time picture */
    tfmt(a, a->date_text[5], sizeof(a->date_text[5]), "%H:%M", &tm);
    snprintf(a->date_text[6], sizeof(a->date_text[6]), "%s %s", a->date_text[0], x);
    for (unsigned i = 0; i < 7; i++) a->date_items[i] = (struct du_item){.text = {a->date_text[i]}};
}
static int date_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    unsigned at = u->dialog.items[4].active;
    if (at >= 7) return 0;
    char text[64]; snprintf(text, sizeof(text), "%s", a->date_text[at]);
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

/* ---- options and settings (View > Options) ---------------------------------------------------------------------------------- */

static const char *const format_label[WP_FORMATS] = {"Rich Text", "Text", "Word", "Write"};
static const char *const wrap_keys[WP_FORMATS] = {"wrap_rich", "wrap_text", "wrap_word", "wrap_write"};
static void settings_save(struct wp_app *a)
{
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "units=%d\n%s=%d\n%s=%d\n%s=%d\n%s=%d\ntoolbar=%d\nformatbar=%d\nruler=%d\nstatusbar=%d\n", a->units,
                     wrap_keys[0], a->wrap[0], wrap_keys[1], a->wrap[1], wrap_keys[2], a->wrap[2], wrap_keys[3], a->wrap[3],
                     a->show_toolbar, a->show_format, a->show_ruler, a->show_status);
    if (n > 0) (void)a->fs->save(a->fs->ctx, WP_SETTINGS, buf, (size_t)n);
}
static void settings_load(struct wp_app *a)
{
    char buf[512]; size_t n = 0;
    if (a->fs->load(a->fs->ctx, WP_SETTINGS, buf, sizeof(buf) - 1, &n) || !n) return;
    buf[n] = 0;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n'), *eq = strchr(line, '=');
        if (nl) *nl = 0;
        if (eq) {
            *eq = 0;
            int v = atoi(eq + 1);
            if (!strcmp(line, "units")) { if (v >= 0 && v < WP_UNITS) a->units = v; }
            else if (!strcmp(line, "toolbar")) a->show_toolbar = v != 0;
            else if (!strcmp(line, "formatbar")) a->show_format = v != 0;
            else if (!strcmp(line, "ruler")) a->show_ruler = v != 0;
            else if (!strcmp(line, "statusbar")) a->show_status = v != 0;
            else for (unsigned i = 0; i < WP_FORMATS; i++) if (!strcmp(line, wrap_keys[i]) && v >= 0 && v <= WP_WRAP_RULER) a->wrap[i] = v;
        }
        line = nl ? nl + 1 : NULL;
    }
}
static int options_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct du_dialog *d = &u->dialog;
    for (int i = 0; i < WP_UNITS; i++) if (d->items[4 + i].value) a->units = i;
    for (int i = 0; i < 3; i++) if (d->items[8 + i].value) a->wrap[a->format] = i;
    a->show_toolbar = d->items[11].value != 0; a->show_format = d->items[12].value != 0;
    a->show_ruler = d->items[13].value != 0; a->show_status = d->items[14].value != 0;
    du_dialog_close(u);
    settings_save(a);
    a->dirty_para = 0; a->lay.pcount = 0; a->left = 0;
    wp_layout_ui(a);
    reveal(a); invalidate_doc(a);
    return 0;
}
static void options_paint(struct du_ui *u, void *ctx)
{
    struct wp_app *a = ctx; char t[64];
    label_at(u, 4, "Measurement units:", 0, -18);
    snprintf(t, sizeof(t), "Word wrap, %s documents:", format_label[a->format]); label_at(u, 8, t, 0, -18);
    label_at(u, 11, "Show:", 0, -18);
}
static void options_dialog(struct wp_app *a)
{
    sheet_open(a, D_OPTIONS, "Options", 400, 300, "Options", options_accept, options_paint);
    static const char *const names[WP_UNITS] = {"&Inches", "&Centimeters", "&Points", "Pi&cas"};
    for (int i = 0; i < WP_UNITS; i++) {
        struct du_widget *w = du_property_control(a->ui, 4 + (unsigned)i, DU_RADIO, 400 + i, (struct du_rect){24, 60 + i * 24, 150, 22}, names[i]);
        w->total = 401; w->value = a->units == i;
    }
    static const char *const wraps[3] = {"Wrap to &window", "&No wrap", "Wrap to &ruler"};
    for (int i = 0; i < 3; i++) {
        struct du_widget *w = du_property_control(a->ui, 8 + (unsigned)i, DU_RADIO, 410 + i, (struct du_rect){204, 60 + i * 24, 170, 22}, wraps[i]);
        w->total = 411; w->value = a->wrap[a->format] == i;
    }
    static const char *const shows[4] = {"&Toolbar", "&Format bar", "&Ruler", "&Status bar"};
    const int on[4] = {a->show_toolbar, a->show_format, a->show_ruler, a->show_status};
    for (int i = 0; i < 4; i++) {
        struct du_widget *w = du_property_control(a->ui, 11 + (unsigned)i, DU_CHECKBOX, 420 + i, (struct du_rect){24 + (i % 2) * 170, 168 + (i / 2) * 24, 160, 22}, shows[i]);
        w->value = on[i];
    }
}

/* ---- pictures and objects (Insert menu) ------------------------------------------------------------------------------------- */

#define PIC_FILE_MAX (16u * 1024u * 1024u)
/* The natural size of a picture, scaled down to the printable width of the page when it would not fit. */
static void fit_to_page(const struct wp_app *a, struct wp_pic *p)
{
    int w, h; wp_pic_size(p, &w, &h);
    int pw = (a->page.landscape ? a->page.paper_h : a->page.paper_w) - a->page.left - a->page.right;
    if (pw > 720 && w > pw) { unsigned pct = (unsigned)((int64_t)pw * 100 / w); p->scalex = p->scaley = (uint16_t)(pct < 1 ? 1 : pct); }
}
/* A picture from the bytes of an image file: PNG and JPEG are kept as they are, BMP becomes a DIB, GIF and the rest are re-encoded as PNG. */
static int picture_from_bytes(struct wp_app *a, const uint8_t *data, size_t n, struct wp_pic **out)
{
    if (!a->images || !a->images->decode) return -ENOTSUP;
    uint32_t w0, h0, *px = NULL; unsigned w = 0, h = 0, kind = wp_pic_sniff(data, n, &w0, &h0);
    struct wp_pic *p = NULL;
    uint8_t *tmp = NULL; size_t tl = 0;
    int e;
    if (n >= 14 && data[0] == 'B' && data[1] == 'M') {
        tmp = wp_bmp_to_dib(data, n, &tl);
        if (!tmp) return -EINVAL;
        e = a->images->decode(WP_PIC_DIB, tmp, tl, &px, &w, &h);
        if (!e) p = wp_pic_new(WP_PIC_DIB, tmp, tl);
    } else if (kind == WP_PIC_PNG || kind == WP_PIC_JPEG) {
        e = a->images->decode(kind, data, n, &px, &w, &h);
        if (!e) p = wp_pic_new(kind, data, n);
    } else {   /* GIF and anything else the decoder knows: stored as PNG */
        e = a->images->decode(0, data, n, &px, &w, &h);
        if (!e && (!a->images->png || a->images->png(px, w, h, &tmp, &tl))) e = -ENOTSUP;
        if (!e) p = wp_pic_new(WP_PIC_PNG, tmp, tl);
    }
    free(tmp);
    if (e) { free(px); return e == -ENOTSUP ? -ENOTSUP : -EINVAL; }
    if (!p) { free(px); return -ENOMEM; }
    if (w0 && h0 && w0 <= 65535 && h0 <= 65535) { p->pw = w0; p->ph = h0; } else { p->pw = w; p->ph = h; }   /* the natural size is the file's, not that of the reduced pixels */
    p->px = px; p->pxw = w; p->pxh = h; p->px_state = WP_PX_OK;   /* already decoded: the first paint reuses it */
    fit_to_page(a, p);
    *out = p;
    return 0;
}
static int picture_from_file(struct wp_app *a, const char *typed, struct wp_pic **out)
{
    static char target[CN_PATH]; struct cn_stat st;
    int r = cn_resolve(target, a->dir[0] ? a->dir : "/", typed);
    if (r) return -EINVAL;
    r = a->fs->stat(a->fs->ctx, target, &st);
    if (r) return r;
    if (st.dir) return -EISDIR;
    char *raw = malloc((size_t)PIC_FILE_MAX + 1);
    size_t n = 0;
    if (!raw) return -ENOMEM;
    r = a->fs->load(a->fs->ctx, target, raw, (size_t)PIC_FILE_MAX + 1, &n);
    if (!r && n > PIC_FILE_MAX) r = -EFBIG;
    if (!r) r = picture_from_bytes(a, (const uint8_t *)raw, n, out);
    free(raw);
    return r;
}
static const char *picture_error(int r)
{
    return r == -ENOENT ? "The file was not found." : r == -EISDIR ? "That name is a folder." : r == -ENOTSUP ? "WordPad cannot read this kind of picture here." :
           r == -EFBIG ? "The picture is too large." : r == -ENOMEM ? "There is not enough memory for the picture." :
           "The file is not a picture WordPad can read (BMP, PNG, JPEG or GIF).";
}
static void insert_blank_bitmap(struct wp_app *a)
{
    enum { W = 256, H = 192 };
    size_t bytes = 40 + (size_t)W * 3 * H;
    uint8_t *dib = malloc(bytes);
    if (!dib) { memory_note(a, -ENOMEM); return; }
    memset(dib, 0xff, bytes);
    memset(dib, 0, 40);
    dib[0] = 40; dib[4] = W & 255; dib[5] = W >> 8; dib[8] = H & 255; dib[9] = H >> 8; dib[12] = 1; dib[14] = 24;
    struct wp_pic *p = wp_pic_new(WP_PIC_DIB, dib, bytes);
    free(dib);
    if (!p) { memory_note(a, -ENOMEM); return; }
    insert_picture(a, p);
}
static int object_accept(struct du_ui *u, int button, void *ctx)
{
    struct wp_app *a = ctx;
    if (button != 0) return 0;
    struct du_dialog *d = &u->dialog;
    if (!d->items[6].value && !d->items[7].text[0]) {   /* Create New: a blank Bitmap Image (a typed file name selects Create from File) */
        du_dialog_close(u);
        insert_blank_bitmap(a);
        return 0;
    }
    struct wp_pic *p = NULL;
    int r = d->items[7].text[0] ? picture_from_file(a, d->items[7].text, &p) : -ENOENT;
    if (r) { snprintf(d->text, sizeof(d->text), "%s", picture_error(r)); return 1; }
    du_dialog_close(u);
    insert_picture(a, p);
    return 0;
}
static void object_paint(struct du_ui *u, void *ctx)
{
    (void)ctx;
    label_at(u, 7, "File name of a BMP, PNG, JPEG or GIF picture:", 0, -16);
    label_at(u, 7, "The picture is inserted at the caret and kept in the document.", 0, 30);
}
static void object_dialog(struct wp_app *a)
{
    sheet_open(a, D_INSERTOBJ, "Insert Object", 400, 300, "Insert Object", object_accept, object_paint);
    struct du_widget *r1 = du_property_control(a->ui, 4, DU_RADIO, 430, (struct du_rect){24, 48, 160, 22}, "Create &New");
    r1->total = 431; r1->value = 1;
    a->object_items[0] = (struct du_item){.text = {"Bitmap Image"}};
    struct du_widget *l = du_property_control(a->ui, 5, DU_LISTVIEW, 432, (struct du_rect){40, 72, 330, 52}, "");
    l->items = a->object_items; l->count = 1; l->view = DU_DETAILS; l->column_count = 1; l->columns[0] = (struct du_column){"Object Type", 310, 0};
    l->inactive_selection = 1; l->selection_active = 1; l->active = 0;
    struct du_widget *r2 = du_property_control(a->ui, 6, DU_RADIO, 433, (struct du_rect){24, 140, 160, 22}, "Create from &File");
    r2->total = 431; r2->value = 0;
    du_property_control(a->ui, 7, DU_EDIT, 434, (struct du_rect){40, 188, 330, 22}, "");
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

/* The Help window (G15) when CiukiOS Help can be started; this box when it cannot. */
static void help_box(struct wp_app *a)
{
    box(a, D_INFO, "WordPad Help", DU_DLG_ICON_INFO, NULL, 0, 0, 0,
        "File: Ctrl+N New, Ctrl+O Open, Ctrl+S Save, Ctrl+P Print, Print Preview, Page Setup\n"
        "Edit: Ctrl+Z Undo, Ctrl+Y Redo, Ctrl+X/C/V, Ctrl+A, Ctrl+F Find, F3 Next, Ctrl+H Replace\n"
        "Format: Ctrl+B Bold, Ctrl+I Italic, Ctrl+U Underline, Ctrl+L Left, Ctrl+E Center,\n"
        "Ctrl+R Right, Ctrl+J Justify, Bullet Style, Paragraph, Tabs\n"
        "Insert: Date and Time, Picture, Object\n"
        "---\nFiles: Rich Text Format (.rtf), text (.txt), Word 6, 95 and 97 (.doc) and Write (.wri) up to 16 MiB.");
}
static void page_to_cp(const struct wp_page_setup *s, struct cp_page *p)
{
    memset(p, 0, sizeof(*p));
    p->width = (uint32_t)((s->paper_w + 10) / 20); p->height = (uint32_t)((s->paper_h + 10) / 20); p->landscape = s->landscape != 0;   /* twips to points, rounded: A4 is 595 x 842 */
    p->left = (uint32_t)((s->left + 10) / 20); p->right = (uint32_t)((s->right + 10) / 20); p->top = (uint32_t)((s->top + 10) / 20); p->bottom = (uint32_t)((s->bottom + 10) / 20);
    p->copies = 1;
}
static void print_document(struct wp_app *a)
{
    if (!a->print) { box(a, D_INFO, "WordPad", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "Printing is not available in this session."); return; }
    struct cp_page cp;
    page_to_cp(&a->page, &cp);
    a->print(a->host, &cp);
}
static void picture_dialog(struct wp_app *a)
{
    static char folder[CN_PATH];
    snprintf(folder, sizeof(folder), "%s", a->dir[0] ? a->dir : "/");
    snprintf(a->filter, sizeof(a->filter), "%s", PIC_FILTER);
    int r = du_file_dialog(a->ui, D_PICFILE, 0, "Insert Picture", folder, "", lister, a);
    if (r) { strcpy(a->filter, TEXT_FILTER); failure(a, "The folder could not be opened:", folder, r); }
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
        case M_PV_PRINT: case M_PRINT: print_document(a); return;
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
    case M_PRINT: print_document(a); break;
    case M_INSERTPIC: picture_dialog(a); break;
    case M_INSERTOBJ: object_dialog(a); break;
    case M_OPTIONS: options_dialog(a); break;
    case M_JUSTIFY: set_align(a, WP_JUSTIFY); break;
    case M_CONTEXTHELP: du_context_help(a->ui, 1); break;
    case M_PAGESETUP: page_dialog(a); break;
    case M_EXIT: guard(a, WP_AFTER_EXIT); break;
    case M_UNDO: undo_swap(a, 1); break;
    case M_REDO: undo_swap(a, 0); break;
    case M_CUT: if (has_selection(a) && !copy_selection(a)) { size_t lo, hi; sel_range(a, &lo, &hi); delete_range(a, lo, hi, WP_EDIT_OTHER); } break;
    case M_COPY: if (has_selection(a)) copy_selection(a); wp_refresh(a); break;
    case M_PASTE: paste(a); break;
    case M_CLEAR: { size_t lo, hi; sel_range(a, &lo, &hi); delete_range(a, lo, hi, WP_EDIT_OTHER); break; }
    case M_SELALL: a->anchor = 0; a->cursor = a->doc.len; a->pending_valid = 0; moved(a); break;
    case M_FIND: show_find(a, 1); break;
    case M_FINDNEXT: find_next(a); break;
    case M_REPLACE: show_find(a, 2); break;
    case M_TOOLBAR: a->show_toolbar = !a->show_toolbar; wp_layout_ui(a); wp_refresh(a); settings_save(a); break;
    case M_FORMATBAR: a->show_format = !a->show_format; wp_layout_ui(a); wp_refresh(a); settings_save(a); break;
    case M_RULER: a->show_ruler = !a->show_ruler; wp_layout_ui(a); wp_refresh(a); settings_save(a); break;
    case M_STATUS: a->show_status = !a->show_status; wp_layout_ui(a); wp_refresh(a); settings_save(a); break;
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
    case M_HELP: if (!du_help(a->ui, NULL)) break; help_box(a); break;   /* G15: the WordPad topics in CiukiOS Help */
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
    case D_FONT:
        if (!index) {
            struct du_font_choice c;
            if (!du_font_parse(text, &c)) {
                int font = wp_font_find(&a->doc, c.family);
                if (font < 0) font = wp_font_add(&a->doc, c.family, WP_SANS);
                if (font < 0) { failure(a, "The font table is full:", c.family, ENOMEM); break; }
                unsigned flags = (c.style & DU_FONT_BOLD ? WP_BOLD : 0) | (c.style & DU_FONT_ITALIC ? WP_ITALIC : 0) |
                                 (c.underline ? WP_UNDERLINE : 0) | (c.strikeout ? WP_STRIKE : 0);
                struct wp_cstyle st = {font, (int)c.points * 2, wp_color_add(&a->doc, c.color), flags,
                    (WP_BOLD | WP_ITALIC | WP_UNDERLINE | WP_STRIKE) & ~flags};
                apply_chars(a, &st);
            }
        }
        break;
    case D_CHANGES:
        if (index == 0) {
            if (!a->named || imported(a)) { file_dialog(a, 1, ""); return; }
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
    case D_PICFILE:
        strcpy(a->filter, TEXT_FILTER);
        if (index == 0) {
            struct wp_pic *p = NULL;
            int r = picture_from_file(a, text, &p);
            if (r) { char shown[48]; shorten(shown, sizeof(shown), cn_basename(text), 44); box(a, D_INFO, "WordPad", DU_DLG_ICON_ERROR, NULL, 0, 0, 0, "%s\n%s", picture_error(r), shown); }
            else {
                static char folder[CN_PATH]; folder_of(folder, sizeof(folder), text);
                snprintf(a->dir, sizeof(a->dir), "%s", folder);
                insert_picture(a, p);
            }
        }
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

/* ---- printing (G31) -------------------------------------------------------------------------------------------------------- */

/* The page layout, line by line, as drawing operations for the spooler: the same atoms the screen and the print preview use
 * (wp_layout_pieces), so what prints is what the preview showed. Coordinates are twips from the top left of the paper. */
struct pb {
    struct cp_op *ops; unsigned n, cap;
    char *text; size_t tn, tcap;
    struct cp_image *imgs; const struct wp_pic **src; unsigned ni, ci;
    int err;
};
static struct cp_op *pb_op(struct pb *b)
{
    if (b->err) return NULL;
    if (b->n == b->cap) {
        unsigned c = b->cap ? b->cap * 2 : 256;
        struct cp_op *q = realloc(b->ops, c * sizeof(*q));
        if (!q) { b->err = -ENOMEM; return NULL; }
        b->ops = q; b->cap = c;
    }
    memset(&b->ops[b->n], 0, sizeof(*b->ops));
    return &b->ops[b->n++];
}
static int pb_text(struct pb *b, const char *s, size_t n, uint32_t *offset)
{
    if (b->tn + n + 1 > b->tcap) {
        size_t c = b->tcap ? b->tcap * 2 : 8192;
        while (c < b->tn + n + 1) c *= 2;
        char *q = realloc(b->text, c);
        if (!q) return b->err = -ENOMEM;
        b->text = q; b->tcap = c;
    }
    memcpy(b->text + b->tn, s, n);
    *offset = (uint32_t)b->tn; b->tn += n;
    return 0;
}
static void pb_rule(struct pb *b, unsigned page, int x, int y, int w, int h, uint32_t rgb)
{
    struct cp_op *o = pb_op(b);
    if (!o) return;
    o->kind = CP_OP_RULE; o->page = page; o->x = x; o->y = y; o->w = w; o->h = h; o->rgb = rgb;
}
struct print_walk { struct wp_app *a; struct pb *b; unsigned page; int ox, oy, base; };
static void pb_text_op(struct print_walk *w, const struct wp_cattr *at, int x, int baseline, const char *s, size_t n)
{
    const struct wp_doc *d = &w->a->doc;
    struct cp_op *o = pb_op(w->b);
    if (!o) return;
    o->kind = CP_OP_TEXT; o->page = w->page; o->x = x; o->y = baseline;
    o->family = (uint8_t)(at->font < d->nfonts ? d->fonts[at->font].family : WP_SANS);
    o->flags = (uint8_t)((at->flags & WP_BOLD ? 1 : 0) | (at->flags & WP_ITALIC ? 2 : 0));
    o->size = at->size; o->rgb = piece_colour(d, at); o->length = (uint32_t)n;
    pb_text(w->b, s, n, &o->offset);
}
/* Registers the pixels of a picture for the page description, reduced to about 300 dpi; returns the image index or -1. */
static int pb_image(struct print_walk *w, const struct wp_pic *p, int wt)
{
    struct pb *b = w->b;
    for (unsigned i = 0; i < b->ni; i++) if (b->src[i] == p) return (int)i;
    unsigned step = 1, target = (unsigned)((int64_t)wt * 300 / 1440) + 1;
    while (p->pxw / (step + 1) >= target) step++;
    unsigned iw = p->pxw / step, ih = p->pxh / step;
    if (!iw || !ih) return -1;
    if (b->ni == b->ci) {
        unsigned c = b->ci ? b->ci * 2 : 8;
        struct cp_image *q = realloc(b->imgs, c * sizeof(*q));
        const struct wp_pic **r = q ? realloc((void *)b->src, c * sizeof(*r)) : NULL;
        if (q) b->imgs = q;
        if (!q || !r) { b->err = -ENOMEM; return -1; }
        b->src = r; b->ci = c;
    }
    uint32_t *px = malloc((size_t)iw * ih * sizeof(*px));
    if (!px) { b->err = -ENOMEM; return -1; }
    for (unsigned y = 0; y < ih; y++) for (unsigned x = 0; x < iw; x++) px[(size_t)y * iw + x] = p->px[(size_t)(y * step) * p->pxw + x * step];
    b->imgs[b->ni] = (struct cp_image){px, iw, ih}; b->src[b->ni] = p;
    return (int)b->ni++;
}
static int print_piece(void *ctx, const struct wp_piece *pc)
{
    struct print_walk *w = ctx; struct wp_app *a = w->a; const struct wp_doc *d = &a->doc;
    int x = w->ox + pc->x * 15, by = w->oy + w->base * 15;
    if (w->b->err) return 1;
    if (pc->kind == WP_PIECE_PIC) {
        struct wp_pic *pic = wp_pic_at(d, pc->start);
        int pw, ph; wp_layout_pic_size(d, &pc->a, &pw, &ph);
        const struct wp_pic *ok = pic ? pic_ready(a, pic) : NULL;
        int idx = ok ? pb_image(w, ok, pw * 15) : -1;
        if (idx >= 0) {
            struct cp_op *o = pb_op(w->b);
            if (o) { o->kind = CP_OP_IMAGE; o->page = w->page; o->x = x; o->y = by - ph * 15; o->w = pw * 15; o->h = ph * 15; o->offset = (uint32_t)idx; }
        } else if (pic) {   /* a picture that cannot be decoded prints as its frame */
            pb_rule(w->b, w->page, x, by - ph * 15, pw * 15, 15, 0x808080); pb_rule(w->b, w->page, x, by - 15, pw * 15, 15, 0x808080);
            pb_rule(w->b, w->page, x, by - ph * 15, 15, ph * 15, 0x808080); pb_rule(w->b, w->page, x + pw * 15 - 15, by - ph * 15, 15, ph * 15, 0x808080);
        }
        return 0;
    }
    if (pc->kind == WP_PIECE_TEXT) pb_text_op(w, &pc->a, x, by, d->text + pc->start, pc->end - pc->start);
    int asc, desc; tops(a)->vmetrics(tops(a)->ctx, WP_SANS, 0, pc->a.size, &asc, &desc);
    uint32_t rgb = piece_colour(d, &pc->a);
    if (pc->a.flags & WP_UNDERLINE) pb_rule(w->b, w->page, x, by + 15, pc->w * 15, 15, rgb);
    if (pc->a.flags & WP_STRIKE) pb_rule(w->b, w->page, x, by - asc * 5, pc->w * 15, 15, rgb);
    return 0;
}
int wp_print_prepare(void *app, const struct cp_page *page, unsigned driver, struct cp_doc *out)
{
    struct wp_app *a = app;
    memset(out, 0, sizeof(*out));
    int e = wp_text_write(&a->doc, &out->plain, &out->plain_bytes, 1);
    if (e) return e;
    out->pages = 1;
    if (driver == CP_TEXT) return 0;   /* a text printer prints the characters only */
    struct wp_page_setup setup = {(int)page->width * 20, (int)page->height * 20, (int)page->left * 20, (int)page->right * 20,
                                  (int)page->top * 20, (int)page->bottom * 20, page->landscape != 0};
    struct wp_pages pages = {0}; struct wp_layout lay;
    wp_layout_init(&lay);
    e = wp_paginate(&pages, &lay, &a->doc, tops(a), &setup);
    if (e) { wp_layout_free(&lay); cp_doc_free(out); return e; }
    struct pb b; memset(&b, 0, sizeof(b));
    for (unsigned pg = 0; pg < pages.count && !b.err; pg++) {
        unsigned first = pages.first_line[pg], last = pg + 1 < pages.count ? pages.first_line[pg + 1] : lay.n;
        int top = lay.n ? lay.lines[first].y : 0;
        for (unsigned i = first; i < last && !b.err; i++) {
            const struct wp_line *ln = &lay.lines[i];
            struct print_walk w = {a, &b, pg + 1, setup.left, setup.top + (ln->y - top) * 15, ln->base};
            if (ln->flags & WP_LINE_BULLET) {
                struct wp_cattr at = wp_attr_of(&a->doc, ln->start);
                pb_text_op(&w, &at, setup.left + wp_layout_bullet_x(&a->doc, ln) * 15, w.oy + ln->base * 15, "\xe2\x80\xa2", 3);
            }
            wp_layout_pieces(&lay, &a->doc, tops(a), i, print_piece, &w);
        }
    }
    unsigned count = pages.count;
    wp_pages_free(&pages); wp_layout_free(&lay);
    free((void *)b.src);
    if (b.err) { free(b.ops); free(b.text); for (unsigned i = 0; i < b.ni; i++) free(b.imgs[i].pixels); free(b.imgs); cp_doc_free(out); return b.err; }
    out->ops = b.ops; out->nops = b.n; out->text = b.text; out->text_bytes = b.tn; out->images = b.imgs; out->nimages = b.ni; out->pages = count;
    return 0;
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
        case K_J: command(a, M_JUSTIFY); return;
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
{ return wp_layout_hit(&a->lay, &a->doc, tops(a), x - a->doc_box.x - PAD + a->left, y - a->doc_box.y + a->top); }
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
            unsigned disabled = (rows[i].id == M_CUT || rows[i].id == M_COPY) && !sel ? DU_ITEM_DISABLED : rows[i].id == M_PASTE && !clipboard_text()[0] ? DU_ITEM_DISABLED : 0;
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

/* Dragging a ruler marker changes the indents of the selected paragraphs; a click on an empty part of the ruler adds a tab stop,
 * and a tab stop dragged off the ruler is removed (as WordPad). One undo level covers the whole drag. */
static void ruler_apply(struct wp_app *a)
{
    size_t lo, hi; sel_range(a, &lo, &hi);
    const struct wp_pattr *o = &a->rdrag.orig;
    struct wp_pstyle st = {0, *o};
    int pos = a->rdrag.pos;
    switch (a->rdrag.kind) {
    case R_FIRST: st.mask = WP_P_FIRST; st.v.first = pos - o->left; break;
    case R_LEFT: st.mask = WP_P_LEFT | WP_P_FIRST; st.v.left = pos; st.v.first = o->first; break;
    case R_RIGHT: st.mask = WP_P_RIGHT; st.v.right = snap_twips(a, ruler_width_twips(a) - pos); if (st.v.right < 0) st.v.right = 0; break;   /* the margin snaps, not the position */
    case R_TAB: {
        st.mask = WP_P_TABS; st.v.ntabs = 0;
        int list[WP_TABS + 1], n = 0;
        for (unsigned t = 0; t < o->ntabs; t++) if ((int)t != a->rdrag.index) list[n++] = o->tabs[t];
        if (pos > 0 && n <= WP_TABS) list[n++] = pos;   /* a negative position: the tab was dragged away */
        for (int i = 1; i < n; i++) for (int k = i; k > 0 && list[k - 1] > list[k]; k--) { int t = list[k]; list[k] = list[k - 1]; list[k - 1] = t; }
        for (int i = 0; i < n && st.v.ntabs < WP_TABS; i++) if (!st.v.ntabs || list[i] > st.v.tabs[st.v.ntabs - 1]) st.v.tabs[st.v.ntabs++] = list[i];
        break;
    }
    default: return;
    }
    mark_dirty(a, lo);
    wp_style_paras(&a->doc, lo, hi, &st);
    changed(a);
}
static void handle_ruler_pointer(struct wp_app *a, const struct du_event *e)
{
    if (a->preview) return;
    int pos = snap_twips(a, twips_of_px(e->x - ruler_x0(a)));
    if (pos < 0) pos = 0;
    if (pos > WP_INDENT_MAX) pos = WP_INDENT_MAX;
    if (e->index == DU_POINTER_DOWN && e->key == 1) {
        int tab = -1, k = ruler_hit(a, e->x, e->y, &tab);
        struct wp_pattr pa = wp_pattr_at(&a->doc, a->cursor);
        if (k == R_NONE) {   /* a new tab stop at the click */
            if (pa.ntabs >= WP_TABS || pos <= 0 || e->x < ruler_x0(a)) return;
            for (unsigned t = 0; t < pa.ntabs; t++) if (pa.tabs[t] == pos) return;
            k = R_TAB; tab = -1;
        }
        edit_begin(a, WP_EDIT_OTHER);
        a->last_edit = WP_EDIT_OTHER;
        a->rdrag.kind = k; a->rdrag.index = tab; a->rdrag.orig = pa; a->rdrag.active = 1;
        a->rdrag.pos = k == R_FIRST ? pa.left + pa.first : k == R_LEFT ? pa.left : k == R_RIGHT ? ruler_width_twips(a) - pa.right : tab >= 0 ? pa.tabs[tab] : pos;
        if (k == R_TAB && tab < 0) a->rdrag.pos = pos;
        ruler_apply(a);
        return;
    }
    if (!a->rdrag.active) return;
    if (e->index == DU_POINTER_CANCEL) { a->rdrag.active = 0; invalidate_doc(a); return; }
    if ((e->index == DU_POINTER_MOVE || e->index == DU_POINTER_UP) && e->key == 1) {
        int away = e->y < a->ruler_box.y - 12 || e->y > a->ruler_box.y + a->ruler_box.h + 12;
        a->rdrag.pos = a->rdrag.kind == R_TAB && away ? -1 : pos;
        ruler_apply(a);
        if (e->index == DU_POINTER_UP) { a->rdrag.active = 0; invalidate_doc(a); wp_refresh(a); }
        else invalidate_doc(a);
    }
    if (e->index == DU_POINTER_UP) a->rdrag.active = 0;
}

/* Enter in the editable font or size box: a known font name or a size in points (half points allowed) is applied;
 * anything else restores the box from the selection. */
static void format_combo_commit(struct wp_app *a, int id)
{
    struct du_widget *w = W(a, id);
    if (!w) return;
    if (id == W_FONTBTN) {
        for (unsigned i = 0; i < a->doc.nfonts; i++)
            if (!strcasecmp(a->doc.fonts[i].name, w->text)) { struct wp_cstyle st = {(int)i, 0, -1, 0, 0}; apply_chars(a, &st); return; }
    } else {
        const char *p = w->text; int whole = 0, digits = 0, half = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) { whole = whole * 10 + (*p++ - '0'); digits++; }
        if (digits && p[0] == '.' && (p[1] == '5' || p[1] == '0') && !p[2]) { half = p[1] == '5'; p += 2; }
        int hp = whole * 2 + half;
        if (digits && !*p && hp >= WP_SIZE_MIN && hp <= WP_SIZE_MAX) { struct wp_cstyle st = {-1, hp, -1, 0, 0}; apply_chars(a, &st); return; }
    }
    w->text[0] = 0;   /* refreshed from the selection by format_state */
}

void wp_event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct wp_app *a = ctx; int id = e->id;
    switch (e->type) {
    case DU_EV_CHANGE:
        if (id == W_SCROLL) { a->top = W(a, W_SCROLL)->value; invalidate_doc(a); }
        else if (id == W_HSCROLL) { a->left = W(a, W_HSCROLL)->value; invalidate_doc(a); }
        return;
    case DU_EV_POINTER:
        if (id == W_DOC) handle_pointer(a, e);
        else if (id == W_RULER) handle_ruler_pointer(a, e);
        return;
    case DU_EV_KEY:
        if (e->key == DU_KEY_ESC && a->find_mode && id >= W_FLABEL && id <= W_RALL) { hide_find(a); return; }
        if (id == W_DOC || id == 0 || a->preview) handle_key(a, e);
        return;
    case DU_EV_TICK:
        if (e->id == 1) { a->caret_on = !a->caret_on; if (!has_selection(a) && !a->preview) {
            int x, y, h; caret_xy(a, a->cursor, &x, &y, &h);
            du_invalidate(u, du_intersection(a->doc_box, (struct du_rect){a->doc_box.x + PAD - a->left + x - 1, a->doc_box.y - a->top + y, 3, h})); } }
        return;
    case DU_EV_SELECT:
        if (id == W_FONTBTN && e->index >= 0 && (unsigned)e->index < a->doc.nfonts) { struct wp_cstyle st = {e->index, 0, -1, 0, 0}; apply_chars(a, &st); }
        else if (id == W_SIZEBTN && e->index >= 0 && (unsigned)e->index < sizeof(sizes_hp) / sizeof(*sizes_hp)) { struct wp_cstyle st = {-1, (int)sizes_hp[e->index], -1, 0, 0}; apply_chars(a, &st); }
        else return;
        du_focus(u, W_DOC);
        break;
    case DU_EV_COMMIT:
        if (id == W_FONTBTN || id == W_SIZEBTN) { format_combo_commit(a, id); du_focus(u, W_DOC); break; }
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

static void add_tool(struct wp_app *a, int id, const char *object, unsigned size, int dropdown, const char *tip, const char *help)
{
    struct du_widget *b = du_add(a->ui, DU_TOOLBUTTON, id, (struct du_rect){0}, "");
    if (!b) return;
    b->icon = object ? icon(object, size) : NULL; b->icon_size = (int)size; b->dropdown = dropdown;
    du_set_tip(b, tip); b->help = help;   /* G15: the tooltip and the What's This? text */
    if (!b->icon) b->icon_size = 0;
}
int wp_attach(struct wp_app *a, struct du_ui *u)
{
    a->ui = u; u->event = wp_event; u->user = a; u->layout = resize_layout; u->paint = paint_hook;
    if (wp_init(&a->doc) || wp_init(&a->clip)) return -ENOMEM;
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
    static const struct { int id; const char *tip, *help; } toolbar_tips[] = {
        {M_NEW, "New", "Creates a new, empty document."}, {M_OPEN, "Open", "Opens a Rich Text Format, Word, Write or text document."},
        {M_SAVE, "Save", "Saves the document under its name."}, {M_PRINT, "Print", "Prints the document."},
        {M_PREVIEW, "Print Preview", "Shows the pages as they will print."}, {M_FIND, "Find", "Looks for text in the document."},
        {M_CUT, "Cut", "Moves the selection to the Clipboard."}, {M_COPY, "Copy", "Copies the selection to the Clipboard."},
        {M_PASTE, "Paste", "Inserts the contents of the Clipboard at the caret."}, {M_UNDO, "Undo", "Takes back the last change; repeat to go back further."},
        {M_DATETIME, "Date/Time", "Inserts the date or the time at the caret."}};
    for (unsigned i = 0; i < sizeof(toolbar_buttons) / sizeof(*toolbar_buttons); i++) {
        if (toolbar_buttons[i].icon) {
            const char *tip = NULL, *help = NULL;
            for (unsigned k = 0; k < sizeof(toolbar_tips) / sizeof(*toolbar_tips); k++) if (toolbar_tips[k].id == toolbar_buttons[i].id) { tip = toolbar_tips[k].tip; help = toolbar_tips[k].help; }
            add_tool(a, toolbar_buttons[i].id, toolbar_buttons[i].icon, 22, 0, tip, help);
        } else du_add(u, DU_SEPARATOR, toolbar_buttons[i].id, zero, "");
    }
    du_add(u, DU_TOOLBAR, W_FORMATBAR, zero, "");
    /* Win98 WordPad: the font name and size are editable combo boxes with drop-down lists. */
    struct du_widget *f = du_add(u, DU_COMBO, W_FONTBTN, zero, ""); if (f) { f->editable = 1; du_set_tip(f, "Font"); f->help = "Changes the font of the selected text."; }
    struct du_widget *sz = du_add(u, DU_COMBO, W_SIZEBTN, zero, ""); if (sz) { sz->editable = 1; du_set_tip(sz, "Font Size"); sz->help = "Changes the size of the selected text."; }
    add_tool(a, W_COLORBTN, "preferences-desktop-theme", 22, 0, "Color", "Changes the color of the selected text.");
    du_add(u, DU_SEPARATOR, W_SEP1 + 10, zero, ""); du_add(u, DU_SEPARATOR, W_SEP1 + 11, zero, "");
    /* B, I and U are drawn as letters by paint_hook (bold, slanted, underlined), not as an icon. */
    add_tool(a, M_BOLD, NULL, 22, 0, "Bold", "Makes the selected text bold, or turns bold off."); add_tool(a, M_ITALIC, NULL, 22, 0, "Italic", "Makes the selected text italic, or turns italic off.");
    add_tool(a, M_UNDERLINE, NULL, 22, 0, "Underline", "Underlines the selected text, or removes the underline.");
    add_tool(a, M_LEFT, "action:align-left", 22, 0, "Align Left", "Aligns the paragraph with the left margin."); add_tool(a, M_CENTER, "action:align-center", 22, 0, "Center", "Centers the paragraph between the margins.");
    add_tool(a, M_RIGHT, "action:align-right", 22, 0, "Align Right", "Aligns the paragraph with the right margin.");
    add_tool(a, M_BULLET, "format-indent-more", 22, 0, "Bullets", "Starts or ends a bulleted list.");
    du_add(u, DU_TOOLBAR, W_PVBAR, zero, "");
    static const struct { int id; const char *label; } pv[] = {{M_PV_PRINT, "&Print..."}, {M_PV_PREV, "Pre&vious"}, {M_PV_NEXT, "&Next Page"}, {M_PV_ZOOMOUT, "Zoom &Out"}, {M_PV_ZOOMIN, "Zoom &In"}, {M_PV_CLOSE, "&Close"}};
    for (unsigned i = 0; i < 6; i++) du_add(u, DU_BUTTON, pv[i].id, zero, pv[i].label);
    struct du_widget *doc = du_add(u, DU_CANVAS, W_DOC, zero, "");
    if (!doc) return -ENOMEM;
    doc->accepts_tab = 1; doc->wheel_events = 1; doc->pointer_cursor = DESK_CURSOR_TEXT;
    du_set_tip(doc, NULL); doc->help = "Type your document here. Use the Format menu or the format bar to change how it looks.";
    struct du_widget *ruler = du_add(u, DU_CANVAS, W_RULER, zero, "");
    if (ruler) { du_set_tip(ruler, NULL); ruler->help = "Drag the markers to change the indents of the paragraph; click to add a tab stop and drag it off the ruler to remove it."; }
    struct du_widget *sb = du_add(u, DU_SCROLLBAR, W_SCROLL, zero, ""); if (sb) sb->vertical = 1;
    struct du_widget *hb = du_add(u, DU_SCROLLBAR, W_HSCROLL, zero, ""); if (hb) { hb->vertical = 0; hb->hidden = 1; }
    du_add(u, DU_STATUS, W_STATUS, zero, "");
    du_add(u, DU_LABEL, W_FLABEL, zero, "Find &what:"); du_add(u, DU_EDIT, W_FIND, zero, "");
    du_add(u, DU_BUTTON, W_FNEXT, zero, "Find &Next"); du_add(u, DU_BUTTON, W_FDIR, zero, "&Down");
    du_add(u, DU_BUTTON, W_FCASE, zero, "[ ] Match &case"); du_add(u, DU_BUTTON, W_FCLOSE, zero, "X");
    du_add(u, DU_LABEL, W_RLABEL, zero, "Replace &with:"); du_add(u, DU_EDIT, W_REPL, zero, "");
    du_add(u, DU_BUTTON, W_RONE, zero, "&Replace"); du_add(u, DU_BUTTON, W_RALL, zero, "Replace &All");
    du_timer_start(u, 1, 530000000ull, 1);
    settings_load(a);
    wp_layout_ui(a);
    du_focus(u, W_DOC);
    relayout(a);
    wp_refresh(a);
    return 0;
}
void wp_detach(struct wp_app *a)
{
    wp_free(&a->doc); undo_forget(a); wp_free(&a->clip); free(a->clip_plain); a->clip_plain = NULL;
    wp_layout_free(&a->lay); wp_layout_free(&a->plo); wp_pages_free(&a->pages);
}
