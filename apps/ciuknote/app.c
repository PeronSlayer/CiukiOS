/* SPDX-License-Identifier: MIT */
#include "app.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_N = 0x31, K_O = 0x18, K_S = 0x1f, K_Z = 0x2c, K_X = 0x2d, K_C = 0x2e, K_V = 0x2f, K_A = 0x1e,
       K_F = 0x21, K_H = 0x23, K_G = 0x22, K_F3 = 0x3d };
/* Positions inside the Edit menu and the context menu (cn_refresh updates their flags). */
enum { E_UNDO = 0, E_CUT = 2, E_COPY, E_PASTE, E_DELETE, E_FIND = 7, E_FINDNEXT, E_REPLACE, E_GOTO, E_SELALL = 12, E_TIME };
_Static_assert(CN_LIMIT == 61440u, "CN_LIMIT_TEXT must name CN_LIMIT");
#define STATUS_H 20
#define MENUBAR_H 20   /* libdeskui menu bar height */
#define EDIT_H 22
#define TEXT_FILTER "*.txt"

static const struct du_menu_item file_template[] = {
    {"&New", M_NEW, 0x31, DU_CTRL, 0}, {"&Open...", M_OPEN, 0x18, DU_CTRL, 0}, {"&Save", M_SAVE, 0x1f, DU_CTRL, 0},
    {"Save &As...", M_SAVEAS, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", M_EXIT, 0, 0, 0}};
static const struct du_menu_item edit_template[] = {
    {"&Undo", M_UNDO, 0x2c, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Cu&t", M_CUT, 0x2d, DU_CTRL, 0}, {"&Copy", M_COPY, 0x2e, DU_CTRL, 0}, {"&Paste", M_PASTE, 0x2f, DU_CTRL, 0},
    {"De&lete", M_DELETE, DU_KEY_DELETE, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Find...", M_FIND, 0x21, DU_CTRL, 0}, {"Find &Next", M_FINDNEXT, 0x3d, 0, 0}, {"&Replace...", M_REPLACE, 0x23, DU_CTRL, 0},
    {"&Go To...", M_GOTO, 0x22, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Select &All", M_SELALL, 0x1e, DU_CTRL, 0}, {"Time/&Date", M_TIME, DU_KEY_F5, 0, 0}};
static const struct du_menu_item format_template[] = {{"&Word Wrap", M_WRAP, 0, 0, 0}, {"&Font...", M_FONT, 0, 0, 0}};
static const struct du_menu_item view_template[] = {{"&Status Bar", M_STATUS, 0, 0, 0}};
static const struct du_menu_item help_template[] = {
    {"&Help Topics", M_HELP, DU_KEY_F1, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"&About CiukNote", M_ABOUT, 0, 0, 0}};
static const struct du_menu_item popup_template[] = {
    {"&Undo", M_UNDO, 0, 0, 0}, {"Cu&t", M_CUT, 0, 0, 0}, {"&Copy", M_COPY, 0, 0, 0}, {"&Paste", M_PASTE, 0, 0, 0},
    {"De&lete", M_DELETE, 0, 0, 0}, {"Select &All", M_SELALL, 0, 0, 0}};

static const char *const yes_no_cancel[] = {"&Yes", "&No", "Cancel"};
static const char *const yes_no[] = {"&Yes", "&No"};
static const char *const font_buttons[] = {"&Regular", "&Bold", "Cancel"};

/* ---- small helpers ------------------------------------------------------ */

static struct du_widget *W(struct cn_app *a, int id) { return du_get(a->ui, id); }

static void box(struct cn_app *a, int id, const char *title, int icon, const char *const *buttons, unsigned count,
                int def, int cancel, const char *fmt, ...)
{
    char text[DU_DIALOG_TEXT]; va_list ap;
    va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    du_message(a->ui, id, title, text, icon, buttons, count, def, cancel);
}

/* Copies name, shortened to at most `max` cells with "..." (dialog lines are as wide as the window). */
static void shorten(char *out, size_t size, const char *name, size_t max)
{
    snprintf(out, size, "%s", name);
    if (strlen(name) > max && max >= 3 && max < size) strcpy(out + max - 3, "...");
}
static void failure(struct cn_app *a, const char *what, const char *name, int error)
{
    char shown[48]; shorten(shown, sizeof(shown), name, 44);
    if (error < 0) error = -error;
    box(a, D_INFO, "CiukNote", DU_DLG_ICON_ERROR, NULL, 0, 0, 0, "%s\n%s.\n---\n%s (error %d)", what, shown, strerror(error), error);
}
static const char *document_name(const struct cn_app *a) { return a->named ? cn_basename(a->path) : "Untitled"; }
static const char *text_of(struct cn_app *a, int id) { return W(a, id)->text; }
static void select_all_text(struct cn_app *a, int id)
{ struct du_widget *w = W(a, id); w->anchor = 0; w->cursor = strlen(w->text); w->offset = 0; a->ui->dirty = 1; }

uint32_t cn_digest(const struct du_ui *ui)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)ui->canvas.width * (size_t)ui->canvas.height; i++)
        for (unsigned s = 0; s < 32; s += 8) h = (h ^ ((ui->canvas.pixels[i] >> s) & 255u)) * 16777619u;
    return h;
}
void cn_describe(const struct cn_app *a, int stage, char *line, size_t cap)
{
    static const char *const names[] = {"", "ready", "saved", "opened"};
    snprintf(line, cap, "CiukNote checkpoint=%d event=%s path=%s text=%u digest=%08x",
             stage, stage >= 0 && stage <= CN_CHECKPOINTS ? names[stage] : "?", a->named ? a->path : "-", (unsigned)a->ta.len, cn_digest(a->ui));
}
static void checkpoint(struct cn_app *a, int stage)
{
    if (!a->test || stage != a->stage + 1) return;
    a->stage = stage;
    if (a->checkpoint) a->checkpoint(a, stage);
}

/* Deterministic box-filter size conversion of the approved portrait array (as Files). */
static void shrink32(const uint32_t *src, uint32_t *dst)
{
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            uint32_t p = src[(y * 8 + yy) * 256 + x * 8 + xx];
            r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
        }
        dst[y * 32 + x] = 0xff000000u | ((r + 32) / 64) << 16 | ((g + 32) / 64) << 8 | (b + 32) / 64;
    }
}

/* ---- menus and status ---------------------------------------------------- */

static int paste_available(const struct cn_app *a) { return a->clip_len || du_clipboard_get()[0]; }
static void set_flag(struct du_menu_item *item, unsigned flag, int on) { if (on) item->flags |= flag; else item->flags &= ~flag; }

void cn_refresh(struct cn_app *a)
{
    unsigned line, col; char text[160];
    du_ta_line_col(&a->ta, a->ta.cursor, &line, &col);
    snprintf(text, sizeof(text), "%s%s%s%s\tLn %u, Col %u", document_name(a), a->ta.modified ? " *" : "",
             a->note[0] ? "   " : "", a->note, line, col);
    du_set_text(W(a, W_STATUS), text);
    int selection = du_ta_has_selection(&a->ta);
    set_flag(&a->edit_items[E_UNDO], DU_ITEM_DISABLED, !du_ta_can_undo(&a->ta));
    set_flag(&a->edit_items[E_CUT], DU_ITEM_DISABLED, !selection);
    set_flag(&a->edit_items[E_COPY], DU_ITEM_DISABLED, !selection);
    set_flag(&a->edit_items[E_DELETE], DU_ITEM_DISABLED, !selection);
    set_flag(&a->edit_items[E_PASTE], DU_ITEM_DISABLED, !paste_available(a));
    set_flag(&a->edit_items[E_GOTO], DU_ITEM_DISABLED, a->ta.wrap);  /* Go To is off with Word Wrap */
    set_flag(&a->format_items[0], DU_ITEM_CHECKED, a->ta.wrap);
    set_flag(&a->view_items[0], DU_ITEM_CHECKED, a->show_status);
    set_flag(&a->popup[0], DU_ITEM_DISABLED, !du_ta_can_undo(&a->ta));
    set_flag(&a->popup[1], DU_ITEM_DISABLED, !selection);
    set_flag(&a->popup[2], DU_ITEM_DISABLED, !selection);
    set_flag(&a->popup[3], DU_ITEM_DISABLED, !paste_available(a));
    set_flag(&a->popup[4], DU_ITEM_DISABLED, !selection);
    a->ui->dirty = 1;
}

void cn_layout(struct cn_app *a)
{
    struct du_ui *u = a->ui;
    int w = u->canvas.width, h = u->canvas.height;
    int bar_h = a->find_mode == 2 ? 58 : a->find_mode == 1 ? 30 : 0, status_h = a->show_status ? STATUS_H : 0;
    int bar_y = h - status_h - bar_h, top = MENUBAR_H + 1;
    W(a, W_TEXT)->box = (struct du_rect){0, top, w, bar_y - top};
    struct du_widget *status = W(a, W_STATUS);
    status->box = (struct du_rect){0, h - STATUS_H, w, STATUS_H};
    status->panels[0] = w > 220 ? w - 130 : w / 2; status->panels[1] = 0;
    int y = bar_y + 4;
    W(a, W_FLABEL)->box = (struct du_rect){6, y, 84, EDIT_H};
    W(a, W_FIND)->box = (struct du_rect){92, y, 150, EDIT_H};
    W(a, W_FNEXT)->box = (struct du_rect){248, y, 76, EDIT_H};
    W(a, W_FDIR)->box = (struct du_rect){328, y, 54, EDIT_H};
    W(a, W_FCASE)->box = (struct du_rect){386, y, 108, EDIT_H};
    W(a, W_FCLOSE)->box = (struct du_rect){498, y, 24, EDIT_H};
    y += EDIT_H + 2;
    W(a, W_RLABEL)->box = (struct du_rect){6, y, 84, EDIT_H};
    W(a, W_REPL)->box = (struct du_rect){92, y, 150, EDIT_H};
    W(a, W_RONE)->box = (struct du_rect){248, y, 76, EDIT_H};
    W(a, W_RALL)->box = (struct du_rect){328, y, 90, EDIT_H};
    static const int find_ids[] = {W_FLABEL, W_FIND, W_FNEXT, W_FDIR, W_FCASE, W_FCLOSE};
    static const int replace_ids[] = {W_RLABEL, W_REPL, W_RONE, W_RALL};
    for (unsigned i = 0; i < 6; i++) W(a, find_ids[i])->hidden = !a->find_mode;
    for (unsigned i = 0; i < 4; i++) W(a, replace_ids[i])->hidden = a->find_mode != 2;
    status->hidden = !a->show_status;
    du_ta_fit(&a->ta, W(a, W_TEXT)->box); du_ta_reveal(&a->ta);
    u->dirty = 1;
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
        if (a->recent) a->recent(a->path);
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
        a->named = 1; a->ta.modified = 0; a->note[0] = 0;
        if (a->recent) a->recent(a->path);
        checkpoint(a, CN_CP_SAVED);
    }
    return r;
}

