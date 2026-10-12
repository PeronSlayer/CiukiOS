/* SPDX-License-Identifier: MIT
 * f2-32: CiukNote application logic against a fake file system, driven through
 * the production libdeskui input path (du_input) and renderer.
 *
 *   ciuknote_test                 run every scenario
 *   ciuknote_test <prefix>        also write rendered screens as <prefix>-<name>.xrgb
 *   ciuknote_test replay <script> replay a key script (K <set-1 code> <0|1> per line) in test mode
 */
#include "app.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake file system --------------------------------------------------- */

struct node { char path[CN_PATH]; int dir; char *data; size_t size; int64_t mtime; };
static struct node nodes[96]; static unsigned node_count;
static int fail_save, fail_load, fail_list, saves;
static struct node *find(const char *path) { for (unsigned i = 0; i < node_count; i++) if (!strcmp(nodes[i].path, path)) return &nodes[i]; return NULL; }
static void add_dir(const char *path) { snprintf(nodes[node_count].path, CN_PATH, "%s", path); nodes[node_count++].dir = 1; }
static void add_file(const char *path, const void *data, size_t size, int64_t mtime)
{
    struct node *n = &nodes[node_count++]; snprintf(n->path, CN_PATH, "%s", path);
    n->data = malloc(size + 1); memcpy(n->data, data, size); n->size = size; n->mtime = mtime;
}
static void fs_reset(void)
{
    for (unsigned i = 0; i < node_count; i++) { free(nodes[i].data); memset(&nodes[i], 0, sizeof(nodes[i])); }
    node_count = 0; fail_save = fail_load = fail_list = saves = 0;
    add_dir("/home"); add_dir("/system"); add_dir("/system/tests"); add_dir("/system/tests/sub"); add_dir("/bin");
    add_file("/system/tests/beta.txt", "lf only\nsecond\n", 15, 1791000000);
    add_file("/system/tests/Alpha.TXT", "Alpha\r\nline two\r\n", 17, 1790000000);
    add_file("/system/tests/data.bin", "\1\2\3", 3, 1792000000);
    add_file("/system/hello.txt", "hello", 5, 1791500000);
    add_file("/bin/ciuknote", "ELF", 3, 1);
}
static int f_stat(void *ctx, const char *path, struct cn_stat *st)
{
    (void)ctx; if (!strcmp(path, "/")) { *st = (struct cn_stat){1, 0, 0}; return 0; }
    struct node *n = find(path); if (!n) return -ENOENT;
    *st = (struct cn_stat){n->dir, n->size, n->mtime}; return 0;
}
static int f_list(void *ctx, const char *dir, cn_emit emit, void *arg)
{
    (void)ctx; if (fail_list) return fail_list;
    if (strcmp(dir, "/") && !(find(dir) && find(dir)->dir)) return -ENOENT;
    for (unsigned i = 0; i < node_count; i++) {
        char parent[CN_PATH]; snprintf(parent, sizeof(parent), "%s", nodes[i].path);
        char *slash = strrchr(parent, '/'); if (slash == parent) parent[1] = 0; else *slash = 0;
        if (strcmp(parent, dir)) continue;
        struct cn_stat st = {nodes[i].dir, nodes[i].size, nodes[i].mtime};
        int r = emit(strrchr(nodes[i].path, '/') + 1, &st, arg); if (r) return r;
    }
    return 0;
}
static int f_load(void *ctx, const char *path, char *buf, size_t cap, size_t *len)
{
    (void)ctx; if (fail_load) return fail_load;
    struct node *n = find(path); if (!n) return -ENOENT; if (n->dir) return -EISDIR;
    if (n->size > cap) return -EFBIG;
    memcpy(buf, n->data, n->size); *len = n->size; return 0;
}
static int f_save(void *ctx, const char *path, const char *data, size_t len)
{
    (void)ctx; if (fail_save) return fail_save;
    char parent[CN_PATH]; snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/'); if (slash == parent) parent[1] = 0; else *slash = 0;
    if (strcmp(parent, "/") && !(find(parent) && find(parent)->dir)) return -ENOENT;
    struct node *n = find(path);
    if (n && n->dir) return -EISDIR;
    if (!n) { n = &nodes[node_count++]; snprintf(n->path, CN_PATH, "%s", path); }
    free(n->data); n->data = malloc(len + 1); memcpy(n->data, data, len); n->size = len; n->mtime = 1791711660; saves++;
    return 0;
}
static const struct cn_fs fake_fs = {NULL, f_stat, f_list, f_load, f_save};
static int expect_bytes(const char *path, const char *content, size_t size)
{ struct node *n = find(path); return n && !n->dir && n->size == size && !memcmp(n->data, content, size); }
static int expect_file(const char *path, const char *content) { return expect_bytes(path, content, strlen(content)); }

/* ---- host ---------------------------------------------------------------- */

#define NW 640
#define NH 440   /* the window size of --test runs */
static uint32_t canvas[NW * NH];
static struct du_ui ui; static struct cn_app app;
static time_t clock_seconds = 1791711660; static uint64_t mono_ns = 5000000000ull; static int quit_count;
static int cp_pending, cp_sent, portrait_loaded;
static time_t host_now(void *p) { (void)p; return clock_seconds; }
static void host_quit(void *p) { (void)p; quit_count++; }
static void host_checkpoint(struct cn_app *a, int stage) { (void)a; cp_pending = stage; }
static int host_portrait(uint32_t *pixels)
{ for (unsigned i = 0; i < 256 * 256; i++) pixels[i] = (i * 2654435761u) & 0xffffffu; portrait_loaded++; return 0; }
static const char *dump_prefix;

