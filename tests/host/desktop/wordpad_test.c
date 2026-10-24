/* WordPad application host tests (G28): the production application logic against a fake file system and
 * fixed-advance fonts, driven through libdeskui's input path and renderer.
 *
 *   wordpad_test                run every scenario
 *   wordpad_test <prefix>       also write rendered screens as <prefix>-<name>.xrgb
 * SPDX-License-Identifier: MIT */
#include "app.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake file system ------------------------------------------------------- */
struct node { char path[CN_PATH]; int dir; char *data; size_t size; int64_t mtime; };
static struct node nodes[64]; static unsigned node_count;
static int fail_save, saves;
static struct node *find(const char *path) { for (unsigned i = 0; i < node_count; i++) if (!strcmp(nodes[i].path, path)) return &nodes[i]; return NULL; }
static void add_dir(const char *path) { snprintf(nodes[node_count].path, CN_PATH, "%s", path); nodes[node_count++].dir = 1; }
static void add_file(const char *path, const void *data, size_t size)
{
    struct node *n = find(path);
    if (!n) { n = &nodes[node_count++]; snprintf(n->path, CN_PATH, "%s", path); }
    free(n->data); n->data = malloc(size + 1); memcpy(n->data, data, size); n->size = size; n->mtime = 1791000000;
}
static void fs_reset(void)
{
    for (unsigned i = 0; i < node_count; i++) { free(nodes[i].data); memset(&nodes[i], 0, sizeof(nodes[i])); }
    node_count = 0; fail_save = saves = 0;
    add_dir("/Documents"); add_dir("/system"); add_dir("/system/tests");
}
static int f_stat(void *ctx, const char *path, struct cn_stat *st)
{
    (void)ctx; if (!strcmp(path, "/")) { *st = (struct cn_stat){1, 0, 0}; return 0; }
    struct node *n = find(path); if (!n) return -ENOENT;
    *st = (struct cn_stat){n->dir, n->size, n->mtime}; return 0;
}
static int f_list(void *ctx, const char *dir, cn_emit emit, void *arg)
{
    (void)ctx;
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
    (void)ctx; struct node *n = find(path); if (!n) return -ENOENT; if (n->dir) return -EISDIR;
    if (n->size > cap) return -EFBIG;
    memcpy(buf, n->data, n->size); *len = n->size; return 0;
}
static int f_save(void *ctx, const char *path, const char *data, size_t len)
{
    (void)ctx; if (fail_save) return fail_save;
    struct node *n = find(path);
    if (!n) { n = &nodes[node_count++]; snprintf(n->path, CN_PATH, "%s", path); }
    free(n->data); n->data = malloc(len + 1); memcpy(n->data, data, len); n->size = len; saves++;
    return 0;
}
static const struct cn_fs fake_fs = {NULL, f_stat, f_list, f_load, f_save};

/* ---- fake fonts: a character is size_hp / 2 pixels wide; text is a bar of that width ------- */
static int fw(void *c, unsigned fam, unsigned fl, unsigned size, const char *s, size_t n)
{ (void)c; (void)fam; (void)fl; int k = 0; for (size_t i = 0; i < n; i++) k += ((unsigned char)s[i] & 0xc0) != 0x80; return k * (int)size * 32; }
static void fv(void *c, unsigned fam, unsigned fl, unsigned size, int *a, int *d)
{ (void)c; (void)fam; (void)fl; int px = (int)size * 2 / 3; *a = px * 3 / 4; *d = px / 4 + 1; }
static void fd(void *c, struct du_canvas *cv, unsigned fam, unsigned fl, unsigned em, int x, int base, const char *s, size_t n, uint32_t rgb)
{
    (void)c; (void)fam; (void)fl;
    int chars = 0; for (size_t i = 0; i < n; i++) chars += ((unsigned char)s[i] & 0xc0) != 0x80;
    int px = (int)em / 64, w = chars * px / 2;
    du_fill(cv, (struct du_rect){x + 1, base - px * 3 / 4, w > 2 ? w - 2 : w, px * 3 / 4}, rgb);
}
static const struct wp_draw_ops fake_draw = {{NULL, fw, fv}, fd};

/* ---- host ------------------------------------------------------------------ */
#define NW 640
#define NH 440
static uint32_t canvas[NW * NH];
static struct du_ui ui; static struct wp_app app;
static time_t clock_seconds = 1791711660; static uint64_t mono_ns = 5000000000ull; static int quit_count, declined_count;
static const char *dump_prefix;
static time_t host_now(void *p) { (void)p; return clock_seconds; }
static void host_quit(void *p) { (void)p; quit_count++; }
static void host_declined(void *p) { (void)p; declined_count++; }

