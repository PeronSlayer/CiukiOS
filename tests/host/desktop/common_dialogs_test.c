/* SPDX-License-Identifier: MIT
 * G16: the toolkit's common controls and dialogs against the Windows 98 /
 * Win32 behaviour documented in docs/design/common-dialogs.md: combo box,
 * trackbar, up-down, tooltips, single-level edit undo, Open/Save As (filters,
 * default extension, overwrite and existence prompts, New Folder), Font, Color,
 * Browse for Folder, Find and Replace. Production toolkit, fake file system. */
#include "deskui.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 560
#define H 440
static uint32_t *pixels, *reference;
static struct du_ui u;
static struct du_event events[256]; static unsigned count;
static char texts[256][DU_TEXT_MAX];
static uint64_t now = 1000000000ull;
static const char *out;

static void handler(struct du_ui *ui, const struct du_event *e, void *arg)
{
    (void)ui; (void)arg;
    assert(count < 256);
    events[count] = *e;
    if (e->text) { snprintf(texts[count], DU_TEXT_MAX, "%s", e->text); events[count].text = texts[count]; }
    count++;
}
static struct du_event *last(void) { assert(count); return &events[count - 1]; }
static struct du_event *find_event(enum du_event_type type, int id)
{
    for (unsigned i = count; i > 0; i--) if (events[i - 1].type == type && events[i - 1].id == id) return &events[i - 1];
    return NULL;
}
static void input(int type, int code, int value, int value2)
{ now += 10000000ull; struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = now}; du_input(&u, &e); }
static void key(int code) { input(CIUKI_INPUT_KEY, code, 1, 0); input(CIUKI_INPUT_KEY, code, 0, 0); }
static void hold(int code, int down) { input(CIUKI_INPUT_KEY, code, down, 0); }
static void ctrl_key(int code) { hold(DU_KEY_LCTRL, 1); key(code); hold(DU_KEY_LCTRL, 0); }
static void motion(int x, int y) { input(CIUKI_INPUT_MOTION, 0, x, y); }
static void down(int button) { input(CIUKI_INPUT_BUTTON, button, 1, 0); }
static void up(int button) { input(CIUKI_INPUT_BUTTON, button, 0, 0); }
static void click(int x, int y) { motion(x, y); down(1); up(1); now += 700000000ull; }
static void tick_ms(unsigned ms) { now += (uint64_t)ms * 1000000ull; du_tick(&u, now); }
static void type_digits(const char *s)
{
    static const int code[10] = {0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};
    for (; *s; s++) key(code[*s - '0']);
}

/* The retained frame must equal a full redraw after every transient. */
static void equivalent(const char *what)
{
    du_render(&u);
    memcpy(reference, pixels, (size_t)W * H * 4);
    u.dirty = 1; du_render(&u);
    if (memcmp(reference, pixels, (size_t)W * H * 4)) fprintf(stderr, "partial/full mismatch: %s\n", what);
    assert(!memcmp(reference, pixels, (size_t)W * H * 4));
}
static void frame(const char *name)
{
    du_render(&u);
    assert(pixels[W * H] == 0x5a5a5a5au);
    if (!out) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.xrgb", out, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(pixels, 4, W * H, f) == (size_t)W * H); fclose(f);
    snprintf(path, sizeof(path), "%s/%s.size", out, name);
    f = fopen(path, "w"); assert(f); fprintf(f, "%d %d\n", W, H); fclose(f);
}

/* ---- fake file system ---- */
static struct { char path[96]; int dir; uint64_t size; } fs[64]; static unsigned fs_count;
static char made[96];
static void fs_add(const char *path, int dir, uint64_t size)
{ snprintf(fs[fs_count].path, sizeof(fs[0].path), "%s", path); fs[fs_count].dir = dir; fs[fs_count].size = size; fs_count++; }
static int fs_find(const char *path) { for (unsigned i = 0; i < fs_count; i++) if (!strcmp(fs[i].path, path)) return (int)i; return -1; }
static int fs_lister(void *ctx, const char *path, struct du_file_entry *out_entries, unsigned cap, unsigned *n)
{
    (void)ctx; *n = 0;
    if (strcmp(path, "/") && fs_find(path) < 0) return -ENOENT;
    size_t len = strlen(path);
    for (unsigned i = 0; i < fs_count && *n < cap; i++) {
        const char *p = fs[i].path;
        if (strncmp(p, path, len) || (len > 1 && p[len] != '/') || !p[len > 1 ? len + 1 : len]) continue;
        const char *name = p + (len > 1 ? len + 1 : len);
        if (strchr(name, '/')) continue;
        struct du_file_entry *e = &out_entries[(*n)++]; memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name); e->directory = fs[i].dir; e->size = fs[i].size;
    }
    return 0;
}
static int fs_mkdir(void *ctx, const char *path)
{
    (void)ctx; if (fs_find(path) >= 0) return -EEXIST;
    snprintf(made, sizeof(made), "%s", path); fs_add(path, 1, 0); return 0;
}
static void fs_reset(void)
{
    fs_count = 0; made[0] = 0;
    fs_add("/docs", 1, 0); fs_add("/docs/sub", 1, 0); fs_add("/docs/a.txt", 0, 1500); fs_add("/docs/b.log", 0, 200);
    fs_add("/docs/c.TXT", 0, 3); fs_add("/docs/pic.bmp", 0, 90000); fs_add("/docs/noext", 0, 5);
    fs_add("/other", 1, 0); fs_add("/other/deep", 1, 0); fs_add("/docs/sub/inner", 1, 0);
}

static struct du_widget *dlg(int id)
{
    for (unsigned i = 0; i < u.dialog.count; i++) if (u.dialog.items[i].id == id) return &u.dialog.items[i];
    return NULL;
}
static void click_widget(const struct du_widget *w) { click(w->box.x + w->box.w / 2, w->box.y + w->box.h / 2); }
static void click_row(struct du_widget *list, unsigned row)
{
    struct du_rect r = du_list_label(list, row); click(r.x + 6, r.y + r.h / 2);
}

enum { ID_ACCEPT = -1000, ID_CANCEL = -1001, ID_UP = -2000, ID_LIST = -3000, ID_NAME = -4000, ID_FOLDER = -5000, ID_PLACES = -8000,
       ID_NEWDIR = -9000, ID_VLIST = -9100, ID_VDETAILS = -9200, ID_TYPE = -9300, ID_SUB_YES = -9400, ID_SUB_NO = -9401, ID_SUB_EDIT = -9402, ID_VICONS = -9403, ID_READONLY = -9404 };

static void patterns(void)
{
    assert(du_pattern_match("*.txt", "a.txt") && du_pattern_match("*.txt", "A.TXT") && !du_pattern_match("*.txt", "a.txt.bak"));
    assert(du_pattern_match("*.txt;*.log", "x.log") && du_pattern_match("*.txt; *.log", "x.log") && !du_pattern_match("*.txt;*.log", "x.bmp"));
    assert(du_pattern_match("*.*", "noext") && du_pattern_match("*.*", "a.b") && du_pattern_match("*", "anything") && du_pattern_match(NULL, "x"));
    assert(du_pattern_match("a?c.t*", "abc.txt") && !du_pattern_match("a?c.t*", "ac.txt") && du_pattern_match("*a*b*", "xxaYYbzz"));
    assert(!du_pattern_match("*.txt", "") && du_pattern_match("*", ""));
    puts("PASS G16 wildcard filters: case, ';' lists, *.*, ?, backtracking");
}

