/* WordPad application host tests (G28): the production application logic against a fake file system and
 * fixed-advance fonts, driven through libdeskui's input path and renderer.
 *
 *   wordpad_test                run every scenario
 *   wordpad_test <prefix>       also write rendered screens as <prefix>-<name>.xrgb
 * SPDX-License-Identifier: MIT */
#include "app.h"
#include "../../../apps/libdeskui/clipboard.h"
#include "../../../apps/libdeskui/clipstore.h"
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

/* ---- fake pictures (G28b): a decoder that reads the size from the header, an encoder that writes a PNG header ----------- */
static unsigned char png_head[33] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 16, 0, 0, 0, 8, 8, 2, 0, 0, 0, 1, 2, 3, 4};
static void png_size(unsigned char *h, unsigned w, unsigned ht) { h[18] = (unsigned char)(w >> 8); h[19] = (unsigned char)w; h[22] = (unsigned char)(ht >> 8); h[23] = (unsigned char)ht; }
static int fake_decode(unsigned kind, const uint8_t *d, size_t n, uint32_t **px, unsigned *w, unsigned *h)
{
    uint32_t sw = 0, sh = 0;
    if (kind == WP_PIC_WMF || kind == WP_PIC_EMF || kind == WP_PIC_PICT) return -ENOTSUP;
    if (!wp_pic_sniff(d, n, &sw, &sh)) { if (n >= 3 && !memcmp(d, "GIF", 3)) { sw = sh = 4; } else return -EINVAL; }
    if (!sw || !sh || (uint64_t)sw * sh > 4000000) return -EFBIG;
    *px = malloc((size_t)sw * sh * 4);
    for (unsigned y = 0; y < sh; y++) for (unsigned x = 0; x < sw; x++) (*px)[(size_t)y * sw + x] = ((x * 255 / sw) << 16) | ((y * 255 / sh) << 8) | 0x40;
    *w = sw; *h = sh;
    return 0;
}
static int fake_png(const uint32_t *px, unsigned w, unsigned h, uint8_t **out, size_t *len)
{
    (void)px; *out = malloc(sizeof(png_head)); memcpy(*out, png_head, sizeof(png_head)); png_size(*out, w, h); *len = sizeof(png_head);
    return 0;
}
static const struct wp_image_ops fake_images = {fake_decode, fake_png};
static int printed; static struct cp_page printed_page;
static void host_print(void *p, const struct cp_page *initial) { (void)p; printed++; printed_page = *initial; }
static char help_seen[64];
static int fake_help(struct du_ui *u, const char *topic, void *ctx) { (void)u; (void)ctx; snprintf(help_seen, sizeof(help_seen), "%s", topic ? topic : ""); return 0; }

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
    app.images = &fake_images; app.print = host_print; printed = 0; help_seen[0] = 0;
    ui.help_open = fake_help; du_help_topic(&ui, "wordpad");
    assert(!wp_attach(&app, &ui));
}
/* A new session on the same file system: the settings are read again. */
static void reattach(void)
{
    wp_detach(&app); memset(&app, 0, sizeof(app)); memset(canvas, 0, sizeof(canvas));
    du_init(&ui, canvas, NW, NH);
    app.fs = &fake_fs; app.draw = &fake_draw; app.now = host_now; app.quit = host_quit; app.declined = host_declined; app.a4 = 1; app.images = &fake_images;
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
    /* Print, the preview and page setup are present (G31: Print goes to the host's print hook) */
    assert(!(app.file_items[5].flags & DU_ITEM_DISABLED) && !(app.file_items[6].flags & DU_ITEM_DISABLED));
    assert(!W(M_PRINT)->disabled);
    screen("empty");
}

