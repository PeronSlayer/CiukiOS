/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "../desktop/shell_icons.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_N = 0x31, K_O = 0x18, K_S = 0x1f, K_Z = 0x2c, K_X = 0x2d, K_C = 0x2e, K_V = 0x2f, K_A = 0x1e,
       K_F = 0x21, K_H = 0x23, K_G = 0x22, K_E = 0x12, K_F3 = 0x3d, K_F5 = 0x3f, K_DEL = 0x153 };
_Static_assert(CN_LIMIT == 61440u, "CN_LIMIT_TEXT must name CN_LIMIT");
#define LIST_ROWS 9
#define DOUBLE_CLICK_NS 400000000ull

static void show_page(struct cn_app *);
static void begin_page(struct cn_app *, enum cn_page);

/* ---- small helpers ------------------------------------------------------ */

static void info(struct cn_app *a, const char *title, const char *fmt, ...)
{
    char message[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    du_dialog_show(a->ui, DU_MESSAGE, D_INFO, title, message, NULL);
    const char *ok[] = {"OK"}; du_dialog_buttons(a->ui, ok, 1);
}
/* Copies name, shortened to at most `max` cells with "..." (dialog lines hold 54 cells). */
static void shorten(char *out, size_t size, const char *name, size_t max)
{
    snprintf(out, size, "%s", name);
    if (strlen(name) > max && max >= 3 && max < size) strcpy(out + max - 3, "...");
}
static void failure(struct cn_app *a, const char *what, const char *name, int error)
{
    char shown[48]; shorten(shown, sizeof(shown), name, 44);
    if (error < 0) error = -error;
    info(a, "CiukiOS CiukNote", "%s\n%s.\n\n%s (error %d)", what, shown, strerror(error), error);
}
static const char *document_name(const struct cn_app *a) { return a->named ? cn_basename(a->path) : "Untitled"; }
static struct du_widget *widget(struct cn_app *a, int id) { return du_get(a->ui, id); }
static void set_text(struct cn_app *a, int id, const char *text)
{
    struct du_widget *w = widget(a, id);
    snprintf(w->text, sizeof(w->text), "%s", text);
    w->cursor = strlen(w->text); w->anchor = 0; w->offset = 0; a->ui->dirty = 1;
}
static const char *text_of(struct cn_app *a, int id) { return widget(a, id)->text; }
static void select_all_text(struct cn_app *a, int id)
{ struct du_widget *w = widget(a, id); w->anchor = 0; w->cursor = strlen(w->text); w->offset = 0; }
static struct du_rect text_box(struct cn_app *a) { return widget(a, W_TEXT)->box; }

uint32_t cn_digest(const struct du_ui *ui)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)ui->canvas.width * ui->canvas.height; i++)
        for (unsigned s = 0; s < 32; s += 8) h = (h ^ ((ui->canvas.pixels[i] >> s) & 255u)) * 16777619u;
    return h;
}
int cn_checkpoint(struct cn_app *a, char *line, size_t cap)
{
    if (!a->checkpoint || a->ui->dirty) return 0;
    static const char *const names[] = {"", "ready", "saved", "opened"};
    snprintf(line, cap, "CiukNote checkpoint=%u event=%s path=%s text=%u digest=%08x",
             (unsigned)a->checkpoint, names[a->checkpoint], a->named ? a->path : "-", (unsigned)a->ta.len, cn_digest(a->ui));
    a->checkpoint = CN_CP_NONE;
    return 1;
}

/* ---- menus -------------------------------------------------------------- */