static void reset(int test_mode)
{
    if (app.ui) cn_detach(&app);
    fs_reset(); du_clipboard_set(""); memset(&app, 0, sizeof(app)); memset(canvas, 0, sizeof(canvas)); quit_count = 0; mono_ns = 5000000000ull;
    clock_seconds = 1791711660; cp_pending = cp_sent = 0;
    du_init(&ui, canvas, NW, NH);
    app.fs = &fake_fs; app.now = host_now; app.quit = host_quit; app.portrait = host_portrait; app.test = test_mode;
    if (test_mode) app.checkpoint = host_checkpoint;
    assert(!cn_attach(&app, &ui));
}
static void ev(int type, int code, int value, int value2)
{
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = mono_ns};
    du_input(&ui, &e);
}
static void settle(void) { if (ui.dirty) du_render(&ui); }
/* The kernel's unshifted text for a key (src/kernel/drivers/i8042.c: us[]); the toolkit ignores TEXT. */
static int unshifted(int code)
{
    static const char us[0x59] = {
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
        [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
        [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\r',[0x1E]='a',[0x1F]='s',[0x20]='d',
        [0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]=39,[0x29]='`',[0x2B]=92,
        [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '};
    return code >= 0 && code < 0x59 ? us[code] : 0;
}
static void key_event(int code, int value)
{
    ev(CIUKI_INPUT_KEY, code, value, 0);
    if (value == 1 && unshifted(code)) ev(CIUKI_INPUT_TEXT, unshifted(code), 0, 0);
}
static void press(int code) { key_event(code, 1); key_event(code, 0); settle(); }
static void chord(int modifier, int code) { key_event(modifier, 1); press(code); key_event(modifier, 0); settle(); }
#define CTRL(c) chord(0x1d, c)
static void alt(int code) { key_event(0x38, 1); press(code); key_event(0x38, 0); settle(); }
/* Alt+mnemonic opens a menu with its first item selected; Down walks (separators are skipped); Enter chooses. */
static void menu_pick(int mnemonic, int index) { alt(mnemonic); for (int i = 0; i < index; i++) press(0x150); press(0x1c); }
#define ENTER press(0x1c)
#define ESC press(0x01)
static int code_of(char c, int *shift)
{
    static const char rows[] = "qwertyuiop\0asdfghjkl\0zxcvbnm";
    static const int base[] = {0x10, 0x1e, 0x2c};
    static const char digits[] = "1234567890", plain[] = "-=[];',./`\\ ", shifted[] = "_+{}:\"<>?~|";
    static const int plain_codes[] = {0x0c, 0x0d, 0x1a, 0x1b, 0x27, 0x28, 0x33, 0x34, 0x35, 0x29, 0x2b, 0x39};
    static const char sym[] = "!@#$%^&*()";
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c + 32); }
    if (c >= 'a' && c <= 'z') {
        int row = 0; const char *p = rows;
        for (; row < 3; row++) { const char *at = strchr(p, c); if (at && (at - p) < (int)strlen(p) ) return base[row] + (int)(at - p); p += strlen(p) + 1; }
    }
    const char *at = strchr(digits, c); if (c && at) return 0x02 + (int)(at - digits);
    at = strchr(sym, c); if (c && at) { *shift = 1; return 0x02 + (int)(at - sym); }
    at = strchr(plain, c); if (c && at) return plain_codes[at - plain];
    at = strchr(shifted, c); if (c && at) { *shift = 1; return plain_codes[at - shifted]; }
    return 0;
}
static void type(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') { press(0x1c); continue; }
        if (*s == '\t') { press(0x0f); continue; }
        int shift, code = code_of(*s, &shift); assert(code);
        if (shift) key_event(0x2a, 1);
        key_event(code, 1); key_event(code, 0);
        if (shift) key_event(0x2a, 0);
    }
    settle();
}
static void mouse(int x, int y) { ev(CIUKI_INPUT_MOTION, 0, x, y); }
static void click_button(int button, int x, int y) { mouse(x, y); ev(CIUKI_INPUT_BUTTON, button, 1, 0); ev(CIUKI_INPUT_BUTTON, button, 0, 0); settle(); }
static void click(int x, int y) { click_button(1, x, y); }
static void screen(const char *name)
{
    settle();
    if (!dump_prefix) return;
    char path[1040]; snprintf(path, sizeof(path), "%s-%s.xrgb", dump_prefix, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(canvas, 4, sizeof(canvas) / 4, f) == sizeof(canvas) / 4); fclose(f);
}
static struct du_widget *W(int id) { return du_get(&ui, id); }
static int has_text(const char *s) { return app.ta.len == strlen(s) && !memcmp(app.ta.text, s, app.ta.len); }
/* The status bar holds "<document>\t<Ln, Col>". */
static char name_text[256];
static const char *status_name(void) { snprintf(name_text, sizeof(name_text), "%s", W(W_STATUS)->text); *strchr(name_text, '\t') = 0; return name_text; }
static const char *status_pos(void) { return strchr(W(W_STATUS)->text, '\t') + 1; }
static int dialog_is(int id) { return ui.dialog.kind && ui.dialog.id == id; }
static int widget_visible(int id) { return !W(id)->hidden; }
static int focus_is(int id) { return du_focused(&ui) == id; }
static void select_range(size_t a, size_t b) { du_ta_select(&app.ta, a, b); cn_refresh(&app); settle(); }
static void fresh_text(const char *s) { du_ta_select_all(&app.ta); du_ta_insert(&app.ta, s, strlen(s), DU_TA_EDIT_OTHER); cn_refresh(&app); settle(); }
static int centre_x(int id) { struct du_rect r = W(id)->box; return r.x + r.w / 2; }
static int centre_y(int id) { struct du_rect r = W(id)->box; return r.y + r.h / 2; }
static void click_widget(int id) { click(centre_x(id), centre_y(id)); }
/* Text area geometry: cell (col, line) of the first visible line. */
static int cell_x(int col) { return W(W_TEXT)->box.x + 4 + col * DU_TA_CELL_W + 1; }
static int cell_y(int line) { return W(W_TEXT)->box.y + 4 + line * DU_TA_CELL_H + 4; }

/* ---- scenarios ----------------------------------------------------------- */

static void test_startup(void)
{
    reset(1);
    assert(ui.menu_count == 5 && widget_visible(W_TEXT) && !widget_visible(W_FIND) && !widget_visible(W_REPL) && focus_is(W_TEXT));
    assert(!strcmp(status_name(), "Untitled") && !strcmp(status_pos(), "Ln 1, Col 1"));
    assert(app.stage == CN_CP_READY && cp_pending == CN_CP_READY && portrait_loaded == 1 && app.ciuki);
    struct du_rect t = W(W_TEXT)->box;
    assert(t.x == 0 && t.y == 21 && t.w == NW && t.h == NH - 21 - 20 && app.ta.cols == 102 && app.ta.rows == 28);
    static const char *const titles[] = {"&File", "&Edit", "F&ormat", "&View", "&Help"};
    for (unsigned m = 0; m < 5; m++) {
        assert(!strcmp(ui.menus[m].label, titles[m]));
        struct du_rect title = du_menu_title(&ui, m), drop = du_menu_box(&ui, ui.menus[m].items, ui.menus[m].count, title.x, title.y + title.h);
        assert(drop.x >= 0 && drop.y + drop.h <= NH && drop.x + drop.w <= NW);
    }
    /* Accelerators carry the exact keys the directive names: F3 find next, F5 time/date, Ctrl+Z undo. */
    int f3 = 0, f5 = 0, undo = 0;
    for (unsigned i = 0; i < ui.menus[1].count; i++) {
        const struct du_menu_item *it = &ui.menus[1].items[i];
        f3 |= it->id == M_FINDNEXT && it->key == 0x3d && !it->modifiers; f5 |= it->id == M_TIME && it->key == DU_KEY_F5 && !it->modifiers;
        undo |= it->id == M_UNDO && it->key == 0x2c && it->modifiers == DU_CTRL;
    }
    assert(f3 && f5 && undo);
    screen("empty");
}

static void test_typing(void)
{
    reset(0);
    type("Hello, World! 123 #a-z_A+Z=[x]{y}|:;\"'<>?/\\`~"); assert(has_text("Hello, World! 123 #a-z_A+Z=[x]{y}|:;\"'<>?/\\`~"));
    assert(app.ta.modified && !strcmp(status_name(), "Untitled *"));
    fresh_text(""); press(0x3a); type("abc 1"); assert(has_text("ABC 1")); press(0x3a); type("d"); assert(has_text("ABC 1d"));
    fresh_text("");
    key_event(0x2a, 1); press(0x3a); key_event(0x2a, 0); assert(ui.caps == 1);   /* Caps Lock toggles on its own press */
    press(0x3a); assert(ui.caps == 0);
    type("a\tb\nc"); assert(has_text("a\tb\nc")); assert(!strcmp(status_pos(), "Ln 2, Col 2"));
    /* Backspace, Delete, arrows, Home/End, Enter. */
    fresh_text("abcdef"); press(0x147); press(0x14d); press(0x14d); press(0x153); assert(has_text("abdef"));
    press(0x0e); assert(has_text("adef")); press(0x14f); press(0x0e); assert(has_text("ade")); assert(!strcmp(status_pos(), "Ln 1, Col 4"));
    key_event(0x2a, 1); press(0x147); key_event(0x2a, 0); assert(du_ta_has_selection(&app.ta)); type("Z"); assert(has_text("Z"));
    /* Non-ASCII file content stays intact and is shown as placeholders. */
    fresh_text("caf\xc3\xa9 \xe7\x8a\xac"); assert(app.ta.len == 9); press(0x14f); press(0x0e); assert(has_text("caf\xc3\xa9 "));
}

static void test_clipboard_and_undo(void)
{
    reset(0);
    type("one two three"); assert(du_ta_can_undo(&app.ta));
    CTRL(0x2c); assert(has_text("")); CTRL(0x2c); assert(has_text("one two three"));            /* undo toggles */
    CTRL(0x1e); assert(du_ta_has_selection(&app.ta)); CTRL(0x2d); assert(has_text(""));         /* Ctrl+A, Ctrl+X */
    CTRL(0x2f); assert(has_text("one two three"));                                              /* Ctrl+V */
    select_range(4, 7); CTRL(0x2e); assert(!strcmp(du_clipboard_get(), "two"));
    press(0x14f); CTRL(0x2f); assert(has_text("one two threetwo"));
    CTRL(0x2c); assert(has_text("one two three"));
    /* Delete: with a selection the accelerator fires, without it the key deletes forward. */
    select_range(0, 4); assert(!(app.edit_items[5].flags & DU_ITEM_DISABLED));
    press(0x153); assert(has_text("two three"));
    press(0x153); assert(has_text("wo three"));
    /* The clipboard holds a whole document, not the 1 KiB edit-box mirror. */
    char big[4000]; memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    reset(0); fresh_text(big); CTRL(0x1e); CTRL(0x2e); assert(strlen(du_clipboard_get()) == DU_TEXT_MAX - 1);
    press(0x14f); CTRL(0x2f); assert(app.ta.len == 2 * (sizeof(big) - 1));
    /* A string copied in an edit box wins over the document clipboard. */
    du_clipboard_set("from-elsewhere"); CTRL(0x2f); assert(app.ta.len == 2 * (sizeof(big) - 1) + 14);
    /* Menu enable states. */
    reset(0); cn_refresh(&app);
    assert((app.edit_items[0].flags & DU_ITEM_DISABLED) && (app.edit_items[2].flags & DU_ITEM_DISABLED) &&
           (app.edit_items[3].flags & DU_ITEM_DISABLED) && (app.edit_items[5].flags & DU_ITEM_DISABLED) && (app.edit_items[4].flags & DU_ITEM_DISABLED));
    type("ab"); select_range(0, 2);
    assert(!(app.edit_items[0].flags & DU_ITEM_DISABLED) && !(app.edit_items[2].flags & DU_ITEM_DISABLED) && !(app.edit_items[5].flags & DU_ITEM_DISABLED));
}

static void test_save_and_open(void)
{
    reset(1);
    type("Hello\nWorld"); assert(app.ta.modified);
    CTRL(0x1f);                                         /* Ctrl+S on an untitled document: the Save As dialog */
    assert(dialog_is(D_SAVE) && ui.dialog.kind == DU_DLG_SAVE && !strcmp(ui.dialog.title, "Save As") && !strcmp(ui.dialog.folder, "/"));
    assert(!strcmp(ui.dialog.items[5].text, "") && ui.dialog.focus == 5 && !strcmp(ui.dialog.items[0].text, "&Save"));
    type("/system/tests/note.txt"); ENTER;
    assert(!ui.dialog.kind && app.named && !strcmp(app.path, "/system/tests/note.txt") && !app.ta.modified);
    assert(expect_file("/system/tests/note.txt", "Hello\r\nWorld") && saves == 1);   /* new documents use CRLF */
    assert(!strcmp(status_name(), "note.txt") && app.stage == CN_CP_SAVED && cp_pending == CN_CP_SAVED && !strcmp(app.dir, "/system/tests"));
    /* Save again without a dialog; edits are written through the temporary replacement. */
    type("!"); assert(!strcmp(status_name(), "note.txt *")); CTRL(0x1f); assert(!ui.dialog.kind && expect_file("/system/tests/note.txt", "Hello\r\nWorld!") && saves == 2);
    /* New, then Open it back. */
    CTRL(0x31); assert(has_text("") && !app.named);
    CTRL(0x18); assert(dialog_is(D_OPEN) && !strcmp(ui.dialog.folder, "/system/tests") && !strcmp(ui.dialog.title, "Open"));
    /* The list shows folders and text files only: sub, Alpha.TXT, beta.txt, note.txt (data.bin is filtered out). */
    assert(ui.dialog.entry_count == 4 && ui.dialog.entries[0].directory && !strcmp(ui.dialog.entries[0].name, "sub"));
    type("/system/tests/note.txt"); ENTER;
    assert(!ui.dialog.kind && has_text("Hello\nWorld!") && app.named && !app.ta.modified && app.crlf && app.stage == CN_CP_OPENED);
    /* File > Exit through F10, Up (wraps to the last item), Enter. */
    press(0x44); press(0x148); ENTER; assert(quit_count == 1);
    reset(0); fresh_text("x"); app.ta.modified = 0; alt(0x21); assert(ui.menu == 0); press(0x2d); assert(quit_count == 1);   /* Alt+F, X */
}

static void test_unsaved_prompt(void)
{
    reset(0);
    type("draft"); CTRL(0x31);                           /* New with unsaved text */
    assert(dialog_is(D_CHANGES) && ui.dialog.buttons == 3 && strstr(ui.dialog.text, "The text in the Untitled file has changed.") &&
           strstr(ui.dialog.text, "Do you want to save the changes?") && ui.dialog.icon == DU_DLG_ICON_QUESTION);
    ESC; assert(!ui.dialog.kind && has_text("draft") && app.after == CN_AFTER_NONE);   /* Cancel */
    CTRL(0x31); press(0x0f); ENTER; assert(!ui.dialog.kind && has_text(""));           /* Tab to No, Enter: discard */
    type("keep"); CTRL(0x31); ENTER;                                                   /* Yes: untitled, so Save As */
    assert(dialog_is(D_SAVE) && app.after == CN_AFTER_NEW);
    type("kept"); ENTER;                                                               /* default extension, then New */
    assert(expect_file("/kept.txt", "keep") && !ui.dialog.kind && has_text("") && !app.named);
    /* Mnemonics: N answers No. Open with unsaved changes: No proceeds to the Open dialog; Cancel there leaves the text. */
    type("more"); CTRL(0x18); press(0x31); assert(dialog_is(D_OPEN));                 /* "&No" is key N */
    ESC; assert(!ui.dialog.kind && has_text("more") && app.after == CN_AFTER_NONE);
    /* Exit asks too; Yes on a named document saves and quits. */
    CTRL(0x1f); type("/exit-test.txt"); ENTER; assert(app.named && !app.ta.modified);
    type("?"); press(0x44); press(0x148); ENTER; assert(dialog_is(D_CHANGES) && quit_count == 0);
    ENTER; assert(quit_count == 1 && !app.ta.modified && expect_file("/exit-test.txt", "more?"));
    /* The window's close request: refuse while unsaved, accept when clean. */
    reset(0); assert(cn_close_request(&app) == 1);
    type("x"); assert(cn_close_request(&app) == 0 && dialog_is(D_CHANGES) && app.after == CN_AFTER_EXIT);
    press(0x0f); ENTER; assert(quit_count == 1);                                       /* No: quit */
    /* A long name is shortened in the prompt. */
    reset(0); app.named = 1; snprintf(app.path, CN_PATH, "/a-very-long-document-name-indeed.txt"); type("x");
    assert(cn_close_request(&app) == 0 && strstr(ui.dialog.text, "a-very-long-document-...") && !strstr(ui.dialog.text, "indeed"));
}

static void test_save_errors_and_replace(void)
{
    reset(0);
    type("data"); CTRL(0x1f); type("/system/hello"); ENTER;                    /* default extension -> hello.txt exists */
    assert(dialog_is(D_REPLACE) && strstr(ui.dialog.text, "hello.txt already exists.") && ui.dialog.focus == 1);
    ENTER;                                                                      /* the default is No: back to the dialog */
    assert(dialog_is(D_SAVE) && !strcmp(ui.dialog.items[5].text, "hello.txt") && expect_file("/system/hello.txt", "hello"));
    ENTER; assert(dialog_is(D_REPLACE)); press(0x15); assert(!ui.dialog.kind);  /* "&Yes" replaces */
    assert(expect_file("/system/hello.txt", "data") && !strcmp(app.path, "/system/hello.txt") && !app.ta.modified);
    /* Failures leave the document modified and report the error; the dialog comes back with the name. */
    reset(0); type("x"); CTRL(0x1f); type("/lost.txt"); fail_save = -ENOSPC; ENTER;
    assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "could not be saved") && strstr(ui.dialog.text, "error 28") && app.ta.modified && !app.named);
    ENTER; fail_save = 0; assert(!ui.dialog.kind); CTRL(0x1f); type("/lost.txt"); ENTER; assert(!ui.dialog.kind && expect_file("/lost.txt", "x"));
    /* A missing folder fails and keeps the document modified. */
    reset(0); type("x"); CTRL(0x1f); type("/nowhere/file.txt"); ENTER; assert(dialog_is(D_INFO) && app.ta.modified); ENTER;
    /* A folder name navigates instead of saving. */
    reset(0); type("x"); CTRL(0x1f); type("system"); ENTER; assert(dialog_is(D_SAVE) && !strcmp(ui.dialog.folder, "/system") && !ui.dialog.items[5].text[0]);
    type("n.txt"); ENTER; assert(expect_file("/system/n.txt", "x"));
}

