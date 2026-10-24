/* G15 libdeskui help: tooltips, F1 (WM_HELP), Shift+F1 What's This?, the dialog
 * "?" button and the production LAUNCH of CiukiOS Help.
 * Windows 98 reference: tooltips appear after the pointer rests on a control
 * (500 ms) and hide after 5 s, a click or a key; F1 raises WM_HELP for the
 * focused control and opens the program's help; Shift+F1 and the caption "?"
 * turn the pointer into the help cursor and the next click shows the control's
 * context help (RK-keys, RK-vmm, MS-syscmd SC_CONTEXTHELP).
 * SPDX-License-Identifier: MIT */
#include "deskui.h"
#include "resize_transport.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 400
#define H 300
static uint32_t *pixels;
static struct du_ui u;
static uint64_t now = 1000000000ull;
static const char *out;
static struct du_event events[64]; static unsigned event_count;
static char texts[64][256];
static int launches; static char launched[64]; static int launch_result;
static int consume_help;

static void handler(struct du_ui *ui, const struct du_event *e, void *arg)
{
    (void)arg;
    assert(event_count < 64);
    events[event_count] = *e;
    if (e->text) { snprintf(texts[event_count], sizeof(texts[0]), "%s", e->text); events[event_count].text = texts[event_count]; }
    event_count++;
    if (e->type == DU_EV_HELP && consume_help) ui->help_consumed = 1;
}
static unsigned count_of(enum du_event_type type)
{ unsigned n = 0; for (unsigned i = 0; i < event_count; i++) n += events[i].type == type; return n; }
static const struct du_event *last_of(enum du_event_type type)
{ for (unsigned i = event_count; i > 0; i--) if (events[i - 1].type == type) return &events[i - 1]; return NULL; }
static int fake_launch(struct du_ui *ui, const char *topic, void *ctx)
{ (void)ui; (void)ctx; launches++; snprintf(launched, sizeof(launched), "%s", topic ? topic : "(contents)"); return launch_result; }
static void input(int type, int code, int value, int value2)
{ now += 1000000ull; struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = now}; du_input(&u, &e); }
static void key(int code) { input(CIUKI_INPUT_KEY, code, 1, 0); input(CIUKI_INPUT_KEY, code, 0, 0); }
static void chord(int modifier, int code) { input(CIUKI_INPUT_KEY, modifier, 1, 0); key(code); input(CIUKI_INPUT_KEY, modifier, 0, 0); }
static void move(int x, int y) { input(CIUKI_INPUT_MOTION, 0, x, y); }
static void press(int button) { input(CIUKI_INPUT_BUTTON, button, 1, 0); }
static void release(int button) { input(CIUKI_INPUT_BUTTON, button, 0, 0); }
static void at(uint64_t ns) { du_tick(&u, ns); }
static void reset_events(void) { event_count = 0; }
static void frame(const char *name)
{
    du_render(&u);
    assert(pixels[W * H] == 0x5a5a5a5au);
    if (!out) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/help-%s.xrgb", out, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(pixels, 4, W * H, f) == W * H); fclose(f);
}
static int pixel_count(struct du_rect r, uint32_t colour)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) n += pixels[y * W + x] == colour;
    return n;
}
static void fresh(void)
{
    du_init(&u, pixels, W, H);
    u.event = handler; u.help_open = fake_launch;
    reset_events(); launches = 0; launched[0] = 0; launch_result = 0; consume_help = 0;
    u.now = now;
}