static void test_typing_undo(void)
{
    reset();
    type("Hello, World!"); assert(has_text("Hello, World!") && app.cursor == 13 && app.modified && app.doc.npara == 1);
    assert(strstr(W(W_STATUS)->text, "Document *") && strstr(W(W_STATUS)->text, "Col 14"));
    assert(app.doc.nruns == 1 && app.doc.runs[0].len == 13);
    CTRL(0x2c); assert(has_text("") && app.nredo == 1 && !app.nundo);       /* typing is one undo step */
    CTRL(0x15); assert(has_text("Hello, World!") && !app.nredo && app.nundo == 1);   /* redo */
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
    assert(!strcmp(ui.dialog.items[4].text, "0\"") && ui.dialog.items[7].value == 1);
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
    cmd(M_TABS); assert(!strcmp(ui.dialog.items[4].text, "0.5\" 1\" 2\"")); du_set_text(&ui.dialog.items[4], ""); answer(0); assert(!app.doc.paras[1].a.ntabs);
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
    /* errors: a missing file, damaged Word and Write documents, a folder */
    assert(wp_open_path(&app, "/system/missing.rtf") == -ENOENT && dialog_is(D_INFO)); ENTER;
    add_file("/system/tests/old.doc", "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1....", 12);
    assert(wp_open_path(&app, "/system/tests/old.doc") == -EINVAL && strstr(ui.dialog.text, "not a valid Word document")); ENTER;
    char wri[256]; memset(wri, 0, sizeof(wri)); wri[0] = 0x31; wri[1] = (char)0xbe; wri[5] = (char)0xab;   /* a Write header whose pnMac is 0: a Word document */
    add_file("/system/tests/old.wri", wri, sizeof(wri));
    assert(wp_open_path(&app, "/system/tests/old.wri") == -EINVAL && strstr(ui.dialog.text, "Word document")); ENTER;
    assert(wp_open_path(&app, "/system/tests") == -EISDIR); ENTER;
    add_file("/system/tests/photo.png", png_head, sizeof(png_head));
    assert(wp_open_path(&app, "/system/tests/photo.png") == -EINVAL && dialog_is(D_INFO) && strstr(ui.dialog.text, "Insert > Picture")); ENTER;   /* a picture is not read as text */
    /* pictures are kept (a metafile shows as a frame); a picture without data is reported, not silently lost */
    const char *pic = "{\\rtf1 before{\\pict\\wmetafile8 0102}after}";
    add_file("/system/tests/pic.rtf", pic, strlen(pic));
    assert(!wp_open_path(&app, "/system/tests/pic.rtf") && !ui.dialog.kind && app.doc.len == 6 + 3 + 5 && wp_pic_count_used(&app.doc) == 1);
    const char *nopic = "{\\rtf1 before{\\pict\\pngblip}after}";
    add_file("/system/tests/nopic.rtf", nopic, strlen(nopic));
    assert(!wp_open_path(&app, "/system/tests/nopic.rtf") && has_text("beforeafter") && dialog_is(D_NOTICE) && strstr(ui.dialog.text, "1 picture")); ENTER;
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
    assert(!strcmp(app.date_text[0], "10/11/26") && !strcmp(app.date_text[1], "Sunday, October 11, 2026") && !strcmp(app.date_text[4], "09:41:00") && !strcmp(app.date_text[6], "10/11/26 09:41:00"));   /* the C locale's pictures */
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
    cmd(M_PAGESETUP); assert(dialog_is(D_PAGESETUP) && ui.dialog.items[4].active == 2 && !ui.dialog.items[5].value && !strcmp(ui.dialog.items[6].text, "0.75\""));
    ui.dialog.items[4].active = 0; ui.dialog.items[5].value = 1; du_set_text(&ui.dialog.items[8], "1.5"); answer(0);
    assert(!ui.dialog.kind && app.page.paper_w == 12240 && app.page.landscape && app.page.top == 2160);
    cmd(M_PAGESETUP); du_set_text(&ui.dialog.items[6], "9"); du_set_text(&ui.dialog.items[7], "9"); answer(0); assert(dialog_is(D_PAGESETUP) && strstr(ui.dialog.text, "no room")); answer(1);
    ui.help_open = NULL; cmd(M_HELP); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Ctrl+B")); ENTER;   /* without CiukiOS Help: the summary box */
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

/* ---- G28b scenarios ------------------------------------------------------------------------------------------------------ */

static void drag(int x0, int y0, int x1, int y1)
{
    mouse(x0, y0); ev(CIUKI_INPUT_BUTTON, 1, 1, 0); settle();
    mouse((x0 + x1) / 2, (y0 + y1) / 2); mouse(x1, y1); settle();
    ev(CIUKI_INPUT_BUTTON, 1, 0, 0); settle();
}
static void test_undo_levels(void)
{
    reset(); type("one two three");
    select_range(0, 3); CTRL(0x30); select_range(4, 7); CTRL(0x17); select_range(8, 13); CTRL(0x16);
    assert(app.nundo == 4 && !app.nredo && (attr_at(0).flags & WP_BOLD) && (attr_at(4).flags & WP_ITALIC) && (attr_at(8).flags & WP_UNDERLINE));
    CTRL(0x2c); assert(!(attr_at(8).flags & WP_UNDERLINE) && (attr_at(4).flags & WP_ITALIC) && sel_is(8, 13));   /* the selection comes back too */
    CTRL(0x2c); assert(!(attr_at(4).flags & WP_ITALIC) && (attr_at(0).flags & WP_BOLD));
    CTRL(0x2c); assert(!(attr_at(0).flags & WP_BOLD) && has_text("one two three") && app.nundo == 1 && app.nredo == 3);
    CTRL(0x15); CTRL(0x15); assert((attr_at(0).flags & WP_BOLD) && (attr_at(4).flags & WP_ITALIC) && !(attr_at(8).flags & WP_UNDERLINE) && app.nredo == 1);
    assert(!(app.edit_items[1].flags & DU_ITEM_DISABLED) && !(app.edit_items[0].flags & DU_ITEM_DISABLED));
    /* a new edit ends the redo chain */
    select_range(13, 13); type("!"); assert(!app.nredo && (app.edit_items[1].flags & DU_ITEM_DISABLED));
    CTRL(0x2c); assert(has_text("one two three")); CTRL(0x2c); CTRL(0x2c); CTRL(0x2c); assert(!app.nundo && (app.edit_items[0].flags & DU_ITEM_DISABLED) && has_text(""));   /* the typing itself was the first level */
    CTRL(0x2c); assert(has_text(""));   /* nothing left to undo: harmless */
    /* at most WP_UNDO_LEVELS levels */
    reset(); type("abc");
    for (int i = 0; i < WP_UNDO_LEVELS + 30; i++) { select_range(0, 3); CTRL(0x30); }
    assert(app.nundo == WP_UNDO_LEVELS);
    for (int i = 0; i < WP_UNDO_LEVELS; i++) CTRL(0x2c);
    assert(!app.nundo && app.nredo == WP_UNDO_LEVELS && has_text("abc"));
    /* the byte cap (lowered for the test) drops the oldest levels; one document over the cap keeps none */
    reset();
    { char line[64]; size_t n = 0; char *big = malloc(300000);
      memset(line, 'x', 62); line[62] = '\n'; line[63] = 0;
      for (; n + 63 < 300000; n += 63) memcpy(big + n, line, 63);
      assert(!wp_insert(&app.doc, 0, big, n, NULL)); free(big); app.dirty_para = 0; app.lay.pcount = 0; wp_refresh(&app);
      for (int i = 0; i < 6; i++) { select_range(0, 1); CTRL(0x30); }
      assert(app.nundo >= 1 && app.nundo < 6 && app.undo_bytes <= WP_UNDO_BYTES);   /* the oldest levels were dropped */
      big = malloc(700000); memset(big, 'y', 700000);
      assert(!wp_insert(&app.doc, app.doc.len, big, 700000, NULL)); free(big);
      select_range(0, 1); CTRL(0x17); assert(!app.nundo && !app.nredo);   /* a snapshot larger than the cap: no undo, no crash */
    }
    /* a picture insertion is one undo level, and its redo brings the picture back */
    reset(); type("ab"); select_range(1, 1);
    cmd(M_INSERTOBJ); answer(0); assert(app.doc.len == 5 && wp_pic_at(&app.doc, 1));
    CTRL(0x2c); assert(has_text("ab") && !wp_pic_count_used(&app.doc));
    CTRL(0x15); assert(app.doc.len == 5 && wp_pic_at(&app.doc, 1) && wp_pic_count_used(&app.doc) == 1);
}

static void test_justify_ui(void)
{
    reset(); type("aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq rr ss tt uu vv ww xx yy zz");
    select_range(0, 0); CTRL(0x24); settle();   /* Ctrl+J */
    assert(app.doc.paras[0].a.align == WP_JUSTIFY && app.pmix.align == WP_JUSTIFY);
    assert(app.lay.n > 1 && app.lay.lines[0].gaps > 0 && app.lay.lines[0].extra > 0 && !app.lay.lines[app.lay.n - 1].extra);
    screen("justified");
    /* the Paragraph dialog has the fourth alignment; the others switch back */
    cmd(M_PARAGRAPH); assert(dialog_is(D_PARAGRAPH) && ui.dialog.items[10].kind == DU_RADIO && ui.dialog.items[10].value == 1 && !ui.dialog.items[7].value);
    ui.dialog.items[10].value = 0; ui.dialog.items[8].value = 1; answer(0); assert(app.doc.paras[0].a.align == WP_CENTER && !app.lay.lines[0].extra);
    cmd(M_PARAGRAPH); ui.dialog.items[8].value = 0; ui.dialog.items[10].value = 1; answer(0); assert(app.doc.paras[0].a.align == WP_JUSTIFY);
    /* the caret and the pointer agree on a justified line */
    for (size_t pos = 0; pos <= app.lay.lines[0].end; pos += 3) { click(doc_x(pos), doc_y(pos)); assert(app.cursor == pos || app.doc.text[pos] == ' ' || app.doc.text[pos - 1] == ' '); }
}

static void test_options_units_wrap(void)
{
    reset(); type("text");
    cmd(M_OPTIONS); assert(dialog_is(D_OPTIONS) && ui.dialog.items[4].value && ui.dialog.items[8].value);
    screen("options-dialog");
    ui.dialog.items[4].value = 0; ui.dialog.items[5].value = 1;   /* centimeters */
    answer(0); assert(!ui.dialog.kind && app.units == WP_UNIT_CM);
    cmd(M_PARAGRAPH); assert(!strcmp(ui.dialog.items[4].text, "0 cm"));
    du_set_text(&ui.dialog.items[4], "1.27"); du_set_text(&ui.dialog.items[5], "5 mm"); du_set_text(&ui.dialog.items[6], "1.27"); answer(0);
    assert(app.doc.paras[0].a.left == 720 && app.doc.paras[0].a.right == 283 && app.doc.paras[0].a.first == 0);   /* the units follow: 1.27 cm = 0.5 inch, 5 mm = 283 twips */
    cmd(M_PARAGRAPH); assert(!strcmp(ui.dialog.items[4].text, "1.27 cm") && !strcmp(ui.dialog.items[5].text, "0.5 cm")); du_set_text(&ui.dialog.items[4], "wide"); answer(0);
    assert(dialog_is(D_PARAGRAPH) && strstr(ui.dialog.text, "centimeters")); answer(1);
    du_set_text(&ui.dialog.items[4], "1 in");   /* a unit typed with the number wins over the default */
    cmd(M_PARAGRAPH); du_set_text(&ui.dialog.items[4], "1 in"); answer(0); assert(app.doc.paras[0].a.left == 1440);
    cmd(M_TABS); du_set_text(&ui.dialog.items[4], "2cm 36pt 1.5pi"); answer(0);
    assert(app.doc.paras[0].a.ntabs == 3 && app.doc.paras[0].a.tabs[0] == 360 && app.doc.paras[0].a.tabs[1] == 720 && app.doc.paras[0].a.tabs[2] == 1134);   /* 1.5 picas, 36 pt, 2 cm */
    screen("ruler-units");
    /* points and picas on the Page Setup margins */
    cmd(M_OPTIONS); ui.dialog.items[5].value = 0; ui.dialog.items[6].value = 1; answer(0); assert(app.units == WP_UNIT_POINT);
    cmd(M_PAGESETUP); assert(!strcmp(ui.dialog.items[6].text, "54 pt")); answer(1);
    cmd(M_OPTIONS); ui.dialog.items[6].value = 0; ui.dialog.items[7].value = 1; answer(0); cmd(M_PAGESETUP); assert(!strcmp(ui.dialog.items[6].text, "4.5 pi")); answer(1);
    /* word wrap: no wrap runs the lines on and shows the horizontal scroll bar */
    reset();
    type("this long line has no wrap and needs the horizontal scroll bar to be read to the very end of it, then more");
    assert(!widget_visible(W_HSCROLL) && app.lay.n > 1);
    cmd(M_OPTIONS); ui.dialog.items[8].value = 0; ui.dialog.items[9].value = 1; answer(0);
    assert(app.wrap[app.format] == WP_WRAP_NONE && app.lay.n == 1 && app.lay.content_w > app.doc_w && widget_visible(W_HSCROLL) && app.hs_visible);
    assert(app.doc_box.h == NH - 20 - 99 - 16 && W(W_HSCROLL)->box.y == app.doc_box.y + app.doc_box.h);
    assert(W(W_HSCROLL)->total == app.lay.content_w + 16 && W(W_HSCROLL)->page == app.doc_box.w);
    assert(app.left > 0);   /* the caret is at the end of the line and stays in view */
    int left = app.left;
    wp_event(&ui, &(struct du_event){.type = DU_EV_CHANGE, .id = W_HSCROLL}, &app);   /* the toolkit moves the bar and tells us */
    press(0x147); assert(app.cursor == 0 && app.left == 0 && left > 0);                   /* Home scrolls back */
    screen("nowrap-hscroll");
    click(doc_x(5), doc_y(5)); assert(app.cursor == 5);
    /* wrap to ruler: the width of the printable page */
    cmd(M_OPTIONS); ui.dialog.items[9].value = 0; ui.dialog.items[10].value = 1; answer(0);
    assert(app.wrap[app.format] == WP_WRAP_RULER && app.lay.width == 650 && !app.lay.nowrap && app.lay.n > 1);
    cmd(M_OPTIONS); ui.dialog.items[10].value = 0; ui.dialog.items[8].value = 1; answer(0); assert(app.wrap[app.format] == WP_WRAP_WINDOW && !widget_visible(W_HSCROLL) && app.left == 0);
    /* every setting is kept for the next session */
    cmd(M_OPTIONS); ui.dialog.items[4].value = 0; ui.dialog.items[7].value = 1; ui.dialog.items[8].value = 0; ui.dialog.items[9].value = 1;
    ui.dialog.items[12].value = 0; ui.dialog.items[14].value = 0; answer(0);
    assert(file_is(WP_SETTINGS, "units=3\n") && strstr(find(WP_SETTINGS)->data, "wrap_rich=1") && strstr(find(WP_SETTINGS)->data, "formatbar=0") && strstr(find(WP_SETTINGS)->data, "statusbar=0"));
    reattach();
    assert(app.units == WP_UNIT_PICA && app.wrap[WP_FMT_RTF] == WP_WRAP_NONE && app.wrap[WP_FMT_TEXT] == WP_WRAP_WINDOW && !app.show_format && !app.show_status && app.show_toolbar && !widget_visible(W_FORMATBAR));
    /* the wrap of a document type applies to that type: a text file opened keeps its own setting */
    add_file("/system/tests/long.txt", "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z", 155);
    assert(!wp_open_path(&app, "/system/tests/long.txt") && app.format == WP_FMT_TEXT && !app.lay.nowrap);
}

static void test_ruler_drag(void)
{
    reset(); type("one\ntwo"); select_range(0, 0);
    int y_top = app.ruler_box.y + 3, y_low = app.ruler_box.y + app.ruler_box.h - 5, x0 = 8;
    /* the first-line marker (upper triangle) */
    unsigned levels = app.nundo;
    drag(x0, y_top, x0 + 48, y_top);
    assert(app.doc.paras[0].a.first == 720 && !app.doc.paras[0].a.left && app.doc.paras[1].a.first == 0 && app.nundo == levels + 1);
    CTRL(0x2c); assert(app.doc.paras[0].a.first == 0 && app.nundo == levels);        /* one undo level for the whole drag */
    /* the left marker moves the whole paragraph: the hanging offset stays */
    drag(x0, y_low, x0 + 96, y_low);
    assert(app.doc.paras[0].a.left == 1440);
    /* the right marker */
    int width = app.lay.width;
    drag(x0 + width, y_low, x0 + width - 48, y_low);
    assert(app.doc.paras[0].a.right == 720);
    screen("ruler-drag");
    /* a click adds a tab stop; dragging it moves it; dragging it off the ruler removes it */
    int y_mid = app.ruler_box.y + app.ruler_box.h - 6;
    click(x0 + 144, y_mid);    assert(app.doc.paras[0].a.ntabs == 1 && app.doc.paras[0].a.tabs[0] == 2160);
    click(x0 + 192, y_mid); assert(app.doc.paras[0].a.ntabs == 2 && app.doc.paras[0].a.tabs[1] == 2880);
    drag(x0 + 144, y_mid, x0 + 240, y_mid); assert(app.doc.paras[0].a.ntabs == 2 && app.doc.paras[0].a.tabs[0] == 2880 && app.doc.paras[0].a.tabs[1] == 3600);
    drag(x0 + 192, y_mid, x0 + 192, app.ruler_box.y + 80); assert(app.doc.paras[0].a.ntabs == 1 && app.doc.paras[0].a.tabs[0] == 3600);
    click(x0 + 240, y_mid - 2); assert(app.doc.paras[0].a.ntabs == 1 && app.doc.paras[0].a.tabs[0] == 3600);   /* clicking on a tab stop only picks it up */
    /* the selected paragraphs change together */
    select_range(0, 7);
    drag(x0, y_top, x0 + 24, y_top); assert(app.doc.paras[0].a.first == 360 && app.doc.paras[1].a.first == 360);
    /* a drag that starts outside the ruler's paper does nothing; a preview ruler is inert */
    unsigned before = app.nundo; click(2, y_top); assert(app.nundo == before);
    cmd(M_PREVIEW); drag(x0, y_top, x0 + 48, y_top); assert(app.doc.paras[0].a.first == 360); cmd(M_PV_CLOSE);
}

static void new_png_file(const char *path, unsigned w, unsigned h)
{
    unsigned char hdr[33]; memcpy(hdr, png_head, sizeof(hdr)); png_size(hdr, w, h); add_file(path, hdr, sizeof(hdr));
}
static void test_pictures(void)
{
    reset(); type("before after"); select_range(7, 7);
    /* Insert > Picture: a PNG; a picture wider than the page is scaled to fit */
    new_png_file("/Documents/small.png", 64, 32); new_png_file("/Documents/wide.png", 2000, 500);
    cmd(M_INSERTPIC); assert(dialog_is(D_PICFILE) && !strcmp(app.filter, "*.bmp;*.png;*.jpg;*.jpeg;*.gif"));
    type("/Documents/small.png"); ENTER;
    assert(!ui.dialog.kind && app.doc.len == 12 + 3 && wp_pic_at(&app.doc, 7) && wp_pic_at(&app.doc, 7)->kind == WP_PIC_PNG && app.cursor == 10 && app.modified);
    assert(!strcmp(app.filter, "*.rtf;*.doc;*.wri;*.txt") && app.nundo >= 2);
    struct wp_pic *p = wp_pic_at(&app.doc, 7), *small = p;
    assert(p->px_state == WP_PX_OK && p->pxw == 64 && p->pxh == 32 && p->scalex == 100 && app.lay.lines[0].h >= 32);
    select_range(10, 10); cmd(M_INSERTPIC); type("/Documents/wide.png"); ENTER;
    p = wp_pic_at(&app.doc, 10); assert(p && p->scalex < 40 && p->scalex == p->scaley);
    { int w, h; wp_pic_size(p, &w, &h); assert(w <= 9746 && w > 9000); }   /* the printable width of A4 */
    select_range(7, 10); screen("pictures");                                 /* a selected picture has a frame */
    /* the picture is a character: it moves with the caret, is selected by a double click and deleted by Backspace */
    select_range(10, 10); press(0x14b); assert(app.cursor == 7);
    press(0x14d); assert(app.cursor == 10);
    click(doc_x(7) + 5, doc_y(7)); assert(app.cursor == 7);
    press(0x14d); press(0x0e); assert(app.doc.len == 12 + 3 && wp_pic_at(&app.doc, 7) != small && wp_pic_count_used(&app.doc) == 1);
    CTRL(0x2c); assert(wp_pic_count_used(&app.doc) == 2);
    /* BMP is stored as a DIB, GIF is re-encoded as PNG, other files are refused with a reason */
    reset(); select_range(0, 0);
    uint8_t bmp[14 + 40 + 24]; memset(bmp, 0, sizeof(bmp)); bmp[0] = 'B'; bmp[1] = 'M'; bmp[2] = sizeof(bmp); bmp[10] = 54;
    bmp[14] = 40; bmp[18] = 4; bmp[22] = 2; bmp[26] = 1; bmp[28] = 24; add_file("/Documents/pic.bmp", bmp, sizeof(bmp));
    add_file("/Documents/anim.gif", "GIF89a\x04\0\x04\0", 10); add_file("/Documents/notes.txt", "hello, not a picture", 20);
    cmd(M_INSERTPIC); type("/Documents/pic.bmp"); ENTER; p = wp_pic_at(&app.doc, 0);
    cmd(M_INSERTPIC); type("/Documents/anim.gif"); ENTER; p = wp_pic_at(&app.doc, 3); assert(p && p->kind == WP_PIC_PNG && p->pw == 4 && p->ph == 4);
    cmd(M_INSERTPIC); type("/Documents/notes.txt"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "not a picture WordPad can read")); ENTER;
    cmd(M_INSERTPIC); type("/Documents/missing.png"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "was not found")); ENTER;
    cmd(M_INSERTPIC); ESC; assert(!ui.dialog.kind && !strcmp(app.filter, "*.rtf;*.doc;*.wri;*.txt"));
    /* a metafile cannot be decoded: it is drawn as a frame and still printed and saved */
    { struct wp_pic *w = wp_pic_new(WP_PIC_WMF, "\x01\0\x09\0", 4); w->goalw = 1440; w->goalh = 720; select_range(0, 0); assert(!wp_insert_pic(&app.doc, app.doc.len, w, NULL)); app.dirty_para = 0; app.lay.pcount = 0; wp_layout_ui(&app); wp_refresh(&app); }
    assert(wp_pic_count_used(&app.doc) == 3 && app.lay.lines[0].h >= 48);
    select_range(0, 0); du_invalidate(&ui, app.doc_box); settle(); screen("pictures-frame");
    /* Insert > Object: a blank bitmap, or a file typed in the dialog */
    reset(); new_png_file("/Documents/small.png", 64, 32); add_file("/Documents/notes.txt", "hello, not a picture", 20); type("x"); cmd(M_INSERTOBJ); assert(dialog_is(D_INSERTOBJ) && ui.dialog.items[4].value && ui.dialog.items[5].count == 1);
    screen("insert-object");
    answer(0); p = wp_pic_at(&app.doc, 1); assert(p && p->kind == WP_PIC_DIB && p->pw == 256 && p->ph == 192 && p->data[40] == 0xff && p->len == 40 + 256 * 3 * 192);
    cmd(M_INSERTOBJ); du_set_text(&ui.dialog.items[7], "/Documents/small.png"); answer(0); p = wp_pic_at(&app.doc, 4); assert(p && p->kind == WP_PIC_PNG && app.doc.len == 7);
    cmd(M_INSERTOBJ); du_set_text(&ui.dialog.items[7], "/Documents/notes.txt"); answer(0); assert(dialog_is(D_INSERTOBJ) && strstr(ui.dialog.text, "not a picture")); answer(1);
    cmd(M_INSERTOBJ); ui.dialog.items[6].value = 1; ui.dialog.items[4].value = 0; answer(0); assert(dialog_is(D_INSERTOBJ) && strstr(ui.dialog.text, "was not found")); answer(1);
    /* the document with its pictures saves as RTF and reads back */
    CTRL(0x1f); assert(dialog_is(D_SAVE)); type("/Documents/pics"); ENTER;
    assert(file_is("/Documents/pics.rtf", "{\\rtf1") && strstr(find("/Documents/pics.rtf")->data, "\\dibitmap0") && strstr(find("/Documents/pics.rtf")->data, "\\pngblip"));
    assert(!wp_open_path(&app, "/Documents/pics.rtf") && app.doc.len == 7 && wp_pic_count_used(&app.doc) == 2 && wp_pic_at(&app.doc, 1)->kind == WP_PIC_DIB && wp_pic_at(&app.doc, 4)->kind == WP_PIC_PNG);
    /* no decoder: pictures are frames and Insert Picture says so */
    app.images = NULL; add_file("/Documents/again.png", png_head, sizeof(png_head));
    cmd(M_INSERTPIC); type("/Documents/again.png"); ENTER; assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "cannot read this kind")); ENTER;
}