static void colours_and_fonts(void)
{
    unsigned h, s, l;
    du_rgb_to_hsl(0xff0000, &h, &s, &l); assert(h == 0 && s == 240 && l == 120);
    du_rgb_to_hsl(0xffffff, &h, &s, &l); assert(s == 0 && l == 240);
    du_rgb_to_hsl(0x000000, &h, &s, &l); assert(s == 0 && l == 0);
    du_rgb_to_hsl(0x00ff00, &h, &s, &l); assert(h == 80 && s == 240 && l == 120);
    du_rgb_to_hsl(0x0000ff, &h, &s, &l); assert(h == 160 && s == 240 && l == 120);
    assert(du_hsl_to_rgb(0, 240, 120) == 0xff0000 && du_hsl_to_rgb(80, 240, 120) == 0x00ff00 && du_hsl_to_rgb(160, 240, 120) == 0x0000ff);
    assert(du_hsl_to_rgb(0, 0, 240) == 0xffffff && du_hsl_to_rgb(99, 0, 0) == 0);
    /* The 240-step scale loses precision: every colour returns within one HLS step per channel. */
    for (unsigned c = 0; c < 0x1000000u; c += 0x010305u) {
        du_rgb_to_hsl(c, &h, &s, &l);
        uint32_t back = du_hsl_to_rgb(h, s, l);
        for (int shift = 0; shift < 24; shift += 8) {
            int a = (int)((c >> shift) & 0xff), b = (int)((back >> shift) & 0xff);
            assert(a - b <= 9 && b - a <= 9);
        }
    }
    uint32_t rgb; assert(!du_color_parse("#00FF7f", &rgb) && rgb == 0x00ff7f && du_color_parse("00ff7f", &rgb) && du_color_parse("#12", &rgb) && du_color_parse("#zzzzzz", &rgb));
    struct du_font_choice c = {.family = "Liberation Mono", .style = DU_FONT_BOLD | DU_FONT_ITALIC, .points = 12, .underline = 1, .strikeout = 0, .color = 0xff0000}, back;
    char text[160]; du_font_format(text, sizeof(text), &c); assert(!strcmp(text, "Liberation Mono|3|12|1|0|FF0000"));
    assert(!du_font_parse(text, &back) && !strcmp(back.family, "Liberation Mono") && back.style == 3 && back.points == 12 && back.underline && back.color == 0xff0000);
    assert(du_font_parse("nofield", &back) && du_font_parse("a|1|2|3", &back) && du_font_parse(NULL, &back));
    puts("PASS G16 colour (HLSMAX 240) and font conversions: primaries, greys, round trip, parse errors");
}

static void find_search(void)
{
    struct du_find f = {.what = "the", .flags = DU_FIND_DOWN}; size_t lo, hi;
    const char *t = "The other theme, then there";
    assert(du_find_text(t, strlen(t), 0, &f, &lo, &hi) && lo == 0 && hi == 3);                /* case-insensitive */
    assert(du_find_text(t, strlen(t), 1, &f, &lo, &hi) && lo == 5 && hi == 8);                /* inside "other" */
    f.flags |= DU_FIND_CASE; assert(du_find_text(t, strlen(t), 0, &f, &lo, &hi) && lo == 5);  /* match case skips "The" */
    f.flags = DU_FIND_DOWN | DU_FIND_WHOLE; strcpy(f.what, "the");
    assert(du_find_text(t, strlen(t), 0, &f, &lo, &hi) && lo == 0);
    assert(du_find_text(t, strlen(t), 3, &f, &lo, &hi) == 0);                                  /* "other", "theme", "then", "there": none whole */
    f.flags = DU_FIND_WHOLE; strcpy(f.what, "then");                                          /* up: ends at or before from */
    assert(du_find_text(t, strlen(t), strlen(t), &f, &lo, &hi) && lo == 17);
    f.flags = 0; strcpy(f.what, "the"); assert(du_find_text(t, strlen(t), strlen(t), &f, &lo, &hi) && lo == 22);
    assert(du_find_text(t, strlen(t), 22, &f, &lo, &hi) && lo == 17);
    assert(du_find_text(t, strlen(t), 3, &f, &lo, &hi) && lo == 0 && !du_find_text(t, strlen(t), 2, &f, &lo, &hi));
    f.what[0] = 0; assert(!du_find_text(t, strlen(t), 0, &f, &lo, &hi));
    puts("PASS G16 find helper: case, whole word, up and down");
}

/* ---- controls in the window ---- */
static void combo_box(void)
{
    static struct du_item rows[12]; static const char *names[12] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta", "Iota", "Kappa", "Lambda", "Mu"};
    for (int i = 0; i < 12; i++) rows[i].text[0] = names[i];
    struct du_widget *c = du_add(&u, DU_COMBO, 31, (struct du_rect){20, 40, 160, 22}, "");
    du_combo_set(c, rows, 12, 1);
    assert(!strcmp(du_combo_text(c), "Beta") && !u.combo_open);
    count = 0;
    click(c->box.x + c->box.w - 8, c->box.y + 10);              /* the arrow opens the list */
    assert(u.combo_open == c && u.combo_hot == 1);
    equivalent("combo opened");
    frame("combo-open");
    /* Move over Delta, release selects it. */
    motion(c->box.x + 30, c->box.y + c->box.h - 1 + 2 + 3 * DU_ROW + 5);
    assert(u.combo_hot == 3);
    equivalent("combo hot row");
    down(1); up(1);
    assert(!u.combo_open && c->value == 3 && last()->type == DU_EV_SELECT && last()->id == 31 && last()->index == 3 && !strcmp(du_combo_text(c), "Delta"));
    equivalent("combo closed");
    /* Outside click closes without a selection and does not reach the widget below. */
    du_focus(&u, 31); key(DU_KEY_F4); assert(u.combo_open == c);
    unsigned before = count; click(400, 300); assert(!u.combo_open && c->value == 3 && count == before);
    /* Keyboard: Alt+Down opens, arrows move, Enter picks, Esc cancels, type-ahead jumps. */
    hold(DU_KEY_LALT, 1); input(CIUKI_INPUT_KEY, DU_KEY_DOWN, 1, 0); input(CIUKI_INPUT_KEY, DU_KEY_DOWN, 0, 0); hold(DU_KEY_LALT, 0);
    assert(u.combo_open == c);
    key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(!u.combo_open && c->value == 5 && last()->index == 5);
    key(DU_KEY_F4); key(DU_KEY_UP); key(DU_KEY_ESC); assert(!u.combo_open && c->value == 5);
    key(0x26); assert(c->value == 10);                              /* 'l' selects Lambda */
    key(DU_KEY_HOME); assert(c->value == 0); key(DU_KEY_END); assert(c->value == 11); key(DU_KEY_UP); assert(c->value == 10);
    key(DU_KEY_F4); key(DU_KEY_END); assert(u.combo_hot == 11 && c->scroll == 4);   /* eight rows are visible */
    hold(DU_KEY_LCTRL, 0); input(CIUKI_INPUT_WHEEL, 0, 1, 0); assert(c->scroll == 1);
    key(DU_KEY_ESC);
    /* A combo near the bottom opens upward. */
    struct du_widget *low = du_add(&u, DU_COMBO, 32, (struct du_rect){20, H - 30, 160, 22}, "");
    du_combo_set(low, rows, 12, 0); du_combo_drop(&u, low, 1);
    equivalent("combo opened upward"); assert(u.combo_open == low); du_combo_drop(&u, low, 0); equivalent("combo closed upward");
    /* Editable: the text is the edit's; picking a row replaces it; typing posts changes. */
    struct du_widget *e = du_add(&u, DU_COMBO, 33, (struct du_rect){200, 40, 160, 22}, "");
    e->editable = 1; du_combo_set(e, rows, 12, -1); du_set_text(e, "free text");
    assert(!strcmp(du_combo_text(e), "free text"));
    du_focus(&u, 33); count = 0; key(0x1e); assert(!strcmp(e->text, "free texta") && last()->type == DU_EV_CHANGE);
    key(DU_KEY_DOWN); assert(e->value == 0 && !strcmp(e->text, "Alpha") && find_event(DU_EV_SELECT, 33)->index == 0);
    key(DU_KEY_ENTER); assert(last()->type == DU_EV_COMMIT);
    equivalent("editable combo");
    frame("combo-closed");
    puts("PASS G16 combo box: drop-down, hot row, release selects, outside click, Alt+Down/F4/Esc, type-ahead, wheel, upward drop, editable text");
}

