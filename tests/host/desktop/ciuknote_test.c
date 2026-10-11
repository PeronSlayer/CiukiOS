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

static uint32_t canvas[CN_WIDTH * CN_HEIGHT];
static struct du_ui ui; static struct cn_app app;
static time_t clock_seconds = 1791711660; static uint64_t mono_ns = 5000000000ull; static int quit_count;
static time_t host_now(void *p) { (void)p; return clock_seconds; }
static uint64_t host_mono(void *p) { (void)p; return mono_ns; }
static void host_quit(void *p) { (void)p; quit_count++; }
static const char *dump_prefix;

static void reset(int test_mode)
{
    if (app.ui) cn_detach(&app);
    fs_reset(); du_clipboard_set(""); memset(&app, 0, sizeof(app)); memset(canvas, 0, sizeof(canvas)); quit_count = 0; mono_ns = 5000000000ull;
    clock_seconds = 1791711660;
    du_init(&ui, canvas, CN_WIDTH, CN_HEIGHT);
    app.fs = &fake_fs; app.now = host_now; app.mono = host_mono; app.quit = host_quit; app.test = test_mode;
    assert(!cn_attach(&app, &ui));
}
static void ev(int type, int code, int value, int value2)
{
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = mono_ns};
    du_input(&ui, &e);
}
static void settle(void) { if (ui.dirty) du_render(&ui); }
/* The kernel's unshifted text for a key (src/kernel/drivers/i8042.c: us[]). */
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
        if (*s == '\n') { ENTER; continue; }
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
static int has_text(const char *s) { return app.ta.len == strlen(s) && !memcmp(app.ta.text, s, app.ta.len); }
static const char *status_name(void) { return du_get(&ui, 101)->text; }
static const char *status_pos(void) { return du_get(&ui, 102)->text; }
static int ta_cursor_is(size_t c) { return app.ta.cursor == c; }
static int dialog_is(int id) { return ui.dialog.visible && ui.dialog.id == id; }
static int widget_visible(int id) { return !du_get(&ui, id)->hidden; }
static void select_range(size_t a, size_t b) { du_ta_select(&app.ta, a, b); cn_refresh(&app); settle(); }
static void fresh_text(const char *s) { du_ta_select_all(&app.ta); du_ta_insert(&app.ta, s, strlen(s), DU_TA_EDIT_OTHER); settle(); }

/* ---- scenarios ----------------------------------------------------------- */

static void test_startup(void)
{
    reset(1);
    assert(app.page == CN_PAGE_EDIT && ui.menu_count == 5 && widget_visible(W_TEXT) && !widget_visible(W_FIND) && !widget_visible(W_LIST));
    assert(ui.focus >= 0 && ui.widgets[ui.focus].id == W_TEXT);
    assert(!strcmp(status_name(), "Untitled") && !strcmp(status_pos(), "Ln 1, Col 1"));
    assert(app.checkpoint == CN_CP_READY); char line[256]; assert(!cn_checkpoint(&app, line, sizeof(line))); /* not rendered yet */
    settle(); assert(cn_checkpoint(&app, line, sizeof(line)) && strstr(line, "event=ready") && strstr(line, "path=-"));
    assert(!cn_checkpoint(&app, line, sizeof(line)));
    /* Menu labels fit the 224 pixel drop-down (26 cells, 24 behind a check mark). */
    for (unsigned m = 0; m < 5; m++) for (unsigned i = 0; i < app.menus[m].count; i++) {
        const char *l = app.menus[m].items[i].label; size_t n = strlen(l) - (l[0] == 1 || l[0] == 2);
        assert(n <= (l[0] == 1 || l[0] == 2 ? 24u : 26u));
    }
    assert(app.ta.cap == 61440 && ui.menu_keys == app.menu_keys);
    screen("empty");
}

static void test_typing(void)
{
    reset(0);
    type("Hello, World! 123 #a-z_A+Z=[x]{y}|:;\"'<>?/\\`~"); assert(has_text("Hello, World! 123 #a-z_A+Z=[x]{y}|:;\"'<>?/\\`~"));
    assert(app.ta.modified && !strcmp(status_name(), "Untitled *"));
    fresh_text(""); press(0x3a); type("abc 1"); assert(has_text("ABC 1")); press(0x3a); type("d"); assert(has_text("ABC 1d"));
    fresh_text("");
    key_event(0x2a, 1); press(0x3a); key_event(0x2a, 0); assert(app.ui->caps == 1); /* shift+caps: caps toggled on, shift held */
    press(0x3a); assert(app.ui->caps == 0);
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
    /* Delete menu item / key: with a selection the accelerator fires, without it the key deletes forward. */
    select_range(0, 4); assert(!app.edit_items[4].disabled);
    press(0x153); assert(has_text("two three"));
    press(0x153); assert(has_text("wo three"));
    /* The clipboard holds a whole document, not the 1 KiB edit-box mirror. */
    char big[4000]; memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    reset(0); fresh_text(big); CTRL(0x1e); CTRL(0x2e); assert(strlen(du_clipboard_get()) == DU_TEXT_MAX - 1);
    press(0x14f); CTRL(0x2f); assert(app.ta.len == 2 * (sizeof(big) - 1));
    /* A string copied in a dialog edit box wins over the document clipboard. */
    du_clipboard_set("from-elsewhere"); CTRL(0x2f); assert(app.ta.len == 2 * (sizeof(big) - 1) + 14);
    /* Menu enable states. */
    reset(0); cn_refresh(&app);
    assert(app.edit_items[0].disabled && app.edit_items[1].disabled && app.edit_items[2].disabled && app.edit_items[4].disabled);
    type("ab"); select_range(0, 2);
    assert(!app.edit_items[0].disabled && !app.edit_items[1].disabled && !app.edit_items[4].disabled);
}