#define MENU_WIDTH 26  /* cells inside the 224 pixel drop-down */
static char *item(struct cn_app *a, struct du_menu_item *slot, const char *label, const char *accel, int id,
                 unsigned key, unsigned modifiers, int checkable)
{
    char *buf = a->labels[a->label_count++];
    int width = MENU_WIDTH - (checkable ? 2 : 0) - (int)strlen(accel);
    snprintf(buf, sizeof(a->labels[0]), "%s%-*s%s", checkable ? "\002" : "", width, label, accel);
    *slot = (struct du_menu_item){buf, id, key, modifiers, 0};
    return buf;
}
static void build_menus(struct cn_app *a)
{
    a->label_count = 0;
    item(a, &a->file_items[0], "New", "Ctrl+N", M_NEW, K_N, DU_CTRL, 0);
    item(a, &a->file_items[1], "Open...", "Ctrl+O", M_OPEN, K_O, DU_CTRL, 0);
    item(a, &a->file_items[2], "Save", "Ctrl+S", M_SAVE, K_S, DU_CTRL, 0);
    item(a, &a->file_items[3], "Save As...", "", M_SAVEAS, 0, 0, 0);
    item(a, &a->file_items[4], "Exit", "", M_EXIT, 0, 0, 0);
    item(a, &a->edit_items[0], "Undo", "Ctrl+Z", M_UNDO, K_Z, DU_CTRL, 0);
    item(a, &a->edit_items[1], "Cut", "Ctrl+X", M_CUT, K_X, DU_CTRL, 0);
    item(a, &a->edit_items[2], "Copy", "Ctrl+C", M_COPY, K_C, DU_CTRL, 0);
    item(a, &a->edit_items[3], "Paste", "Ctrl+V", M_PASTE, K_V, DU_CTRL, 0);
    item(a, &a->edit_items[4], "Delete", "Del", M_DELETE, K_DEL, 0, 0);
    item(a, &a->edit_items[5], "Find...", "Ctrl+F", M_FIND, K_F, DU_CTRL, 0);
    item(a, &a->edit_items[6], "Find Next", "F3", M_FINDNEXT, K_F3, 0, 0);
    item(a, &a->edit_items[7], "Replace...", "Ctrl+H", M_REPLACE, K_H, DU_CTRL, 0);
    item(a, &a->edit_items[8], "Go To...", "Ctrl+G", M_GOTO, K_G, DU_CTRL, 0);
    item(a, &a->edit_items[9], "Select All", "Ctrl+A", M_SELALL, K_A, DU_CTRL, 0);
    item(a, &a->edit_items[10], "Time/Date", "F5", M_TIME, K_F5, 0, 0);
    a->wrap_label = item(a, &a->format_items[0], "Word Wrap", "", M_WRAP, 0, 0, 1);
    item(a, &a->format_items[1], "Font...", "", M_FONT, 0, 0, 0);
    a->status_label = item(a, &a->view_items[0], "Status Bar", "", M_STATUS, 0, 0, 1);
    item(a, &a->help_items[0], "Help Topics", "", M_HELP, 0, 0, 0);
    item(a, &a->help_items[1], "About CiukNote", "", M_ABOUT, 0, 0, 0);
    static const struct { const char *label; int id; } context[] = {
        {"Undo", M_UNDO}, {"Cut", M_CUT}, {"Copy", M_COPY}, {"Paste", M_PASTE}, {"Delete", M_DELETE}, {"Select All", M_SELALL}};
    for (unsigned i = 0; i < 6; i++) a->context_items[i] = (struct du_menu_item){context[i].label, context[i].id, 0, 0, 0};
    a->menus[0] = (struct du_menu){"File", a->file_items, 5};
    a->menus[1] = (struct du_menu){"Edit", a->edit_items, 11};
    a->menus[2] = (struct du_menu){"Format", a->format_items, 2};
    a->menus[3] = (struct du_menu){"View", a->view_items, 1};
    a->menus[4] = (struct du_menu){"Help", a->help_items, 2};
    static const unsigned keys[5] = {0x21, K_E, K_O, 0x2f, K_H};  /* Alt+F, E, O, V, H */
    memcpy(a->menu_keys, keys, sizeof(keys));
}
static int paste_available(const struct cn_app *a) { return a->clip_len || du_clipboard_get()[0]; }
void cn_refresh(struct cn_app *a)
{
    struct du_ui *u = a->ui; unsigned line, col;
    du_ta_line_col(&a->ta, a->ta.cursor, &line, &col);
    char text[160];
    snprintf(text, sizeof(text), "Ln %u, Col %u", line, col); set_text(a, W_POS, text);
    snprintf(text, sizeof(text), "%s%s%s%s", document_name(a), a->ta.modified ? " *" : "", a->note[0] ? "   " : "", a->note);
    set_text(a, W_NAME, text);
    int selection = du_ta_has_selection(&a->ta);
    a->edit_items[0].disabled = !du_ta_can_undo(&a->ta);
    a->edit_items[1].disabled = a->edit_items[2].disabled = a->edit_items[4].disabled = !selection;
    a->edit_items[3].disabled = !paste_available(a);
    a->edit_items[8].disabled = a->ta.wrap;                 /* Go To is off with Word Wrap */
    a->wrap_label[0] = a->ta.wrap ? '\001' : '\002';     /* \001 checked, \002 unchecked */
    a->status_label[0] = a->show_status ? '\001' : '\002';
    for (unsigned i = 0; i < 5; i++) a->context_items[i].disabled = i < 4 ? a->edit_items[i].disabled : a->edit_items[4].disabled;
    a->context_items[5].disabled = 0;
    u->dirty = 1;
}

/* ---- layout and pages --------------------------------------------------- */