static void track_bar(void)
{
    struct du_widget *t = du_add(&u, DU_TRACKBAR, 41, (struct du_rect){20, 90, 200, 34}, "");
    du_trackbar_set(t, 0, 100, 20, 0, 0, 10);
    du_focus(&u, 41); count = 0;
    key(DU_KEY_RIGHT); assert(t->value == 21 && last()->type == DU_EV_CHANGE && last()->index == 21);
    key(DU_KEY_DOWN); assert(t->value == 22); key(DU_KEY_LEFT); key(DU_KEY_UP); assert(t->value == 20);
    key(DU_KEY_PGDN); assert(t->value == 40);                      /* the page is a fifth of the range */
    key(DU_KEY_PGUP); key(DU_KEY_PGUP); assert(t->value == 0); key(DU_KEY_LEFT); assert(t->value == 0);
    key(DU_KEY_END); assert(t->value == 100); key(DU_KEY_HOME); assert(t->value == 0);
    equivalent("trackbar keys");
    /* Clicking the channel pages toward the pointer; holding it repeats until the thumb arrives. */
    t->value = 10; du_invalidate(&u, t->box);
    count = 0; motion(t->box.x + 150, t->box.y + 20); down(1);
    assert(t->value == 30);
    tick_ms(400); tick_ms(100); assert(t->value >= 50);
    up(1); assert(last()->type == DU_EV_COMMIT);
    int stopped = t->value; tick_ms(500); assert(t->value == stopped);
    /* Dragging the thumb follows the pointer and ends with a commit. */
    du_trackbar_set(t, 0, 100, 50, 1, 20, 10);
    equivalent("trackbar before drag");
    int thumb_x = t->box.x + 6 + 1 + (200 - 11 - 2) / 2;
    motion(thumb_x + 1, t->box.y + 12); down(1); motion(t->box.x + 200, t->box.y + 12);
    assert(t->value == 100); motion(t->box.x - 40, t->box.y + 12); assert(t->value == 0);
    count = 0; up(1); assert(last()->type == DU_EV_COMMIT && last()->index == 0);
    input(CIUKI_INPUT_WHEEL, 0, -1, 0); assert(t->value == 0 || t->value == 3);
    /* Vertical: the box is taller than wide; the minimum is at the top. */
    struct du_widget *v = du_add(&u, DU_TRACKBAR, 42, (struct du_rect){240, 80, 34, 120}, "");
    du_trackbar_set(v, 1, 5, 3, 1, 1, 1);
    du_focus(&u, 42); key(DU_KEY_DOWN); assert(v->value == 4); key(DU_KEY_END); assert(v->value == 5);
    equivalent("vertical trackbar"); frame("trackbar");
    puts("PASS G16 trackbar: arrows, page and Home/End, channel paging with repeat, thumb drag and commit, wheel, vertical");
}

static void up_down(void)
{
    struct du_widget *e = du_add(&u, DU_EDIT, 51, (struct du_rect){300, 90, 80, 22}, "5");
    struct du_widget *ud = du_add(&u, DU_UPDOWN, 52, (struct du_rect){0, 0, 16, 22}, "");
    du_updown_set(ud, 0, 30, 5, 0);
    assert(!du_updown_attach(&u, 52, 51) && e->box.w == 64 && ud->box.x == 364 && ud->box.w == 16 && !strcmp(e->text, "5"));
    count = 0;
    click(ud->box.x + 8, ud->box.y + 5);                            /* up arrow */
    assert(ud->value == 6 && !strcmp(e->text, "6") && find_event(DU_EV_CHANGE, 52)->index == 6);
    click(ud->box.x + 8, ud->box.y + 17); assert(ud->value == 5 && !strcmp(e->text, "5"));
    /* Holding: after the first delay it repeats, and accelerates after 2 s (5 per step) and 5 s (20). */
    motion(ud->box.x + 8, ud->box.y + 5); down(1); assert(ud->value == 6);
    tick_ms(400); assert(ud->value == 7);
    tick_ms(100); assert(ud->value == 8);
    up(1); int v = ud->value; tick_ms(1000); assert(ud->value == v);
    ud->value = 0; du_set_text(e, "0"); now += 10000000000ull;
    motion(ud->box.x + 8, ud->box.y + 5); down(1); assert(ud->value == 1);
    for (int i = 0; i < 25; i++) tick_ms(100);
    assert(ud->value >= 20 && ud->value <= 30);
    up(1);
    /* The buddy: Up and Down in the edit step it, typed text is read back and clamped. */
    du_focus(&u, 51); du_set_text(e, "12"); key(DU_KEY_UP); assert(ud->value == 13 && !strcmp(e->text, "13"));
    du_set_text(e, "99"); key(DU_KEY_DOWN); assert(ud->value == 29 && !strcmp(e->text, "29"));  /* 99 is clamped to 30 first */
    du_set_text(e, "junk"); assert(du_updown_value(&u, ud) == 29);
    /* Wrap and inverted range. */
    du_updown_set(ud, 0, 3, 3, 1); du_set_text(e, "3"); key(DU_KEY_UP); assert(ud->value == 0); key(DU_KEY_DOWN); assert(ud->value == 3);
    du_updown_set(ud, 3, 0, 3, 0); du_set_text(e, "3"); key(DU_KEY_UP); assert(ud->value == 2);   /* lo > hi: up moves toward hi */
    equivalent("updown");
    frame("updown");
    puts("PASS G16 up-down: buddy text both ways, arrows, held repeat with 1/5/20 acceleration, Up/Down in the edit, clamp, wrap, inverted range");
}