enum { B_OK = 1, B_PLAIN, E_NAME, B_TIPPED };
static void controls(void)
{
    struct du_widget *w;
    fresh();
    w = du_add(&u, DU_BUTTON, B_OK, (struct du_rect){20, 40, 80, 24}, "OK");
    w->tip = "Accepts the changes"; w->help = "Saves what you changed and closes the page.";
    du_add(&u, DU_BUTTON, B_PLAIN, (struct du_rect){120, 40, 80, 24}, "Plain");
    w = du_add(&u, DU_EDIT, E_NAME, (struct du_rect){20, 90, 200, 22}, "");
    w->help = "Type your name here.";
    w = du_add(&u, DU_BUTTON, B_TIPPED, (struct du_rect){300, 260, 90, 30}, "Corner");
    w->tip = "A tip near the lower right corner of the window";
    du_focus(&u, E_NAME);
    du_render(&u);
}

static void test_tooltips(void)
{
    controls();
    assert(du_tooltip(&u, B_PLAIN, "Plain button") && !strcmp(du_get(&u, B_PLAIN)->tip, "Plain button") && !du_tooltip(&u, 99, "x"));
    du_get(&u, B_PLAIN)->tip = NULL;
    assert(u.tip.widget == -1 && !u.tip.visible && du_next_deadline(&u) == UINT64_MAX);
    /* Resting on a control with a tip arms the 500 ms delay and the deadline wakes the loop. */
    move(40, 50); uint64_t armed = u.tip.show_ns;
    assert(u.tip.widget == 0 && !u.tip.visible && armed == u.now + DU_TIP_DELAY_NS && du_next_deadline(&u) == armed);
    at(armed - 1); assert(!u.tip.visible);
    at(armed); assert(u.tip.visible && !u.tip.context && !strcmp(u.tip.text, "Accepts the changes"));
    struct du_rect box = u.tip.box;
    assert(box.w == du_text_width("Accepts the changes") + 10 && box.h > 0 && box.x >= 0 && box.y >= 40 + 24 - 5 &&
           box.x + box.w <= W && box.y + box.h <= H);
    assert(du_next_deadline(&u) == u.tip.hide_ns && u.tip.hide_ns == armed + DU_TIP_SHOW_NS);
    assert(u.dirty);
    du_render(&u);
    assert(pixels[box.y * W + box.x] == 0x000000u && pixels[(box.y + 2) * W + box.x + 2] != 0 && pixel_count(box, 0xffffe1u) > box.w * box.h / 2);
    frame("tooltip");
    /* The tip leaves by itself after five seconds. */
    at(u.tip.hide_ns - 1); assert(u.tip.visible);
    at(u.tip.hide_ns); assert(!u.tip.visible && u.tip.widget == -1 && u.dirty);
    du_render(&u); assert(pixel_count(box, 0xffffe1u) == 0);
    /* Moving to another control hides the tip, and a control without one arms nothing. */
    move(300, 200); move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    move(130, 50); assert(!u.tip.visible && u.tip.widget == -1);
    move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    move(300, 200); assert(!u.tip.visible && u.tip.widget == -1);
    /* Any button press, key press or wheel step hides it. */
    move(40, 50); at(u.tip.show_ns); assert(u.tip.visible); press(1); assert(!u.tip.visible && u.tip.widget == -1); release(1);
    move(300, 200); move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    key(DU_KEY_SPACE); assert(!u.tip.visible);
    move(300, 200); move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    struct ciuki_input_event wheel = {.type = CIUKI_INPUT_WHEEL, .value = 1, .monotonic_ns = now}; du_input(&u, &wheel); assert(!u.tip.visible);
    /* Near the lower right corner the tip is kept inside the window, above the pointer when below does not fit. */
    move(310, 270); at(u.tip.show_ns); assert(u.tip.visible);
    box = u.tip.box; assert(box.x + box.w <= W && box.y + box.h <= H && box.x >= 0 && box.y < 270);
    move(300, 200);
    /* No tip while a dialog, a menu or a capture is active, or the window is inactive. */
    move(40, 50); du_message(&u, 7, "T", "Dialog", DU_DLG_ICON_INFO, NULL, 0, 0, 0); at(u.tip.show_ns + 10); assert(!u.tip.visible);
    du_dialog_close(&u); reset_events();
    move(300, 200); move(40, 50); du_window_focus(&u, 0); at(u.tip.show_ns); assert(!u.tip.visible);
    du_window_focus(&u, 1);
    /* A resize or a focus loss drops a shown tip. */
    move(300, 200); move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    du_window_focus(&u, 0); assert(!u.tip.visible); du_window_focus(&u, 1);
    move(300, 200); move(40, 50); at(u.tip.show_ns); assert(u.tip.visible);
    du_resize(&u, pixels, W, H); assert(!u.tip.visible);
    /* The tooltip is not an event of the program. */
    assert(!count_of(DU_EV_HELP) && !launches);
}