static void layout(struct cn_app *a)
{
    int bar_h = a->find_mode == 2 ? 62 : a->find_mode == 1 ? 32 : 0, status_h = a->show_status ? 24 : 0;
    int bar_y = CN_HEIGHT - status_h - bar_h, y = bar_y;
    widget(a, W_TEXT)->box = (struct du_rect){0, 24, CN_WIDTH, bar_y - 24};
    widget(a, W_NAME)->box = (struct du_rect){2, CN_HEIGHT - 23, 446, 22};
    widget(a, W_POS)->box = (struct du_rect){450, CN_HEIGHT - 23, 148, 22};
    widget(a, W_FLABEL)->box = (struct du_rect){6, y + 10, 80, 16};
    widget(a, W_FIND)->box = (struct du_rect){90, y + 4, 190, 24};
    widget(a, W_FNEXT)->box = (struct du_rect){284, y + 4, 88, 24};
    widget(a, W_FDIR)->box = (struct du_rect){376, y + 4, 56, 24};
    widget(a, W_FCASE)->box = (struct du_rect){436, y + 4, 130, 24};
    widget(a, W_FCLOSE)->box = (struct du_rect){570, y + 4, 26, 24};
    widget(a, W_RLABEL)->box = (struct du_rect){6, y + 40, 104, 16};
    widget(a, W_REPL)->box = (struct du_rect){114, y + 34, 166, 24};
    widget(a, W_RONE)->box = (struct du_rect){284, y + 34, 88, 24};
    widget(a, W_RALL)->box = (struct du_rect){376, y + 34, 108, 24};
    du_ta_fit(&a->ta, text_box(a)); du_ta_reveal(&a->ta);
    a->ui->dirty = 1;
}
static void show_page(struct cn_app *a)
{
    struct du_ui *u = a->ui; int edit = a->page == CN_PAGE_EDIT;
    widget(a, W_TEXT)->hidden = !edit;
    widget(a, W_NAME)->hidden = widget(a, W_POS)->hidden = !edit || !a->show_status;
    static const int find_ids[] = {W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE};
    static const int replace_ids[] = {W_RLABEL, W_REPL, W_RONE, W_RALL};
    for (unsigned i = 0; i < 6; i++) widget(a, find_ids[i])->hidden = !edit || !a->find_mode;
    for (unsigned i = 0; i < 4; i++) widget(a, replace_ids[i])->hidden = !edit || a->find_mode != 2;
    static const int page_ids[] = {W_PLABEL, W_PATH, W_LIST, W_SCROLL, W_NLABEL, W_NAMEEDIT, W_TLABEL, W_TYPE, W_OK, W_CANCEL, W_NOTE};
    for (unsigned i = 0; i < 11; i++) widget(a, page_ids[i])->hidden = edit;
    u->menu_count = edit ? 5 : 0; u->menu = -1; u->context = NULL;
    layout(a);
    du_focus(u, edit ? W_TEXT : W_NAMEEDIT);
    cn_refresh(a);
}

/* ---- document commands -------------------------------------------------- */

static void new_document(struct cn_app *a)
{
    du_ta_set_text(&a->ta, "", 0);
    a->path[0] = 0; a->named = 0; a->crlf = 1; a->note[0] = 0;
    du_ta_reveal(&a->ta); cn_refresh(a);
}
/* Reads and decodes a file into the editor. No UI on failure. */
static int load_file(struct cn_app *a, const char *path)
{
    size_t raw_cap = 2 * (size_t)CN_LIMIT + 1, n = 0, len = 0; int crlf = 1;
    char *raw = malloc(raw_cap), *text = malloc((size_t)CN_LIMIT + 1);
    int r = raw && text ? 0 : -ENOMEM;
    if (!r) r = a->fs->load(a->fs->ctx, path, raw, raw_cap, &n);
    if (!r) r = cn_decode(raw, n, text, CN_LIMIT, &len, &crlf);
    if (!r) {
        du_ta_set_text(&a->ta, text, len);
        snprintf(a->path, sizeof(a->path), "%s", path); a->named = 1; a->crlf = crlf; a->note[0] = 0;
        if (cn_is_log(a->ta.text, a->ta.len)) {  /* a first line .LOG appends the time and date on each opening */
            struct tm tm; time_t t = a->now(a->host); char stamp[48], add[64]; size_t k = 0;
            if (gmtime_r(&t, &tm)) {
                cn_timestamp(stamp, sizeof(stamp), &tm);
                if (a->ta.len && a->ta.text[a->ta.len - 1] != '\n') add[k++] = '\n';
                k += (size_t)snprintf(add + k, sizeof(add) - k, "%s\n", stamp);
                du_ta_move(&a->ta, DU_TA_DOC_END, 0);
                du_ta_insert(&a->ta, add, k, DU_TA_EDIT_OTHER);
                a->ta.limit_hit = 0;
            }
        }
        du_ta_reveal(&a->ta);
    }
    free(raw); free(text);
    return r;
}
static int save_file(struct cn_app *a, const char *path)
{
    size_t need = cn_encoded_size(a->ta.text, a->ta.len, a->crlf);
    char *data = malloc(need + 1);
    if (!data) return -ENOMEM;
    cn_encode(a->ta.text, a->ta.len, a->crlf, data);
    int r = a->fs->save(a->fs->ctx, path, data, need);
    free(data);
    if (!r) {
        if (path != a->path) snprintf(a->path, sizeof(a->path), "%s", path);
        a->named = 1; a->ta.modified = 0; a->note[0] = 0; a->checkpoint = CN_CP_SAVED;
    }
    return r;
}
static void proceed(struct cn_app *a)
{
    enum cn_after after = a->after; a->after = CN_AFTER_NONE;
    if (after == CN_AFTER_NEW) new_document(a);
    else if (after == CN_AFTER_OPEN) begin_page(a, CN_PAGE_OPEN);
    else if (after == CN_AFTER_EXIT) a->quit(a->host);
}
static void ask_save_changes(struct cn_app *a, enum cn_after after)
{
    char name[28]; shorten(name, sizeof(name), document_name(a), 20);
    char message[160];
    snprintf(message, sizeof(message), "The text in the %s file has changed.\n\nDo you want to save the changes?", name);
    a->after = after;
    du_dialog_show(a->ui, DU_MESSAGE, D_CHANGES, "CiukNote", message, NULL);
    const char *buttons[] = {"Yes", "No", "Cancel"}; du_dialog_buttons(a->ui, buttons, 3);
}
static void guard(struct cn_app *a, enum cn_after after)
{
    if (a->ta.modified) ask_save_changes(a, after);
    else { a->after = after; proceed(a); }
}
int cn_close_request(struct cn_app *a)
{
    if (a->page != CN_PAGE_EDIT) { a->page = CN_PAGE_EDIT; show_page(a); }
    if (a->ta.modified) { ask_save_changes(a, CN_AFTER_EXIT); return 0; }
    return 1;
}
static void command_save(struct cn_app *a)
{
    if (!a->named) { begin_page(a, CN_PAGE_SAVE); return; }
    int r = save_file(a, a->path);
    if (r) failure(a, "The file could not be saved:", document_name(a), r);
    cn_refresh(a);
}
static void copy_selection(struct cn_app *a)
{
    size_t s, e; du_ta_selection(&a->ta, &s, &e);
    size_t n = e - s, m = 0;
    memcpy(a->clip, a->ta.text + s, n); a->clip[n] = 0; a->clip_len = n;
    /* Edit boxes are single-line: mirror the first line, within their size, on a character boundary. */
    while (m < n && m < DU_TEXT_MAX - 1 && a->clip[m] != '\n') m++;
    while (m > 0 && m < n && ((unsigned char)a->clip[m] & 0xc0) == 0x80) m--;
    memcpy(a->mirror, a->clip, m); a->mirror[m] = 0;
    du_clipboard_set(a->mirror);
}
static void paste(struct cn_app *a)
{
    const char *external = du_clipboard_get(), *source = external; size_t n = strlen(external);
    if (a->clip_len && !strcmp(external, a->mirror)) { source = a->clip; n = a->clip_len; }
    if (n) du_ta_insert(&a->ta, source, n, DU_TA_EDIT_OTHER);
}
static void insert_time(struct cn_app *a)
{
    struct tm tm; time_t t = a->now(a->host); char stamp[48];
    if (!gmtime_r(&t, &tm)) return;
    cn_timestamp(stamp, sizeof(stamp), &tm);
    du_ta_insert(&a->ta, stamp, strlen(stamp), DU_TA_EDIT_OTHER);
}