static void tooltips(void)
{
    struct du_widget *b = du_add(&u, DU_BUTTON, 61, (struct du_rect){20, 160, 90, 24}, "&Tip");
    du_set_tip(b, "Opens the tip");
    count = 0; tick_ms(10);
    motion(40, 170);
    assert(!u.tip.visible && du_next_deadline(&u) <= now + 500000000ull + 20000000ull);
    tick_ms(400); assert(!u.tip.visible);
    motion(42, 171);                                                  /* moving restarts the initial delay */
    tick_ms(400); assert(!u.tip.visible);
    tick_ms(110); assert(u.tip.visible && u.dirty && !strcmp(u.tip.text, "Opens the tip"));
    assert(u.tip.box.y >= 171 + 18 && u.tip.box.x >= 0 && u.tip.box.x + u.tip.box.w <= W);
    equivalent("tooltip shown");
    frame("tooltip");
    /* Autopop after five seconds, and it does not return until the pointer leaves. */
    tick_ms(5000); assert(!u.tip.visible); equivalent("tooltip popped");
    tick_ms(2000); assert(!u.tip.visible);
    motion(300, 300); motion(40, 170); tick_ms(600); assert(u.tip.visible);
    /* Leaving hides it; entering a second tool while one is shown waits only the reshow delay. */
    struct du_widget *b2 = du_add(&u, DU_BUTTON, 62, (struct du_rect){120, 160, 90, 24}, "T&wo");
    du_set_tip(b2, "Second");
    motion(130, 170); assert(!u.tip.visible); tick_ms(50); assert(!u.tip.visible); tick_ms(60); assert(u.tip.visible && !strcmp(u.tip.text, "Second"));
    equivalent("tooltip moved");
    motion(300, 300); assert(!u.tip.visible); equivalent("tooltip left");
    /* A press, a key and the wheel hide it. */
    motion(40, 170); tick_ms(600); assert(u.tip.visible); down(1); assert(!u.tip.visible); up(1);
    motion(41, 171); tick_ms(600); assert(u.tip.visible); key(DU_KEY_F5); assert(!u.tip.visible);
    motion(42, 170); tick_ms(600); assert(u.tip.visible); input(CIUKI_INPUT_WHEEL, 0, 1, 0); assert(!u.tip.visible);
    /* A widget without text never shows one. */
    motion(300, 300); tick_ms(100); struct du_widget *plain = du_add(&u, DU_BUTTON, 63, (struct du_rect){220, 160, 60, 24}, "None");
    plain->tip[0] = 0;   /* G15 gives every control a fallback tip; a program removes it by clearing it */
    motion(plain->box.x + 4, plain->box.y + 4); tick_ms(900); assert(!u.tip.visible && !u.tip.w);   /* G12's caret blink may still own du_next_deadline */
    motion(300, 300);
    puts("PASS G16 tooltips: 500 ms initial delay restarted by movement, 5 s autopop, 100 ms reshow, hide on leave/press/key/wheel, bounded invalidation");
}

static void edit_undo(void)
{
    struct du_widget *e = du_add(&u, DU_EDIT, 71, (struct du_rect){300, 160, 160, 22}, "");
    du_focus(&u, 71);
    key(0x1e); key(0x30); key(0x2e); assert(!strcmp(e->text, "abc"));
    ctrl_key(0x2c); assert(!strcmp(e->text, ""));                    /* typing undoes as one step */
    ctrl_key(0x2c); assert(!strcmp(e->text, "abc"));                  /* undoing the undo redoes it */
    key(DU_KEY_BACKSPACE); key(DU_KEY_BACKSPACE); assert(!strcmp(e->text, "a"));
    hold(DU_KEY_LALT, 1); key(DU_KEY_BACKSPACE); hold(DU_KEY_LALT, 0); assert(!strcmp(e->text, "abc"));
    du_clipboard_set("zz"); key(DU_KEY_END); ctrl_key(0x2f); assert(!strcmp(e->text, "abczz"));
    ctrl_key(0x2c); assert(!strcmp(e->text, "abc"));
    key(DU_KEY_HOME); hold(DU_KEY_LSHIFT, 1); key(DU_KEY_RIGHT); hold(DU_KEY_LSHIFT, 0); ctrl_key(0x2d); assert(!strcmp(e->text, "bc"));
    ctrl_key(0x2c); assert(!strcmp(e->text, "abc") && e->cursor == 1 && e->anchor == 0);
    /* A second edit control has its own history only for the control last edited. */
    struct du_widget *other = du_add(&u, DU_EDIT, 72, (struct du_rect){300, 190, 160, 22}, "");
    du_focus(&u, 72); key(0x1e); assert(!strcmp(other->text, "a"));
    du_focus(&u, 71); ctrl_key(0x2c); assert(!strcmp(e->text, "abc") && !strcmp(other->text, "a"));
    du_focus(&u, 72); ctrl_key(0x2c); assert(!strcmp(other->text, ""));
    equivalent("edit undo");
    puts("PASS G16 edit undo: typing and deleting runs, cut, paste, redo by a second undo, Alt+Backspace, last edited control");
}

static void controls(void)
{
    combo_box(); track_bar(); up_down(); tooltips(); edit_undo();
}