static void test_f1(void)
{
    /* No topic: F1 stays an ordinary key (a DOS window or a program with its own F1). */
    controls();
    key(DU_KEY_F1);
    assert(!launches && count_of(DU_EV_KEY) == 1 && last_of(DU_EV_KEY)->key == DU_KEY_F1 && !count_of(DU_EV_HELP));
    /* With a topic F1 raises WM_HELP for the focused control and opens Help at the topic. */
    reset_events(); du_help_topic(&u, "paint"); assert(!strcmp(u.help_topic, "paint"));
    key(DU_KEY_F1);
    assert(count_of(DU_EV_HELP) == 1 && !count_of(DU_EV_KEY) && launches == 1 && !strcmp(launched, "paint"));
    const struct du_event *e = last_of(DU_EV_HELP);
    assert(e->id == E_NAME && e->index == DU_HELP_F1 && !strcmp(e->text, "Type your name here."));
    /* The focused control without help text still reports its id; none focused reports 0. */
    reset_events(); du_focus(&u, B_PLAIN); key(DU_KEY_F1);
    assert(last_of(DU_EV_HELP)->id == B_PLAIN && !last_of(DU_EV_HELP)->text && launches == 2);
    /* The program handles it itself (WM_HELP handled): no launch. */
    reset_events(); consume_help = 1; key(DU_KEY_F1); assert(count_of(DU_EV_HELP) == 1 && launches == 2 && u.help_consumed);
    consume_help = 0; key(DU_KEY_F1); assert(launches == 3 && !u.help_consumed);
    /* Ctrl+F1 and Alt+F1 are not help keys. */
    reset_events(); chord(DU_KEY_LCTRL, DU_KEY_F1); chord(DU_KEY_LALT, DU_KEY_F1);
    assert(launches == 3 && !count_of(DU_EV_HELP) && count_of(DU_EV_KEY) == 2);
    /* F1 works with a dialog open (the dialog's focused control is reported) and with a menu open. */
    reset_events(); du_message(&u, 5, "Question", "Continue?", DU_DLG_ICON_QUESTION, NULL, 0, 0, 0);
    key(DU_KEY_F1); assert(launches == 4 && u.dialog.kind && count_of(DU_EV_HELP) == 1 && last_of(DU_EV_HELP)->index == DU_HELP_F1);
    du_dialog_close(&u);
    static struct du_menu_item items[] = {{"&One", 20, 0, 0, 0}};
    static struct du_menu menu = {"&File", items, 1};
    u.menus = &menu; u.menu_count = 1; du_menu_open(&u, 0, 1);
    key(DU_KEY_F1); assert(launches == 5 && u.menu == 0);
    du_menu_close(&u);
    /* A launcher failure is reported to du_help's caller; F1 itself never loops or crashes. */
    launch_result = -EAGAIN; assert(du_help(&u, NULL) == -EAGAIN && launches == 6);
    launch_result = 0; assert(!du_help(&u, "files") && !strcmp(launched, "files"));
    assert(!du_help(&u, NULL) && !strcmp(launched, "paint"));
    u.help_topic = NULL; assert(!du_help(&u, NULL) && !strcmp(launched, "(contents)"));
    u.help_open = NULL; assert(du_help(&u, "files") == -ENOSYS);
    /* Esc and other keys do not turn F1 into anything else; Shift+F1 is What's This?, not Help. */
    u.help_open = fake_launch; u.help_topic = "paint"; launches = 0;
    chord(DU_KEY_LSHIFT, DU_KEY_F1); assert(!launches && u.whats_this);
    du_context_help(&u, 0);
}