static void file_dialog(struct cn_app *a, int save, const char *name);
static void proceed(struct cn_app *a)
{
    enum cn_after after = a->after; a->after = CN_AFTER_NONE;
    if (after == CN_AFTER_NEW) new_document(a);
    else if (after == CN_AFTER_OPEN) file_dialog(a, 0, "");
    else if (after == CN_AFTER_EXIT) a->quit(a->host);
}
static void ask_save_changes(struct cn_app *a, enum cn_after after)
{
    char name[28]; shorten(name, sizeof(name), document_name(a), 24);
    a->after = after;
    box(a, D_CHANGES, "CiukNote", DU_DLG_ICON_QUESTION, yes_no_cancel, 3, 0, 2,
        "The text in the %s file has changed.\n\nDo you want to save the changes?", name);
}
static void guard(struct cn_app *a, enum cn_after after)
{
    if (a->ta.modified) ask_save_changes(a, after);
    else { a->after = after; proceed(a); }
}
int cn_close_request(struct cn_app *a)
{
    if (a->ta.modified) { ask_save_changes(a, CN_AFTER_EXIT); return 0; }
    return 1;
}
static void command_save(struct cn_app *a)
{
    if (!a->named) { file_dialog(a, 1, ""); return; }
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
        du_set_text(W(a, W_FIND), needle);
    }
    cn_layout(a);
    du_focus(a->ui, W_FIND); select_all_text(a, W_FIND);
}
static void hide_find(struct cn_app *a)
{
    a->find_mode = 0; cn_layout(a); du_focus(a->ui, W_TEXT);
}
static int find_next(struct cn_app *a)
{
    const char *needle = text_of(a, W_FIND); size_t n = strlen(needle);
    if (!n) { show_find(a, a->find_mode ? a->find_mode : 1); return -1; }
    if (du_ta_find_select(&a->ta, needle, n, a->match_case, a->find_up)) {
        char shown[48]; shorten(shown, sizeof(shown), needle, 40);
        box(a, D_INFO, "CiukNote", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "Cannot find \"%s\"", shown);
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
    if (!count) { box(a, D_INFO, "CiukNote", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "Cannot find \"%.40s\"", needle); return; }
    snprintf(a->note, sizeof(a->note), "Replaced %ld occurrence%s", count, count == 1 ? "" : "s");
}
static void limit_message(struct cn_app *a, int replacing)
{
    a->ta.limit_hit = 0;
    box(a, D_INFO, "CiukNote", DU_DLG_ICON_WARNING, NULL, 0, 0, 0, replacing ?
        "The text limit of " CN_LIMIT_TEXT " bytes would be exceeded.\nNothing was replaced." :
        "The text limit of " CN_LIMIT_TEXT " bytes was reached.\nThe last change was not made.");
}
static void after_edit(struct cn_app *a)
{
    a->note[0] = 0;
    if (a->ta.limit_hit) limit_message(a, 0);
    cn_refresh(a);
}

/* ---- file dialogs ------------------------------------------------------- */

struct collect { struct du_file_entry *out; unsigned capacity, count; const char *pattern; };
static int collect_entry(const char *name, const struct cn_stat *st, void *arg)
{
    struct collect *c = arg;
    if (c->count >= c->capacity || strlen(name) >= sizeof(c->out->name)) return 0;
    if (!st->dir && !cn_match(c->pattern, name)) return 0;
    struct du_file_entry *f = &c->out[c->count++];
    snprintf(f->name, sizeof(f->name), "%s", name); f->directory = st->dir != 0; f->size = st->size;
    return 0;
}
/* Folders always, files matching the current filter (`*.txt` until a pattern is typed in the name box). */
static int lister(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count)
{
    struct cn_app *a = ctx; struct collect c = {out, capacity, 0, a->filter};
    int r = a->fs->list(a->fs->ctx, path, collect_entry, &c);
    *count = c.count;
    return r;
}
static void file_dialog(struct cn_app *a, int save, const char *name)
{
    char folder[CN_PATH]; snprintf(folder, sizeof(folder), "%s", a->dir[0] ? a->dir : "/");
    if (a->named && a->path[0]) {
        snprintf(folder, sizeof(folder), "%s", a->path);
        char *slash = strrchr(folder, '/');
        if (slash == folder) folder[1] = 0; else if (slash) *slash = 0;
        if (save && !name[0]) name = cn_basename(a->path);
    }
    int r = du_file_dialog(a->ui, save ? D_SAVE : D_OPEN, save, save ? "Save As" : "Open", folder, name, lister, a);
    if (r) { failure(a, "The folder could not be opened:", folder, r); a->after = CN_AFTER_NONE; }
}
static const char *folder_of(char *out, size_t size, const char *path)
{
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == out) out[1] = 0; else if (slash) *slash = 0;
    return out;
}
static void save_as(struct cn_app *a, const char *target)
{
    int r = save_file(a, target);
    if (r) { failure(a, "The file could not be saved:", cn_basename(target), r); return; }
    char folder[CN_PATH]; folder_of(folder, sizeof(folder), target);
    snprintf(a->dir, sizeof(a->dir), "%s", folder);
    proceed(a);
}
/* A name chosen in the Open or Save As dialog (the toolkit joins it with the folder). */
static void file_chosen(struct cn_app *a, int save, const char *chosen)
{
    char target[CN_PATH], folder[CN_PATH], with[CN_PATH + 8]; struct cn_stat st;
    if (cn_resolve(target, "/", chosen)) { failure(a, "That file name is not valid:", cn_basename(chosen), EINVAL); a->after = CN_AFTER_NONE; return; }
    folder_of(folder, sizeof(folder), target);
    snprintf(a->dir, sizeof(a->dir), "%s", folder);
    const char *base = cn_basename(target);
    if (cn_has_wildcard(base)) {        /* a pattern in the name box is a filter, as in classic file dialogs */
        snprintf(a->filter, sizeof(a->filter), "%s", base);
        file_dialog(a, save, "");
        return;
    }
    int r = a->fs->stat(a->fs->ctx, target, &st);
    int default_extension = !cn_has_extension(base) && !strcmp(a->filter, TEXT_FILTER);
    strcpy(a->filter, TEXT_FILTER);      /* a typed filter lasts for one dialog */
    if (!save) {
        if (r == -ENOENT && default_extension) {
            snprintf(with, sizeof(with), "%s.txt", target);
            if (!cn_resolve(target, "/", with)) r = a->fs->stat(a->fs->ctx, target, &st);
        }
        if (!r && st.dir) r = -EISDIR;
        if (r) { failure(a, r == -ENOENT ? "The file was not found:" : "The file could not be opened:", cn_basename(target), r); a->after = CN_AFTER_NONE; return; }
        r = load_file(a, target);
        if (r) { failure(a, "The file could not be opened:", cn_basename(target), r); a->after = CN_AFTER_NONE; return; }
        checkpoint(a, CN_CP_OPENED);
        a->after = CN_AFTER_NONE;
        return;
    }
    if (default_extension) {
        snprintf(with, sizeof(with), "%s.txt", target);
        if (!cn_resolve(target, "/", with)) r = a->fs->stat(a->fs->ctx, target, &st);
    }
    if (!r && st.dir) { failure(a, "That name is a folder:", cn_basename(target), EISDIR); a->after = CN_AFTER_NONE; return; }
    if (!r) { /* the target exists: ask before replacing it */
        char shown[40]; shorten(shown, sizeof(shown), cn_basename(target), 36);
        snprintf(a->pending, sizeof(a->pending), "%s", target);
        box(a, D_REPLACE, "Save As", DU_DLG_ICON_WARNING, yes_no, 2, 1, 1,
            "%s already exists.\n\nDo you want to replace it?", shown);
        return;
    }
    save_as(a, target);
}