static void reset(void)
{
    if (app.ui) wp_detach(&app);
    fs_reset(); du_clipboard_set(""); memset(&app, 0, sizeof(app)); memset(canvas, 0, sizeof(canvas));
    quit_count = declined_count = 0; mono_ns = 5000000000ull; clock_seconds = 1791711660;
    du_init(&ui, canvas, NW, NH);
    app.fs = &fake_fs; app.draw = &fake_draw; app.now = host_now; app.quit = host_quit; app.declined = host_declined; app.a4 = 1;
    assert(!wp_attach(&app, &ui));
}
static void ev(int type, int code, int value, int value2)
{
    struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = mono_ns};
    du_input(&ui, &e);
}
static void settle(void) { if (ui.dirty) du_render(&ui); }
static void key_event(int code, int value) { ev(CIUKI_INPUT_KEY, code, value, 0); }
static void press(int code) { key_event(code, 1); key_event(code, 0); settle(); }
static void chord(int modifier, int code) { key_event(modifier, 1); press(code); key_event(modifier, 0); settle(); }
#define CTRL(c) chord(0x1d, c)
#define SHIFT(c) chord(0x2a, c)
static void alt(int code) { key_event(0x38, 1); press(code); key_event(0x38, 0); settle(); }
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
        for (; row < 3; row++) { const char *at = strchr(p, c); if (at && (at - p) < (int)strlen(p)) return base[row] + (int)(at - p); p += strlen(p) + 1; }
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
static int widget_visible(int id) { return W(id) && !W(id)->hidden; }
static int dialog_is(int id) { return ui.dialog.kind && ui.dialog.id == id; }
static int centre_x(int id) { struct du_rect r = W(id)->box; return r.x + r.w / 2; }
static int centre_y(int id) { struct du_rect r = W(id)->box; return r.y + r.h / 2; }
static void click_widget(int id) { click(centre_x(id), centre_y(id)); }
static void cmd(int id) { du_post(&ui, DU_EV_ACTIVATE, id, 0); settle(); }
static void answer(int index)
{
    struct du_rect r = du_dialog_button(&ui, (unsigned)index);
    click(r.x + r.w / 2, r.y + r.h / 2);
}
static int has_text(const char *s) { return app.doc.len == strlen(s) && !memcmp(app.doc.text, s, app.doc.len); }
static int sel_is(size_t lo, size_t hi) { return app.anchor == lo && app.cursor == hi; }
static int file_is(const char *path, const char *prefix)
{ struct node *n = find(path); return n && !n->dir && n->size >= strlen(prefix) && !memcmp(n->data, prefix, strlen(prefix)); }
static struct wp_cattr attr_at(size_t pos) { return wp_attr_of(&app.doc, pos); }
/* The document area: screen position of text offset `pos` on its line (x) and the line's centre (y). */
static int doc_x(size_t pos) { int x, y, h; wp_layout_caret(&app.lay, &app.doc, &fake_draw.text, pos, &x, &y, &h); return app.doc_box.x + 8 + x; }
static int doc_y(size_t pos) { int x, y, h; wp_layout_caret(&app.lay, &app.doc, &fake_draw.text, pos, &x, &y, &h); return app.doc_box.y - app.top + y + h / 2; }
static void select_range(size_t a, size_t b) { app.anchor = a; app.cursor = b; wp_refresh(&app); du_invalidate(&ui, app.doc_box); settle(); }
static void check_doc(void)
{
    size_t sum = 0; for (unsigned i = 0; i < app.doc.nruns; i++) sum += app.doc.runs[i].len;
    assert(sum == app.doc.len);
    sum = 0; for (unsigned i = 0; i < app.doc.npara; i++) sum += app.doc.paras[i].len;
    assert(sum == app.doc.len);
}

/* ---- scenarios ------------------------------------------------------------------ */
static void test_startup(void)
{
    reset();
    assert(ui.menu_count == 6 && widget_visible(W_DOC) && widget_visible(W_TOOLBAR) && widget_visible(W_FORMATBAR) && widget_visible(W_STATUS) && du_focused(&ui) == W_DOC);
    static const char *const titles[] = {"&File", "&Edit", "&View", "&Insert", "F&ormat", "&Help"};
    for (unsigned m = 0; m < 6; m++) {
        assert(!strcmp(ui.menus[m].label, titles[m]));
        struct du_rect title = du_menu_title(&ui, m), drop = du_menu_box(&ui, ui.menus[m].items, ui.menus[m].count, title.x, title.y + title.h);
        assert(drop.x >= 0 && drop.y + drop.h <= NH && drop.x + drop.w <= NW);
    }
    /* stacked bars: menu bar, toolbar, format bar, ruler, document, status */
    assert(W(W_TOOLBAR)->box.y == 21 && W(W_FORMATBAR)->box.y == 51 && app.ruler_box.y == 79 && app.doc_box.y == 99);
    assert(app.doc_box.y + app.doc_box.h == NH - 20 && app.doc_box.w == NW - 16 && W(W_SCROLL)->box.x == NW - 16);
    assert(!strcmp(W(W_FONTBTN)->text, "Arial") && !strcmp(W(W_SIZEBTN)->text, "10"));
    assert(!strncmp(W(W_STATUS)->text, "Document\t", 9) && strstr(W(W_STATUS)->text, "Ln 1, Col 1"));
    assert(app.lay.n == 1 && app.lay.width == NW - 16 - 16);
    /* every toolbar button has a visible, clipped box */
    for (unsigned i = 0; i < ui.count; i++) {
        struct du_widget *w = &ui.widgets[i];
        if (w->hidden || w->kind != DU_TOOLBUTTON) continue;
        assert(w->box.x >= 0 && w->box.x + w->box.w <= NW && w->box.w > 0 && w->box.h > 0);
        assert(w->icon && w->icon_size == 22);
    }
    /* Print is announced as unavailable, the preview and page setup are present */
    assert((app.file_items[5].flags & DU_ITEM_DISABLED) && !(app.file_items[6].flags & DU_ITEM_DISABLED));
    assert(W(M_PRINT)->disabled);
    screen("empty");
}