static void test_find_replace(void)
{
    reset(0);
    fresh_text("The cat and the CAT sat; the End"); select_range(0, 0);
    CTRL(0x21); assert(app.find_mode == 1 && widget_visible(W_FIND) && !widget_visible(W_REPL) && focus_is(W_FIND));
    assert(W(W_TEXT)->box.h == NH - 21 - 20 - 30);
    type("the"); ENTER; size_t a, b;
    du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3);
    ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 25 && b == 28);
    ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Cannot find \"the\"")); ENTER; assert(!ui.dialog.kind);
    assert(!strcmp(W(W_FIND)->text, "the") && !strcmp(status_pos(), "Ln 1, Col 29"));
    /* Match case through its button, direction through the other. */
    click_widget(W_FCASE); assert(app.match_case && !strcmp(W(W_FCASE)->text, "[x] Match &case"));
    select_range(0, 0); du_focus(&ui, W_FIND); ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    click_widget(W_FDIR); assert(app.find_up && !strcmp(W(W_FDIR)->text, "&Up"));
    click_widget(W_FNEXT); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Cannot find \"the\"")); ENTER;   /* nothing before it, case-sensitive */
    click_widget(W_FCASE); assert(!app.match_case);
    click_widget(W_FNEXT); du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3 && app.ta.cursor == 0);   /* Up finds "The" */
    click_widget(W_FDIR); assert(!app.find_up);
    /* Esc closes the bar from a button, F3 repeats from the text area; the needle survives. */
    ESC; assert(app.find_mode == 0 && !widget_visible(W_FIND) && focus_is(W_TEXT));
    select_range(0, 0); press(0x3d); du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3);
    press(0x3d); du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    /* Ctrl+F picks up a single-line selection as the needle; Esc in the edit closes the bar. */
    select_range(4, 7); CTRL(0x21); assert(!strcmp(W(W_FIND)->text, "cat")); ESC; assert(app.find_mode == 0 && focus_is(W_TEXT));
    /* Find with nothing typed asks for a needle. */
    reset(0); press(0x3d); assert(app.find_mode == 1 && focus_is(W_FIND));
    /* Replace and Replace All through the bar. */
    reset(0);
    fresh_text("The cat and the CAT sat; the End"); select_range(0, 0);
    CTRL(0x23); assert(app.find_mode == 2 && widget_visible(W_REPL) && W(W_TEXT)->box.h == NH - 21 - 20 - 58);
    type("the"); click_widget(W_REPL); type("a");
    click_widget(W_RONE);                                  /* Replace: nothing selected, so it finds the first match */
    du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3 && has_text("The cat and the CAT sat; the End"));
    click_widget(W_RONE); assert(has_text("a cat and the CAT sat; the End"));          /* replaces, then selects the next */
    du_ta_selection(&app.ta, &a, &b); assert(a == 10 && b == 13);
    click_widget(W_RALL); assert(has_text("a cat and a CAT sat; a End") && !strcmp(app.note, "Replaced 2 occurrences"));
    assert(strstr(W(W_STATUS)->text, "Replaced 2 occurrences")); du_focus(&ui, W_TEXT); CTRL(0x2c); assert(has_text("a cat and the CAT sat; the End"));
    reset(0); fresh_text("aaa"); CTRL(0x23); type("a"); click_widget(W_REPL); type("bb"); click_widget(W_RALL);
    assert(has_text("bbbbbb") && !strcmp(app.note, "Replaced 3 occurrences")); du_focus(&ui, W_TEXT); CTRL(0x2c); assert(has_text("aaa"));
    reset(0); fresh_text("xyz"); CTRL(0x23); type("q"); click_widget(W_RALL); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Cannot find \"q\"")); ENTER;
    /* Replace All beyond the limit changes nothing and says so. */
    reset(0); char big[CN_LIMIT + 1]; memset(big, 'a', CN_LIMIT); big[CN_LIMIT] = 0; fresh_text(big);
    CTRL(0x23); type("a"); click_widget(W_REPL); type("bb"); click_widget(W_RALL);
    assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "would be exceeded") && app.ta.len == CN_LIMIT && app.ta.text[0] == 'a'); ENTER;
    /* Enter in the replace edit replaces; Tab and Shift+Tab walk the bar. */
    reset(0); fresh_text("one two"); select_range(0, 0); CTRL(0x23); type("one"); click_widget(W_REPL); type("1"); ENTER; ENTER;
    assert(has_text("1 two"));
    reset(0); CTRL(0x23); assert(focus_is(W_FIND));
    for (int i = 0; i < 5; i++) press(0x0f);
    assert(focus_is(W_REPL));
    key_event(0x2a, 1); press(0x0f); key_event(0x2a, 0); assert(focus_is(W_FCLOSE));
    press(0x0f); press(0x0f); assert(focus_is(W_RONE)); click_widget(W_FCLOSE); assert(app.find_mode == 0 && focus_is(W_TEXT));
    CTRL(0x23); screen("replace-bar");
    /* Alt+letter presses a bar button: Alt+N is Find Next. */
    reset(0); fresh_text("find me"); select_range(0, 0); CTRL(0x21); type("me"); alt(0x31); du_ta_selection(&app.ta, &a, &b); assert(a == 5 && b == 7);
}