/* The system clipboard (G5): Unicode text, Rich Text Format and a DIB, between two WordPad sessions. */
static struct clip_store cstore; static uint32_t cclient = 1;
static int ctransport(void *ctx, struct desk_clip_wire *w, const void *data, struct desk_clip_wire *r, void **out) { return clip_request(ctx, cclient, w, data, r, out); }
static void test_rich_clipboard(void)
{
    clip_init(&cstore); du_clip_bind(ctransport, &cstore);
    reset(); type("bold plain"); select_range(0, 4); CTRL(0x30);
    { struct wp_pic *p = wp_pic_new(WP_PIC_PNG, png_head, sizeof(png_head)); select_range(10, 10); assert(!wp_insert_pic(&app.doc, 10, p, NULL)); }
    select_range(0, 13); CTRL(0x2e);
    /* the clipboard holds text, RTF and a DIB for a lone picture */
    struct desk_clip_wire q; assert(!du_clip_query(&q));
    int rtf = du_clip_register("Rich Text Format"); assert(rtf > 0);
    int has_text_fmt = 0, has_rtf = 0, has_dib = 0; for (unsigned i = 0; i < q.count; i++) { has_text_fmt |= q.formats[i] == 13; has_rtf |= (int)q.formats[i] == rtf; has_dib |= q.formats[i] == 8; }
    assert(has_text_fmt && has_rtf && !has_dib && !strcmp(du_system_clipboard_text_get(), "bold plain"));   /* the text without the picture */
    void *d; uint32_t n; assert(!du_clip_open() && !du_clip_get((unsigned)rtf, &d, &n) && !du_clip_close());
    assert(n > 100 && ((char *)d)[n - 1] == 0 && !strncmp(d, "{\\rtf1", 6) && strstr(d, "\\b bold") && strstr(d, "\\pngblip")); free(d);
    /* a new session pastes the formatting and the picture: nothing is shared but the clipboard */
    reattach();   /* a new session: the private copy is gone, only the clipboard remains */
    CTRL(0x2f); assert(app.doc.len == 13 && !app.clip_valid && (attr_at(0).flags & WP_BOLD) && !(attr_at(5).flags & WP_BOLD) && wp_pic_at(&app.doc, 10) && wp_pic_at(&app.doc, 10)->kind == WP_PIC_PNG);
    assert(app.doc.pics[0]->len == sizeof(png_head) && !memcmp(app.doc.pics[0]->data, png_head, sizeof(png_head)));
    /* a lone picture also goes out as a DIB, and a program that only has a DIB pastes a picture */
    select_range(10, 13); CTRL(0x2e); assert(!du_clip_query(&q)); has_dib = 0; for (unsigned i = 0; i < q.count; i++) has_dib |= q.formats[i] == 8;
    assert(has_dib);
    reset(); cclient = 2;
    { uint32_t px[6] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0, 0x808080}; void *dib; uint32_t db; assert(!clip_dib_encode(px, 3, 3, 2, &dib, &db));
      assert(!du_clip_open() && !du_clip_empty() && !du_clip_put(8, dib, db) && !du_clip_close()); free(dib); }
    cclient = 1; CTRL(0x2f);
    assert(app.doc.len == 3 && wp_pic_at(&app.doc, 0) && wp_pic_at(&app.doc, 0)->kind == WP_PIC_DIB && wp_pic_at(&app.doc, 0)->pw == 3 && wp_pic_at(&app.doc, 0)->ph == 2);
    /* text from another program wins over nothing, and plain text pastes as plain text */
    reset(); cclient = 2; assert(!du_system_clipboard_text_set("from\nelsewhere", 14)); cclient = 1;
    type("x"); CTRL(0x2f); assert(has_text("xfrom\nelsewhere") && app.doc.npara == 2);
    /* Cut: the selection goes out in every format and is removed */
    reset(); type("cut me"); select_range(0, 3); CTRL(0x30); CTRL(0x2d); assert(has_text(" me") && !strcmp(du_system_clipboard_text_get(), "cut"));
    reattach(); CTRL(0x2f); assert(has_text("cut"));
    assert(attr_at(0).flags & WP_BOLD);
    du_clip_bind(NULL, NULL); clip_destroy(&cstore);
}