/* ---- dialogs ---- */
static void file_dialog(void)
{
    fs_reset();
    static const struct du_file_filter filters[] = {{"Text Files (*.txt)", "*.txt"}, {"Log Files (*.log)", "*.log"}, {"All Files (*.*)", "*.*"}};
    struct du_file_options o = {.flags = DU_FILE_SAVE | DU_FILE_OVERWRITE_PROMPT, .title = "Save As", .folder = "/docs", .name = "",
                                .filters = filters, .filter_count = 3, .default_ext = "txt", .lister = fs_lister, .make_dir = fs_mkdir};
    count = 0;
    assert(!du_file_open(&u, 70, &o));
    struct du_widget *list = dlg(ID_LIST), *name = dlg(ID_NAME), *type = dlg(ID_TYPE);
    assert(u.dialog.kind == DU_DLG_SAVE && list && name && type);
    /* The first filter: folders always, then only *.txt files (case-insensitive). */
    assert(list->count == 3 && !strcmp(list->items[0].text[0], "sub") && !strcmp(list->items[1].text[0], "a.txt") && !strcmp(list->items[2].text[0], "c.TXT"));
    assert(!strcmp(du_combo_text(type), "Text Files (*.txt)") && type->count == 3);
    assert(dlg(ID_NEWDIR) && dlg(ID_NEWDIR)->tip[0] && !strcmp(dlg(ID_UP)->tip, "Up One Level"));
    frame("file-save");
    /* Look in is an editable drop-down: the folder hierarchy (indented) then the other places; a pick navigates. */
    struct du_widget *look = dlg(ID_FOLDER);
    assert(look->kind == DU_COMBO && look->editable && look->count >= 3 && look->value == 1 && !strcmp(look->text, "/docs"));
    assert(!strcmp(look->items[1].text[0], "  docs") && look->items[1].icon16 && look->items[0].icon16);
    du_combo_drop(&u, look, 1); assert(u.combo_open == look); frame("file-lookin-open"); du_combo_drop(&u, look, 0);
    u.dialog.focus = (int)(look - u.dialog.items); key(DU_KEY_UP);
    assert(!strcmp(u.dialog.folder, "/") && !strcmp(look->text, "/") && look->value == 0);
    du_set_text(look, "/docs"); key(DU_KEY_ENTER); assert(!strcmp(u.dialog.folder, "/docs") && look->value == 1);
    u.dialog.focus = (int)(name - u.dialog.items);
    /* The view and navigation buttons are icon tool buttons with tooltips. */
    assert(dlg(ID_UP)->kind == DU_TOOLBUTTON && dlg(ID_UP)->icon && dlg(ID_VLIST)->kind == DU_TOOLBUTTON && dlg(ID_VICONS)->tip[0] && dlg(ID_VDETAILS)->tip[0]);
    /* Changing the type with the keyboard re-filters and reports the index with the result. */
    u.dialog.focus = (int)(type - u.dialog.items);
    key(DU_KEY_DOWN); assert(type->value == 1 && list->count == 2 && !strcmp(list->items[1].text[0], "b.log"));
    key(DU_KEY_DOWN); assert(type->value == 2 && list->count == 6);   /* *.*: the folder and every file */
    key(DU_KEY_UP); key(DU_KEY_UP); assert(type->value == 0 && list->count == 3);
    /* A name without extension gets the default one; an existing file asks first (default button No). */
    du_set_text(name, "notes"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(!u.dialog.kind && find_event(DU_EV_DIALOG, 70) && last()->index == 0 && !strcmp(last()->text, "/docs/notes.txt") && last()->key == 0);
    count = 0; assert(!du_file_open(&u, 71, &o)); name = dlg(ID_NAME);
    du_set_text(name, "a"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.kind && u.dialog.sub == 1 && !dlg(ID_SUB_YES)->hidden && dlg(ID_SUB_EDIT)->hidden && !count);
    frame("file-overwrite");
    key(DU_KEY_ENTER); assert(u.dialog.kind && !u.dialog.sub && !count);             /* default is No */
    u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER); assert(u.dialog.sub == 1);
    click_widget(dlg(ID_SUB_YES));
    assert(!u.dialog.kind && last()->type == DU_EV_DIALOG && last()->id == 71 && !strcmp(last()->text, "/docs/a.txt"));
    /* Existing name typed with its extension: also a prompt; Esc in the prompt means No. */
    count = 0; assert(!du_file_open(&u, 72, &o)); name = dlg(ID_NAME);
    du_set_text(name, "b.log"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.sub == 1); key(DU_KEY_ESC); assert(u.dialog.kind && !u.dialog.sub && !count);
    /* Invalid characters and wildcards. */
    du_set_text(name, "a|b"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.sub == 2 && dlg(ID_SUB_NO)->hidden); frame("file-invalid"); key(DU_KEY_ENTER); assert(!u.dialog.sub && !count);
    du_set_text(name, "*.log"); key(DU_KEY_ENTER);
    list = dlg(ID_LIST); assert(!u.dialog.sub && list->count == 2 && !strcmp(list->items[1].text[0], "b.log") && !strcmp(name->text, ""));
    /* Another folder, an absolute name, and the Up button. */
    click_widget(dlg(ID_UP)); assert(!strcmp(u.dialog.folder, "/") && dlg(ID_LIST)->count == 2);
    du_set_text(name, "/other/x.txt"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(!u.dialog.kind && !strcmp(last()->text, "/other/x.txt"));
    /* List and Details views. */
    count = 0; assert(!du_file_open(&u, 73, &o)); list = dlg(ID_LIST);
    assert(list->view == DU_LIST); click_widget(dlg(ID_VDETAILS)); assert(list->view == DU_DETAILS && list->column_count == 3);
    frame("file-details"); click_widget(dlg(ID_VLIST)); assert(list->view == DU_LIST);
    equivalent("file dialog views");
    /* New Folder: a name prompt over the dialog, default "New Folder", created through make_dir, listed and selected. */
    click_widget(dlg(ID_NEWDIR)); assert(u.dialog.sub == 3 && !strcmp(dlg(ID_SUB_EDIT)->text, "New Folder"));
    frame("file-newfolder");
    du_set_text(dlg(ID_SUB_EDIT), "Reports"); u.dialog.focus = (int)(dlg(ID_SUB_EDIT) - u.dialog.items); key(DU_KEY_ENTER);
    assert(!u.dialog.sub && !strcmp(made, "/docs/Reports") && dlg(ID_LIST)->count == 4);
    assert(dlg(ID_LIST)->items[dlg(ID_LIST)->active].selected && !strcmp(dlg(ID_LIST)->items[dlg(ID_LIST)->active].text[0], "Reports"));
    click_widget(dlg(ID_NEWDIR)); assert(!strcmp(dlg(ID_SUB_EDIT)->text, "New Folder")); du_set_text(dlg(ID_SUB_EDIT), "Reports");
    click_widget(dlg(ID_SUB_YES)); assert(u.dialog.sub == 2);                        /* already exists */
    key(DU_KEY_ENTER); du_set_text(name, "x/y"); click_widget(dlg(ID_NEWDIR)); du_set_text(dlg(ID_SUB_EDIT), "a/b"); click_widget(dlg(ID_SUB_YES)); assert(u.dialog.sub == 2);
    key(DU_KEY_ENTER);
    du_dialog_close(&u);
    /* Open: FILE_MUST_EXIST, PATH_MUST_EXIST, the default extension only when the bare name is absent. */
    struct du_file_options open_o = {.flags = DU_FILE_FILE_MUST_EXIST, .title = "Open", .folder = "/docs", .name = "", .filters = filters, .filter_count = 3,
                                     .filter_index = 2, .default_ext = "txt", .lister = fs_lister};
    count = 0; assert(!du_file_open(&u, 74, &open_o)); name = dlg(ID_NAME);
    assert(u.dialog.kind == DU_DLG_OPEN && dlg(ID_LIST)->count == 7);   /* the Reports folder made above is listed too */
    du_set_text(name, "zzz.txt"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.sub == 2 && !count); frame("file-notfound"); key(DU_KEY_ENTER);
    du_set_text(name, "noext"); key(DU_KEY_ENTER);                                   /* exists exactly as typed: no extension appended */
    assert(!u.dialog.kind && !strcmp(last()->text, "/docs/noext") && last()->key == 2);
    count = 0; assert(!du_file_open(&u, 75, &open_o)); name = dlg(ID_NAME);
    du_set_text(name, "c"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(!u.dialog.kind && !strcmp(last()->text, "/docs/c.txt") && last()->index == 0);   /* c.TXT exists: names match without case */
    count = 0; assert(!du_file_open(&u, 76, &open_o)); name = dlg(ID_NAME);
    du_set_text(name, "/nowhere/f.txt"); u.dialog.focus = (int)(name - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.sub == 2 && !count); key(DU_KEY_ENTER); du_dialog_close(&u);
    /* The legacy entry point keeps working and a Save As through it also asks before replacing. */
    count = 0; assert(!du_file_dialog(&u, 77, 1, NULL, "/docs", "a.txt", fs_lister, NULL));
    assert(!strcmp(u.dialog.title, "Save As") && !strcmp(du_combo_text(dlg(ID_TYPE)), "All Files (*.*)"));
    key(DU_KEY_ENTER); assert(u.dialog.sub == 1); click_widget(dlg(ID_SUB_YES)); assert(!strcmp(last()->text, "/docs/a.txt") && last()->index == 0);
    count = 0; assert(!du_file_dialog(&u, 78, 0, "Open", "/docs", "", fs_lister, NULL));
    click_row(dlg(ID_LIST), 2); assert(!strcmp(dlg(ID_NAME)->text, "a.txt")); click_widget(dlg(ID_ACCEPT));
    assert(!u.dialog.kind && !strcmp(last()->text, "/docs/a.txt"));
    /* Cancel and the type combo's drop-down with the mouse. */
    count = 0; assert(!du_file_open(&u, 79, &o)); type = dlg(ID_TYPE);
    click(type->box.x + type->box.w - 8, type->box.y + 10); assert(u.combo_open == type);
    equivalent("file type list open"); frame("file-type-list");
    {   /* three rows; the list drops up when the box leaves no room below */
        int top = type->box.y + type->box.h - 1, height = 3 * DU_ROW + 4;
        if (top + height > H) top = type->box.y + 1 - height;
        click(type->box.x + 20, top + 2 + 1 * DU_ROW + 6);
    }
    assert(!u.combo_open && type->value == 1 && dlg(ID_LIST)->count == 3);   /* Reports, sub, b.log */
    click_widget(dlg(ID_CANCEL)); assert(!u.dialog.kind && last()->index == 1 && last()->id == 79 && last()->key == 1);
    puts("PASS G16 Open/Save As: filters and type combo, default extension, overwrite confirmation, existence messages, New Folder, wildcard filter, views, tooltips, legacy entry");
}

/* G16B: modifiers, buffer lifetime/overflow, read-only and the icon renderer. */
static void file_multiple(void)
{
    fs_reset(); char files[1024];
    struct du_file_result result = {.files = files, .capacity = sizeof(files)};
    struct du_file_options o = {.flags = DU_FILE_ALLOW_MULTIPLE | DU_FILE_READONLY_BOX | DU_FILE_READONLY | DU_FILE_FILE_MUST_EXIST,
        .folder = "/docs", .lister = fs_lister, .result = &result};
    count = 0; assert(!du_file_open(&u, 101, &o));
    struct du_widget *list = dlg(ID_LIST);
    assert(!dlg(ID_READONLY)->hidden && dlg(ID_READONLY)->value);
    click_row(list, 1); hold(DU_KEY_LCTRL, 1); click_row(list, 2); hold(DU_KEY_LCTRL, 0);
    assert(list->items[1].selected && list->items[2].selected);
    equivalent("multiple selection"); frame("file-multiple");
    result.capacity = 2; click_widget(dlg(ID_ACCEPT));
    assert(u.dialog.kind && u.dialog.sub && !result.count && result.required > 2);
    key(DU_KEY_ENTER); result.capacity = sizeof(files); click_widget(dlg(ID_ACCEPT));
    assert(!u.dialog.kind && result.count == 2 && result.read_only && last()->modifiers == DU_FILE_READONLY);
    assert(!strcmp(files, "/docs") && !strcmp(files + 6, "a.txt") && !strcmp(files + 12, "b.log") && !files[18]);
    assert(result.required == 19);
    assert(!du_file_open(&u, 102, &o)); list = dlg(ID_LIST);
    click_row(list, 1); hold(DU_KEY_LSHIFT, 1); click_row(list, 3); hold(DU_KEY_LSHIFT, 0);
    assert(list->items[1].selected && list->items[2].selected && list->items[3].selected);
    hold(DU_KEY_LCTRL, 1); click_row(list, 2); hold(DU_KEY_LCTRL, 0);
    assert(list->items[1].selected && !list->items[2].selected && list->items[3].selected);
    click_widget(dlg(ID_READONLY)); click_widget(dlg(ID_VICONS));
    assert(list->view == DU_ICONS && list->items[1].icon32);
    equivalent("file large icons"); frame("file-icons");
    click_widget(dlg(ID_ACCEPT)); assert(result.count == 2 && !result.read_only);
    /* Quoted names typed in File name, including failure without closing. */
    assert(!du_file_open(&u, 106, &o));
    du_set_text(dlg(ID_NAME), "\"a.txt\" \"missing.txt\"");
    u.dialog.focus = (int)(dlg(ID_NAME) - u.dialog.items); key(DU_KEY_ENTER);
    assert(u.dialog.sub && !result.count); key(DU_KEY_ENTER);
    du_set_text(dlg(ID_NAME), "\"a.txt\" \"c.TXT\"");
    u.dialog.focus = (int)(dlg(ID_NAME) - u.dialog.items); key(DU_KEY_ENTER);
    assert(!u.dialog.kind && result.count == 2 && !strcmp(files + 12, "c.TXT"));
    /* Save ignores multiple selection, a single file retains the full path. */
    o.flags = DU_FILE_SAVE | DU_FILE_ALLOW_MULTIPLE; o.name = "new.txt";
    assert(!du_file_open(&u, 103, &o) && dlg(ID_READONLY)->hidden);
    click_widget(dlg(ID_ACCEPT)); assert(result.count == 1 && !strcmp(files, "/docs/new.txt") && !files[strlen(files) + 1]);
    o.flags = DU_FILE_ALLOW_MULTIPLE; o.result = NULL;
    assert(du_file_open(&u, 104, &o) == -EINVAL);
    puts("PASS G16B file selection: Ctrl/Shift, overflow retry, readonly result, 32px icons, Save single result");
}

struct find_log { unsigned calls; int action; struct du_find f; };
static struct find_log fl;
static void find_cb(struct du_ui *ui, int action, const struct du_find *f, void *ctx)
{
    struct find_log *l = ctx; l->calls++; l->action = action; l->f = *f;
    if (!strcmp(f->what, "missing")) du_find_status(ui, "Cannot find \"missing\".");
}
enum { FID_WHAT = -10300, FID_WITH, FID_WHOLE, FID_CASE, FID_UP, FID_DOWN, FID_REPLACE, FID_ALL };
static struct du_widget *last_item(void) { return &u.dialog.items[u.dialog.count - 1]; }
static void focus_item(const struct du_widget *w) { u.dialog.focus = (int)(w - u.dialog.items); }

static void find_dialogs(void)
{
    memset(&fl, 0, sizeof(fl)); count = 0;
    struct du_find init = {.what = "word", .flags = DU_FIND_DOWN | DU_FIND_CASE};
    assert(!du_find_dialog(&u, 90, 0, &init, find_cb, &fl) && u.dialog.kind == DU_DLG_FIND);
    assert(!strcmp(dlg(FID_WHAT)->text, "word") && dlg(FID_CASE)->value && !dlg(FID_WHOLE)->value && dlg(FID_DOWN)->value && !dlg(FID_UP)->value);
    assert(dlg(FID_WITH)->hidden && dlg(FID_REPLACE)->hidden && !dlg(FID_UP)->hidden);
    frame("find");
    key(DU_KEY_ENTER); assert(fl.calls == 1 && fl.action == DU_FIND_NEXT && !strcmp(fl.f.what, "word") && fl.f.flags == (DU_FIND_DOWN | DU_FIND_CASE) && u.dialog.kind);
    click_widget(dlg(FID_UP)); click_widget(dlg(FID_WHOLE)); click_widget(dlg(-1000));
    assert(fl.calls == 2 && fl.f.flags == (DU_FIND_WHOLE | DU_FIND_CASE) && u.dialog.kind);
    du_set_text(dlg(FID_WHAT), "missing"); click_widget(dlg(-1000));
    assert(fl.calls == 3 && !strcmp(last_item()->text, "Cannot find \"missing\".")); frame("find-status");
    focus_item(dlg(FID_WHAT)); key(0x1e); assert(!last_item()->text[0] && !strcmp(dlg(FID_WHAT)->text, "missinga"));   /* typing clears the status */
    du_set_text(dlg(FID_WHAT), ""); focus_item(dlg(FID_WHAT)); key(0x1e); key(DU_KEY_BACKSPACE);
    assert(dlg(-1000)->disabled);                                                              /* Find Next needs text */
    key(DU_KEY_ESC); assert(!u.dialog.kind && last()->type == DU_EV_DIALOG && last()->id == 90 && last()->index == 1 && fl.calls == 3);
    /* Replace: no direction, Replace and Replace All reach the callback with the replacement text. */
    memset(&fl, 0, sizeof(fl)); count = 0;
    struct du_find rep_init = {.what = "old", .with = "new", .flags = 0};
    assert(!du_find_dialog(&u, 91, 1, &rep_init, find_cb, &fl) && !strcmp(u.dialog.title, "Replace"));
    assert(dlg(FID_UP)->hidden && !dlg(FID_WITH)->hidden && !dlg(FID_REPLACE)->hidden && !dlg(FID_ALL)->hidden);
    frame("replace");
    click_widget(dlg(FID_REPLACE)); assert(fl.calls == 1 && fl.action == DU_FIND_REPLACE && !strcmp(fl.f.what, "old") && !strcmp(fl.f.with, "new") && (fl.f.flags & DU_FIND_DOWN));
    click_widget(dlg(FID_ALL)); assert(fl.calls == 2 && fl.action == DU_FIND_REPLACE_ALL && u.dialog.kind);
    focus_item(dlg(FID_WHAT)); key(DU_KEY_ENTER); assert(fl.calls == 3 && fl.action == DU_FIND_NEXT);
    click_widget(dlg(-1001)); assert(!u.dialog.kind && last()->index == 1 && last()->id == 91);
    puts("PASS G16 Find and Replace: initial flags, Find Next/Replace/Replace All keep the dialog open, direction and options, status text, disabled without text, Close and Esc");
}

enum { NID_FACE = -10500, NID_FACES, NID_STYLE, NID_STYLES, NID_SIZE, NID_SIZES, NID_STRIKE, NID_UNDER, NID_COLOR, NID_SAMPLE, NID_SCRIPT };
static void font_dialog(void)
{
    struct du_font_choice init = {.family = "Liberation Serif", .style = DU_FONT_BOLD, .points = 14, .underline = 1};
    count = 0; assert(!du_font_dialog(&u, 80, &init, NULL, 0, 0, NULL, NULL) && u.dialog.kind == DU_DLG_FONT);
    assert(!strcmp(dlg(NID_FACE)->text, "Liberation Serif") && !strcmp(dlg(NID_STYLE)->text, "Bold") && !strcmp(dlg(NID_SIZE)->text, "14"));
    assert(dlg(NID_UNDER)->value && !dlg(NID_STRIKE)->value && dlg(NID_FACES)->count == 3 && dlg(NID_STYLES)->count == 4 && dlg(NID_SIZES)->count == 16);
    assert(dlg(NID_FACES)->active == 2 && dlg(NID_FACES)->items[2].selected && dlg(NID_STYLES)->active == 2 && dlg(NID_SIZES)->active == 5);
    assert(dlg(NID_SCRIPT)->count == 1 && !strcmp(du_combo_text(dlg(NID_SCRIPT)), "Western"));
    frame("font");
    click_widget(dlg(-1000));
    assert(!u.dialog.kind && last()->type == DU_EV_DIALOG && last()->index == 0 && !strcmp(last()->text, "Liberation Serif|1|14|1|0|000000"));
    /* Lists fill the edits, a typed size selects its row, effects and colour reach the result. */
    count = 0; assert(!du_font_dialog(&u, 81, NULL, NULL, 0, 0, NULL, NULL));
    assert(!strcmp(dlg(NID_FACE)->text, "Liberation Mono") && !strcmp(dlg(NID_SIZE)->text, "10") && dlg(NID_SIZES)->active == 2);
    click_row(dlg(NID_STYLES), 1); assert(!strcmp(dlg(NID_STYLE)->text, "Italic"));
    click_row(dlg(NID_FACES), 2); assert(!strcmp(dlg(NID_FACE)->text, "Liberation Serif"));
    click_row(dlg(NID_SIZES), 0); assert(!strcmp(dlg(NID_SIZE)->text, "8"));
    du_set_text(dlg(NID_SIZE), "0"); click_widget(dlg(-1000));
    assert(u.dialog.kind == DU_DLG_FONT && strstr(last_item()->text, "between 1 and 1638"));          /* refused: the dialog stays */
    frame("font-invalid");
    focus_item(dlg(NID_SIZE)); ctrl_key(0x1e); type_digits("12"); assert(!strcmp(dlg(NID_SIZE)->text, "12") && dlg(NID_SIZES)->active == 4 && !last_item()->text[0]);
    click_widget(dlg(NID_STRIKE)); assert(dlg(NID_STRIKE)->value);
    focus_item(dlg(NID_COLOR)); for (int i = 0; i < 9; i++) key(DU_KEY_DOWN);
    assert(dlg(NID_COLOR)->value == 9);
    click_widget(dlg(-1000));
    struct du_font_choice got; assert(!u.dialog.kind && !du_font_parse(last()->text, &got));
    assert(!strcmp(got.family, "Liberation Serif") && got.style == DU_FONT_ITALIC && got.points == 12 && !got.underline && got.strikeout && got.color == 0xff0000);
    /* A custom face list and an application sample painter; Esc cancels. */
    static const char *const faces[] = {"Alpha", "Beta"};
    assert(!du_font_dialog(&u, 82, NULL, faces, 2, DU_FONT_NO_EFFECTS, NULL, NULL) && dlg(NID_FACES)->count == 2 && dlg(NID_UNDER)->hidden);
    key(DU_KEY_ESC); assert(!u.dialog.kind && last()->index == 1);
    assert(!du_font_dialog(&u, 83, NULL, faces, 2, 0, NULL, NULL)); focus_item(dlg(NID_FACES)); key(DU_KEY_ENTER);   /* Enter in a list is OK */
    assert(!u.dialog.kind && last()->index == 0 && !strncmp(last()->text, "Alpha|", 6));
    puts("PASS G16 Font dialog: initial selection, list/edit sync, size validation, effects and colour, result text, custom faces, no-effects, cancel");
}

enum { CID_DEFINE = -10400, CID_ADD, CID_BASIC, CID_CUSTOM, CID_SPECTRUM, CID_LUM, CID_PREVIEW, CID_ED = -10420, CID_UD = -10440 };
static void color_dialog(void)
{
    uint32_t custom[16]; for (int i = 0; i < 16; i++) custom[i] = 0xffffff;
    count = 0; assert(!du_color_dialog(&u, 83, 0x336699, custom, 0) && u.dialog.kind == DU_DLG_COLOR);
    assert(u.dialog.box.w < 260 && dlg(CID_SPECTRUM)->hidden && dlg(CID_ADD)->hidden && !dlg(CID_DEFINE)->disabled);
    frame("color");
    struct du_rect basic = dlg(CID_BASIC)->box;
    click(basic.x + 10, basic.y + 10);                                                     /* first swatch: FF8080 */
    click_widget(dlg(-1000)); assert(!u.dialog.kind && last()->index == 0 && !strcmp(last()->text, "#FF8080"));
    count = 0; assert(!du_color_dialog(&u, 84, 0x336699, custom, 0));
    click_widget(dlg(CID_DEFINE));
    assert(u.dialog.box.w > 450 && !dlg(CID_SPECTRUM)->hidden && dlg(CID_DEFINE)->disabled);
    assert(!strcmp(dlg(CID_ED - 3)->text, "51") && !strcmp(dlg(CID_ED - 4)->text, "102") && !strcmp(dlg(CID_ED - 5)->text, "153"));
    unsigned h, s, l; du_rgb_to_hsl(0x336699, &h, &s, &l);
    char want[8]; snprintf(want, sizeof(want), "%u", h); assert(!strcmp(dlg(CID_ED)->text, want));
    equivalent("color expanded"); frame("color-full");
    /* A typed red channel updates the others; the up arrow of the blue spinner steps it. */
    focus_item(dlg(CID_ED - 3)); ctrl_key(0x1e); type_digits("200");
    assert(!strcmp(dlg(CID_ED - 3)->text, "200") && !strcmp(dlg(CID_ED - 4)->text, "102"));
    struct du_widget *blue_up = dlg(CID_UD - 5); click(blue_up->box.x + 8, blue_up->box.y + 4);
    assert(!strcmp(dlg(CID_ED - 5)->text, "154"));
    click_widget(dlg(CID_ADD)); assert(custom[0] == 0xc8669a);                               /* Add to Custom Colors */
    struct du_rect sp = dlg(CID_SPECTRUM)->box, lum = dlg(CID_LUM)->box;
    click(sp.x, sp.y + sp.h - 1);                                                          /* hue 0, saturation 0 */
    click(lum.x + 5, lum.y);                                                                /* luminance 240 */
    click_widget(dlg(-1000)); assert(!u.dialog.kind && !strcmp(last()->text, "#FFFFFF"));
    /* Custom swatch, the prevented full open, and cancel. */
    count = 0; assert(!du_color_dialog(&u, 85, 0, custom, DU_COLOR_PREVENT_FULL) && dlg(CID_DEFINE)->disabled && dlg(CID_SPECTRUM)->hidden);
    struct du_rect cu = dlg(CID_CUSTOM)->box; click(cu.x + 10, cu.y + 10);
    click_widget(dlg(-1000)); assert(!strcmp(last()->text, "#C8669A"));
    count = 0; assert(!du_color_dialog(&u, 86, 0xabcdef, NULL, DU_COLOR_FULL_OPEN) && u.dialog.box.w > 450);
    key(DU_KEY_ESC); assert(!u.dialog.kind && last()->index == 1);
    puts("PASS G16 Color dialog: basic and custom swatches, expansion, channel edits and spinners, Add to Custom Colors, spectrum and luminance, prevent/full-open flags");
}

enum { BID_TREE = -10600, BID_NEW = -10601 };
static int fs_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    if (!strcmp(to, "/docs/Denied")) return -EACCES;
    if (fs_find(to) >= 0) return -EEXIST;
    size_t len = strlen(from);
    for (unsigned i = 0; i < fs_count; i++) if (!strncmp(fs[i].path, from, len) && (!fs[i].path[len] || fs[i].path[len] == '/')) {
        char old[DU_TEXT_MAX]; snprintf(old, sizeof(old), "%s", fs[i].path + len);
        snprintf(fs[i].path, sizeof(fs[i].path), "%s%s", to, old);
    }
    return 0;
}
static void folder_rename(void)
{
    fs_reset();
    struct du_folder_options o = {.root = "/docs", .initial = "/docs/sub", .lister = fs_lister, .make_dir = fs_mkdir, .rename_dir = fs_rename};
    assert(!du_folder_dialog(&u, 105, &o));
    key(DU_KEY_F2); struct du_widget *edit = dlg(-10602);
    assert(edit && !edit->hidden && !strcmp(edit->text, "sub"));
    du_set_text(edit, "../escape"); key(DU_KEY_ENTER); assert(!edit->hidden && fs_find("/docs/sub") >= 0);
    du_set_text(edit, "Denied"); key(DU_KEY_ENTER); assert(!edit->hidden && fs_find("/docs/sub") >= 0);
    du_set_text(edit, "Renamed"); equivalent("inline folder rename"); frame("folder-rename"); key(DU_KEY_ENTER);
    assert(edit->hidden && fs_find("/docs/Renamed/inner") >= 0);
    key(DU_KEY_F2); du_set_text(edit, "Cancelled"); key(DU_KEY_ESC);
    assert(u.dialog.kind && edit->hidden && fs_find("/docs/Cancelled") < 0);
    click_widget(dlg(-10601)); assert(!edit->hidden && !strcmp(edit->text, "New Folder"));
    du_set_text(edit, "Child"); key(DU_KEY_ENTER); click_widget(dlg(-1000));
    assert(!strcmp(last()->text, "/docs/Renamed/Child") && fs_find(last()->text) >= 0);
    puts("PASS G16B folder rename: F2, inline new folder, invalid and denied retry, cancel, descendants");
}

static void folder_dialog(void)
{
    fs_reset();
    struct du_folder_options fo = {.title = "Browse For Folder", .prompt = "Select the folder:", .root = "/", .initial = "/docs/sub",
                                   .lister = fs_lister, .make_dir = fs_mkdir};
    count = 0; assert(!du_folder_dialog(&u, 87, &fo) && u.dialog.kind == DU_DLG_FOLDER);
    struct du_widget *tree = dlg(BID_TREE);
    assert(tree->count == 4 && !strcmp(tree->items[0].text[0], "CiukiOS (C:)") && !strcmp(tree->items[1].text[0], "docs") && !strcmp(tree->items[2].text[0], "sub") &&
           !strcmp(tree->items[3].text[0], "other") && tree->active == 2 && tree->items[2].selected && tree->items[2].depth == 2 && tree->items[1].expanded);
    frame("folder");
    click_widget(dlg(-1000)); assert(!u.dialog.kind && !strcmp(last()->text, "/docs/sub") && last()->index == 0);
    count = 0; assert(!du_folder_dialog(&u, 88, &fo)); tree = dlg(BID_TREE);
    click_row(tree, 3); assert(tree->active == 3); key(DU_KEY_RIGHT); assert(tree->count == 5 && !strcmp(tree->items[4].text[0], "deep") && tree->items[3].expanded);
    key(DU_KEY_LEFT); assert(tree->count == 4);
    click_row(tree, 1); click_widget(dlg(BID_NEW));
    assert(!strcmp(made, "/docs/New Folder") && !strcmp(tree->items[tree->active].text[0], "New Folder") && tree->items[tree->active].selected);
    click_widget(dlg(BID_NEW)); assert(!strcmp(made, "/docs/New Folder/New Folder"));
    click_widget(dlg(-1000)); assert(!strcmp(last()->text, "/docs/New Folder/New Folder"));
    struct du_folder_options plain = {.root = "/docs", .lister = fs_lister, .flags = DU_FILE_NO_NEW_FOLDER};
    assert(!du_folder_dialog(&u, 89, &plain) && dlg(BID_NEW)->hidden && dlg(BID_TREE)->count >= 2 && !strcmp(dlg(BID_TREE)->items[0].text[0], "docs"));
    click_widget(dlg(-1000)); assert(!strcmp(last()->text, "/docs"));
    count = 0; assert(!du_folder_dialog(&u, 90, &plain)); key(DU_KEY_ESC); assert(last()->index == 1);
    puts("PASS G16 Browse for Folder: lazy tree, initial folder, expand/collapse, Make New Folder, root restriction, no-new-folder flag, cancel");
}

static void dialogs(void)
{
    find_dialogs(); font_dialog(); color_dialog(); folder_dialog();
}

int main(int argc, char **argv)
{
    out = argc > 1 ? argv[1] : NULL;
    pixels = calloc((size_t)W * H + 1, 4); reference = calloc((size_t)W * H, 4); assert(pixels && reference); pixels[W * H] = 0x5a5a5a5au;
    du_init(&u, pixels, W, H); u.event = handler;
    patterns(); colours_and_fonts(); find_search();
    controls();
    file_dialog(); file_multiple();
    dialogs(); folder_rename();
    puts("PASS G16 common controls and dialogs");
    return 0;
}