static void test_goto_wrap_status(void)
{
    reset(0);
    fresh_text("l1\nl2\nl3\nl4"); select_range(0, 0);
    CTRL(0x22); assert(dialog_is(D_GOTO) && ui.dialog.kind == DU_DLG_INPUT && !strcmp(ui.dialog.items[2].text, "1"));
    type("3"); ENTER; assert(app.ta.cursor == 6 && !strcmp(status_pos(), "Ln 3, Col 1"));
    CTRL(0x22); type("9"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "beyond the total number of lines")); ENTER;
    CTRL(0x22); type("x"); ENTER; assert(dialog_is(D_INFO)); ENTER;
    CTRL(0x22); ESC; assert(!ui.dialog.kind && app.ta.cursor == 6);
    /* Format > Word Wrap (Alt+O, Enter) wraps, checks the item and disables Go To. */
    alt(0x18); assert(ui.menu == 2);
    screen("format-menu");
    ENTER; assert(app.ta.wrap && (app.format_items[0].flags & DU_ITEM_CHECKED) && (app.edit_items[10].flags & DU_ITEM_DISABLED));
    CTRL(0x22); assert(!ui.dialog.kind);
    reset(0); char line[400]; memset(line, 'x', 300); line[300] = 0; fresh_text(line);
    assert(du_ta_visual_lines(&app.ta) == 1);
    menu_pick(0x18, 0); assert(du_ta_visual_lines(&app.ta) > 2);
    menu_pick(0x18, 0); assert(!app.ta.wrap && du_ta_visual_lines(&app.ta) == 1 && !(app.format_items[0].flags & DU_ITEM_CHECKED));
    /* View > Status Bar toggles the status widget and the text area grows. */
    int h = W(W_TEXT)->box.h;
    alt(0x2f); assert(ui.menu == 3); ENTER;
    assert(!app.show_status && !widget_visible(W_STATUS) && W(W_TEXT)->box.h == h + 20 && !(app.view_items[0].flags & DU_ITEM_CHECKED));
    menu_pick(0x2f, 0); assert(app.show_status && widget_visible(W_STATUS) && W(W_TEXT)->box.h == h);
    /* Mnemonics: Alt+F, Alt+E and Alt+H open the matching menus; the arrow keys move across menus. */
    alt(0x21); assert(ui.menu == 0); press(0x14d); assert(ui.menu == 1); ESC; assert(ui.menu == -1);
    alt(0x23); assert(ui.menu == 4); ESC;
    press(0x44); assert(ui.menu == 0); press(0x44); assert(ui.menu == -1);
    /* Font, Help and About dialogs. */
    reset(0); fresh_text("x");
    menu_pick(0x18, 1); assert(dialog_is(D_FONT) && ui.dialog.buttons == 3 && strstr(ui.dialog.text, "Style:\tRegular"));
    press(0x0f); ENTER; assert(app.ta.bold == 1);
    menu_pick(0x18, 1); assert(strstr(ui.dialog.text, "Style:\tBold") && ui.dialog.focus == 1); ESC; assert(app.ta.bold == 1);
    screen("bold");
    menu_pick(0x23, 0); assert(dialog_is(D_INFO) && strstr(ui.dialog.title, "CiukNote Help") && strstr(ui.dialog.text, "F3 Find next")); screen("help"); ENTER;
    menu_pick(0x23, 1); assert(dialog_is(D_INFO) && !strcmp(ui.dialog.title, "About CiukNote") && strstr(ui.dialog.text, "Classic computing, one woof at a time.") &&
           strstr(ui.dialog.text, "Tango 0.8.90 icons: Public Domain.") && strstr(ui.dialog.text, "Ciuki portrait: separate project asset.") &&
           ui.dialog.icon_pixels == app.ciuki32 && app.ciuki32[0] >> 24 == 0xff);
    screen("about"); ENTER;
}