/* ---- find and replace --------------------------------------------------- */

static void show_find(struct cn_app *a, int mode)
{
    a->find_mode = mode;
    size_t s, e; du_ta_selection(&a->ta, &s, &e);
    if (e > s && e - s < 200 && !memchr(a->ta.text + s, '\n', e - s)) {
        char needle[208]; memcpy(needle, a->ta.text + s, e - s); needle[e - s] = 0;
        set_text(a, W_FIND, needle);
    }
    show_page(a);
    du_focus(a->ui, W_FIND); select_all_text(a, W_FIND);
}
static void hide_find(struct cn_app *a)
{
    a->find_mode = 0; show_page(a); du_focus(a->ui, W_TEXT);
}
static int find_next(struct cn_app *a)
{
    const char *needle = text_of(a, W_FIND); size_t n = strlen(needle);
    if (!n) { show_find(a, a->find_mode ? a->find_mode : 1); return -1; }
    if (du_ta_find_select(&a->ta, needle, n, a->match_case, a->find_up)) {
        char shown[48]; shorten(shown, sizeof(shown), needle, 40);
        info(a, "CiukNote", "Cannot find \"%s\"", shown);
        return -1;
    }
    cn_refresh(a);
    return 0;
}
static void replace_one(struct cn_app *a)
{
    const char *needle = text_of(a, W_FIND), *repl = text_of(a, W_REPL);
    if (du_ta_selection_equals(&a->ta, needle, strlen(needle), a->match_case)) du_ta_insert(&a->ta, repl, strlen(repl), DU_TA_EDIT_OTHER);
    find_next(a);
}
static void replace_all(struct cn_app *a)
{
    const char *needle = text_of(a, W_FIND), *repl = text_of(a, W_REPL);
    if (!needle[0]) { show_find(a, 2); return; }
    long count = du_ta_replace_all(&a->ta, needle, strlen(needle), repl, strlen(repl), a->match_case);
    if (count < 0) return; /* the limit message is shown by the caller */
    if (!count) { info(a, "CiukNote", "Cannot find \"%.40s\"", needle); return; }
    snprintf(a->note, sizeof(a->note), "Replaced %ld occurrence%s", count, count == 1 ? "" : "s");
}
static void after_edit(struct cn_app *a)
{
    a->note[0] = 0;
    if (a->ta.limit_hit) {
        a->ta.limit_hit = 0;
        info(a, "CiukNote", "The text limit of " CN_LIMIT_TEXT " bytes was reached.\nThe last change was not made.");
    }
    cn_refresh(a);
}

/* ---- file dialog page --------------------------------------------------- */