static void test_whats_this(void)
{
    controls(); du_help_topic(&u, "paint");
    /* Shift+F1: the pointer becomes the help cursor, keys are ignored, Esc cancels. */
    assert(du_cursor_at(&u) != DESK_CURSOR_HELP);
    chord(DU_KEY_LSHIFT, DU_KEY_F1);
    assert(u.whats_this && du_cursor_at(&u) == DESK_CURSOR_HELP && !launches && !count_of(DU_EV_HELP));
    reset_events(); du_focus(&u, E_NAME); key(0x1e); key(DU_KEY_ENTER);
    assert(u.whats_this && !du_get(&u, E_NAME)->text[0] && !event_count);              /* typing and Enter are swallowed */
    key(DU_KEY_ESC); assert(!u.whats_this && du_cursor_at(&u) != DESK_CURSOR_HELP && !event_count);
    /* A click on a control with help text: WM_HELP (HELPINFO_WINDOW) and the popup; no control action. */
    reset_events(); chord(DU_KEY_LSHIFT, DU_KEY_F1);
    move(40, 50); press(1);
    assert(!u.whats_this && count_of(DU_EV_HELP) == 1 && !count_of(DU_EV_ACTIVATE));
    const struct du_event *e = last_of(DU_EV_HELP);
    assert(e->id == B_OK && e->index == DU_HELP_CONTEXT && e->x == 40 && e->y == 50 && !strcmp(e->text, "Saves what you changed and closes the page."));
    assert(u.tip.visible && u.tip.context && u.tip.text == du_get(&u, B_OK)->help && u.tip.hide_ns == 0);
    struct du_rect popup = u.tip.box; assert(popup.w > 0 && popup.x >= 0 && popup.y >= 0 && popup.x + popup.w <= W && popup.y + popup.h <= H);
    release(1); assert(u.tip.visible && !du_get(&u, B_OK)->pressed);                    /* the release of the same click keeps it */
    assert(du_next_deadline(&u) == UINT64_MAX);
    du_render(&u); assert(pixel_count(popup, 0xffffe1u) > popup.w * popup.h / 2);
    frame("whats-this");
    move(300, 200); move(130, 50); assert(u.tip.visible);                                /* motion keeps it */
    at(now + 60ull * 1000000000ull); assert(u.tip.visible);                              /* time never does */
    press(1); assert(!u.tip.visible); release(1);                                        /* the next click or key hides it */
    assert(count_of(DU_EV_HELP) == 1 && launches == 0);
    chord(DU_KEY_LSHIFT, DU_KEY_F1); move(300, 200); press(1); release(1);
    assert(u.tip.visible == 0 && last_of(DU_EV_HELP)->id == 0 && !last_of(DU_EV_HELP)->text);   /* nothing under the pointer */
    reset_events(); chord(DU_KEY_LSHIFT, DU_KEY_F1); move(40, 50); key(DU_KEY_ESC); press(1); release(1);
    assert(count_of(DU_EV_HELP) == 0 && count_of(DU_EV_ACTIVATE) == 1);                  /* Esc cancelled: a normal click */
    /* A control without help text: the event, no popup. */
    reset_events(); chord(DU_KEY_LSHIFT, DU_KEY_F1); move(130, 50); press(1); release(1);
    assert(last_of(DU_EV_HELP)->id == B_PLAIN && !last_of(DU_EV_HELP)->text && !u.tip.visible && !u.whats_this);
    /* The program shows its own help for the control. */
    reset_events(); consume_help = 1; chord(DU_KEY_LSHIFT, DU_KEY_F1); move(40, 50); press(1); release(1);
    assert(count_of(DU_EV_HELP) == 1 && !u.tip.visible);
    consume_help = 0;
    /* The mode ends with a resync or a focus loss, and never survives a new dialog. */
    chord(DU_KEY_LSHIFT, DU_KEY_F1); assert(u.whats_this); du_window_focus(&u, 0); assert(!u.whats_this); du_window_focus(&u, 1);
    chord(DU_KEY_LSHIFT, DU_KEY_F1); du_message(&u, 6, "T", "x", DU_DLG_ICON_INFO, NULL, 0, 0, 0); assert(!u.whats_this); du_dialog_close(&u);
    /* The right button asks as well; the click is not delivered to the control. */
    reset_events(); chord(DU_KEY_LSHIFT, DU_KEY_F1); move(40, 50); press(2); release(2);
    assert(count_of(DU_EV_HELP) == 1 && !u.whats_this && u.tip.visible);
    press(1); release(1);
}