static void test_time_log_encoding(void)
{
    reset(0);
    press(0x3f); assert(has_text("9:41 AM 10/11/2026"));
    clock_seconds = 1791677100; press(0x3f); assert(has_text("9:41 AM 10/11/202612:05 AM 10/11/2026"));   /* 00:05 UTC */
    fresh_text(""); clock_seconds = 1791721800; press(0x3f); assert(has_text("12:30 PM 10/11/2026"));              /* 12:30 */
    fresh_text(""); clock_seconds = 1791763140; press(0x3f); assert(has_text("11:59 PM 10/11/2026"));              /* 23:59 */
    fresh_text(""); clock_seconds = 1791711660; press(0x3f); CTRL(0x2c); assert(has_text(""));                    /* one undo step */
    /* A first line of .LOG appends the time and date on each opening. */
    reset(0);
    add_file("/system/log.txt", ".LOG\nentry one\n", 15, 1);
    assert(!cn_open_argument(&app, "/system/log.txt"));
    assert(has_text(".LOG\nentry one\n9:41 AM 10/11/2026\n") && app.ta.modified && app.ta.cursor == app.ta.len);
    add_file("/system/log2.txt", ".LOG\r\nentry", 11, 1);
    assert(!cn_open_argument(&app, "/system/log2.txt")); assert(has_text(".LOG\nentry\n9:41 AM 10/11/2026\n") && app.crlf);
    add_file("/system/log3.txt", ".LOG", 4, 1); assert(!cn_open_argument(&app, "/system/log3.txt")); assert(has_text(".LOG\n9:41 AM 10/11/2026\n"));
    add_file("/system/log4.txt", ".LOGX\n", 6, 1); assert(!cn_open_argument(&app, "/system/log4.txt")); assert(has_text(".LOGX\n") && !app.ta.modified);
    add_file("/system/log5.txt", " .LOG\n", 6, 1); assert(!cn_open_argument(&app, "/system/log5.txt")); assert(has_text(" .LOG\n"));
    /* Line endings: CRLF files stay CRLF, LF files stay LF; new documents use CRLF. */
    reset(0); assert(!cn_open_argument(&app, "/system/tests/Alpha.TXT")); assert(has_text("Alpha\nline two\n") && app.crlf);
    CTRL(0x14f); type("x"); CTRL(0x1f); assert(expect_file("/system/tests/Alpha.TXT", "Alpha\r\nline two\r\nx"));
    reset(0); assert(!cn_open_argument(&app, "/system/tests/beta.txt")); assert(has_text("lf only\nsecond\n") && !app.crlf);
    CTRL(0x14f); type("y"); CTRL(0x1f); assert(expect_file("/system/tests/beta.txt", "lf only\nsecond\ny"));
    reset(0); add_file("/system/none.txt", "no line breaks", 14, 1); assert(!cn_open_argument(&app, "/system/none.txt")); assert(app.crlf);
    reset(0); add_file("/system/mixed.txt", "a\nb\r\nc\rd", 8, 1); assert(!cn_open_argument(&app, "/system/mixed.txt")); assert(has_text("a\nb\nc\rd") && !app.crlf);
    CTRL(0x14f); type("!"); CTRL(0x1f); assert(expect_file("/system/mixed.txt", "a\nb\nc\rd!"));
    /* A lone carriage return and a NUL byte survive as characters. */
    reset(0); add_file("/system/odd.txt", "a\0b", 3, 1); assert(!cn_open_argument(&app, "/system/odd.txt")); assert(app.ta.len == 3 && app.ta.text[1] == 0);
    CTRL(0x14f); type("."); CTRL(0x1f); assert(app.ta.len == 4 && expect_bytes("/system/odd.txt", "a\0b.", 4));
}