static void test_print(void)
{
    reset(); type("Heading\nsecond paragraph with some words that wrap around the page edge when the page is narrow enough to need it");
    select_range(0, 7); CTRL(0x30); select_range(8, 14); cmd(M_BULLET); select_range(0, 0);
    { struct wp_pic *p = wp_pic_new(WP_PIC_PNG, png_head, sizeof(png_head)); png_size(((unsigned char *)p->data), 64, 32); p->pw = 64; p->ph = 32; assert(!wp_insert_pic(&app.doc, app.doc.len, p, NULL)); }
    app.dirty_para = 0; app.lay.pcount = 0; wp_refresh(&app);
    select_range(0, 7); CTRL(0x16);   /* underlined heading */
    /* File > Print and the preview's Print button reach the host with the page the document was set up with */
    cmd(M_PRINT); assert(printed == 1 && printed_page.width == 595 && printed_page.height == 842 && printed_page.left == 54 && printed_page.top == 54 && !printed_page.landscape);
    cmd(M_PREVIEW); assert(app.preview); click_widget(M_PV_PRINT); assert(printed == 2); cmd(M_PV_CLOSE);
    app.page.landscape = 1; app.page.left = 1440; cmd(M_PRINT); assert(printed_page.landscape == 1 && printed_page.left == 72); app.page.landscape = 0; app.page.left = 1080;
    /* the page description: the same atoms as the preview, in twips from the paper's top left */
    struct cp_page page = {595, 842, 0, 54, 54, 54, 54, 1, 0, 0};
    struct cp_doc doc;
    assert(!wp_print_prepare(&app, &page, CP_POSTSCRIPT, &doc));
    assert(doc.pages >= 1 && doc.nops > 4 && doc.plain && !strncmp(doc.plain, "Heading\r\n", 9) && doc.nimages == 1);
    const struct cp_op *o = &doc.ops[0];
    assert(o->kind == CP_OP_TEXT && o->page == 1 && o->x == 1080 && o->flags == 1 && o->size == 20 && o->length == 7 && !memcmp(doc.text + o->offset, "Heading", 7) && o->y > 1080 && o->y < 1080 + 300);
    int rule = 0, bullet = 0, image = 0, lines = 0;
    for (unsigned i = 0; i < doc.nops; i++) {
        const struct cp_op *p = &doc.ops[i];
        if (p->kind == CP_OP_RULE) { rule++; assert(p->h == 15 && p->w > 0); }
        if (p->kind == CP_OP_TEXT && p->length == 3 && !memcmp(doc.text + p->offset, "\xe2\x80\xa2", 3)) bullet++;
        if (p->kind == CP_OP_IMAGE) { image++; assert(p->offset == 0 && p->w == 64 * 15 && p->h == 32 * 15 && doc.images[0].width == 64 && doc.images[0].pixels[0] == 0x000040); }
        if (p->kind == CP_OP_TEXT) { lines++; assert(p->x >= 1080 && p->y >= 1080 && p->y <= 842 * 20 - 1080 + 400); if (i) assert(p->page >= doc.ops[i - 1].page); }
    }
    assert(rule == 1 && bullet == 1 && image == 1 && lines > 6);   /* the underline, the bullet, the picture, many words */
    cp_doc_free(&doc);
    /* the same pages as the preview */
    app.page = wp_default_page_setup(1); cmd(M_PREVIEW); unsigned preview_pages = app.pages.count; cmd(M_PV_CLOSE);
    assert(!wp_print_prepare(&app, &page, CP_POSTSCRIPT, &doc) && doc.pages == preview_pages); cp_doc_free(&doc);
    /* a text-only printer gets the characters only, a small paper breaks into more pages */
    assert(!wp_print_prepare(&app, &page, CP_TEXT, &doc) && doc.nops == 0 && doc.pages == 1 && doc.plain_bytes > 20); cp_doc_free(&doc);
    struct cp_page small = {160, 120, 0, 20, 20, 20, 20, 1, 0, 0};
    assert(!wp_print_prepare(&app, &small, CP_PCL, &doc) && doc.pages > preview_pages); cp_doc_free(&doc);
    /* spooled through the production renderers */
    const char *tmp = getenv("TMPDIR"); char root[600]; snprintf(root, sizeof(root), "%s/wp-spool", tmp ? tmp : ".");
    assert(!cp_init(root));
    struct cp_config *cfg = malloc(sizeof(*cfg)); assert(cfg && !cp_config_load(root, cfg)); cfg->printers[0].driver = CP_POSTSCRIPT; assert(!cp_config_save(root, cfg)); free(cfg);
    assert(!wp_print_prepare(&app, &page, CP_POSTSCRIPT, &doc));
    int id = cp_submit_doc(root, CP_PRINTERS, "WordPad", &doc, &page, NULL); assert(id > 0); cp_doc_free(&doc);
    char path[700]; snprintf(path, sizeof(path), "%s/j%03d/data", root, id);
    FILE *f = fopen(path, "rb"); assert(f); static char buf[65536]; size_t got = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[got] = 0;
    assert(!strncmp(buf, "%!PS-Adobe-3.0", 14) && strstr(buf, "(Heading) show") && strstr(buf, "colorimage") && strstr(buf, "/F1 findfont 10.0 scalefont setfont") && strstr(buf, "showpage"));
    /* the hook is optional: without it Print says so */
    reset(); app.print = NULL; cmd(M_PRINT); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "not available")); ENTER;
}