static void test_save_and_open(void)
{
    reset(1);
    type("Hello\nWorld"); assert(app.ta.modified);
    CTRL(0x1f);                                         /* Ctrl+S on an untitled document: Save As page */
    assert(app.page == CN_PAGE_SAVE && !widget_visible(W_TEXT) && widget_visible(W_LIST) && ui.menu_count == 0);
    assert(ui.widgets[ui.focus].id == W_NAMEEDIT && !strcmp(du_get(&ui, W_OK)->text, "Save"));
    assert(!strcmp(du_get(&ui, W_PATH)->text, "/") && app.has_parent == 0);
    type("/system/tests/note.txt"); ENTER;
    assert(app.page == CN_PAGE_EDIT && app.named && !strcmp(app.path, "/system/tests/note.txt") && !app.ta.modified);
    assert(expect_file("/system/tests/note.txt", "Hello\r\nWorld") && saves == 1);   /* new documents use CRLF */
    assert(!strcmp(status_name(), "note.txt"));
    char line[256]; settle(); assert(cn_checkpoint(&app, line, sizeof(line)) && strstr(line, "event=saved") && strstr(line, "path=/system/tests/note.txt"));
    /* Save again without a dialog; edits are written through the temporary replacement. */
    type("!"); assert(!strcmp(status_name(), "note.txt *")); CTRL(0x1f); assert(app.page == CN_PAGE_EDIT && expect_file("/system/tests/note.txt", "Hello\r\nWorld!") && saves == 2);
    /* New, then Open it back. */
    CTRL(0x31); assert(has_text("") && !app.named);
    CTRL(0x18); assert(app.page == CN_PAGE_OPEN && !strcmp(du_get(&ui, W_OK)->text, "Open"));
    type("/system/tests/note.txt"); ENTER;
    assert(app.page == CN_PAGE_EDIT && has_text("Hello\nWorld!") && app.named && !app.ta.modified && app.crlf);
    settle(); assert(cn_checkpoint(&app, line, sizeof(line)) && strstr(line, "event=opened"));
    /* File > Exit through F10, Up (wraps to the last item), Enter. */
    press(0x44); press(0x148); ENTER; assert(quit_count == 1);
}

static void test_unsaved_prompt(void)
{
    reset(0);
    type("draft"); CTRL(0x31);                           /* New with unsaved text */
    assert(dialog_is(200) && ui.dialog.count == 3 && strstr(ui.dialog.message, "The text in the Untitled file has changed.") &&
           strstr(ui.dialog.message, "Do you want to save the changes?"));
    ESC; assert(!ui.dialog.visible && has_text("draft") && app.after == CN_AFTER_NONE);   /* Cancel */
    CTRL(0x31); press(0x0f); ENTER; assert(!ui.dialog.visible && has_text(""));           /* Tab to No, Enter: discard */
    type("keep"); CTRL(0x31); ENTER;                                                      /* Yes: untitled, so Save As */
    assert(app.page == CN_PAGE_SAVE && app.after == CN_AFTER_NEW);
    type("kept"); ENTER;                                                                  /* default extension, then New */
    assert(expect_file("/kept.txt", "keep") && app.page == CN_PAGE_EDIT && has_text("") && !app.named);
    /* Open with unsaved changes: No proceeds to the Open page; Cancel on that page leaves the text. */
    type("more"); CTRL(0x18); press(0x0f); ENTER; assert(app.page == CN_PAGE_OPEN);
    ESC; assert(app.page == CN_PAGE_EDIT && has_text("more") && app.after == CN_AFTER_NONE);
    /* Exit asks too; Yes on a named document saves and quits. */
    CTRL(0x1f); type("/exit-test.txt"); ENTER; assert(app.named && !app.ta.modified);
    type("?"); press(0x44); press(0x148); ENTER; assert(dialog_is(200) && quit_count == 0);
    ENTER; assert(quit_count == 1 && !app.ta.modified && expect_file("/exit-test.txt", "more?"));
    /* The window's close request: refuse while unsaved, accept when clean. */
    reset(0); assert(cn_close_request(&app) == 1);
    type("x"); assert(cn_close_request(&app) == 0 && dialog_is(200) && app.after == CN_AFTER_EXIT);
    press(0x0f); ENTER; assert(quit_count == 1);                                          /* No: quit */
    reset(0); type("x"); CTRL(0x1f); assert(app.page == CN_PAGE_SAVE); assert(cn_close_request(&app) == 0 && app.page == CN_PAGE_EDIT && dialog_is(200));
    /* A long name is shortened in the prompt. */
    reset(0); app.named = 1; snprintf(app.path, CN_PATH, "/a-very-long-document-name-indeed.txt"); type("x");
    assert(cn_close_request(&app) == 0 && strstr(ui.dialog.message, "a-very-long-docum...") );
}