static void test_limits(void)
{
    reset(0);
    char *big = malloc(CN_LIMIT + 2); memset(big, 'a', CN_LIMIT + 1);
    add_file("/system/full.txt", big, CN_LIMIT, 1); add_file("/system/over.txt", big, CN_LIMIT + 1, 1);
    assert(!cn_open_argument(&app, "/system/full.txt")); assert(app.ta.len == CN_LIMIT && !app.ta.modified);
    type("b"); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "text limit of 61,440 bytes") && app.ta.len == CN_LIMIT && !app.ta.modified && !app.ta.limit_hit);
    screen("limit"); ENTER;
    CTRL(0x1e); CTRL(0x2e); press(0x14f); CTRL(0x2f); assert(dialog_is(D_INFO) && app.ta.len == CN_LIMIT); ENTER;   /* paste over a full buffer */
    reset(0); add_file("/system/over.txt", big, CN_LIMIT + 1, 1); assert(cn_open_argument(&app, "/system/over.txt") == -EFBIG);
    assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "could not be opened") && strstr(ui.dialog.text, "File too large") && app.ta.len == 0 && !app.named);
    ENTER;
    /* The limit counts the edited text: a CRLF file of 60000 text bytes opens and saves back unchanged. */
    reset(0); char *crlf = malloc(80000); for (int i = 0; i < 20000; i++) memcpy(crlf + 4 * i, "ab\r\n", 4);
    add_file("/system/crlf.txt", crlf, 80000, 1);
    assert(!cn_open_argument(&app, "/system/crlf.txt")); assert(app.ta.len == 60000 && app.crlf);
    CTRL(0x14f); type("zzz"); CTRL(0x1f);
    struct node *n = find("/system/crlf.txt"); assert(n->size == 80003 && !memcmp(n->data, crlf, 80000) && !memcmp(n->data + 80000, "zzz", 3));
    /* A raw file beyond twice the limit is refused before decoding. */
    char *huge = malloc(130000); memset(huge, 'a', 130000);
    reset(0); add_file("/system/huge.txt", huge, 130000, 1); assert(cn_open_argument(&app, "/system/huge.txt") == -EFBIG); assert(dialog_is(D_INFO)); ENTER;
    free(big); free(crlf); free(huge);
    /* Read errors are reported and keep the current document. */
    reset(0); type("keep"); fail_load = -EIO; assert(cn_open_argument(&app, "/system/hello.txt") == -EIO); assert(dialog_is(D_INFO) && has_text("keep")); ENTER; fail_load = 0;
}

static void test_open_argument(void)
{
    reset(0);
    assert(!cn_open_argument(&app, "/system/hello.txt")); assert(has_text("hello") && app.named && !strcmp(status_name(), "hello.txt"));
    reset(0); assert(!cn_open_argument(&app, "system/./tests/../hello.txt")); assert(has_text("hello") && !strcmp(app.path, "/system/hello.txt"));
    reset(0); assert(cn_open_argument(&app, "/system/new-note.txt") == -ENOENT);
    assert(dialog_is(D_CREATE) && strstr(ui.dialog.text, "Cannot find the new-note.txt file.") && strstr(ui.dialog.text, "create a new file")); ENTER;
    assert(expect_file("/system/new-note.txt", "") && app.named && !strcmp(app.path, "/system/new-note.txt") && !app.ta.modified);
    reset(0); assert(cn_open_argument(&app, "/system/new-note2.txt") == -ENOENT); press(0x31); assert(!find("/system/new-note2.txt") && !app.named);
    reset(0); assert(cn_open_argument(&app, "/system") == -EISDIR); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Is a directory")); ENTER;
    reset(0); assert(cn_open_argument(&app, "bad:name") == -EINVAL); assert(dialog_is(D_INFO)); ENTER;
    reset(0); fail_save = -EACCES; assert(cn_open_argument(&app, "/system/denied.txt") == -ENOENT); ENTER;
    assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "could not be created") && !app.named); fail_save = 0; ENTER;
}


enum { F_OK, F_CANCEL, F_UP, F_FOLDER, F_LIST, F_NAME };   /* indices of the toolkit's file dialog items */
static int entry_x(int index) { struct du_rect r = du_list_label(&ui.dialog.items[F_LIST], (unsigned)index); return r.x + r.w / 2; }
static int entry_y(int index) { struct du_rect r = du_list_label(&ui.dialog.items[F_LIST], (unsigned)index); return r.y + r.h / 2; }
static int entry_is(int index, const char *name) { return (unsigned)index < ui.dialog.entry_count && !strcmp(ui.dialog.entries[index].name, name); }
static const char *name_box(void) { return ui.dialog.items[F_NAME].text; }

static void test_file_dialogs(void)
{
    reset(0);
    CTRL(0x18);
    assert(dialog_is(D_OPEN) && ui.dialog.entry_count == 3 && entry_is(0, "bin") && entry_is(1, "home") && entry_is(2, "system"));
    screen("open-root");
    type("/system/tests"); ENTER;                      /* an absolute folder in the name box navigates */
    assert(dialog_is(D_OPEN) && !strcmp(ui.dialog.folder, "/system/tests") && !name_box()[0]);
    /* Folders first, then the text files (case-insensitive *.txt); data.bin is hidden. */
    assert(ui.dialog.entry_count == 3 && ui.dialog.entries[0].directory && entry_is(0, "sub") && entry_is(1, "Alpha.TXT") && entry_is(2, "beta.txt"));
    screen("open-tests");
    click(entry_x(1), entry_y(1)); assert(!strcmp(name_box(), "Alpha.TXT"));
    click(entry_x(2), entry_y(2)); assert(!strcmp(name_box(), "beta.txt"));
    /* Double-click on a folder enters it; Up leaves it. */
    click(entry_x(0), entry_y(0)); mono_ns += 100000000ull; click(entry_x(0), entry_y(0));
    assert(!strcmp(ui.dialog.folder, "/system/tests/sub") && ui.dialog.entry_count == 0);
    { struct du_rect up = ui.dialog.items[F_UP].box; click(up.x + up.w / 2, up.y + up.h / 2); }
    assert(!strcmp(ui.dialog.folder, "/system/tests"));
    { struct du_rect nb = ui.dialog.items[F_NAME].box; click(nb.x + nb.w / 2, nb.y + nb.h / 2); }
    CTRL(0x1e);   /* Ctrl+A in the box, then the typed pattern replaces the clicked name */
    /* A pattern in the name box is a filter for the next listing. */
    type("*.*"); ENTER; assert(dialog_is(D_OPEN) && !strcmp(ui.dialog.folder, "/system/tests") && ui.dialog.entry_count == 4 && entry_is(3, "data.bin"));
    type("*.bin"); ENTER; assert(dialog_is(D_OPEN) && ui.dialog.entry_count == 2 && entry_is(0, "sub") && entry_is(1, "data.bin"));
    click(entry_x(1), entry_y(1)); assert(!strcmp(name_box(), "data.bin")); ENTER;
    assert(!ui.dialog.kind && app.ta.len == 3 && !strcmp(app.path, "/system/tests/data.bin"));
    CTRL(0x18); assert(ui.dialog.entry_count == 3 && !strcmp(ui.dialog.folder, "/system/tests"));   /* the filter lasted one dialog */
    type("*.bin"); ENTER; ESC; CTRL(0x18); assert(ui.dialog.entry_count == 3); ESC;                  /* Cancel drops it too */
    /* Double-click on a file opens it. */
    CTRL(0x18); click(entry_x(2), entry_y(2)); mono_ns += 100000000ull; click(entry_x(2), entry_y(2));
    assert(!ui.dialog.kind && has_text("lf only\nsecond\n") && !strcmp(app.path, "/system/tests/beta.txt") && !app.crlf);
    /* Names without an extension get .txt; the report names the file that was tried. */
    CTRL(0x18); type("Alpha.TXT"); ENTER; assert(!ui.dialog.kind && has_text("Alpha\nline two\n") && app.crlf);
    CTRL(0x18); type("beta"); ENTER; assert(!ui.dialog.kind && has_text("lf only\nsecond\n") && !app.crlf);
    CTRL(0x18); type("nothing"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "The file was not found:") && strstr(ui.dialog.text, "nothing.txt")); ENTER;
    assert(has_text("lf only\nsecond\n"));
    CTRL(0x18); type("../hello"); ENTER; assert(!ui.dialog.kind && has_text("hello"));
    CTRL(0x18); type("a:b"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "That file name is not valid:")); ENTER;
    CTRL(0x18); assert(!strcmp(ui.dialog.folder, "/system")); type("tests"); ENTER; assert(dialog_is(D_OPEN) && !strcmp(ui.dialog.folder, "/system/tests")); ESC;
    /* Folder errors are reported. */
    fail_list = -EACCES; CTRL(0x18); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "The folder could not be opened:")); ENTER; fail_list = 0;
    /* A name that is a folder cannot be opened or saved over. */
    reset(0); CTRL(0x18); type("/system/tests/sub"); ENTER; assert(dialog_is(D_OPEN)); ESC;
    /* Save As starts in the document's folder with its name; a typed folder navigates; an explicit extension is kept. */
    reset(0); assert(!cn_open_argument(&app, "/system/tests/beta.txt")); CTRL(0x14f); type("!");
    menu_pick(0x21, 3);
    assert(dialog_is(D_SAVE) && !strcmp(ui.dialog.folder, "/system/tests") && !strcmp(name_box(), "beta.txt") && !strcmp(ui.dialog.items[F_OK].text, "&Save"));
    type("sub"); ENTER; assert(dialog_is(D_SAVE) && !strcmp(ui.dialog.folder, "/system/tests/sub"));
    type("copy.log"); ENTER;
    assert(!ui.dialog.kind && expect_file("/system/tests/sub/copy.log", "lf only\nsecond\n!") && !strcmp(app.path, "/system/tests/sub/copy.log"));
    CTRL(0x1f); menu_pick(0x21, 3); assert(dialog_is(D_SAVE) && !strcmp(ui.dialog.folder, "/system/tests/sub") && !strcmp(name_box(), "copy.log")); ESC;
    /* Saving over a folder name is refused. */
    reset(0); type("x"); CTRL(0x1f); type("/system/tests"); ENTER; assert(dialog_is(D_SAVE)); type("sub"); ENTER; assert(dialog_is(D_SAVE)); ESC;
    reset(0); type("x"); CTRL(0x1f); type("/home/sub.txt"); fail_save = -EIO; ENTER; assert(dialog_is(D_INFO)); fail_save = 0; ENTER;
    add_dir("/home/dir.txt"); CTRL(0x1f); type("/home/dir"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "That name is a folder:")); ENTER;
    /* The Save As dialog's own cancel button keeps the document and the prompt chain stops. */
    reset(0); type("x"); CTRL(0x31); ENTER; assert(dialog_is(D_SAVE) && app.after == CN_AFTER_NEW); ESC; assert(!ui.dialog.kind && app.after == CN_AFTER_NONE && has_text("x"));
}