static void test_typing_undo(void)
{
    reset();
    type("Hello, World!"); assert(has_text("Hello, World!") && app.cursor == 13 && app.modified && app.doc.npara == 1);
    assert(strstr(W(W_STATUS)->text, "Document *") && strstr(W(W_STATUS)->text, "Col 14"));
    assert(app.doc.nruns == 1 && app.doc.runs[0].len == 13);
    CTRL(0x2c); assert(has_text("") && app.undo_is_redo);                   /* typing is one undo step */
    CTRL(0x15); assert(has_text("Hello, World!") && !app.undo_is_redo);     /* redo */
    /* a cursor move starts a new step: undo takes back only the later text */
    reset(); type("one"); press(0x14b); press(0x14b); type("X"); assert(has_text("oXne"));
    CTRL(0x2c); assert(has_text("one") && app.cursor == 1);
    /* backspaces coalesce into one step; Delete too */
    reset(); type("abc"); press(0x0e); assert(has_text("ab")); press(0x0e); assert(has_text("a"));
    CTRL(0x2c); assert(has_text("abc"));
    reset(); type("abcd"); press(0x147); press(0x153); press(0x153); assert(has_text("cd"));
    CTRL(0x2c); assert(has_text("abcd"));
    /* Enter splits a paragraph, Tab inserts a tab */
    reset(); type("first\nsecond\tx"); assert(has_text("first\nsecond\tx") && app.doc.npara == 2);
    check_doc();
    /* the format of the previous character is used at a caret */
    reset(); type("ab"); select_range(0, 2); CTRL(0x30); select_range(2, 2); type("c"); assert(attr_at(2).flags & WP_BOLD);
}

static void test_navigation_selection(void)
{
    reset();
    type("alpha beta gamma\nsecond line of text\nthird");   /* paragraphs start at 0, 17 and 37; one line each */
    press(0x147); assert(app.cursor == 37);                                           /* Home */
    press(0x148); assert(app.cursor == 17);                                           /* Up keeps the column */
    key_event(0x1d, 1); press(0x147); key_event(0x1d, 0); assert(app.cursor == 0);    /* Ctrl+Home */
    key_event(0x1d, 1); press(0x14f); key_event(0x1d, 0); assert(app.cursor == 42);   /* Ctrl+End */
    key_event(0x1d, 1); press(0x14b); key_event(0x1d, 0); assert(app.cursor == 37);   /* Ctrl+Left: start of "third" */
    key_event(0x1d, 1); press(0x14b); key_event(0x1d, 0); assert(app.cursor == 32);   /* "text" */
    press(0x147); assert(app.cursor == 17); press(0x14d); assert(app.cursor == 18);
    press(0x14f); assert(app.cursor == 36);                                           /* End */
    /* shift extends, a plain arrow collapses to the edge */
    reset(); type("abcdef"); press(0x147);
    SHIFT(0x14d); SHIFT(0x14d); assert(sel_is(0, 2)); press(0x14d); assert(sel_is(2, 2));
    reset(); type("abcdef"); press(0x147); SHIFT(0x14d); SHIFT(0x14d); press(0x14b); assert(sel_is(0, 0));
    CTRL(0x1e); assert(sel_is(0, 6)); type("x"); assert(has_text("x"));
    /* mouse: click, double click (a word with its space), triple click (the paragraph), drag, shift+click */
    reset(); type("hello brave world\nnext para");
    click(doc_x(0) + 52, doc_y(0)); assert(sel_is(5, 5));                              /* a character is 10 px wide */
    click(doc_x(0) + 1, doc_y(0)); assert(app.cursor == 0);
    mono_ns += 1000000000ull;
    click(doc_x(6) + 12, doc_y(6)); mono_ns += 100000000ull; click(doc_x(6) + 12, doc_y(6)); assert(sel_is(6, 12));
    mono_ns += 100000000ull; click(doc_x(6) + 12, doc_y(6)); assert(sel_is(0, 18));
    mono_ns += 2000000000ull;
    mouse(doc_x(0) + 1, doc_y(0)); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); mouse(doc_x(0) + 41, doc_y(0)); ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
    assert(sel_is(0, 4));
    key_event(0x2a, 1); click(doc_x(0) + 101, doc_y(0)); key_event(0x2a, 0); assert(sel_is(0, 10));
    mono_ns += 2000000000ull;
    click_button(2, doc_x(0) + 151, doc_y(0)); assert(ui.popup && sel_is(15, 15)); ESC;
    click(doc_x(0) + 500, app.doc_box.y + app.doc_box.h - 5); assert(app.cursor == app.doc.len);
    /* vertical movement through wrapped lines keeps the column */
    reset(); type("aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa\nxx"); assert(app.lay.n == 3);
    press(0x148); assert(app.cursor == 62); press(0x148); assert(app.cursor == 2);
    press(0x150); assert(app.cursor == 62);
}

static void test_scrolling(void)
{
    reset();
    for (int i = 0; i < 80; i++) type("line\n");
    assert(app.lay.height > app.doc_h && W(W_SCROLL)->total > W(W_SCROLL)->page);
    assert(app.top > 0 && app.top == W(W_SCROLL)->value);              /* the caret at the end is visible */
    int y; { int x, h; wp_layout_caret(&app.lay, &app.doc, &fake_draw.text, app.cursor, &x, &y, &h); assert(y - app.top >= 0 && y + h - app.top <= app.doc_h); }
    key_event(0x1d, 1); press(0x147); key_event(0x1d, 0); assert(app.top == 0 && app.cursor == 0);
    ev(CIUKI_INPUT_MOTION, 0, 300, 200); ev(CIUKI_INPUT_WHEEL, 0, -3, 0); settle();
    assert(app.top > 0 && app.top == W(W_SCROLL)->value);
    int t = app.top; ev(CIUKI_INPUT_WHEEL, 0, 3, 0); settle(); assert(app.top < t);
    press(0x151); assert(app.cursor > 0 && app.top >= 0);                   /* Page Down moves the caret by a page */
    /* the scroll bar */
    click(W(W_SCROLL)->box.x + 8, W(W_SCROLL)->box.y + W(W_SCROLL)->box.h - 40); assert(app.top > 0);
    check_doc();
}