static void test_save_errors_and_replace(void)
{
    reset(0);
    type("data"); CTRL(0x1f); type("/system/hello"); ENTER;                    /* default extension -> hello.txt exists */
    assert(dialog_is(201) && strstr(ui.dialog.message, "hello.txt already exists.") && app.page == CN_PAGE_SAVE);
    press(0x0f); ENTER;                                                         /* No: back to the page */
    assert(!ui.dialog.visible && app.page == CN_PAGE_SAVE && expect_file("/system/hello.txt", "hello"));
    ENTER; assert(dialog_is(201)); ENTER;                                       /* Yes replaces */
    assert(app.page == CN_PAGE_EDIT && expect_file("/system/hello.txt", "data") && !strcmp(app.path, "/system/hello.txt"));
    /* Failures leave the document modified and report the error. */
    reset(0); type("x"); CTRL(0x1f); type("/lost.txt"); fail_save = -ENOSPC; ENTER;
    assert(dialog_is(202) && strstr(ui.dialog.message, "could not be saved") && strstr(ui.dialog.message, "error 28") && app.ta.modified && !app.named);
    ENTER; fail_save = 0; assert(app.page == CN_PAGE_SAVE); ENTER; assert(app.page == CN_PAGE_EDIT && expect_file("/lost.txt", "x"));
    /* A missing folder fails and keeps the page. */
    reset(0); type("x"); CTRL(0x1f); type("/nowhere/file.txt"); ENTER; assert(dialog_is(202)); ENTER;
    assert(app.page == CN_PAGE_SAVE && app.ta.modified);
}

static void test_find_replace(void)
{
    reset(0);
    fresh_text("The cat and the CAT sat; the End"); select_range(0, 0);
    CTRL(0x21); assert(app.find_mode == 1 && widget_visible(W_FIND) && !widget_visible(W_REPL) && ui.widgets[ui.focus].id == W_FIND);
    assert(du_get(&ui, W_TEXT)->box.h == CN_HEIGHT - 24 - 24 - 32);
    type("the"); ENTER; size_t a, b;
    du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3);
    ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 25 && b == 28);
    ENTER; assert(dialog_is(202) && strstr(ui.dialog.message, "Cannot find \"the\"")); ENTER; assert(!ui.dialog.visible);
    assert(du_get(&ui, W_FIND)->text[0] == 't' && !strcmp(status_pos(), "Ln 1, Col 29"));
    /* Match case through its button, direction through the other. */
    click(500, 312); assert(app.match_case && !strcmp(du_get(&ui, W_FCASE)->text, "[x] Match case"));
    select_range(0, 0); du_focus(&ui, W_FIND); ENTER; du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    click(400, 312); assert(app.find_up && !strcmp(du_get(&ui, W_FDIR)->text, "Up"));
    click(320, 312); assert(dialog_is(202) && strstr(ui.dialog.message, "Cannot find \"the\"")); ENTER;   /* nothing before it, case-sensitive */
    click(500, 312); assert(!app.match_case);
    click(320, 312); du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3 && ta_cursor_is(0));          /* Up finds "The" */
    click(400, 312); assert(!app.find_up);
    /* F3 repeats from the text area; the needle survives closing the bar. */
    ESC; assert(app.find_mode == 0 && !widget_visible(W_FIND) && ui.widgets[ui.focus].id == W_TEXT);
    select_range(0, 0); press(0x3d); du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3);
    press(0x3d); du_ta_selection(&app.ta, &a, &b); assert(a == 12 && b == 15);
    /* Ctrl+F picks up a single-line selection as the needle. */
    select_range(4, 7); CTRL(0x21); assert(!strcmp(du_get(&ui, W_FIND)->text, "cat"));
    /* Find with nothing typed asks for a needle. */
    reset(0); press(0x3d); assert(app.find_mode == 1 && ui.widgets[ui.focus].id == W_FIND);
    /* Replace and Replace All through the bar. */
    reset(0);
    fresh_text("The cat and the CAT sat; the End"); select_range(0, 0);
    CTRL(0x23); assert(app.find_mode == 2 && widget_visible(W_REPL) && du_get(&ui, W_TEXT)->box.h == CN_HEIGHT - 24 - 24 - 62);
    type("the"); click(200, 312); type("a");
    click(320, 312);                                      /* Replace: nothing selected, so it finds the first match */
    du_ta_selection(&app.ta, &a, &b); assert(a == 0 && b == 3 && has_text("The cat and the CAT sat; the End"));
    click(320, 312); assert(has_text("a cat and the CAT sat; the End"));          /* replaces, then selects the next */
    du_ta_selection(&app.ta, &a, &b); assert(a == 10 && b == 13);
    click(430, 312); assert(has_text("a cat and a CAT sat; a End") && !strcmp(app.note, "Replaced 2 occurrences"));
    assert(strstr(status_name(), "Replaced 2 occurrences")); du_focus(&ui, W_TEXT); CTRL(0x2c); assert(has_text("a cat and the CAT sat; the End"));
    reset(0); fresh_text("aaa"); CTRL(0x23); type("a"); click(200, 312); type("bb"); click(430, 312);
    assert(has_text("bbbbbb") && !strcmp(app.note, "Replaced 3 occurrences")); du_focus(&ui, W_TEXT); CTRL(0x2c); assert(has_text("aaa"));
    reset(0); fresh_text("xyz"); CTRL(0x23); type("q"); click(430, 312); assert(dialog_is(202) && strstr(ui.dialog.message, "Cannot find \"q\"")); ENTER;
    /* Replace All beyond the limit changes nothing and says so. */
    reset(0); char big[CN_LIMIT + 1]; memset(big, 'a', CN_LIMIT); big[CN_LIMIT] = 0; fresh_text(big);
    CTRL(0x23); type("a"); click(200, 312); type("bb"); click(430, 312);
    assert(dialog_is(202) && strstr(ui.dialog.message, "would be exceeded") && app.ta.len == CN_LIMIT && app.ta.text[0] == 'a'); ENTER;
    /* Typing in the edit moves between fields with Tab. */
    reset(0); CTRL(0x23); assert(ui.widgets[ui.focus].id == W_FIND);
    for (int i = 0; i < 5; i++) press(0x0f);
    assert(ui.widgets[ui.focus].id == W_REPL);
    key_event(0x2a, 1); press(0x0f); key_event(0x2a, 0); assert(ui.widgets[ui.focus].id == W_FCLOSE);
    screen("replace-bar");
}