static int item_y(const struct du_menu_item *items, unsigned n, struct du_rect box, unsigned index)
{
    int y = box.y + 3;
    for (unsigned i = 0; i < index; i++) y += items[i].flags & DU_ITEM_SEPARATOR ? DU_MENU_SEP_H : DU_MENU_ITEM_H;
    (void)n; return y + DU_MENU_ITEM_H / 2;
}

static void test_menus_and_context(void)
{
    reset(0); fresh_text("hello world");
    struct du_rect title = du_menu_title(&ui, 1);
    click(title.x + title.w / 2, title.y + 5); assert(ui.menu == 1);
    screen("menu-edit");
    struct du_rect drop = du_menu_box(&ui, ui.menus[1].items, ui.menus[1].count, title.x, title.y + title.h);
    click(drop.x + 40, item_y(ui.menus[1].items, ui.menus[1].count, drop, 12)); assert(ui.menu == -1 && du_ta_has_selection(&app.ta) && app.ta.cursor == 11);   /* Select All */
    /* The context menu: right click opens it at the pointer without moving the caret. */
    click_button(2, 300, 100); assert(ui.popup == app.popup && ui.popup_count == 6);
    assert(!(app.popup[1].flags & DU_ITEM_DISABLED) && !(app.popup[2].flags & DU_ITEM_DISABLED) && !(app.popup[4].flags & DU_ITEM_DISABLED));
    screen("context");
    struct du_rect pop = du_menu_box(&ui, app.popup, 6, 300, 100);
    click(pop.x + 40, item_y(app.popup, 6, pop, 5)); assert(!ui.popup && du_ta_has_selection(&app.ta));                       /* Select All */
    click(cell_x(3), cell_y(0)); assert(!du_ta_has_selection(&app.ta) && app.ta.cursor == 3);
    click_button(2, 300, 100); assert(app.ta.cursor == 3 && (app.popup[1].flags & DU_ITEM_DISABLED) && (app.popup[2].flags & DU_ITEM_DISABLED) && (app.popup[4].flags & DU_ITEM_DISABLED));
    click(pop.x + 40, item_y(app.popup, 6, pop, 1)); assert(ui.popup && has_text("hello world")); ESC; assert(!ui.popup);   /* a disabled Cut does nothing and the menu stays open */
    click_button(2, 300, 100); press(0x148); ENTER; assert(du_ta_has_selection(&app.ta));                                     /* Up wraps to the last item */
    click_button(2, 300, 100); ESC; assert(!ui.popup);
    /* Copy then Paste from the popup. */
    select_range(0, 5); click_button(2, 300, 100); click(pop.x + 40, item_y(app.popup, 6, pop, 2)); assert(!strcmp(du_clipboard_get(), "hello"));
    press(0x14f); click_button(2, 300, 100); click(pop.x + 40, item_y(app.popup, 6, pop, 3)); assert(has_text("hello worldhello"));
    /* Menu bar keys: F10 opens File, Down walks the items, Left wraps across menus. */
    press(0x44); assert(ui.menu == 0 && ui.menu_item == 0); press(0x150); assert(ui.menu_item == 1);
    press(0x14b); assert(ui.menu == 4); press(0x14d); assert(ui.menu == 0); ESC; ESC;
    /* A right click on the status bar opens nothing. */
    click_button(2, 100, NH - 10); assert(!ui.popup);
    /* Disabled menu items do not run: Undo on a fresh document, Cut without a selection. */
    reset(0); alt(0x12); assert(ui.menu == 1); press(0x1c); assert(has_text("")); ESC;
    /* Mouse tracking: press on a title, drag to an item, release runs it. */
    reset(0); fresh_text("abc"); select_range(0, 0);
    title = du_menu_title(&ui, 1); drop = du_menu_box(&ui, ui.menus[1].items, ui.menus[1].count, title.x, title.y + title.h);
    mouse(title.x + 5, title.y + 5); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); mouse(drop.x + 40, item_y(ui.menus[1].items, ui.menus[1].count, drop, 12)); ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
    assert(ui.menu == -1 && du_ta_has_selection(&app.ta));
}

static void test_mouse_text(void)
{
    reset(0); fresh_text("one two three\nsecond line\nthird"); select_range(0, 0);
    click(cell_x(4), cell_y(0)); assert(app.ta.cursor == 4 && !strcmp(status_pos(), "Ln 1, Col 5"));
    key_event(0x2a, 1); click(cell_x(7), cell_y(0)); key_event(0x2a, 0); { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 4 && b == 7); }
    mono_ns += 2000000000ull; mouse(cell_x(2), cell_y(0)); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); mouse(cell_x(3), cell_y(1)); mouse(cell_x(5), cell_y(2));
    ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
    { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 2 && b == 14 + 12 + 5 && app.ta.cursor == b); }       /* drag across three lines */
    assert(!strcmp(status_pos(), "Ln 3, Col 6"));
    mono_ns += 2000000000ull; click(cell_x(5), cell_y(0)); mono_ns += 100000000ull; click(cell_x(5), cell_y(0));
    { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 4 && b == 7); }                                       /* double-click: "two" */
    mono_ns += 100000000ull; click(cell_x(5), cell_y(0)); assert(!du_ta_has_selection(&app.ta));                         /* the third click is single */
    click_button(2, cell_x(8), cell_y(1)); assert(app.ta.cursor == 5 && ui.popup);                                      /* right click keeps the caret */
    ESC;
    /* The scroll bars: 40 lines scroll vertically; the arrow and the track move the view. */
    char many[800]; many[0] = 0; for (int i = 0; i < 40; i++) strcat(many, "line number\n");
    fresh_text(many); select_range(0, 0);
    struct du_rect box = W(W_TEXT)->box;
    int rows = (int)app.ta.rows, bar_x = box.x + box.w - 2 - 8, down_y = box.y + box.h - 2 - 16 - 8;
    click(bar_x, down_y); assert(app.ta.top == 1);
    click(bar_x, down_y - 30); assert(app.ta.top == 1 + (unsigned)rows - 0 || app.ta.top > 1);
    unsigned top = app.ta.top; click(bar_x, box.y + 2 + 6); assert(app.ta.top == top - 1);
    screen("scrolled-bars");
    /* Dragging the thumb. */
    CTRL(0x147); assert(app.ta.top == 0);
    mouse(bar_x, box.y + 2 + 40); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); mouse(bar_x, box.y + 2 + 120); ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
    assert(app.ta.top > 5 && app.ta.top < 13 && !app.ta.drag && app.ta.cursor == 0);   /* the view moved, the caret did not */
}