static void test_character_formatting(void)
{
    reset(); type("Hello World");
    select_range(0, 5);
    assert(!strcmp(W(W_FONTBTN)->text, "Arial") && !strcmp(W(W_SIZEBTN)->text, "10") && app.cmix.bold == 0);
    CTRL(0x30); assert((attr_at(0).flags & WP_BOLD) && !(attr_at(5).flags & WP_BOLD) && app.cmix.bold == 1 && app.doc.nruns == 2);
    CTRL(0x30); assert(!(attr_at(0).flags & WP_BOLD) && app.doc.nruns == 1);                      /* toggles off, runs merge */
    cmd(M_ITALIC); cmd(M_UNDERLINE); assert(attr_at(0).flags == (WP_ITALIC | WP_UNDERLINE));
    CTRL(0x17); CTRL(0x16); assert(attr_at(0).flags == 0);
    /* the tool buttons are the same commands */
    click_widget(M_BOLD); assert(attr_at(0).flags & WP_BOLD); click_widget(M_BOLD); assert(!attr_at(0).flags);
    /* mixed selection: the format bar shows no size and toggling sets everything */
    select_range(0, 11); cmd(M_BOLD); select_range(3, 8); cmd(M_BOLD); select_range(0, 11); assert(app.cmix.bold == -1); cmd(M_BOLD); assert(app.cmix.bold == 1 && attr_at(5).flags & WP_BOLD);
    /* font, size and colour popups */
    select_range(0, 5);
    click_widget(W_FONTBTN); assert(ui.popup && ui.popup_count == 3); ESC; assert(!ui.popup);
    cmd(P_FONT + 1); assert(!strcmp(app.doc.fonts[attr_at(0).font].name, "Times New Roman") && !strcmp(W(W_FONTBTN)->text, "Times New Roman"));
    click_widget(W_SIZEBTN); assert(ui.popup); ESC;
    cmd(P_SIZE + 9); assert(attr_at(0).size == 44 && !strcmp(W(W_SIZEBTN)->text, "22"));
    click_widget(W_COLORBTN); assert(ui.popup && ui.popup_count == 17); ESC;
    cmd(P_COLOR + 10); assert(app.doc.colors[attr_at(0).color] == 0xff0000);
    assert(attr_at(5).size != 44 && attr_at(5).color == 0);
    /* with no selection the change applies to the next typed text only */
    assert(attr_at(10).flags & WP_BOLD);
    select_range(11, 11); cmd(M_BOLD); type("!");  assert(!(attr_at(11).flags & WP_BOLD) && (attr_at(10).flags & WP_BOLD));
    check_doc();
    screen("formatted");
    /* a layout with mixed sizes grows the line */
    assert(app.lay.lines[0].h > 13);
}

static void test_paragraph_formatting(void)
{
    reset(); type("one\ntwo\nthree");
    select_range(5, 5);
    CTRL(0x12); assert(app.doc.paras[1].a.align == WP_CENTER && app.doc.paras[0].a.align == WP_LEFT && app.pmix.align == WP_CENTER);
    assert(app.lay.lines[1].shift > 0);
    CTRL(0x13); assert(app.doc.paras[1].a.align == WP_RIGHT && app.lay.lines[1].shift > app.lay.width / 2);
    CTRL(0x26); assert(app.doc.paras[1].a.align == WP_LEFT);
    select_range(0, app.doc.len); cmd(M_CENTER); assert(app.doc.paras[0].a.align == WP_CENTER && app.doc.paras[2].a.align == WP_CENTER);
    cmd(M_LEFT);
    /* bullets */
    select_range(5, 5); cmd(M_BULLET);
    assert(app.doc.paras[1].a.bullet && app.doc.paras[1].a.left == 360 && app.doc.paras[1].a.first == -360 && (app.lay.lines[1].flags & WP_LINE_BULLET));
    assert((app.format_items[1].flags & DU_ITEM_CHECKED) && app.pmix.bullet == 1);
    /* Enter continues the list, Enter on an empty bullet ends it, Backspace at its start removes it */
    select_range(7, 7); press(0x1c); assert(app.doc.paras[2].a.bullet && app.doc.npara == 4);
    press(0x1c); assert(!app.doc.paras[2].a.bullet && app.doc.npara == 4 && app.doc.paras[2].a.left == 0);
    cmd(M_BULLET); assert(app.doc.paras[2].a.bullet); press(0x0e); assert(!app.doc.paras[2].a.bullet && app.doc.npara == 4);
    select_range(5, 5); cmd(M_BULLET); assert(!app.doc.paras[1].a.bullet && app.doc.paras[1].a.left == 0);
    /* the Paragraph dialog */
    select_range(0, 3); cmd(M_PARAGRAPH); assert(dialog_is(D_PARAGRAPH));
    assert(!strcmp(ui.dialog.items[4].text, "0.00") && ui.dialog.items[7].value == 1);
    du_set_text(&ui.dialog.items[4], "0.5"); du_set_text(&ui.dialog.items[5], "1"); du_set_text(&ui.dialog.items[6], "0.25");
    ui.dialog.items[7].value = 0; ui.dialog.items[9].value = 1;
    answer(0); assert(!ui.dialog.kind);
    assert(app.doc.paras[0].a.left == 720 && app.doc.paras[0].a.right == 1440 && app.doc.paras[0].a.first == -360 && app.doc.paras[0].a.align == WP_RIGHT);
    assert(app.lay.lines[0].indent == 24 && !app.doc.paras[1].a.left);
    /* bad input keeps the dialog open with a message */
    cmd(M_PARAGRAPH); du_set_text(&ui.dialog.items[4], "wide"); answer(0); assert(dialog_is(D_PARAGRAPH) && strstr(ui.dialog.text, "inches")); answer(1);
    assert(!ui.dialog.kind && app.doc.paras[0].a.left == 720);
    /* Tabs */
    select_range(4, 4); cmd(M_TABS); assert(dialog_is(D_TABS));
    du_set_text(&ui.dialog.items[4], "2, 0.5 1"); answer(0);
    assert(app.doc.paras[1].a.ntabs == 3 && app.doc.paras[1].a.tabs[0] == 720 && app.doc.paras[1].a.tabs[1] == 1440 && app.doc.paras[1].a.tabs[2] == 2880);
    cmd(M_TABS); assert(!strcmp(ui.dialog.items[4].text, "0.50 1.00 2.00")); du_set_text(&ui.dialog.items[4], ""); answer(0); assert(!app.doc.paras[1].a.ntabs);
    check_doc(); screen("paragraphs");
}