static void test_goto_wrap_status(void)
{
    reset(0);
    fresh_text("l1\nl2\nl3\nl4"); select_range(0, 0);
    CTRL(0x22); assert(dialog_is(203) && ui.dialog.kind == DU_INPUT && !strcmp(ui.dialog.edit.text, "1"));
    type("3"); ENTER; assert(app.ta.cursor == 6 && !strcmp(status_pos(), "Ln 3, Col 1"));
    CTRL(0x22); type("9"); ENTER; assert(dialog_is(202) && strstr(ui.dialog.message, "beyond the total number of lines")); ENTER;
    CTRL(0x22); type("x"); ENTER; assert(dialog_is(202)); ENTER;
    CTRL(0x22); ESC; assert(!ui.dialog.visible && app.ta.cursor == 6);
    /* Format > Word Wrap (Alt+O, Enter) wraps, checks the item and disables Go To. */
    alt(0x18); assert(ui.menu == 2);
    screen("format-menu");
    ENTER; assert(app.ta.wrap && app.wrap_label[0] == 1 && app.edit_items[8].disabled);
    CTRL(0x22); assert(!ui.dialog.visible);
    reset(0); char line[400]; memset(line, 'x', 300); line[300] = 0; fresh_text(line);
    assert(du_ta_visual_lines(&app.ta) == 1);
    menu_pick(0x18, 0); assert(du_ta_visual_lines(&app.ta) > 4);
    menu_pick(0x18, 0); assert(!app.ta.wrap && du_ta_visual_lines(&app.ta) == 1 && app.wrap_label[0] == 2);
    /* View > Status Bar toggles the status widgets and the text area grows. */
    int h = du_get(&ui, W_TEXT)->box.h;
    alt(0x2f); assert(ui.menu == 3); ENTER;
    assert(!app.show_status && !widget_visible(W_NAME) && !widget_visible(W_POS) && du_get(&ui, W_TEXT)->box.h == h + 24 && app.status_label[0] == 2);
    menu_pick(0x2f, 0); assert(app.show_status && widget_visible(W_NAME) && du_get(&ui, W_TEXT)->box.h == h);
    /* Mnemonics: Alt+F, Alt+E and Alt+H open the matching menus; the arrow keys move across menus. */
    alt(0x21); assert(ui.menu == 0); press(0x14d); assert(ui.menu == 1); ESC; assert(ui.menu == -1);
    alt(0x23); assert(ui.menu == 4); ESC;
    press(0x44); assert(ui.menu == 0); press(0x44); assert(ui.menu == -1);
    /* Font and About/Help dialogs. */
    reset(0); fresh_text("x");
    menu_pick(0x18, 1); assert(dialog_is(204) && ui.dialog.count == 3 && strstr(ui.dialog.message, "Current style: Regular"));
    press(0x0f); ENTER; assert(app.ta.bold == 1);
    menu_pick(0x18, 1); assert(strstr(ui.dialog.message, "Current style: Bold")); ESC; assert(app.ta.bold == 1);
    screen("bold");
    menu_pick(0x23, 0); assert(dialog_is(202) && strstr(ui.dialog.message, "Replace Ctrl+H")); screen("help"); ENTER;
    menu_pick(0x23, 1); assert(dialog_is(202) && strstr(ui.dialog.title, "About CiukNote") && strstr(ui.dialog.message, "A modern Retro OS") &&
           strstr(ui.dialog.message, "Tango 0.8.90 icons: Public Domain.") && strstr(ui.dialog.message, "61,440")); screen("about"); ENTER;
    /* Every message fits the dialog: at most four lines of 54 cells. */
    for (int m = 0; m < 2; m++) {
        menu_pick(0x23, m);
        int lines = 1, width = 0, longest = 0; for (const char *c = ui.dialog.message; *c; c++) { if (*c == '\n') { lines++; width = 0; } else if (++width > longest) longest = width; }
        assert(lines <= 4 && longest <= 54); ENTER;
    }
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
    type("b"); assert(dialog_is(202) && strstr(ui.dialog.message, "text limit of 61,440 bytes") && app.ta.len == CN_LIMIT && !app.ta.modified && !app.ta.limit_hit);
    ENTER; screen("limit");
    CTRL(0x1e); CTRL(0x2e); press(0x14f); CTRL(0x2f); assert(dialog_is(202) && app.ta.len == CN_LIMIT); ENTER;   /* paste over a full buffer */
    reset(0); add_file("/system/over.txt", big, CN_LIMIT + 1, 1); assert(cn_open_argument(&app, "/system/over.txt") == -EFBIG);
    assert(dialog_is(202) && strstr(ui.dialog.message, "could not be opened") && strstr(ui.dialog.message, "File too large") && app.ta.len == 0 && !app.named);
    ENTER;
    /* The limit counts the edited text: a CRLF file of 60000 text bytes opens and saves back unchanged. */
    reset(0); char *crlf = malloc(80000); for (int i = 0; i < 20000; i++) memcpy(crlf + 4 * i, "ab\r\n", 4);
    add_file("/system/crlf.txt", crlf, 80000, 1);
    assert(!cn_open_argument(&app, "/system/crlf.txt")); assert(app.ta.len == 60000 && app.crlf);
    CTRL(0x14f); type("zzz"); CTRL(0x1f);
    struct node *n = find("/system/crlf.txt"); assert(n->size == 80003 && !memcmp(n->data, crlf, 80000) && !memcmp(n->data + 80000, "zzz", 3));
    /* A raw file beyond twice the limit is refused before decoding. */
    char *huge = malloc(130000); memset(huge, 'a', 130000);
    reset(0); add_file("/system/huge.txt", huge, 130000, 1); assert(cn_open_argument(&app, "/system/huge.txt") == -EFBIG); assert(dialog_is(202)); ENTER;
    free(big); free(crlf); free(huge);
    /* Read errors are reported and keep the current document. */
    reset(0); type("keep"); fail_load = -EIO; assert(cn_open_argument(&app, "/system/hello.txt") == -EIO); assert(dialog_is(202) && has_text("keep")); ENTER; fail_load = 0;
}