static void test_wrap_navigation(void)
{
    reset(0);
    fresh_text("The quick brown fox jumps over the lazy dog and keeps running through the forest until night falls, "
               "while the moon rises above the quiet hills and every window in the village goes dark one by one.");
    menu_pick(0x18, 0); assert(app.ta.wrap);
    unsigned lines = du_ta_visual_lines(&app.ta); assert(lines == 2);
    CTRL(0x147); assert(app.ta.cursor == 0);
    press(0x150); size_t second = app.ta.cursor; assert(second > 90 && !app.ta.affinity && !strncmp(status_pos(), "Ln 1, Col ", 10) && app.ta.cursor == second);
    press(0x147); assert(app.ta.cursor == second);                                              /* Home on the second visual line */
    CTRL(0x147); press(0x14f); assert(app.ta.cursor <= second && app.ta.cursor > 0 && app.ta.affinity); /* End: the end of the first visual line */
    CTRL(0x14f); assert(app.ta.cursor == app.ta.len);
    screen("wrapped");
    menu_pick(0x18, 0); assert(!app.ta.wrap && du_ta_visual_lines(&app.ta) == 1);
    CTRL(0x147); assert(app.ta.left == 0);
}

static void test_screens(void)
{
    reset(0); fresh_text("CiukNote - A modern Retro OS\n\tTabbed line with caf\xc3\xa9\nlast line"); select_range(3, 20); screen("typed");
    ESC; menu_pick(0x21, 3); screen("save-page"); type("kept"); ENTER; screen("saved");
    reset(0); fresh_text("one"); CTRL(0x31); screen("prompt-changes"); ESC;
    CTRL(0x21); screen("find-bar"); ESC;
    menu_pick(0x2f, 0); screen("status-hidden");
    reset(0); alt(0x21); screen("menu-file"); ESC;
}

/* Random keys, text and pointer events against the whole application: it must stay consistent. */
static uint32_t rng = 0x9e3779b9u;
static unsigned rnd(unsigned n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }
static void test_random_input(void)
{
    static const int keys[] = {0x01, 0x0e, 0x0f, 0x1c, 0x39, 0x44, 0x3d, 0x3f, 0x147, 0x148, 0x149, 0x14b, 0x14d, 0x14f, 0x150, 0x151, 0x153,
                               0x1e, 0x1f, 0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x18, 0x21, 0x23, 0x22, 0x23, 0x12, 0x2b, 0x10, 0x11, 0x13, 0x14};
    static const int modifiers[] = {0, 0, 0x1d, 0x2a, 0x38};
    reset(0);
    for (unsigned step = 0; step < 25000; step++) {
        mono_ns += 70000000ull;
        switch (rnd(8)) {
        case 0: case 1: case 2: {
            int m = modifiers[rnd(5)], k = keys[rnd(sizeof(keys) / sizeof(*keys))];
            if (m) key_event(m, 1);
            key_event(k, 1); if (rnd(4) == 0) key_event(k, 2); key_event(k, 0);
            if (m) key_event(m, 0);
            break;
        }
        case 3: ev(CIUKI_INPUT_TEXT, 32 + (int)rnd(95), 0, 0); break;
        case 4: mouse((int)rnd(NW + 20) - 10, (int)rnd(NH + 20) - 10); break;
        case 5: ev(CIUKI_INPUT_BUTTON, 1 + (int)rnd(3), 1, 0); ev(CIUKI_INPUT_BUTTON, 1 + (int)rnd(3), 0, 0); break;
        case 6: ev(CIUKI_INPUT_MOTION, 0, (int)rnd(NW), (int)rnd(NH)); break;
        case 7: if (!rnd(40)) ev(CIUKI_INPUT_RESYNC, 0, 0, 0); if (!rnd(15)) { fail_save = rnd(3) ? 0 : -ENOSPC; fail_list = rnd(4) ? 0 : -EIO; fail_load = rnd(4) ? 0 : -EIO; } break;
        }
        if (rnd(3) == 0) settle();
        assert(ui.focus >= -1 && ui.focus < (int)ui.count);
        assert(app.ta.cursor <= app.ta.len && app.ta.anchor <= app.ta.len && app.ta.len <= CN_LIMIT && !app.ta.text[app.ta.len]);
        assert(ui.menu < (int)ui.menu_count);
        if (quit_count) { quit_count = 0; app.after = CN_AFTER_NONE; }
    }
    fail_save = fail_load = fail_list = 0;
    settle();
}

/* Replays a key script as the QEMU case sends it and reports what the guest would print. */
static int replay(const char *script, const char *result_path)
{
    reset(1);
    FILE *f = fopen(script, "r"); if (!f) { perror(script); return 2; }
    char kind[8]; int code, value, reported = 0, close_result = -1; char line[256];
    for (;;) {
        if (fscanf(f, "%7s", kind) != 1) break;
        if (kind[0] == 'K') { if (fscanf(f, "%d %d", &code, &value) != 2) return 2; key_event(code, value); }
        else if (kind[0] == 'C') { close_result = cn_close_request(&app); if (close_result) quit_count++; }
        else if (kind[0] == 'S') {
            settle();
            while (cp_pending > reported) { cn_describe(&app, reported + 1, line, sizeof(line)); printf("CHECKPOINT %s\n", line); reported++; }
        } else return 2;
    }
    fclose(f);
    struct node *n = find(result_path);
    printf("FILE %s ", result_path);
    if (n) for (size_t i = 0; i < n->size; i++) printf("%02x", (unsigned char)n->data[i]);
    printf("\nQUIT %d\nCLOSE %d\nDIALOG %d\nTEXT %zu\n", quit_count, close_result, ui.dialog.kind, app.ta.len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "replay")) return replay(argv[2], argv[3]);
    dump_prefix = argc > 1 ? argv[1] : NULL;
    test_startup(); test_typing(); test_clipboard_and_undo(); test_save_and_open(); test_unsaved_prompt();
    test_save_errors_and_replace(); test_find_replace(); test_goto_wrap_status(); test_time_log_encoding();
    test_limits(); test_open_argument(); test_file_dialogs(); test_menus_and_context(); test_mouse_text();
    test_wrap_navigation(); test_screens(); test_random_input();
    puts("PASS ciuknote: typing, shift/caps, clipboard, undo, prompts, save/open, replace prompt, find/replace, go to, wrap, status bar, "
         "time/date, .LOG, line endings, limits, argument, file dialogs, menus, context menu, mouse, scroll bars, screens");
    return 0;
}