static void test_font_dialog(void)
{
    reset(); type("sample text"); select_range(0, 6);
    cmd(M_FONT); assert(dialog_is(D_FONT) && ui.dialog.buttons == 3);
    assert(ui.dialog.items[4].count == 3 && ui.dialog.items[5].count == 4 && ui.dialog.items[6].count == 16 && ui.dialog.items[7].count == 17);
    assert(ui.dialog.items[6].active == 2);   /* 10 pt */
    screen("font-dialog");
    ui.dialog.items[4].active = 2;            /* Courier New */
    ui.dialog.items[5].active = 3;            /* Bold Italic */
    ui.dialog.items[6].active = 7;            /* 18 pt */
    ui.dialog.items[7].active = 13;           /* blue */
    ui.dialog.items[8].value = 1;
    ui.dialog.items[9].value = 1;
    answer(0);
    struct wp_cattr a = attr_at(0);
    assert(!ui.dialog.kind && a.font == 2 && a.flags == (WP_BOLD | WP_ITALIC | WP_UNDERLINE | WP_STRIKE) && a.size == 36 && app.doc.colors[a.color] == 0x0000ff);
    assert(attr_at(8).flags == 0 && attr_at(8).font == 0);
    cmd(M_FONT); assert(ui.dialog.items[4].active == 2 && ui.dialog.items[5].active == 3 && ui.dialog.items[6].active == 7 && ui.dialog.items[8].value && ui.dialog.items[9].value);
    answer(1); assert(!ui.dialog.kind && attr_at(0).size == 36);
    /* a mixed selection leaves unspecified values alone in the format bar, and the dialog starts at the first run */
    select_range(0, app.doc.len); assert(app.cmix.font == -1 && app.cmix.size == -1 && !W(W_FONTBTN)->text[0]);
}

static void test_clipboard(void)
{
    reset(); type("bold plain");
    select_range(0, 4); CTRL(0x30);
    select_range(0, 10); CTRL(0x2e);
    assert(!strcmp(du_clipboard_get(), "bold plain") && app.clip_valid);
    select_range(10, 10); type("\n"); CTRL(0x2f);
    assert(has_text("bold plain\nbold plain") && (attr_at(11).flags & WP_BOLD) && !(attr_at(16).flags & WP_BOLD));   /* formatting survives */
    check_doc();
    /* cut removes and copies; undo restores */
    select_range(0, 5); CTRL(0x2d); assert(has_text("plain\nbold plain") && !strcmp(du_clipboard_get(), "bold "));
    CTRL(0x2c); assert(has_text("bold plain\nbold plain"));
    /* text from outside (another program) is pasted as plain text with the caret's format */
    du_clipboard_set("ext\r\nernal\x01");
    select_range(0, 0); CTRL(0x2f); assert(has_text("ext\nernalbold plain\nbold plain"));
    assert(app.doc.npara == 3);
    /* pasting replaces the selection; an empty clipboard disables Paste */
    reset(); type("abc"); select_range(1, 2); du_clipboard_set("ZZ"); CTRL(0x2f); assert(has_text("aZZc"));
    du_clipboard_set(""); select_range(1, 1); assert(app.edit_items[3].flags & DU_ITEM_DISABLED && app.edit_items[4].flags & DU_ITEM_DISABLED && !(app.edit_items[5].flags & DU_ITEM_DISABLED));
    /* Clear deletes the selection without touching the clipboard */
    du_clipboard_set("keep"); select_range(0, 2); cmd(M_CLEAR); assert(has_text("Zc") && !strcmp(du_clipboard_get(), "keep"));
}

static void test_find_replace(void)
{
    reset(); type("The cat sat on the mat. The end."); select_range(0, 0);
    CTRL(0x21); assert(app.find_mode == 1 && widget_visible(W_FIND) && !widget_visible(W_REPL) && du_focused(&ui) == W_FIND);
    type("the"); ENTER; assert(sel_is(0, 3));                                    /* from the start: "The" (case-insensitive) */
    ENTER; assert(sel_is(15, 18)); ENTER; assert(sel_is(24, 27));
    ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "not found")); ENTER;      /* end of document */
    click_widget(W_FCASE); assert(app.match_case); select_range(0, 0); du_focus(&ui, W_FIND); ENTER; assert(sel_is(15, 18));
    click_widget(W_FCASE); click_widget(W_FDIR); assert(app.find_up); select_range(32, 32); click_widget(W_FNEXT); assert(sel_is(24, 27));
    click_widget(W_FDIR);
    ESC; assert(!app.find_mode && du_focused(&ui) == W_DOC);
    /* replace and replace all, one undo step for the whole run */
    CTRL(0x23); assert(app.find_mode == 2 && widget_visible(W_REPL));
    du_set_text(W(W_FIND), "at"); du_set_text(W(W_REPL), "og");
    select_range(0, 0); click_widget(W_RONE); assert(sel_is(5, 7));                  /* finds first, does not replace yet */
    click_widget(W_RONE); assert(has_text("The cog sat on the mat. The end.") && sel_is(9, 11));
    select_range(0, 0); click_widget(W_RALL); assert(has_text("The cog sog on the mog. The end.") && strstr(W(W_STATUS)->text, "Replaced 2"));
    CTRL(0x2c); assert(has_text("The cog sat on the mat. The end."));
    click_widget(W_RALL);
    du_set_text(W(W_FIND), "zzz"); click_widget(W_RALL); assert(dialog_is(D_INFO)); ENTER;
    /* a replacement keeps the format of the text it replaces */
    reset(); type("one two"); select_range(0, 3); CTRL(0x30); CTRL(0x23); du_set_text(W(W_FIND), "one"); du_set_text(W(W_REPL), "uno"); select_range(0, 0); click_widget(W_RALL);
    assert(has_text("uno two") && (attr_at(0).flags & WP_BOLD) && !(attr_at(4).flags & WP_BOLD));
    screen("find");
}