static void test_open_argument(void)
{
    reset(0);
    assert(!cn_open_argument(&app, "/system/hello.txt")); assert(has_text("hello") && app.named && !strcmp(status_name(), "hello.txt"));
    reset(0); assert(!cn_open_argument(&app, "system/./tests/../hello.txt")); assert(has_text("hello") && !strcmp(app.path, "/system/hello.txt"));
    reset(0); assert(cn_open_argument(&app, "/system/new-note.txt") == -ENOENT);
    assert(dialog_is(205) && strstr(ui.dialog.message, "Cannot find the new-note.txt file.") && strstr(ui.dialog.message, "create a new file")); ENTER;
    assert(expect_file("/system/new-note.txt", "") && app.named && !strcmp(app.path, "/system/new-note.txt") && !app.ta.modified);
    reset(0); assert(cn_open_argument(&app, "/system/new-note2.txt") == -ENOENT); press(0x0f); ENTER; assert(!find("/system/new-note2.txt") && !app.named);
    reset(0); assert(cn_open_argument(&app, "/system") == -EISDIR); assert(dialog_is(202) && strstr(ui.dialog.message, "Is a directory")); ENTER;
    reset(0); assert(cn_open_argument(&app, "bad:name") == -EINVAL); assert(dialog_is(202)); ENTER;
    reset(0); fail_save = -EACCES; assert(cn_open_argument(&app, "/system/denied.txt") == -ENOENT); ENTER;
    assert(dialog_is(202) && strstr(ui.dialog.message, "could not be created") && !app.named); fail_save = 0; ENTER;
}