static void note(struct cn_app *a, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(a->fd_note, sizeof(a->fd_note), fmt, ap); va_end(ap);
    set_text(a, W_NOTE, a->fd_note);
}
static const struct cn_entry *row_entry(const struct cn_app *a, unsigned row)
{
    if (a->has_parent && !row) return NULL;
    unsigned i = row - (a->has_parent ? 1u : 0u);
    return i < a->listing.count ? &a->listing.entries[i] : NULL;
}
static void fd_scroll_sync(struct cn_app *a)
{
    struct du_widget *list = widget(a, W_LIST), *bar = widget(a, W_SCROLL);
    bar->total = list->count; bar->page = LIST_ROWS; bar->value = list->first;
    a->ui->dirty = 1;
}
static int fd_open_dir(struct cn_app *a, const char *directory)
{
    char dir[CN_PATH]; snprintf(dir, sizeof(dir), "%s", directory);
    struct cn_listing l; int r = cn_list(a->fs, dir, a->text_filter, &l);
    if (r) return r;
    cn_listing_sort(&l, a->sort_column, a->sort_descending);
    unsigned parent = strcmp(dir, "/") != 0, count = l.count + parent;
    struct du_row *rows = calloc(count ? count : 1, sizeof(*rows));
    struct cn_cells *cells = calloc(count ? count : 1, sizeof(*cells));
    if (!rows || !cells) { free(rows); free(cells); cn_listing_free(&l); return -ENOMEM; }
    cn_listing_free(&a->listing); free(a->rows); free(a->cells);
    a->listing = l; a->rows = rows; a->cells = cells; a->row_count = count; a->has_parent = (int)parent;
    snprintf(a->dir, sizeof(a->dir), "%s", dir);
    for (unsigned i = 0; i < count; i++) {
        const struct cn_entry *e = row_entry(a, i);
        const char *name = e ? e->name : "..";
        int folder = !e || e->dir;
        if (!folder) snprintf(cells[i].size, sizeof(cells[i].size), "%llu", (unsigned long long)e->size);
        if (e && e->mtime > 0) {
            struct tm tm; time_t t = (time_t)e->mtime;
            if (gmtime_r(&t, &tm)) snprintf(cells[i].modified, sizeof(cells[i].modified), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        }
        rows[i] = (struct du_row){name, cells[i].size, folder ? "Folder" : cn_is_text_name(name) ? "Text file" : "File",
                                  cells[i].modified, folder ? desk_icon_folder : desk_icon_text, 0, 0, 0};
    }
    struct du_widget *list = widget(a, W_LIST);
    list->rows = rows; list->count = count; list->first = 0; list->active = 0;
    set_text(a, W_PATH, dir);
    fd_scroll_sync(a);
    return 0;
}
static void fd_select(struct cn_app *a, unsigned index)
{
    for (unsigned i = 0; i < a->row_count; i++) a->rows[i].selected = i == index;
    widget(a, W_LIST)->active = index;
    const struct cn_entry *e = row_entry(a, index);
    if (e && !e->dir) set_text(a, W_NAMEEDIT, e->name);
    fd_scroll_sync(a);
}
static const char *page_hint(const struct cn_app *a)
{ return a->page == CN_PAGE_OPEN ? "Open: type a file name or choose one from the list." : "Save As: type a file name or choose a folder."; }
static int fd_cd(struct cn_app *a, const char *path)
{
    int r = fd_open_dir(a, path);
    if (r) note(a, "Cannot open that folder: %s", strerror(-r));
    else note(a, "%s", page_hint(a));
    return r;
}
static void save_as(struct cn_app *a, const char *target)
{
    int r = save_file(a, target);
    if (r) { failure(a, "The file could not be saved:", cn_basename(target), r); return; }
    a->page = CN_PAGE_EDIT; show_page(a);
    proceed(a);
}
static void fd_commit(struct cn_app *a)
{
    const char *typed = text_of(a, W_NAMEEDIT);
    if (!typed[0]) { note(a, "Type a file name, or choose one from the list."); return; }
    char target[CN_PATH], with[CN_PATH + 8]; struct cn_stat st;
    int r = cn_resolve(target, a->dir, typed);
    if (r) { note(a, "That name is not valid: %s", strerror(-r)); return; }
    r = a->fs->stat(a->fs->ctx, target, &st);
    if (!r && st.dir) { if (!fd_cd(a, target)) set_text(a, W_NAMEEDIT, ""); return; }
    if (r && r != -ENOENT) { note(a, "%s: %s", cn_basename(typed), strerror(-r)); return; }
    int default_extension = a->text_filter && !cn_has_extension(cn_basename(typed));
    if (a->page == CN_PAGE_OPEN) {
        if (r == -ENOENT && default_extension) {
            snprintf(with, sizeof(with), "%s.txt", typed);
            if (!cn_resolve(target, a->dir, with)) { r = a->fs->stat(a->fs->ctx, target, &st); if (!r && st.dir) r = -EISDIR; }
        }
        if (r) { note(a, "Cannot find %s. Check the file name and try again.", cn_basename(typed)); return; }
        r = load_file(a, target);
        if (r) { failure(a, "The file could not be opened:", cn_basename(target), r); return; }
        a->page = CN_PAGE_EDIT; a->checkpoint = CN_CP_OPENED; show_page(a);
        return;
    }
    if (default_extension) {
        snprintf(with, sizeof(with), "%s.txt", typed);
        r = cn_resolve(target, a->dir, with);
        if (r) { note(a, "That name is not valid: %s", strerror(-r)); return; }
        r = a->fs->stat(a->fs->ctx, target, &st);
        if (!r && st.dir) { note(a, "%s is a folder.", cn_basename(target)); return; }
        if (r && r != -ENOENT) { note(a, "%s: %s", cn_basename(target), strerror(-r)); return; }
    }
    if (!r) { /* the target exists: ask before replacing it */
        char shown[40]; shorten(shown, sizeof(shown), cn_basename(target), 36);
        char message[160];
        snprintf(message, sizeof(message), "%s already exists.\n\nDo you want to replace it?", shown);
        snprintf(a->pending, sizeof(a->pending), "%s", target);
        du_dialog_show(a->ui, DU_MESSAGE, D_REPLACE, "Save As", message, NULL);
        const char *buttons[] = {"Yes", "No"}; du_dialog_buttons(a->ui, buttons, 2);
        return;
    }
    save_as(a, target);
}
static void fd_activate(struct cn_app *a, unsigned index)
{
    const struct cn_entry *e = row_entry(a, index);
    char target[CN_PATH];
    if (!e) { if (!cn_resolve(target, a->dir, "..")) fd_cd(a, target); }
    else if (e->dir) { if (!cn_join(target, a->dir, e->name)) fd_cd(a, target); }
    else { set_text(a, W_NAMEEDIT, e->name); fd_commit(a); }
}
static void fd_cancel(struct cn_app *a)
{
    a->after = CN_AFTER_NONE; a->page = CN_PAGE_EDIT; show_page(a);
}
static void fd_toggle_filter(struct cn_app *a)
{
    a->text_filter = !a->text_filter;
    set_text(a, W_TYPE, a->text_filter ? "Text Documents (*.txt)" : "All Files (*.*)");
    fd_cd(a, a->dir);
}
static void fd_clicked(struct cn_app *a, unsigned index)
{
    uint64_t now = a->mono(a->host);
    int again = (int)index == a->last_click && now - a->click_ns < DOUBLE_CLICK_NS && a->ui->buttons;
    a->last_click = (int)index; a->click_ns = now;
    fd_select(a, index);
    if (again) { a->last_click = -1; fd_activate(a, index); }
}
static void fd_sort(struct cn_app *a, unsigned column)
{
    a->sort_descending = column == a->sort_column ? !a->sort_descending : 0;
    a->sort_column = column;
    fd_cd(a, a->dir);
}
static void begin_page(struct cn_app *a, enum cn_page page)
{
    a->page = page; a->fd_note[0] = 0; a->last_click = -1;
    char dir[CN_PATH];
    if (a->named) {
        snprintf(dir, sizeof(dir), "%s", a->path);
        char *slash = strrchr(dir, '/');
        if (slash == dir) dir[1] = 0; else if (slash) *slash = 0;
    } else snprintf(dir, sizeof(dir), "%s", a->dir[0] ? a->dir : "/");
    if (fd_open_dir(a, dir) && fd_open_dir(a, "/")) {
        for (unsigned i = 0; i < a->row_count; i++) a->rows[i].selected = 0;
        widget(a, W_LIST)->count = 0;
    }
    set_text(a, W_OK, page == CN_PAGE_OPEN ? "Open" : "Save");
    set_text(a, W_NAMEEDIT, page == CN_PAGE_SAVE && a->named ? cn_basename(a->path) : "");
    set_text(a, W_TYPE, a->text_filter ? "Text Documents (*.txt)" : "All Files (*.*)");
    note(a, "%s", page_hint(a));
    show_page(a);
    select_all_text(a, W_NAMEEDIT);
}

/* ---- commands and events ------------------------------------------------ */

static void goto_line(struct cn_app *a, const char *text)
{
    char *end; unsigned long line = strtoul(text, &end, 10);
    if (!text[0] || *end || strlen(text) > 9 || du_ta_goto_line(&a->ta, (unsigned)line))
        info(a, "CiukNote - Go To Line", "The line number is beyond the total number of lines.");
}
static void menu_command(struct cn_app *a, int id)
{
    switch (id) {
    case M_NEW: guard(a, CN_AFTER_NEW); break;
    case M_OPEN: guard(a, CN_AFTER_OPEN); break;
    case M_SAVE: command_save(a); break;
    case M_SAVEAS: begin_page(a, CN_PAGE_SAVE); break;
    case M_EXIT: guard(a, CN_AFTER_EXIT); break;
    case M_UNDO: du_ta_undo(&a->ta); after_edit(a); break;
    case M_CUT: if (du_ta_has_selection(&a->ta)) { copy_selection(a); du_ta_delete_selection(&a->ta); } after_edit(a); break;
    case M_COPY: if (du_ta_has_selection(&a->ta)) copy_selection(a); break;
    case M_PASTE: paste(a); after_edit(a); break;
    case M_DELETE: du_ta_delete_selection(&a->ta); after_edit(a); break;
    case M_FIND: show_find(a, 1); break;
    case M_FINDNEXT: find_next(a); break;
    case M_REPLACE: show_find(a, 2); break;
    case M_GOTO:
        if (a->ta.wrap) break;
        { unsigned line, col; char now[16]; du_ta_line_col(&a->ta, a->ta.cursor, &line, &col); snprintf(now, sizeof(now), "%u", line);
          du_dialog_show(a->ui, DU_INPUT, D_GOTO, "Go To Line", "Line number:", now); }
        break;
    case M_SELALL: du_ta_select_all(&a->ta); break;
    case M_TIME: insert_time(a); after_edit(a); break;
    case M_WRAP: du_ta_set_wrap(&a->ta, !a->ta.wrap); layout(a); break;
    case M_FONT: {
        char message[160];
        snprintf(message, sizeof(message), "Font: CiukiOS bitmap font, 8x16 cells.\nCurrent style: %s.\n\nOther faces and sizes are not available.", a->ta.bold ? "Bold" : "Regular");
        du_dialog_show(a->ui, DU_MESSAGE, D_FONT, "Font", message, NULL);
        const char *buttons[] = {"Regular", "Bold", "Cancel"}; du_dialog_buttons(a->ui, buttons, 3);
        break;
    }
    case M_STATUS: a->show_status = !a->show_status; show_page(a); break;
    case M_HELP:
        info(a, "CiukNote Help", "New, Open, Save: Ctrl+N, Ctrl+O, Ctrl+S.\nUndo, Cut, Copy, Paste: Ctrl+Z, X, C, V.\nFind Ctrl+F, Next F3, Replace Ctrl+H, Go To Ctrl+G.\nF5 time and date, F10 menus. Limit " CN_LIMIT_TEXT " bytes.");
        break;
    case M_ABOUT:
        info(a, "About CiukNote", "CiukNote - A modern Retro OS\nText editor for CiukiOS, limit " CN_LIMIT_TEXT " bytes.\nTango 0.8.90 icons: Public Domain.\nlibdeskui and CiukNote code: MIT.");
        break;
    default: break;
    }
}
static void widget_command(struct cn_app *a, int id)
{
    switch (id) {
    case W_FIND: case W_FNEXT: find_next(a); break;
    case W_FDIR:
        a->find_up = !a->find_up; set_text(a, W_FDIR, a->find_up ? "Up" : "Down"); break;
    case W_FCASE:
        a->match_case = !a->match_case; set_text(a, W_FCASE, a->match_case ? "[x] Match case" : "[ ] Match case"); break;
    case W_FCLOSE: hide_find(a); break;
    case W_REPL: case W_RONE: replace_one(a); after_edit(a); break;
    case W_RALL:
        replace_all(a);
        if (a->ta.limit_hit) { a->ta.limit_hit = 0; info(a, "CiukNote", "The text limit of " CN_LIMIT_TEXT " bytes would be exceeded.\nNothing was replaced."); }
        break;
    case W_PATH: {
        char target[CN_PATH];
        int r = cn_resolve(target, a->dir, text_of(a, W_PATH));
        if (r) note(a, "That folder name is not valid: %s", strerror(-r)); else fd_cd(a, target);
        break;
    }
    case W_NAMEEDIT: case W_OK: fd_commit(a); break;
    case W_CANCEL: fd_cancel(a); break;
    case W_TYPE: fd_toggle_filter(a); break;
    default: break;
    }
}
static void dialog_result(struct cn_app *a, int id, int index)
{
    switch (id) {
    case D_CHANGES:
        if (index == 0) {
            if (!a->named) { begin_page(a, CN_PAGE_SAVE); return; }
            int r = save_file(a, a->path);
            if (r) { a->after = CN_AFTER_NONE; failure(a, "The file could not be saved:", document_name(a), r); }
            else proceed(a);
        } else if (index == 1) proceed(a);
        else a->after = CN_AFTER_NONE;
        break;
    case D_REPLACE:
        if (index == 0) save_as(a, a->pending);
        else du_focus(a->ui, W_NAMEEDIT);
        break;
    case D_GOTO: if (index == 0) goto_line(a, a->ui->dialog.edit.text); break;
    case D_FONT: if (index < 2) a->ta.bold = index; break;
    case D_CREATE:
        if (index == 0) {
            int r = a->fs->save(a->fs->ctx, a->pending, "", 0);
            if (r) failure(a, "The file could not be created:", cn_basename(a->pending), r);
            else {
                du_ta_set_text(&a->ta, "", 0);
                snprintf(a->path, sizeof(a->path), "%s", a->pending); a->named = 1; a->crlf = 1; a->note[0] = 0;
            }
        }
        break;
    default: break;
    }
}
static void event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct cn_app *a = ctx; int id = e->id;
    switch (e->type) {
    case DU_CHANGE:
        if (id == W_TEXT) after_edit(a);
        else if (id == W_SCROLL) { widget(a, W_LIST)->first = (unsigned)e->index; u->dirty = 1; }
        return;
    case DU_CONTEXT:
        if (a->page == CN_PAGE_EDIT && id == W_TEXT) { cn_refresh(a); du_context(u, a->context_items, 6, e->x, e->y); }
        return;
    case DU_SELECT:
        if (id == W_LIST && e->index >= 0) fd_clicked(a, (unsigned)e->index);
        return;
    case DU_SORT:
        if (id == W_LIST) fd_sort(a, (unsigned)e->index);
        return;
    case DU_CLOSE:
        if (a->page == CN_PAGE_EDIT && a->find_mode && id >= W_FLABEL && id <= W_RALL) hide_find(a); /* Esc anywhere in the bar */
        else if (a->page != CN_PAGE_EDIT && id >= W_PLABEL) fd_cancel(a);
        return;
    case DU_ACTIVATE: break;
    default: return;
    }
    if (id >= 200) dialog_result(a, id, e->index);
    else if (id == W_LIST) { if (e->index >= 0) fd_activate(a, (unsigned)e->index); }
    else if (id >= 100) widget_command(a, id);
    else menu_command(a, id);
    cn_refresh(a);
    fd_scroll_sync(a);
}