static const char SAMPLE_RTF[] =
    "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}{\\f1\\froman Times New Roman;}}{\\colortbl ;\\red255\\green0\\blue0;}"
    "\\pard\\qc\\f1\\fs36\\b Title\\b0\\par\\pard\\f0\\fs20 Body with \\cf1 red\\cf0  text and caf\\'e9.\\par}";

static void test_files(void)
{
    reset();
    add_file("/system/tests/sample.rtf", SAMPLE_RTF, strlen(SAMPLE_RTF));
    add_file("/system/tests/plain.txt", "line one\r\nline two\r\n", 20);
    add_file("/system/tests/latin.txt", "caf\xe9", 4);
    add_file("/system/tests/binary.dat", "\1\2\3", 3);
    /* File > Open through the dialog: the default filter lists rtf and txt files only */
    cmd(M_OPEN); assert(dialog_is(D_OPEN) && !strcmp(ui.dialog.folder, "/"));
    type("/system/tests"); ENTER; assert(dialog_is(D_OPEN) && ui.dialog.entry_count == 3);
    type("/system/tests/sample.rtf"); ENTER;
    assert(!ui.dialog.kind && app.named && app.format == WP_FMT_RTF && !app.modified && !strcmp(app.path, "/system/tests/sample.rtf"));
    assert(has_text("Title\nBody with red text and caf\xc3\xa9.") && app.doc.npara == 2);
    assert(app.doc.paras[0].a.align == WP_CENTER && (attr_at(0).flags & WP_BOLD) && attr_at(0).size == 36 && app.doc.colors[attr_at(16).color] == 0xff0000);
    assert(!strcmp(app.doc.fonts[attr_at(0).font].name, "Times New Roman") && !strcmp(W(W_FONTBTN)->text, "Times New Roman"));
    assert(strstr(W(W_STATUS)->text, "sample.rtf"));
    screen("opened");
    /* edit and save: the file is RTF again and reads back equal */
    select_range(app.doc.len, app.doc.len); type("!"); assert(app.modified);
    CTRL(0x1f); assert(!app.modified && file_is("/system/tests/sample.rtf", "{\\rtf1\\ansi") && saves == 1);
    struct wp_doc before; assert(!wp_init(&before)); assert(!wp_copy(&before, &app.doc));
    cmd(M_NEW); assert(!app.named && has_text("") && !app.path[0]);
    assert(!wp_open_path(&app, "/system/tests/sample.rtf") && has_text("Title\nBody with red text and caf\xc3\xa9.!"));
    assert(app.doc.nruns == before.nruns && app.doc.npara == before.npara); wp_free(&before);
    /* a failing save keeps the document modified and says why */
    fail_save = -ENOSPC; type("more"); CTRL(0x1f);
    assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "could not be saved") && app.modified); ENTER; fail_save = 0;
    CTRL(0x1f); assert(!app.modified);
    /* text files: line endings and encodings; a plain document saves back without a warning */
    assert(!wp_open_path(&app, "/system/tests/plain.txt") && has_text("line one\nline two\n") && app.format == WP_FMT_TEXT && app.crlf);
    type("x"); CTRL(0x1f);
    assert(!dialog_is(D_TEXTWARN) && file_is("/system/tests/plain.txt", "xline one\r\nline two\r\n") && !app.modified);
    assert(!wp_open_path(&app, "/system/tests/latin.txt") && has_text("caf\xc3\xa9"));
    select_range(0, 3); CTRL(0x30);   /* formatting that a text file cannot hold */
    CTRL(0x1f); assert(dialog_is(D_TEXTWARN)); answer(1);
    assert(!ui.dialog.kind && file_is("/system/tests/latin.txt", "caf\xe9") && app.modified);
    CTRL(0x1f); assert(dialog_is(D_TEXTWARN)); answer(0);
    assert(!ui.dialog.kind && !app.modified && file_is("/system/tests/latin.txt", "caf\xc3\xa9"));
    /* Save As: default extension, overwrite question, text-only warning */
    cmd(M_NEW); type("new document"); select_range(0, 3); CTRL(0x30);
    CTRL(0x1f); assert(dialog_is(D_SAVE)); type("/Documents/memo"); ENTER;
    assert(!ui.dialog.kind && app.named && !strcmp(app.path, "/Documents/memo.rtf") && file_is("/Documents/memo.rtf", "{\\rtf1") && app.format == WP_FMT_RTF && !app.modified);
    cmd(M_SAVEAS); assert(dialog_is(D_SAVE));
    type("/Documents/memo.rtf"); ENTER; assert(dialog_is(D_REPLACE)); answer(1); assert(dialog_is(D_SAVE)); ESC;
    cmd(M_SAVEAS); type("/Documents/memo.txt"); ENTER; assert(dialog_is(D_TEXTWARN)); answer(0);
    assert(file_is("/Documents/memo.txt", "new document") && app.format == WP_FMT_TEXT && !strcmp(app.path, "/Documents/memo.txt"));
    /* errors: a missing file, Word and Write documents, a folder */
    assert(wp_open_path(&app, "/system/missing.rtf") == -ENOENT && dialog_is(D_INFO)); ENTER;
    add_file("/system/tests/old.doc", "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1....", 12);
    assert(wp_open_path(&app, "/system/tests/old.doc") == -ENOTSUP && strstr(ui.dialog.text, ".wri")); ENTER;
    add_file("/system/tests/old.wri", "\x31\xbe\0\0\xab\0\0\0", 8); assert(wp_open_path(&app, "/system/tests/old.wri") == -ENOTSUP); ENTER;
    assert(wp_open_path(&app, "/system/tests") == -EISDIR); ENTER;
    /* pictures in a document are reported, not silently lost */
    const char *pic = "{\\rtf1 before{\\pict\\wmetafile8 0102}after}";
    add_file("/system/tests/pic.rtf", pic, strlen(pic));
    assert(!wp_open_path(&app, "/system/tests/pic.rtf") && has_text("beforeafter") && dialog_is(D_NOTICE) && strstr(ui.dialog.text, "1 picture")); ENTER;
    /* Ctrl+N and Alt+F */
    CTRL(0x31); assert(has_text("") && !app.named);
    alt(0x21); assert(ui.menu == 0); ESC; assert(ui.menu == -1);
}