static void test_file_dialog(void)
{
    reset(0);
    CTRL(0x18);
    assert(app.page == CN_PAGE_OPEN && app.row_count == 3 && !strcmp(app.rows[0].name, "bin") && !strcmp(app.rows[1].name, "home") && !strcmp(app.rows[2].name, "system"));
    assert(!strcmp(du_get(&ui, W_PATH)->text, "/") && strstr(du_get(&ui, 123)->text, "Open: "));
    screen("open-root");
    type("/system/tests"); ENTER;
    assert(!strcmp(app.dir, "/system/tests") && app.has_parent && app.row_count == 4 && !strcmp(app.rows[0].name, "..") &&
           !strcmp(app.rows[1].name, "sub") && !strcmp(app.rows[2].name, "Alpha.TXT") && !strcmp(app.rows[3].name, "beta.txt"));
    assert(!du_get(&ui, W_NAMEEDIT)->text[0] && !strcmp(du_get(&ui, W_PATH)->text, "/system/tests"));
    assert(!strcmp(app.rows[2].type, "Text file") && !strcmp(app.rows[1].type, "Folder") && !strcmp(app.rows[2].size, "17") && !strcmp(app.rows[2].modified, "2026-09-21") && !strcmp(app.rows[3].modified, "2026-10-03"));
    screen("open-tests");
    click(100, 110); assert(!strcmp(du_get(&ui, W_NAMEEDIT)->text, "Alpha.TXT") && app.rows[2].selected && du_get(&ui, W_LIST)->active == 2);
    click(100, 130); assert(!strcmp(du_get(&ui, W_NAMEEDIT)->text, "beta.txt") && !app.rows[2].selected);
    /* Double-click on a folder enters it; on the parent row leaves it. */
    click(100, 90); mono_ns += 100000000ull; click(100, 90);
    assert(!strcmp(app.dir, "/system/tests/sub") && app.row_count == 1);
    mono_ns += 1000000000ull; click(100, 70); mono_ns += 100000000ull; click(100, 70); assert(!strcmp(app.dir, "/system/tests"));
    /* Slow clicks do not activate. */
    click(100, 90); mono_ns += 900000000ull; click(100, 90); assert(!strcmp(app.dir, "/system/tests"));
    /* The file-type button reveals every file. */
    click(300, 310); assert(!app.text_filter && app.row_count == 5 && !strcmp(du_get(&ui, W_TYPE)->text, "All Files (*.*)"));
    assert(!strcmp(app.rows[2].name, "Alpha.TXT") && !strcmp(app.rows[3].name, "beta.txt") && !strcmp(app.rows[4].name, "data.bin"));
    /* Sorting by the Size header; a second click reverses it. Folders stay first. */
    click(300, 41); assert(!strcmp(app.rows[2].name, "data.bin") && !strcmp(app.rows[3].name, "beta.txt") && !strcmp(app.rows[4].name, "Alpha.TXT"));
    click(300, 41); assert(!strcmp(app.rows[2].name, "Alpha.TXT") && !strcmp(app.rows[4].name, "data.bin") && !strcmp(app.rows[1].name, "sub"));
    click(100, 41); assert(!strcmp(app.rows[2].name, "Alpha.TXT")); click(100, 41); assert(!strcmp(app.rows[2].name, "data.bin") && !strcmp(app.rows[4].name, "Alpha.TXT"));
    click(300, 310); assert(app.text_filter);
    /* Opening a file by double-click. */
    assert(!strcmp(app.rows[2].name, "beta.txt"));
    click(100, 110); mono_ns += 100000000ull; click(100, 110);
    assert(app.page == CN_PAGE_EDIT && has_text("lf only\nsecond\n") && !strcmp(app.path, "/system/tests/beta.txt") && !app.crlf);
    /* Open without an extension tries .txt; unknown names stay on the page with a note. */
    CTRL(0x18); assert(!strcmp(app.dir, "/system/tests"));
    type("nothing"); ENTER; assert(app.page == CN_PAGE_OPEN && strstr(app.fd_note, "Cannot find nothing"));
    press(0x01); assert(app.page == CN_PAGE_EDIT);
    CTRL(0x18); type("../hello"); ENTER; assert(app.page == CN_PAGE_EDIT && has_text("hello"));
    CTRL(0x18); type("a:b"); ENTER; assert(app.page == CN_PAGE_OPEN && strstr(app.fd_note, "not valid"));
    ESC; CTRL(0x18); type("tests/data.bin"); ENTER; assert(app.page == CN_PAGE_EDIT && app.ta.len == 3 && !strcmp(app.path, "/system/tests/data.bin"));
    /* The location edit changes folders; Cancel and Esc leave without opening. */
    CTRL(0x18); click(300, 19); key_event(0x1d, 1); press(0x1e); key_event(0x1d, 0); type("/home"); ENTER;
    assert(!strcmp(app.dir, "/home") && app.row_count == 1); click(540, 311); assert(app.page == CN_PAGE_EDIT);
    /* Folder errors are reported on the page. */
    CTRL(0x18); fail_list = -EACCES; type("/system"); ENTER; assert(strstr(app.fd_note, "Cannot open that folder") && !strcmp(app.dir, "/system/tests")); fail_list = 0; ESC;
    /* Save As: starts in the document's folder with its name; typed names get .txt; folders navigate; an explicit extension is kept. */
    reset(0); assert(!cn_open_argument(&app, "/system/tests/beta.txt")); CTRL(0x14f); type("!");
    menu_pick(0x21, 3);
    assert(app.page == CN_PAGE_SAVE && !strcmp(app.dir, "/system/tests") && !strcmp(du_get(&ui, W_NAMEEDIT)->text, "beta.txt") && !strcmp(du_get(&ui, W_OK)->text, "Save"));
    type("sub"); ENTER; assert(!strcmp(app.dir, "/system/tests/sub") && app.page == CN_PAGE_SAVE);
    type("copy.log"); ENTER; assert(app.page == CN_PAGE_EDIT && expect_file("/system/tests/sub/copy.log", "lf only\nsecond\n!") && !strcmp(app.path, "/system/tests/sub/copy.log"));
    CTRL(0x1f); menu_pick(0x21, 3); assert(app.page == CN_PAGE_SAVE && !strcmp(app.dir, "/system/tests/sub") && !strcmp(du_get(&ui, W_NAMEEDIT)->text, "copy.log")); ESC;
}
static void test_menus_and_context(void)
{
    reset(0); fresh_text("hello world");
    click(70, 10); assert(ui.menu == 1);
    screen("menu-edit");
    click(60, 235); assert(ui.menu == -1 && du_ta_has_selection(&app.ta) && app.ta.cursor == 11);      /* Select All (item 9) */
    click_button(2, 300, 100); assert(ui.context == app.context_items && ui.context_count == 6);
    assert(!app.context_items[0].disabled && !app.context_items[1].disabled && !app.context_items[2].disabled && !app.context_items[4].disabled);
    screen("context");
    click(320, 222); assert(!ui.context && du_ta_has_selection(&app.ta));                             /* Select All (item 5) */
    click(300, 100); assert(!du_ta_has_selection(&app.ta));
    click_button(2, 300, 100); assert(app.context_items[1].disabled && app.context_items[2].disabled && app.context_items[4].disabled);
    click(320, 134); assert(!ui.context && has_text("hello world"));                                  /* a disabled Cut does nothing */
    click_button(2, 300, 100); press(0x148); ENTER; assert(du_ta_has_selection(&app.ta));            /* Up wraps to the last item */
    click_button(2, 300, 100); ESC; assert(!ui.context);
    /* Menu bar keys: F10 opens File, Down walks the items, Left wraps across menus. */
    press(0x44); assert(ui.menu == 0 && ui.menu_item == 0); press(0x150); assert(ui.menu_item == 1);
    press(0x14b); assert(ui.menu == 4); press(0x14d); assert(ui.menu == 0); ESC;
    /* A right click outside every widget (on the status bar) opens nothing. */
    click_button(2, 100, CN_HEIGHT - 10); assert(!ui.context);
    /* Menus are closed on the file dialog page and accelerators are inactive there. */
    app.ta.modified = 0; CTRL(0x18); assert(ui.menu_count == 0); press(0x44); assert(ui.menu == -1); CTRL(0x1f); assert(app.page == CN_PAGE_OPEN); ESC;
    assert(ui.menu_count == 5);
}