static void test_dialog_button(void)
{
    controls(); du_help_topic(&u, "paint");
    du_message(&u, 9, "Settings", "Choose an option.", DU_DLG_ICON_INFO, (const char *const[]){"OK", "Cancel"}, 2, 0, 1);
    assert(!u.dialog.help_button && !du_dialog_help_box(&u).w);
    du_dialog_help_button(&u, 1);
    struct du_rect box = du_dialog_help_box(&u);
    assert(u.dialog.help_button && box.w > 0 && box.h > 0 && du_contains(u.dialog.box, box.x, box.y));
    int close[4]; desk_title_button_box(u.dialog.box.x, u.dialog.box.y, u.dialog.box.w, DESK_TB_CLOSE, close);
    assert(box.x + box.w <= close[0] && box.y == close[1]);                              /* left of the close box, in the caption */
    u.dialog.items[0].help = "Accepts the choice and closes the dialog.";
    frame("dialog-button");
    move(box.x + 3, box.y + 3); assert(u.dialog.help_hot);
    move(box.x - 60, box.y + 60); assert(!u.dialog.help_hot);
    /* Click "?": the pointer waits; clicking a dialog control raises WM_HELP for it. */
    reset_events(); move(box.x + 3, box.y + 3); press(1); release(1);
    assert(u.whats_this && u.dialog.kind && !count_of(DU_EV_DIALOG) && du_cursor_at(&u) == DESK_CURSOR_HELP);
    struct du_rect ok = du_dialog_button(&u, 0);
    move(ok.x + 4, ok.y + 4); press(1); release(1);
    assert(!u.whats_this && u.dialog.kind && !count_of(DU_EV_DIALOG) && count_of(DU_EV_HELP) == 1);
    const struct du_event *e = last_of(DU_EV_HELP);
    assert(e->id == u.dialog.items[0].id && e->index == DU_HELP_CONTEXT && !strcmp(e->text, "Accepts the choice and closes the dialog."));
    assert(u.tip.visible && u.tip.context && !strcmp(u.tip.text, u.dialog.items[0].help));
    frame("dialog-whats-this");
    move(u.dialog.box.x + 6, u.dialog.box.y + 40);
    press(1); release(1); assert(!u.tip.visible && u.dialog.kind);                      /* any click hides it (here on empty dialog face) */
    struct du_widget *hit = du_dialog_item_at(&u, ok.x + 4, ok.y + 4); assert(hit == &u.dialog.items[0]);
    assert(!du_dialog_item_at(&u, 0, 0));
    /* Without the "?" button a click on that spot does nothing special; Esc cancels the mode. */
    du_dialog_help_button(&u, 0); assert(!du_dialog_help_box(&u).w);
    reset_events(); move(box.x + 3, box.y + 3); press(1); release(1); assert(!u.whats_this);
    du_dialog_help_button(&u, 1); chord(DU_KEY_LSHIFT, DU_KEY_F1); assert(u.whats_this); key(DU_KEY_ESC); assert(!u.whats_this && u.dialog.kind);
    du_dialog_close(&u);
    du_dialog_help_button(&u, 1); assert(!u.dialog.help_button);                        /* no dialog: nothing to flag */
    /* A new dialog starts without the button. */
    du_message(&u, 1, "A", "B", DU_DLG_ICON_INFO, NULL, 0, 0, 0); assert(!u.dialog.help_button); du_dialog_close(&u);
}