/* ---- lifecycle ---------------------------------------------------------- */

int cn_attach(struct cn_app *a, struct du_ui *ui)
{
    a->ui = ui; ui->event = event; ui->user = a;
    if (du_ta_init(&a->ta, CN_LIMIT)) return -ENOMEM;
    a->crlf = 1; a->show_status = 1; a->text_filter = 1; a->last_click = -1; strcpy(a->dir, "/");
    build_menus(a);
    ui->menus = a->menus; ui->menu_count = 5; ui->menu_keys = a->menu_keys;
    struct du_rect none = {0, 0, 1, 1};
    struct du_widget *text = du_add(ui, DU_TEXTAREA, W_TEXT, none, "");
    if (!text) return -ENOMEM;
    text->data = &a->ta;
    du_add(ui, DU_STATUS, W_NAME, none, ""); du_add(ui, DU_STATUS, W_POS, none, "");
    du_add(ui, DU_LABEL, W_FLABEL, none, "Find what:"); du_add(ui, DU_EDIT, W_FIND, none, "");
    du_add(ui, DU_BUTTON, W_FNEXT, none, "Find Next"); du_add(ui, DU_BUTTON, W_FDIR, none, "Down");
    du_add(ui, DU_BUTTON, W_FCASE, none, "[ ] Match case"); du_add(ui, DU_BUTTON, W_FCLOSE, none, "X");
    du_add(ui, DU_LABEL, W_RLABEL, none, "Replace with:"); du_add(ui, DU_EDIT, W_REPL, none, "");
    du_add(ui, DU_BUTTON, W_RONE, none, "Replace"); du_add(ui, DU_BUTTON, W_RALL, none, "Replace All");
    static const struct { int id; enum du_kind kind; struct du_rect box; const char *text; } page[] = {
        {W_PLABEL, DU_LABEL, {8, 12, 64, 16}, "Look in:"}, {W_PATH, DU_EDIT, {76, 6, 516, 26}, "/"},
        {W_LIST, DU_LIST, {8, 38, 568, 216}, ""}, {W_SCROLL, DU_SCROLLBAR, {578, 38, 14, 216}, ""},
        {W_NLABEL, DU_LABEL, {8, 270, 80, 16}, "File name:"}, {W_NAMEEDIT, DU_EDIT, {92, 264, 388, 26}, ""},
        {W_TLABEL, DU_LABEL, {8, 304, 116, 16}, "Files of type:"}, {W_TYPE, DU_BUTTON, {124, 298, 356, 26}, "Text Documents (*.txt)"},
        {W_OK, DU_BUTTON, {488, 264, 104, 26}, "Open"}, {W_CANCEL, DU_BUTTON, {488, 298, 104, 26}, "Cancel"},
        {W_NOTE, DU_LABEL, {8, 332, 584, 16}, ""}};
    for (unsigned i = 0; i < sizeof(page) / sizeof(*page); i++) du_add(ui, page[i].kind, page[i].id, page[i].box, page[i].text);
    a->page = CN_PAGE_EDIT;
    show_page(a);
    a->checkpoint = CN_CP_READY;
    return 0;
}
void cn_detach(struct cn_app *a)
{
    cn_listing_free(&a->listing); free(a->rows); free(a->cells); a->rows = NULL; a->cells = NULL;
    du_ta_free(&a->ta);
}
int cn_open_argument(struct cn_app *a, const char *path)
{
    char target[CN_PATH]; struct cn_stat st;
    int r = cn_resolve(target, "/", path);
    if (r) { failure(a, "The file could not be opened:", path, r); return r; }
    r = a->fs->stat(a->fs->ctx, target, &st);
    if (r == -ENOENT) {
        snprintf(a->pending, sizeof(a->pending), "%s", target);
        char shown[40]; shorten(shown, sizeof(shown), cn_basename(target), 32);
        char message[160];
        snprintf(message, sizeof(message), "Cannot find the %s file.\n\nDo you want to create a new file?", shown);
        du_dialog_show(a->ui, DU_MESSAGE, D_CREATE, "CiukNote", message, NULL);
        const char *buttons[] = {"Yes", "No"}; du_dialog_buttons(a->ui, buttons, 2);
        return r;
    }
    if (!r && st.dir) r = -EISDIR;
    if (!r) r = load_file(a, target);
    if (r) failure(a, "The file could not be opened:", cn_basename(target), r);
    else a->checkpoint = CN_CP_OPENED;
    cn_refresh(a);
    return r;
}