/* ---- commands and events ------------------------------------------------ */

static void goto_line(struct cn_app *a, const char *text)
{
    char *end; unsigned long line = strtoul(text, &end, 10);
    if (!text[0] || *end || strlen(text) > 9 || du_ta_goto_line(&a->ta, (unsigned)line))
        box(a, D_INFO, "Go To Line", DU_DLG_ICON_INFO, NULL, 0, 0, 0, "The line number is beyond the total number of lines.");
}
static void menu_command(struct cn_app *a, int id)
{
    switch (id) {
    case M_NEW: guard(a, CN_AFTER_NEW); break;
    case M_OPEN: guard(a, CN_AFTER_OPEN); break;
    case M_SAVE: command_save(a); break;
    case M_SAVEAS: file_dialog(a, 1, ""); break;
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
        if (!a->ta.wrap) {
            unsigned line, col; char now[16]; du_ta_line_col(&a->ta, a->ta.cursor, &line, &col); snprintf(now, sizeof(now), "%u", line);
            du_input_box(a->ui, D_GOTO, "Go To Line", "Line number:", now);
        }
        break;
    case M_SELALL: du_ta_select_all(&a->ta); break;
    case M_TIME: insert_time(a); after_edit(a); break;
    case M_WRAP: du_ta_set_wrap(&a->ta, !a->ta.wrap); cn_layout(a); break;
    case M_FONT:
        box(a, D_FONT, "Font", DU_DLG_ICON_INFO, font_buttons, 3, a->ta.bold ? 1 : 0, 2,
            "Font:\tCiukiOS bitmap font, 5x7 glyphs\nStyle:\t%s\n---\nOther faces and sizes are not available.", a->ta.bold ? "Bold" : "Regular");
        break;
    case M_STATUS: a->show_status = !a->show_status; cn_layout(a); break;
    case M_HELP:
        box(a, D_INFO, "CiukNote Help", DU_DLG_ICON_INFO, NULL, 0, 0, 0,
            "File: Ctrl+N New, Ctrl+O Open, Ctrl+S Save\n"
            "Edit: Ctrl+Z Undo, Ctrl+X Cut, Ctrl+C Copy, Ctrl+V Paste\n"
            "Ctrl+A Select all, F5 Time and date\n"
            "Search: Ctrl+F Find, F3 Find next, Ctrl+H Replace, Ctrl+G Go to\n"
            "Menus: F10 or Alt+letter\n---\nDouble-click selects a word. Limit " CN_LIMIT_TEXT " bytes.\n"
            "In a file dialog a pattern such as *.* shows other files.");
        break;
    case M_ABOUT:
        box(a, D_INFO, "About CiukNote", DU_DLG_ICON_INFO, NULL, 0, 0, 0,
            "CiukiOS CiukNote\nClassic computing, one woof at a time.\n---\nDedicated to Ciuki.\nTango 0.8.90 icons: Public Domain.\n"
            "Ciuki portrait: separate project asset.\nlibdeskui and CiukNote: MIT licence.");
        if (a->ciuki) du_dialog_icon(a->ui, a->ciuki32);
        break;
    default: break;
    }
}
static void widget_command(struct cn_app *a, int id)
{
    switch (id) {
    case W_FNEXT: find_next(a); break;
    case W_FDIR: a->find_up = !a->find_up; du_set_text(W(a, W_FDIR), a->find_up ? "&Up" : "&Down"); break;
    case W_FCASE: a->match_case = !a->match_case; du_set_text(W(a, W_FCASE), a->match_case ? "[x] Match &case" : "[ ] Match &case"); break;
    case W_FCLOSE: hide_find(a); break;
    case W_RONE: replace_one(a); after_edit(a); break;
    case W_RALL:
        replace_all(a);
        if (a->ta.limit_hit) limit_message(a, 1);
        break;
    default: break;
    }
}
static void dialog_result(struct cn_app *a, int id, int index, const char *text)
{
    switch (id) {
    case D_CHANGES:
        if (index == 0) {
            if (!a->named) { file_dialog(a, 1, ""); return; }
            int r = save_file(a, a->path);
            if (r) { a->after = CN_AFTER_NONE; failure(a, "The file could not be saved:", document_name(a), r); }
            else proceed(a);
        } else if (index == 1) proceed(a);
        else a->after = CN_AFTER_NONE;
        break;
    case D_OPEN: case D_SAVE:
        if (index == 0) file_chosen(a, id == D_SAVE, text);
        else { strcpy(a->filter, TEXT_FILTER); a->after = CN_AFTER_NONE; }
        break;
    case D_REPLACE:
        if (index == 0) save_as(a, a->pending);
        else file_dialog(a, 1, cn_basename(a->pending));
        break;
    case D_GOTO: if (index == 0) goto_line(a, text); break;
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

void cn_event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct cn_app *a = ctx; int id = e->id;
    switch (e->type) {
    case DU_EV_CHANGE:
        if (id == W_TEXT) after_edit(a);
        return;
    case DU_EV_CONTEXT:
        if (id == W_TEXT) { cn_refresh(a); du_popup(u, a->popup, 6, e->x, e->y, W_TEXT); }
        return;
    case DU_EV_COMMIT:
        if (id == W_FIND) find_next(a);
        else if (id == W_REPL) { replace_one(a); after_edit(a); }
        break;
    case DU_EV_CANCEL:
        if (id == W_FIND || id == W_REPL) hide_find(a);
        return;
    case DU_EV_KEY:   /* Esc on a find bar button */
        if (e->key == DU_KEY_ESC && a->find_mode && id >= W_FLABEL && id <= W_RALL) hide_find(a);
        return;
    case DU_EV_DIALOG: dialog_result(a, id, e->index, e->text); break;
    case DU_EV_ACTIVATE:
        if (id >= W_TEXT) widget_command(a, id); else menu_command(a, id);
        break;
    default: return;
    }
    cn_refresh(a);
}

/* ---- lifecycle ---------------------------------------------------------- */

int cn_attach(struct cn_app *a, struct du_ui *u)
{
    a->ui = u; u->event = cn_event; u->user = a;
    if (du_ta_init(&a->ta, CN_LIMIT)) return -ENOMEM;
    a->crlf = 1; a->show_status = 1; strcpy(a->dir, "/"); strcpy(a->filter, TEXT_FILTER);
    memcpy(a->file_items, file_template, sizeof(file_template));
    memcpy(a->edit_items, edit_template, sizeof(edit_template));
    memcpy(a->format_items, format_template, sizeof(format_template));
    memcpy(a->view_items, view_template, sizeof(view_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    memcpy(a->popup, popup_template, sizeof(popup_template));
    a->menus[0] = (struct du_menu){"&File", a->file_items, sizeof(file_template) / sizeof(*file_template)};
    a->menus[1] = (struct du_menu){"&Edit", a->edit_items, sizeof(edit_template) / sizeof(*edit_template)};
    a->menus[2] = (struct du_menu){"F&ormat", a->format_items, sizeof(format_template) / sizeof(*format_template)};
    a->menus[3] = (struct du_menu){"&View", a->view_items, sizeof(view_template) / sizeof(*view_template)};
    a->menus[4] = (struct du_menu){"&Help", a->help_items, sizeof(help_template) / sizeof(*help_template)};
    u->menus = a->menus; u->menu_count = 5;
    struct du_rect zero = {0, 0, 0, 0};
    struct du_widget *text = du_add(u, DU_TEXTAREA, W_TEXT, zero, "");
    if (!text) return -ENOMEM;
    text->data = &a->ta;
    du_add(u, DU_STATUS, W_STATUS, zero, "");
    du_add(u, DU_LABEL, W_FLABEL, zero, "Find &what:"); du_add(u, DU_EDIT, W_FIND, zero, "");
    du_add(u, DU_BUTTON, W_FNEXT, zero, "Find &Next"); du_add(u, DU_BUTTON, W_FDIR, zero, "&Down");
    du_add(u, DU_BUTTON, W_FCASE, zero, "[ ] Match &case"); du_add(u, DU_BUTTON, W_FCLOSE, zero, "X");
    du_add(u, DU_LABEL, W_RLABEL, zero, "Replace &with:"); du_add(u, DU_EDIT, W_REPL, zero, "");
    du_add(u, DU_BUTTON, W_RONE, zero, "&Replace"); du_add(u, DU_BUTTON, W_RALL, zero, "Replace &All");
    if (a->portrait) {
        uint32_t *pixels = malloc(256 * 256 * 4);
        if (pixels && !a->portrait(pixels)) { shrink32(pixels, a->ciuki32); a->ciuki = 1; }
        free(pixels);
    }
    cn_layout(a);
    du_focus(u, W_TEXT);
    cn_refresh(a);
    checkpoint(a, CN_CP_READY);
    return 0;
}
void cn_detach(struct cn_app *a) { du_ta_free(&a->ta); }

int cn_open_argument(struct cn_app *a, const char *path)
{
    char target[CN_PATH]; struct cn_stat st;
    int r = cn_resolve(target, "/", path);
    if (r) { failure(a, "The file could not be opened:", path, r); return r; }
    r = a->fs->stat(a->fs->ctx, target, &st);
    if (r == -ENOENT) {
        snprintf(a->pending, sizeof(a->pending), "%s", target);
        char shown[40]; shorten(shown, sizeof(shown), cn_basename(target), 32);
        box(a, D_CREATE, "CiukNote", DU_DLG_ICON_QUESTION, yes_no, 2, 0, 1,
            "Cannot find the %s file.\n\nDo you want to create a new file?", shown);
        return r;
    }
    if (!r && st.dir) r = -EISDIR;
    if (!r) r = load_file(a, target);
    if (r) failure(a, "The file could not be opened:", cn_basename(target), r);
    else checkpoint(a, CN_CP_OPENED);
    cn_refresh(a);
    return r;
}