static void load_fixture(const char *name, const char *as)
{
    const char *folder = getenv("WORDPAD_FIXTURES"); assert(folder);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", folder, name);
    FILE *f = fopen(path, "rb"); assert(f); static uint8_t raw[1 << 20]; size_t n = fread(raw, 1, sizeof(raw), f); fclose(f);
    add_file(as, raw, n);
}
static void test_open_word_write(void)
{
    reset();
    load_fixture("w97-basic.doc", "/Documents/basic.doc"); load_fixture("write-basic.wri", "/Documents/letter.wri"); load_fixture("w6-basic.doc", "/Documents/old.doc");
    load_fixture("w97-encrypted.doc", "/Documents/locked.doc"); load_fixture("write-header.wri", "/Documents/hdr.wri"); load_fixture("w97-picture.doc", "/Documents/pic.doc");
    /* a Word 97 document: formatting shows, the notice says it is saved as RTF, Save asks for a name with the .rtf extension */
    assert(!wp_open_path(&app, "/Documents/basic.doc") && app.format == WP_FMT_WORD && app.named && !app.modified);
    assert(strstr(app.doc.text, "Title\nPlain bold") == app.doc.text && (attr_at(0).flags & WP_BOLD) && app.doc.paras[0].a.align == WP_CENTER);
    assert(dialog_is(D_NOTICE) ? strstr(ui.dialog.text, "saved as Rich Text Format") != NULL : 1); if (dialog_is(D_NOTICE)) ENTER;
    screen("word-opened");
    select_range(app.doc.len, app.doc.len); type("!"); CTRL(0x1f);
    assert(dialog_is(D_SAVE)); ENTER;   /* the suggested name basic.rtf */
    assert(!ui.dialog.kind && !strcmp(app.path, "/Documents/basic.rtf") && app.format == WP_FMT_RTF && file_is("/Documents/basic.rtf", "{\\rtf1") && find("/Documents/basic.doc") && !app.modified);
    CTRL(0x1f); assert(!ui.dialog.kind && saves >= 2);   /* now an ordinary RTF document */
    /* Write and Word 6 */
    assert(!wp_open_path(&app, "/Documents/letter.wri") && app.format == WP_FMT_WRITE && has_text("Title\nPlain bold italic sans under\nIndented\ttab\nlast") && app.doc.paras[2].a.align == WP_JUSTIFY);
    if (dialog_is(D_NOTICE)) ENTER;
    cmd(M_SAVEAS); assert(dialog_is(D_SAVE)); ESC;
    assert(!wp_open_path(&app, "/Documents/old.doc") && app.format == WP_FMT_WORD && strstr(app.doc.text, "Hanging paragraph")); if (dialog_is(D_NOTICE)) ENTER;
    /* headers are reported; a picture in a Word document is kept; a protected document is refused with the reason */
    assert(!wp_open_path(&app, "/Documents/hdr.wri") && has_text("body") && dialog_is(D_NOTICE) && strstr(ui.dialog.text, "Headers, footers")); ENTER;
    assert(!wp_open_path(&app, "/Documents/pic.doc") && wp_pic_count_used(&app.doc) == 1 && wp_pic_at(&app.doc, 7)->kind == WP_PIC_PNG); if (dialog_is(D_NOTICE)) ENTER;
    screen("word-picture");
    assert(wp_open_path(&app, "/Documents/locked.doc") == -ENOTSUP && dialog_is(D_INFO) && strstr(ui.dialog.text, "password")); ENTER;
    assert(wp_open_path(&app, "/Documents/locked.doc") == -ENOTSUP); ENTER;
    /* the file dialog lists the new types */
    cmd(M_OPEN); assert(dialog_is(D_OPEN)); type("/Documents"); ENTER; assert(dialog_is(D_OPEN) && ui.dialog.entry_count >= 5); ESC;
}