static void test_large_files(void)
{
    reset();
    size_t n = 3u << 20; char *big = malloc(n + 1);
    for (size_t i = 0; i < n; i++) big[i] = i % 70 == 69 ? '\n' : i % 9 == 8 ? ' ' : 'a' + (char)(i % 26);
    add_file("/system/tests/big.txt", big, n);
    assert(!wp_open_path(&app, "/system/tests/big.txt") && app.doc.len == n && app.doc.npara > 40000 && !ui.dialog.kind);
    assert(app.lay.n >= app.doc.npara && app.lay.height > 100000);
    /* editing and scrolling in the middle of a large document */
    select_range(app.doc.len / 2, app.doc.len / 2);
    type("MID"); assert(app.doc.len == n + 3); check_doc();
    key_event(0x1d, 1); press(0x147); key_event(0x1d, 0); assert(app.top == 0);
    key_event(0x1d, 1); press(0x14f); key_event(0x1d, 0); assert(app.cursor == app.doc.len && app.top > 0);
    /* a file beyond the 16 MiB read limit is refused with its reason */
    char *huge = malloc(WP_FILE_MAX + 2); memset(huge, 'x', WP_FILE_MAX + 1);
    add_file("/system/tests/huge.txt", huge, WP_FILE_MAX + 1);
    assert(wp_open_path(&app, "/system/tests/huge.txt") == -EFBIG && dialog_is(D_INFO) && strstr(ui.dialog.text, "16 MiB")); ENTER;
    /* a document at the 8 MiB text limit refuses more text and says so */
    char *limit = malloc(WP_LIMIT + 8); memset(limit, 'y', WP_LIMIT);
    add_file("/system/tests/limit.txt", limit, WP_LIMIT);
    assert(!wp_open_path(&app, "/system/tests/limit.txt") && app.doc.len == WP_LIMIT);
    type("z"); assert(app.doc.len == WP_LIMIT && strstr(W(W_STATUS)->text, "size limit"));
    free(big); free(huge); free(limit);
}

static void test_changes_prompts(void)
{
    reset();
    assert(wp_close_request(&app) == 1);
    type("text"); assert(wp_close_request(&app) == 0 && dialog_is(D_CHANGES) && app.after == WP_AFTER_EXIT);
    answer(2); assert(!ui.dialog.kind && !quit_count && declined_count == 1);                   /* Cancel declines the close */
    cmd(M_NEW); assert(dialog_is(D_CHANGES) && app.after == WP_AFTER_NEW); answer(1); assert(has_text("") && !app.modified);   /* No: discard */
    type("keep"); cmd(M_OPEN); assert(dialog_is(D_CHANGES)); answer(2); assert(has_text("keep") && !ui.dialog.kind);
    cmd(M_EXIT); assert(dialog_is(D_CHANGES)); answer(0); assert(dialog_is(D_SAVE));                  /* Yes asks for a name */
    type("/Documents/k"); ENTER; assert(file_is("/Documents/k.rtf", "{\\rtf1") && quit_count == 1);
    reset(); cmd(M_EXIT); assert(quit_count == 1 && !ui.dialog.kind);
    /* a dropped file is opened after the question */
    reset(); add_file("/system/tests/d.txt", "dropped", 7); type("x");
    { char drop[512]; const char *tmp = getenv("TMPDIR"); snprintf(drop, sizeof(drop), "%s/wordpad-drop.data", tmp ? tmp : ".");
      FILE *f = fopen(drop, "wb"); assert(f && fputs("X\nF/system/tests/d.txt\n", f) >= 0 && !fclose(f)); assert(!du_drop_source(drop)); }
    wp_event(&ui, &(struct du_event){.type = DU_EV_DROPFILES, .x = 20, .y = 30}, &app);
    assert(dialog_is(D_CHANGES) && app.after == WP_AFTER_DROP); answer(1); assert(has_text("dropped"));
}