static void test_mouse_text(void)
{
    reset(0); fresh_text("one two three\nsecond line\nthird"); select_range(0, 0);
    click(4 + 8 * 4 + 1, 30); assert(app.ta.cursor == 4 && !strcmp(status_pos(), "Ln 1, Col 5"));
    key_event(0x2a, 1); click(4 + 8 * 7 + 1, 30); key_event(0x2a, 0); { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 4 && b == 7); }
    mono_ns += 2000000000ull; mouse(4 + 8 * 2, 30); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); mouse(4 + 8 * 3, 28 + 16 + 3); mouse(4 + 8 * 5, 28 + 32 + 3);
    ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
    { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 2 && b == 26 + 5 && app.ta.cursor == b); }     /* drag across three lines */
    assert(!strcmp(status_pos(), "Ln 3, Col 6"));
    mono_ns += 2000000000ull; click(4 + 8 * 5 + 1, 30); mono_ns += 100000000ull; click(4 + 8 * 5 + 1, 30);
    { size_t a, b; du_ta_selection(&app.ta, &a, &b); assert(a == 4 && b == 7); }                              /* double-click: "two" */
    mono_ns += 100000000ull; click(4 + 8 * 5 + 1, 30); assert(!du_ta_has_selection(&app.ta));                /* the third click is single */
    click_button(2, 200, 100); assert(app.ta.cursor == 5);                                                    /* right click keeps the caret */
    ESC;
    /* The scroll bars: 40 lines scroll vertically; the arrow and track move the view. */
    char many[800]; many[0] = 0; for (int i = 0; i < 40; i++) strcat(many, "line number\n");
    fresh_text(many); select_range(0, 0); assert(app.ta.rows == 17);
    int bar_x = CN_WIDTH - 2 - 8, down_y = 26 + 284 - 8;
    click(bar_x, down_y); assert(app.ta.top == 1); click(bar_x, down_y - 40); assert(app.ta.top == 1 + 17);
    click(bar_x, 26 + 4); assert(app.ta.top == 17);
    screen("scrolled-bars");
}