/* The production client glue: F1 asks the desktop to LAUNCH "help" with "topic:<id>". */
static void test_launch_wire(void)
{
    struct du_window win; struct du_ui client; struct ciuki_message m; struct desk_packet p;
    rt_reset();
    assert(!du_window_open_resizable(&win, &client, 3, 400, 300, "Wire", 200, 160));
    while (rt_pop(&m)) for (unsigned i = 0; i < m.fd_count; i++) rt_close(m.fds[i]);
    assert(client.help_open && client.help_ctx == &win);
    du_add(&client, DU_EDIT, 1, (struct du_rect){8, 30, 200, 24}, ""); du_focus(&client, 1);
    du_help_topic(&client, "files");
    struct ciuki_input_event f1 = {.type = CIUKI_INPUT_KEY, .code = DU_KEY_F1, .value = 1}; du_input(&client, &f1);
    assert(rt_pop(&m) == 0 && win.count == 1);
    assert(du_window_turn(&win, 2) == 1);
    int found = 0;
    while (rt_pop(&m)) {
        if (m.data[1] != DESK_LAUNCH) { for (unsigned i = 0; i < m.fd_count; i++) rt_close(m.fds[i]); continue; }
        assert(m.data[0] == DESK_SHELL_VERSION && desk_parse(&m, 1, NULL, &p) && !strcmp(p.title, "help") && !strcmp(p.argument, "topic:files"));
        found++;
    }
    assert(found == 1);
    assert(!du_help(&client, "dos") && !du_help(&client, NULL) && du_window_turn(&win, 3) == 1);
    const char *wanted[] = {"topic:dos", "topic:files"}; unsigned seen = 0;
    while (rt_pop(&m)) if (m.data[1] == DESK_LAUNCH) { assert(desk_parse(&m, 1, NULL, &p) && !strcmp(p.argument, wanted[seen++])); }
    assert(seen == 2);
    /* Contents only: no argument. A topic id the desktop would refuse is refused here. */
    client.help_topic = NULL; assert(!du_help(&client, NULL) && du_window_turn(&win, 4) == 1);
    found = 0; while (rt_pop(&m)) if (m.data[1] == DESK_LAUNCH) { assert(desk_parse(&m, 1, NULL, &p) && !strcmp(p.title, "help") && !p.argument[0]); found++; }
    assert(found == 1);
    assert(du_help(&client, "Bad Topic") == -EINVAL && du_help(&client, "../etc") == -EINVAL && du_help(&client, "a:b") == -EINVAL);
    char longid[64]; memset(longid, 'a', 50); longid[50] = 0; assert(du_help(&client, longid) == -EINVAL);
    memset(longid, 'a', 40); longid[40] = 0; assert(!du_help(&client, longid));
    du_window_destroy(&win); assert(!rt_surfaces());
}

int main(int argc, char **argv)
{
    out = argc > 1 ? argv[1] : NULL;
    pixels = calloc(W * H + 1, 4); assert(pixels);
    pixels[W * H] = 0x5a5a5a5au;
    du_init(&u, pixels, W, H);
    test_tooltips();
    test_f1();
    test_whats_this();
    test_dialog_button();
    test_launch_wire();
    free(pixels);
    puts("PASS G15 libdeskui help: tooltips (500 ms, 5 s, hide on input), F1 WM_HELP and launch, What's This? mode, dialog ? button, LAUNCH help topic:<id> wire");
    return 0;
}