static void test_dialogs_insert_page_menu(void)
{
    reset(); type("x");
    cmd(M_DATETIME); assert(dialog_is(D_DATETIME) && ui.dialog.items[4].count == 7);
    assert(!strcmp(app.date_text[0], "10/11/2026") && !strcmp(app.date_text[1], "Sunday, October 11, 2026") && !strcmp(app.date_text[4], "9:41 AM") && !strcmp(app.date_text[6], "9:41 AM 10/11/2026"));
    ui.dialog.items[4].active = 1; answer(0); assert(has_text("xSunday, October 11, 2026"));
    cmd(M_DATETIME); ESC; assert(has_text("xSunday, October 11, 2026"));
    /* view toggles */
    int h = app.doc_box.h;
    cmd(M_TOOLBAR); assert(!widget_visible(W_TOOLBAR) && app.doc_box.h == h + 30 && W(W_FORMATBAR)->box.y == 21);
    cmd(M_FORMATBAR); assert(!widget_visible(W_FORMATBAR) && app.doc_box.h == h + 58 && !widget_visible(M_BOLD));
    cmd(M_RULER); assert(app.doc_box.h == h + 78 && app.ruler_box.h == 0);
    cmd(M_STATUS); assert(!widget_visible(W_STATUS) && app.doc_box.h == h + 98);
    cmd(M_TOOLBAR); cmd(M_FORMATBAR); cmd(M_RULER); cmd(M_STATUS); assert(app.doc_box.h == h && (app.view_items[0].flags & DU_ITEM_CHECKED));
    /* Page Setup */
    cmd(M_PAGESETUP); assert(dialog_is(D_PAGESETUP) && ui.dialog.items[4].active == 2 && !ui.dialog.items[5].value && !strcmp(ui.dialog.items[6].text, "0.75"));
    ui.dialog.items[4].active = 0; ui.dialog.items[5].value = 1; du_set_text(&ui.dialog.items[8], "1.5"); answer(0);
    assert(!ui.dialog.kind && app.page.paper_w == 12240 && app.page.landscape && app.page.top == 2160);
    cmd(M_PAGESETUP); du_set_text(&ui.dialog.items[6], "9"); du_set_text(&ui.dialog.items[7], "9"); answer(0); assert(dialog_is(D_PAGESETUP) && strstr(ui.dialog.text, "no room")); answer(1);
    cmd(M_HELP); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Ctrl+B")); ENTER;
    cmd(M_ABOUT); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Classic computing, one woof at a time.")); ENTER;
    /* the context menu */
    reset(); type("abc"); select_range(0, 3); click_button(2, doc_x(1), doc_y(1)); assert(ui.popup && ui.popup_count == 7 && sel_is(0, 3)); screen("context"); ESC;
}

static void test_preview(void)
{
    reset();
    for (int i = 0; i < 160; i++) type("Preview line text\n");
    cmd(M_PREVIEW); assert(app.preview && app.preview_valid && app.pages.count >= 2 && app.preview_page == 0);
    assert(!widget_visible(W_FORMATBAR) && !widget_visible(W_TOOLBAR) && widget_visible(M_PV_NEXT) && !widget_visible(W_FONTBTN));
    assert(strstr(W(W_STATUS)->text, "Page 1 of"));
    screen("preview-1");
    click_widget(M_PV_NEXT); assert(app.preview_page == 1 && strstr(W(W_STATUS)->text, "Page 2 of"));
    click_widget(M_PV_PREV); assert(app.preview_page == 0);
    press(0x151); assert(app.preview_page == 1); press(0x149); assert(app.preview_page == 0);
    int z = app.preview_zoom; click_widget(M_PV_ZOOMOUT); assert(app.preview_zoom <= z); click_widget(M_PV_ZOOMIN); assert(app.preview_zoom >= 25);
    ESC; assert(!app.preview && widget_visible(W_FORMATBAR) && widget_visible(W_TOOLBAR) && !app.pages.count);
    /* editing continues after the preview with a correct layout; landscape pages are more numerous */
    type("tail"); assert(app.doc.len > 100 && app.lay.n > 150);
    cmd(M_PREVIEW); unsigned portrait = app.pages.count; click_widget(M_PV_CLOSE);
    app.page.landscape = 1; cmd(M_PREVIEW); assert(app.pages.count >= portrait); cmd(M_PV_CLOSE); assert(!app.preview);
    /* Exit from the preview still asks about changes */
    cmd(M_PREVIEW); cmd(M_EXIT); assert(dialog_is(D_CHANGES)); answer(2);
}

static void test_ruler_and_layout_ui(void)
{
    reset(); type("text");
    select_range(0, 4); cmd(M_BULLET); screen("ruler-bullet");
    assert(app.ruler_box.h == 20);
    /* a narrow window keeps every widget inside the canvas */
    cmd(M_FIND); cmd(M_REPLACE); screen("replace-bar");
    for (unsigned i = 0; i < ui.count; i++) { struct du_widget *w = &ui.widgets[i]; if (w->hidden) continue; assert(w->box.x >= 0 && w->box.y >= 0 && w->box.x + w->box.w <= NW + 1 && w->box.y + w->box.h <= NH + 1); }
}

int main(int argc, char **argv)
{
    if (argc > 1) dump_prefix = argv[1];
    test_startup(); test_typing_undo(); test_navigation_selection(); test_scrolling(); test_character_formatting();
    test_paragraph_formatting(); test_font_dialog(); test_clipboard(); test_find_replace(); test_files(); test_large_files();
    test_changes_prompts(); test_dialogs_insert_page_menu(); test_preview(); test_ruler_and_layout_ui();
    wp_detach(&app);
    puts("PASS wordpad");
    return 0;
}