static size_t fake_italian(char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    static const char *const mesi[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};
    static const char *const giorni[] = {"domenica", "luned\xc3\xac", "marted\xc3\xac", "mercoled\xc3\xac", "gioved\xc3\xac", "venerd\xc3\xac", "sabato"};
    char t[96];
    if (!strcmp(fmt, "%x")) snprintf(t, sizeof(t), "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
    else if (!strcmp(fmt, "%X")) snprintf(t, sizeof(t), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    else if (!strcmp(fmt, "%c")) snprintf(t, sizeof(t), "%s %d %s %d %02d:%02d:%02d", giorni[tm->tm_wday], tm->tm_mday, mesi[tm->tm_mon], tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
    else if (!strcmp(fmt, "%d %B %Y")) snprintf(t, sizeof(t), "%d %s %d", tm->tm_mday, mesi[tm->tm_mon], tm->tm_year + 1900);
    else if (!strcmp(fmt, "%B %d, %Y")) snprintf(t, sizeof(t), "%s %d, %d", mesi[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    else if (!strcmp(fmt, "%H:%M")) snprintf(t, sizeof(t), "%02d:%02d", tm->tm_hour, tm->tm_min);
    else return strftime(out, cap, fmt, tm);
    if (strlen(t) >= cap) return 0;
    strcpy(out, t); return strlen(t);
}
static void test_locale_help_tips(void)
{
    /* regional dates (G24): the formats come from the locale's pictures through strftime */
    reset(); app.format_time = fake_italian; type("x");
    cmd(M_DATETIME); assert(dialog_is(D_DATETIME));
    assert(!strcmp(app.date_text[0], "11/10/2026") && !strcmp(app.date_text[1], "domenica 11 ottobre 2026") && !strcmp(app.date_text[2], "11 ottobre 2026") &&
           !strcmp(app.date_text[3], "ottobre 11, 2026") && !strcmp(app.date_text[4], "09:41:00") && !strcmp(app.date_text[5], "09:41") && !strcmp(app.date_text[6], "11/10/2026 09:41:00"));
    ui.dialog.items[4].active = 1; answer(0); assert(has_text("xdomenica 11 ottobre 2026"));
    /* Help: F1 and Help Topics open the program's topic in CiukiOS Help (G15); What's This? starts the context mode */
    reset(); cmd(M_HELP); assert(!strcmp(help_seen, "wordpad") && !ui.dialog.kind);
    help_seen[0] = 0; press(DU_KEY_F1); assert(!strcmp(help_seen, "wordpad"));
    cmd(M_CONTEXTHELP); assert(ui.whats_this); click(centre_x(M_BOLD), centre_y(M_BOLD)); assert(!ui.whats_this);
    ui.help_open = NULL; cmd(M_HELP); assert(dialog_is(D_INFO) && strstr(ui.dialog.text, "Ctrl+J")); ENTER;
    /* every button of the toolbar and the format bar has a tooltip and a What's This? text */
    static const int tools[] = {M_NEW, M_OPEN, M_SAVE, M_PRINT, M_PREVIEW, M_FIND, M_CUT, M_COPY, M_PASTE, M_UNDO, M_DATETIME, M_BOLD, M_ITALIC, M_UNDERLINE, M_LEFT, M_CENTER, M_RIGHT, M_BULLET, W_COLORBTN};
    for (unsigned i = 0; i < sizeof(tools) / sizeof(*tools); i++) assert(W(tools[i])->tip[0] && W(tools[i])->help && strlen(W(tools[i])->help) > 10);
    assert(W(W_FONTBTN)->tip[0] && W(W_SIZEBTN)->tip[0] && W(W_DOC)->help && W(W_RULER)->help);
}

int main(int argc, char **argv)
{
    if (argc > 1) dump_prefix = argv[1];
    test_startup(); test_typing_undo(); test_navigation_selection(); test_scrolling(); test_character_formatting();
    test_paragraph_formatting(); test_font_dialog(); test_clipboard(); test_find_replace(); test_files(); test_large_files();
    test_changes_prompts(); test_dialogs_insert_page_menu(); test_preview(); test_ruler_and_layout_ui();
    test_undo_levels(); test_justify_ui(); test_options_units_wrap(); test_ruler_drag(); test_pictures(); test_rich_clipboard(); test_print();
    test_open_word_write(); test_locale_help_tips();
    wp_detach(&app);
    puts("PASS wordpad");
    return 0;
}