static void test_wrap_navigation(void)
{
    reset(0);
    fresh_text("The quick brown fox jumps over the lazy dog and keeps running through the forest until night falls.");
    menu_pick(0x18, 0); assert(app.ta.wrap && du_ta_visual_lines(&app.ta) == 2);
    press(0x147); assert(app.ta.cursor == 70);                                                  /* Home on the second visual line */
    CTRL(0x147); press(0x14f); assert(app.ta.cursor == 70 && app.ta.affinity);                 /* End: the end of the first visual line */
    CTRL(0x147); assert(app.ta.cursor == 0 && !strcmp(status_pos(), "Ln 1, Col 1"));
    press(0x150); assert(app.ta.cursor == 70 && !app.ta.affinity && !strcmp(status_pos(), "Ln 1, Col 71")); /* Down: the second visual line */
    CTRL(0x14f); assert(app.ta.cursor == app.ta.len);
    screen("wrapped");
    /* Resizing never happens, but a toggle re-lays the text out. */
    menu_pick(0x18, 0); assert(!app.ta.wrap && du_ta_visual_lines(&app.ta) == 1);
    CTRL(0x147); assert(app.ta.left == 0);
}

static void test_screens(void)
{
    reset(0); fresh_text("CiukNote - A modern Retro OS\n\tTabbed line with caf\xc3\xa9\nlast line"); select_range(3, 20); screen("typed");
    ESC; menu_pick(0x21, 3); screen("save-page");
    type("kept"); ENTER; screen("saved");
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
        case 4: mouse((int)rnd(CN_WIDTH + 20) - 10, (int)rnd(CN_HEIGHT + 20) - 10); break;
        case 5: ev(CIUKI_INPUT_BUTTON, 1 + (int)rnd(3), 1, 0); ev(CIUKI_INPUT_BUTTON, 1 + (int)rnd(3), 0, 0); break;
        case 6: ev(CIUKI_INPUT_MOTION, 0, (int)rnd(CN_WIDTH), (int)rnd(CN_HEIGHT)); break;
        case 7: if (!rnd(40)) ev(CIUKI_INPUT_RESYNC, 0, 0, 0); if (!rnd(15)) { fail_save = rnd(3) ? 0 : -ENOSPC; fail_list = rnd(4) ? 0 : -EIO; fail_load = rnd(4) ? 0 : -EIO; } break;
        }
        if (rnd(3) == 0) settle();
        assert(app.page <= CN_PAGE_SAVE && ui.focus >= -1 && ui.focus < (int)ui.count);
        assert(app.ta.cursor <= app.ta.len && app.ta.anchor <= app.ta.len && app.ta.len <= CN_LIMIT && !app.ta.text[app.ta.len]);
        assert(app.row_count == 0 || app.rows);
        assert(ui.menu < (int)ui.menu_count && (!ui.context || ui.context_count));
        if (quit_count) { quit_count = 0; app.after = CN_AFTER_NONE; }
    }
    fail_save = fail_load = fail_list = 0;
    settle();
}

static int replay(const char *script, const char *result_path)
{
    reset(1);
    FILE *f = fopen(script, "r"); if (!f) { perror(script); return 2; }
    char kind[8]; int code, value; char line[256];
    for (;;) {
        int n = fscanf(f, "%7s", kind); if (n != 1) break;
        if (kind[0] == 'K') { if (fscanf(f, "%d %d", &code, &value) != 2) return 2; key_event(code, value); }
        else if (kind[0] == 'S') { settle(); if (cn_checkpoint(&app, line, sizeof(line))) printf("CHECKPOINT %s\n", line); }
        else return 2;
    }
    fclose(f);
    struct node *n = find(result_path);
    printf("FILE %s ", result_path);
    if (n) for (size_t i = 0; i < n->size; i++) printf("%02x", (unsigned char)n->data[i]);
    printf("\nQUIT %d\nPAGE %d\nTEXT %zu\n", quit_count, (int)app.page, app.ta.len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "replay")) return replay(argv[2], argv[3]);
    dump_prefix = argc > 1 ? argv[1] : NULL;
    test_startup(); test_typing(); test_clipboard_and_undo(); test_save_and_open(); test_unsaved_prompt();
    test_save_errors_and_replace(); test_find_replace(); test_goto_wrap_status(); test_time_log_encoding();
    test_limits(); test_open_argument(); test_file_dialog(); test_menus_and_context(); test_mouse_text();
    test_wrap_navigation(); test_screens(); test_random_input();
    puts("PASS ciuknote: typing, shift/caps, clipboard, undo, prompts, save/open, replace prompt, find/replace, go to, wrap, status bar, "
         "time/date, .LOG, line endings, limits, argument, file dialog, menus, context menu, mouse, scroll bars, screens");
    return 0;
}
