/* SPDX-License-Identifier: MIT
 * libdeskui widgets: geometry and hit-testing, keyboard/mouse behaviour,
 * menus, dialogs, timers, clipboard, focus releases and rendering frames. */
#include "deskui.h"
#include "textarea.h"
#include <assert.h>
#include "resize_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define W 400
#define H 300
static uint32_t *pixels;
static struct du_ui u;
static struct du_event events[512]; static unsigned count;
static uint64_t now = 1000000000ull;
static const char *out;
static char texts[512][DU_TEXT_MAX];
static void handler(struct du_ui *ui, const struct du_event *e, void *arg)
{
    (void)ui; (void)arg;
    assert(count < 512);
    events[count] = *e;
    /* Event text is valid only during the callback. */
    if (e->text) { snprintf(texts[count], DU_TEXT_MAX, "%s", e->text); events[count].text = texts[count]; }
    count++;
}
static struct du_event *last(void) { assert(count); return &events[count - 1]; }
static void input(int type, int code, int value, int value2)
{ now += 10000000ull; struct ciuki_input_event e = {.type = (uint32_t)type, .code = code, .value = value, .value2 = value2, .monotonic_ns = now}; du_input(&u, &e); }
static void key(int code) { input(CIUKI_INPUT_KEY, code, 1, 0); input(CIUKI_INPUT_KEY, code, 0, 0); }
static void hold(int code, int down) { input(CIUKI_INPUT_KEY, code, down, 0); }
static void click(int x, int y, int button) { input(CIUKI_INPUT_MOTION, 0, x, y); input(CIUKI_INPUT_BUTTON, button, 1, 0); input(CIUKI_INPUT_BUTTON, button, 0, 0); now += 700000000ull; }
static void frame(const char *name)
{
    du_render(&u);
    assert(pixels[W * H] == 0x5a5a5a5au);
    if (!out) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/deskui-%s.xrgb", out, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(pixels, 4, W * H, f) == W * H); fclose(f);
}
static int lister(void *ctx, const char *path, struct du_file_entry *e, unsigned cap, unsigned *n)
{
    (void)ctx; (void)cap;
    if (!strcmp(path, "/docs")) { strcpy(e[0].name, "readme.txt"); e[0].directory = 0; *n = 1; return 0; }
    if (strcmp(path, "/")) return -2;
    strcpy(e[0].name, "zeta.txt"); e[0].directory = 0; e[0].size = 5;
    strcpy(e[1].name, "docs"); e[1].directory = 1;
    strcpy(e[2].name, "Alpha.cfg"); e[2].directory = 0;
    *n = 3; return 0;
}

static void wheel_event(struct du_ui *ui, unsigned type, int value, int value2)
{
    struct ciuki_input_event e={.type=type,.value=value,.value2=value2}; du_input(ui,&e);
}
static int wheel_lister(void *ctx, const char *path, struct du_file_entry *e, unsigned cap, unsigned *n)
{
    (void)ctx; (void)path; assert(cap>=40); *n=40;
    for (unsigned i=0;i<40;i++) { memset(&e[i],0,sizeof(e[i])); snprintf(e[i].name,sizeof(e[i].name),"row%02u.txt",i); }
    return 0;
}
static void test_wheel(void)
{
    struct du_ui *ui=calloc(1,sizeof(*ui)); assert(ui); du_init(ui,pixels,W,H);
    struct du_item rows[40]={0}; for (unsigned i=0;i<40;i++) rows[i].text[0]="row";
    struct du_widget *w=du_add(ui,DU_LISTVIEW,100,(struct du_rect){0,0,180,100},"");
    w->items=rows; w->count=40; w->active=7; rows[7].selected=1;
    wheel_event(ui,CIUKI_INPUT_MOTION,10,30); int focus=ui->focus;
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(w->scroll==3 && ui->dirty);
    assert(ui->mouse_x==10 && ui->mouse_y==30 && w->active==7 && rows[7].selected && ui->focus==focus);
    wheel_event(ui,CIUKI_INPUT_WHEEL,1,0); assert(!w->scroll);
    wheel_event(ui,CIUKI_INPUT_WHEEL,INT_MIN,0); assert(w->scroll==40-du_list_page(w));
    wheel_event(ui,CIUKI_INPUT_WHEEL,INT_MAX,0); assert(!w->scroll);
    w->disabled=1; wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(!w->scroll); w->disabled=0;
    ui->capture=0; wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(!w->scroll); ui->capture=-1;
    ui->menu=0; wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(!w->scroll); ui->menu=-1;
    w->kind=DU_TREE; wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(w->scroll==3);
    w->kind=DU_LISTVIEW; w->view=DU_ICONS; w->scroll=0;
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(w->scroll==3);
    w->view=DU_LIST; w->scroll=0;
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(w->hscroll==3 && !w->scroll);
    wheel_event(ui,CIUKI_INPUT_WHEEL,1,0); assert(!w->hscroll);
    struct du_textarea ta; assert(!du_ta_init(&ta,512));
    char text[160]; for (unsigned i=0;i<40;i++) { text[2*i]='a'; text[2*i+1]='\n'; } text[80]=0;
    assert(!du_ta_set_text(&ta,text,80));
    struct du_widget *area=du_add(ui,DU_TEXTAREA,101,(struct du_rect){200,0,180,110},""); area->data=&ta;
    du_ta_select(&ta,2,6); ta.top=0;
    wheel_event(ui,CIUKI_INPUT_MOTION,210,30);
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(ta.top==3 && ta.cursor==6 && ta.anchor==2 && !ta.modified);
    wheel_event(ui,CIUKI_INPUT_WHEEL,INT_MIN,0); assert(ta.top==ta.vl_count-ta.rows);
    wheel_event(ui,CIUKI_INPUT_WHEEL,INT_MAX,0); assert(!ta.top);
    struct du_widget *bar=du_add(ui,DU_SCROLLBAR,102,(struct du_rect){380,0,16,100},"");
    bar->total=50; bar->page=10;
    wheel_event(ui,CIUKI_INPUT_MOTION,385,30); wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(bar->value==3);
    wheel_event(ui,CIUKI_INPUT_WHEEL,1,0); assert(!bar->value);
    assert(!du_file_dialog(ui,103,0,"Open","/","",wheel_lister,NULL));
    struct du_widget *files=&ui->dialog.items[4]; assert(files->count==40);
    int dialog_focus=ui->dialog.focus;
    wheel_event(ui,CIUKI_INPUT_MOTION,files->box.x+8,files->box.y+25);
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(files->hscroll>0 && !files->scroll && ui->dialog.focus==dialog_focus);
    wheel_event(ui,CIUKI_INPUT_WHEEL,1,0); assert(!files->hscroll);
    files->view=DU_DETAILS;
    wheel_event(ui,CIUKI_INPUT_WHEEL,-1,0); assert(files->scroll==3 && ui->dialog.focus==dialog_focus);
    wheel_event(ui,CIUKI_INPUT_WHEEL,1,0); assert(!files->scroll);
    assert(!w->scroll && !w->hscroll && w->active==7 && rows[7].selected);
    du_dialog_close(ui); du_ta_free(&ta); free(ui);
    puts("PASS F1-68 toolkit wheel: details/icons/list/tree/textarea/scrollbar/modal list, both signs, saturation, selection/focus/pointer retention, disabled/menu/capture guards");
}

static unsigned timer_calls, idle_calls;
static int timer_change;
static void timer_event(struct du_ui *ui,const struct du_event *e,void *arg)
{
    (void)arg; assert(e->type==DU_EV_TICK); timer_calls++;
    if (timer_change==1) {
        struct du_widget *w=du_get(ui,1);
        du_set_text(w,"Changed"); du_invalidate(ui,w->box);
    } else if (timer_change==2) du_progress(ui,2,"Copy","Copying",2,10);
}
static void timer_idle(void *arg) { (void)arg; idle_calls++; }
static void timer_invalidation_test(void)
{
    struct du_ui ui; struct du_window window; rt_reset();
    assert(!du_window_open_resizable(&window,&ui,3,W,H,"Timers",200,160));
    struct du_widget *widget=du_add(&ui,DU_LABEL,1,(struct du_rect){10,30,160,20},"Original");
    window.configured=1; ui.event=timer_event; ui.now=0;
    assert(du_window_turn(&window,0)>0);
    struct ciuki_message m; while(rt_pop(&m)) for(unsigned i=0;i<m.fd_count;i++) rt_close(m.fds[i]);
    assert(!du_timer_start(&ui,7,20000000ull,1));
    rt_renders=rt_compared_rows=timer_calls=idle_calls=0;
    for(unsigned i=1;i<=100;i++) {
        assert(du_window_turn(&window,i*10000000ull)>0); timer_idle(NULL);
        while(rt_pop(&m)) for(unsigned k=0;k<m.fd_count;k++) rt_close(m.fds[k]);
    }
    printf("F2-86 timer idle: ticks=%u idle=%u renders=%u compared_rows=%u per simulated second\n",
        timer_calls,idle_calls,rt_renders,rt_compared_rows); fflush(stdout);
    assert(timer_calls==50 && idle_calls==100 && !rt_renders && !rt_compared_rows);
    timer_change=1; assert(du_window_turn(&window,1020000000ull)>0);
    assert(rt_renders==1 && rt_compared_rows==(unsigned)widget->box.h && rt_render_dirty==3);
    assert(!memcmp(&rt_render_bounds,&widget->box,sizeof(widget->box)));
    uint32_t reference[W*H]; memcpy(reference,ui.canvas.pixels,sizeof(reference));
    ui.dirty=1; du_render(&ui); assert(!memcmp(reference,ui.canvas.pixels,sizeof(reference)));
    timer_change=0;
    for(int dirty=1;dirty<=3;dirty++) {
        ui.dirty=dirty; struct du_rect saved=ui.invalid;
        du_tick(&ui,ui.now+20000000ull);
        assert(ui.dirty==dirty && !memcmp(&saved,&ui.invalid,sizeof(saved)));
    }
    du_progress(&ui,2,"Copy","Copying",1,10); du_render(&ui);
    memcpy(reference,ui.canvas.pixels,sizeof(reference));
    timer_change=2; rt_renders=0;
    assert(du_window_turn(&window,ui.now+20000000ull)>0);
    assert(rt_renders==1 && rt_render_dirty==2 && !memcmp(&rt_render_bounds,&ui.dialog.box,sizeof(rt_render_bounds)));
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) if(!du_contains(ui.dialog.box,x,y))
        assert(reference[y*W+x]==ui.canvas.pixels[y*W+x]);
    memcpy(reference,ui.canvas.pixels,sizeof(reference)); ui.dirty=1; du_render(&ui);
    assert(!memcmp(reference,ui.canvas.pixels,sizeof(reference)));
    rt_renders=rt_compared_rows=0;
    assert(du_window_turn(&window,ui.now+20000000ull)>0);
    assert(!rt_renders && !rt_compared_rows); /* identical progress snapshot */
    du_set_text(widget,"Combined"); du_invalidate(&ui,widget->box);
    du_progress(&ui,2,"Copy","Copying",3,10); assert(ui.dirty==3);
    assert(du_contains(ui.invalid,widget->box.x,widget->box.y) &&
        du_contains(ui.invalid,ui.dialog.box.x+ui.dialog.box.w-1,ui.dialog.box.y+ui.dialog.box.h-1));
    du_render(&ui); memcpy(reference,ui.canvas.pixels,sizeof(reference));
    ui.dirty=1; du_render(&ui); assert(!memcmp(reference,ui.canvas.pixels,sizeof(reference)));
    du_progress(&ui,2,"Copy","Copying",0,0); du_render(&ui);
    ui.now+=100000000ull; du_progress(&ui,2,"Copy","Copying",0,0);
    assert(ui.dirty==2); /* unchanged indeterminate values still animate */
    timer_change=0; du_window_destroy(&window);
    assert(!rt_handles() && !rt_mappings());
    puts("PASS F2-86 timer changed widget: renders=1 bounded; progress: renders=1 dialog-only; pending dirty preserved");
}

int main(int argc, char **argv)
{
    out = argc > 1 ? argv[1] : NULL;
    timer_invalidation_test();
    pixels = calloc(W * H + 1, 4); assert(pixels); pixels[W * H] = 0x5a5a5a5au;
    du_init(&u, pixels, W, H); u.event = handler;

    /* Geometry primitives. */
    assert(du_contains((struct du_rect){0, 0, 2, 2}, 1, 1) && !du_contains((struct du_rect){0, 0, 2, 2}, 2, 0));
    struct du_rect i = du_intersection((struct du_rect){0, 0, 10, 10}, (struct du_rect){5, 5, 10, 10});
    assert(i.x == 5 && i.w == 5 && !du_intersection((struct du_rect){0, 0, 1, 1}, (struct du_rect){2, 2, 1, 1}).w);
    /* ui-look: proportional faces (Inter 12): narrow letters advance less than wide ones. */
    assert(du_text_width("Files") == 26 && du_text_width("iiii") < du_text_width("MMMM") && du_cells("caf\xc3\xa9") == 4);
    char key_name[32]; du_format_key(key_name, sizeof(key_name), 0x2e, DU_CTRL); assert(!strcmp(key_name, "Ctrl+C"));
    du_format_key(key_name, sizeof(key_name), DU_KEY_ENTER, DU_ALT); assert(!strcmp(key_name, "Alt+Enter"));
    du_format_key(key_name, sizeof(key_name), DU_KEY_F5, 0); assert(!strcmp(key_name, "F5"));

    /* Widgets and hit-testing. */
    struct du_widget *button = du_add(&u, DU_BUTTON, 1, (struct du_rect){8, 30, 75, 23}, "&Open");
    struct du_widget *edit = du_add(&u, DU_EDIT, 2, (struct du_rect){90, 30, 200, 22}, "/system");
    static struct du_item rows[12];
    static const char *names[12] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta", "Iota", "Kappa", "Lambda", "Mu"};
    for (int k = 0; k < 12; k++) rows[k] = (struct du_item){{names[k], "1 KB", "Text Document", "2026-10-11 12:00"}, NULL, NULL, 0, 0, 0, 0, 0};
    struct du_widget *list = du_add(&u, DU_LISTVIEW, 3, (struct du_rect){8, 60, 280, 150}, "");
    list->items = rows; list->count = 12;
    list->columns[0] = (struct du_column){"Name", 110, 0}; list->columns[1] = (struct du_column){"Size", 50, 1};
    list->columns[2] = (struct du_column){"Type", 60, 0}; list->column_count = 3; list->sort = 0;
    assert(du_add(&u, DU_LABEL, 3, (struct du_rect){0}, "dup") == NULL);
    assert(du_hit(&u, 10, 32) == 0 && du_hit(&u, 395, 295) == -1 && du_get(&u, 2) == edit);
    /* Details: 18 px header then 17 px rows inside a 2 px border. */
    assert(du_header_hit(list, 20, 65) == 0 && du_header_hit(list, 130, 65) == 1 && du_list_hit(list, 20, 65) == -1);
    assert(du_list_hit(list, 20, 80 + 3) == 0 && du_list_hit(list, 20, 80 + 17 + 3) == 1);
    assert(du_list_page(list) == 7);
    struct du_rect lab = du_list_label(list, 1); assert(lab.x == 10 + 20 && lab.y == 80 + 17 + 1);
    du_list_ensure(list, 11); assert(list->scroll == 5); du_list_ensure(list, 0); assert(list->scroll == 0);
    list->view = DU_ICONS; assert(du_list_page(list) == 2 * 3);   /* 260 px: 3 columns, 2 rows of 68 px */
    assert(du_list_hit(list, 10 + 22 + 10, 62 + 4 + 10) == 0 && du_list_hit(list, 10 + 76 + 30, 62 + 10) == 1);
    list->active = 0; assert(du_list_target(list, DU_KEY_DOWN) == 3 && du_list_target(list, DU_KEY_RIGHT) == 1 && du_list_target(list, DU_KEY_LEFT) == 0);
    list->view = DU_LIST; assert(du_list_page(list) == 8);
    list->active = 2; assert(du_list_target(list, DU_KEY_RIGHT) == 10 && du_list_target(list, DU_KEY_END) == 11);
    list->view = DU_DETAILS; list->active = 0;

    /* Button: press/release inside activates; release outside does not. */
    click(20, 40, 1); assert(last()->type == DU_EV_ACTIVATE && last()->id == 1);
    unsigned before = count;
    input(CIUKI_INPUT_MOTION, 0, 20, 40); input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_MOTION, 0, 200, 250); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    assert(count == before && !button->pressed);

    /* Edit: Shift/Caps text from KEY events, selection, clipboard, UTF-8. */
    du_focus(&u, 2); hold(DU_KEY_LCTRL, 1); key(0x1e); hold(DU_KEY_LCTRL, 0);
    hold(DU_KEY_LSHIFT, 1); key(0x2e); hold(DU_KEY_LSHIFT, 0); key(0x17); key(0x34); key(0x1e);
    assert(!strcmp(edit->text, "Ci.a") && last()->type == DU_EV_CHANGE);
    key(DU_KEY_CAPS); key(0x30); key(DU_KEY_CAPS); assert(!strcmp(edit->text, "Ci.aB"));
    key(DU_KEY_HOME); hold(DU_KEY_LSHIFT, 1); key(DU_KEY_RIGHT); key(DU_KEY_RIGHT); hold(DU_KEY_LSHIFT, 0);
    hold(DU_KEY_LCTRL, 1); key(0x2d); hold(DU_KEY_LCTRL, 0); assert(!strcmp(edit->text, ".aB") && !strcmp(du_clipboard_get(), "Ci"));
    key(DU_KEY_END); hold(DU_KEY_LCTRL, 1); key(0x2f); hold(DU_KEY_LCTRL, 0); assert(!strcmp(edit->text, ".aBCi"));
    du_set_text(edit, "caf\xc3\xa9"); key(DU_KEY_BACKSPACE); assert(!strcmp(edit->text, "caf"));
    input(CIUKI_INPUT_TEXT, 'x', 0, 0); assert(!strcmp(edit->text, "caf"));    /* TEXT is ignored */
    key(DU_KEY_ENTER); assert(last()->type == DU_EV_COMMIT && last()->id == 2);
    key(DU_KEY_ESC); assert(last()->type == DU_EV_CANCEL);
    click(90 + 4 + 6 * 1 + 2, 40, 1); assert(edit->cursor == 1);

    /* List: click, Ctrl/Shift modifiers, double click opens, keyboard. */
    click(20, 83, 1); assert(last()->type == DU_EV_SELECT && last()->index == 0 && !last()->keyboard);
    hold(DU_KEY_LSHIFT, 1); click(20, 83 + 34, 1); hold(DU_KEY_LSHIFT, 0); assert(last()->index == 2 && (last()->modifiers & DU_SHIFT));
    input(CIUKI_INPUT_MOTION, 0, 20, 83); input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_BUTTON, 1, 0, 0); assert(last()->type == DU_EV_OPEN && last()->index == 0);
    now += 700000000ull;
    key(DU_KEY_DOWN); assert(last()->type == DU_EV_SELECT && last()->index == 1 && last()->keyboard && list->active == 1);
    key(DU_KEY_END); assert(list->active == 11 && list->scroll == 5);
    key(DU_KEY_ENTER); assert(last()->type == DU_EV_OPEN && last()->keyboard);
    key(DU_KEY_MENU); assert(last()->type == DU_EV_CONTEXT && last()->index == 11);
    click(150, 65, 1); assert(last()->type == DU_EV_SORT && last()->index == 1);
    click(20, 83 + 17 * 3, 2); assert(last()->type == DU_EV_CONTEXT && last()->index == 8);
    /* Embedded scrollbar: arrow and thumb drag. */
    list->scroll = 0;
    click(8 + 280 - 2 - 8, 60 + 150 - 2 - 8, 1); assert(list->scroll == 1);
    input(CIUKI_INPUT_MOTION, 0, 280, 62 + 16 + 20); input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_MOTION, 0, 280, 205); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    assert(list->scroll == 5);
    /* Column resize by dragging the header divider. */
    input(CIUKI_INPUT_MOTION, 0, 10 + 110, 65); input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_MOTION, 0, 10 + 130, 65); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    assert(list->columns[0].width == 130);
    list->columns[0].width = 110; list->scroll = 0;

    /* Tab order, focus release on window deactivation and RESYNC. */
    du_focus(&u, 1); key(DU_KEY_TAB); assert(du_focused(&u) == 2); key(DU_KEY_TAB); assert(du_focused(&u) == 3);
    hold(DU_KEY_LSHIFT, 1); key(DU_KEY_TAB); hold(DU_KEY_LSHIFT, 0); assert(du_focused(&u) == 2);
    hold(DU_KEY_LCTRL, 1); input(CIUKI_INPUT_MOTION, 0, 20, 40); input(CIUKI_INPUT_BUTTON, 1, 1, 0);
    assert(u.modifiers == DU_CTRL && u.capture == 0 && button->pressed);
    du_window_focus(&u, 0); assert(!u.modifiers && u.capture == -1 && !button->pressed && !u.buttons);
    assert(last()->type == DU_EV_FOCUS && last()->index == 0);
    du_window_focus(&u, 1);
    hold(DU_KEY_LSHIFT, 1); input(CIUKI_INPUT_RESYNC, 0, 0, 0); assert(!u.modifiers && !u.keys[DU_KEY_LSHIFT]);

    /* Menus: mouse, keyboard, mnemonics, accelerators, disabled items. */
    static struct du_menu_item file_items[] = {{"&Open", 10, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
        {"&Delete", 11, DU_KEY_DELETE, 0, DU_ITEM_DISABLED}, {"&Refresh", 12, DU_KEY_F5, 0, 0}, {"&Copy", 13, 0x2e, DU_CTRL, DU_ITEM_CHECKED}};
    static struct du_menu_item view_items[] = {{"&List", 20, 0, 0, DU_ITEM_RADIO | DU_ITEM_CHECKED}, {"&Details", 21, 0, 0, DU_ITEM_RADIO}};
    static struct du_menu menus[] = {{"&File", file_items, 5}, {"&View", view_items, 2}};
    u.menus = menus; u.menu_count = 2;
    struct du_rect t0 = du_menu_title(&u, 0), t1 = du_menu_title(&u, 1);
    assert(t0.x == 2 && t0.w == du_label_width("&File") + 16 && t1.x == t0.x + t0.w && t0.h == 18);
    click(t0.x + 5, 8, 1); assert(u.menu == 0);
    struct du_rect box = du_menu_box(&u, file_items, 5, t0.x, t0.y + t0.h);
    assert(box.y == 19 && box.h == 6 + DU_MENU_ITEM_H * 4 + DU_MENU_SEP_H);
    assert(du_menu_item_at(file_items, 5, box, box.y + 3 + DU_MENU_ITEM_H + 4) == -1 && du_menu_item_at(file_items, 5, box, box.y + 3 + DU_MENU_ITEM_H + DU_MENU_SEP_H + 2) == 2);
    frame("menu");
    before = count; click(box.x + 30, box.y + 3 + DU_MENU_ITEM_H + DU_MENU_SEP_H + 5, 1); assert(count == before && u.menu == 0);   /* disabled: stays open */
    click(box.x + 30, box.y + 3 + 8, 1); assert(last()->type == DU_EV_ACTIVATE && last()->id == 10 && u.menu < 0);
    key(DU_KEY_F10); assert(u.menu == 0 && u.menu_item == 0);
    key(DU_KEY_DOWN); assert(u.menu_item == 2); key(DU_KEY_RIGHT); assert(u.menu == 1 && u.menu_item == 0);
    key(DU_KEY_ESC); assert(u.menu < 0);
    hold(DU_KEY_LALT, 1); key(0x2f); hold(DU_KEY_LALT, 0); assert(u.menu == 1);
    key(0x20); assert(last()->id == 21 && u.menu < 0);           /* mnemonic D inside the menu */
    hold(DU_KEY_LALT, 1); hold(DU_KEY_LALT, 0); assert(u.menu == 0); key(DU_KEY_ESC);   /* Alt tap */
    du_focus(&u, 3);
    key(DU_KEY_F5); assert(last()->type == DU_EV_ACTIVATE && last()->id == 12 && last()->keyboard);
    before = count; key(DU_KEY_DELETE); assert(count == before + 1 && last()->type == DU_EV_KEY && last()->key == DU_KEY_DELETE);
    hold(DU_KEY_LCTRL, 1); key(0x2e); hold(DU_KEY_LCTRL, 0); assert(last()->id == 13);
    du_focus(&u, 2); du_set_text(edit, "abc"); edit->anchor = 0;
    before = count; hold(DU_KEY_LCTRL, 1); key(0x2e); hold(DU_KEY_LCTRL, 0); assert(!strcmp(du_clipboard_get(), "abc") && count == before);
    du_popup(&u, file_items, 5, 380, 290, 3);
    struct du_rect pbox = du_menu_box(&u, file_items, 5, 380, 290);
    assert(pbox.x + pbox.w == W && pbox.y + pbox.h == H);
    input(CIUKI_INPUT_MOTION, 0, pbox.x + 20, pbox.y + 3 + DU_MENU_ITEM_H * 3 + DU_MENU_SEP_H + 5);
    assert(u.menu_item == 4);
    frame("popup");
    input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    assert(last()->id == 13 && last()->index == 3 && !u.popup);
    du_popup(&u, file_items, 5, 100, 100, 3); before = count; click(5, 290, 1); assert(!u.popup && count == before);

    /* Message box: default/cancel, arrows, mnemonics, Esc, modal input. */
    static const char *const buttons[] = {"&Yes", "Yes to &All", "&No", "Cancel"};
    du_message(&u, 50, "Confirm File Replace", "This folder already contains\na file named 'a.txt'.\n---\nType:\tText Document",
               DU_DLG_ICON_QUESTION, buttons, 4, 0, 3);
    assert(du_dialog_id(&u) == 50 && u.dialog.focus == 0);
    frame("message");
    before = count; click(20, 40, 1); assert(count == before);      /* modal */
    key(DU_KEY_RIGHT); key(DU_KEY_RIGHT); assert(u.dialog.focus == 2);
    key(DU_KEY_ENTER); assert(last()->type == DU_EV_DIALOG && last()->id == 50 && last()->index == 2 && !du_dialog_id(&u));
    du_message(&u, 51, "Q", "Text", DU_DLG_ICON_WARNING, buttons, 4, 0, 3);
    key(0x1e); assert(last()->index == 1);
    du_message(&u, 52, "Q", "Text", DU_DLG_ICON_INFO, buttons, 4, 0, 3);
    key(DU_KEY_ESC); assert(last()->index == 3);
    du_message(&u, 53, "Q", "Text", DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
    struct du_rect ok = du_dialog_button(&u, 0); click(ok.x + 5, ok.y + 5, 1); assert(last()->id == 53 && last()->index == 0);

    /* Input box: typing replaces the selected initial text; Enter = OK. */
    du_input_box(&u, 60, "Rename", "Type a new name:", "old.txt");
    frame("input");
    key(0x31); key(0x12); key(0x11); key(DU_KEY_ENTER);
    assert(last()->id == 60 && last()->index == 0 && !strcmp(last()->text, "new"));

    /* File dialog with a deterministic lister: folders first, navigation. */
    assert(!du_file_dialog(&u, 70, 0, "Open", "/", "", lister, NULL));
    assert(u.dialog.entry_count == 3 && !strcmp(u.dialog.entries[0].name, "docs") && !strcmp(u.dialog.entries[1].name, "Alpha.cfg"));
    frame("open");
    key(DU_KEY_TAB);   /* name edit -> next focusable */
    u.dialog.focus = 4; key(DU_KEY_DOWN); assert(!strcmp(u.dialog.items[5].text, "Alpha.cfg"));
    key(DU_KEY_UP); key(DU_KEY_ENTER); assert(!strcmp(u.dialog.folder, "/docs") && u.dialog.entry_count == 1);
    struct du_rect up = u.dialog.items[2].box; click(up.x + 5, up.y + 5, 1); assert(!strcmp(u.dialog.folder, "/"));
    u.dialog.focus = 5; du_set_text(&u.dialog.items[5], "zeta.txt"); key(DU_KEY_ENTER);
    assert(last()->type == DU_EV_DIALOG && last()->id == 70 && !strcmp(last()->text, "/zeta.txt"));
    assert(!du_file_dialog(&u, 71, 1, NULL, "/missing", "new.txt", lister, NULL) && !strcmp(u.dialog.folder, "/"));
    key(DU_KEY_ESC); assert(last()->index == 1);

    /* Progress: updates keep the dialog; Cancel reports button 0. */
    du_progress(&u, 80, "Copying...", "Copying: a.txt\nFrom: /\nTo: /docs", 1, 4);
    du_progress(&u, 80, NULL, NULL, 2, 4); assert(u.dialog.items[1].value == 2);
    frame("progress");
    key(DU_KEY_ENTER); assert(last()->id == 80 && last()->index == 0);

    /* Timers: one-shot and repeating, ordered by the monotonic clock. */
    u.now = 100;
    assert(!du_timer_start(&u, 7, 10, 0) && !du_timer_start(&u, 8, 4, 1));
    assert(du_next_deadline(&u) == 104);
    before = count; du_tick(&u, 103); assert(count == before);
    du_tick(&u, 104); assert(last()->type == DU_EV_TICK && last()->id == 8);
    du_tick(&u, 110); assert(count == before + 3); du_tick(&u, 111); assert(count == before + 3);
    du_timer_stop(&u, 8); du_tick(&u, 200); assert(count == before + 3);

    /* Remaining widget kinds render deterministically. */
    du_add(&u, DU_SCROLLBAR, 90, (struct du_rect){300, 60, 16, 150}, "")->total = 50;
    du_get(&u, 90)->page = 10; du_get(&u, 90)->value = 20;
    struct du_widget *tree = du_add(&u, DU_TREE, 91, (struct du_rect){320, 60, 78, 150}, "");
    static struct du_item nodes[4] = {{{"Root"}, NULL, NULL, 0, 0, 0, 1, 1}, {{"Programs"}, NULL, NULL, 0, 0, 1, 0, 1},
                                      {{"system"}, NULL, NULL, 1, 0, 1, 1, 1}, {{"logs"}, NULL, NULL, 0, 0, 2, 0, 0}};
    tree->items = nodes; tree->count = 4; tree->active = 2;
    assert(du_tree_toggle_hit(tree, 322 + 9 + 16, 62 + 17 + 8) == 1 && du_tree_toggle_hit(tree, 322 + 9 + 32, 62 + 17 * 3 + 8) == -1);
    click(322 + 9 + 16, 62 + 17 + 8, 1); assert(last()->type == DU_EV_CHANGE && last()->index == 1);
    du_add(&u, DU_PROGRESS, 92, (struct du_rect){8, 215, 200, 16}, "")->total = 3;
    du_get(&u, 92)->value = 2;
    struct du_widget *status = du_add(&u, DU_STATUS, 93, (struct du_rect){0, 280, W, 20}, "12 object(s)\t1.50 KB\tSystem");
    status->panels[0] = 200; status->panels[1] = 100;
    du_add(&u, DU_HEADER, 94, (struct du_rect){210, 215, 90, 18}, "Folders");
    du_add(&u, DU_SEPARATOR, 95, (struct du_rect){0, 236, W, 2}, "");
    struct du_widget *tool = du_add(&u, DU_TOOLBUTTON, 96, (struct du_rect){8, 240, 56, 39}, "Views");
    tool->dropdown = 12;
    click(8 + 50, 250, 1); assert(last()->type == DU_EV_CHANGE && last()->id == 96 && last()->index == 1);
    struct du_widget *split = du_add(&u, DU_SPLITTER, 97, (struct du_rect){292, 60, 4, 150}, "");
    input(CIUKI_INPUT_MOTION, 0, 293, 100); input(CIUKI_INPUT_BUTTON, 1, 1, 0); input(CIUKI_INPUT_MOTION, 0, 250, 100); input(CIUKI_INPUT_BUTTON, 1, 0, 0);
    assert(last()->type == DU_EV_CHANGE && last()->id == 97 && last()->index == 249 && !split->drag);
    du_focus(&u, 3); list->active = 1; rows[1].selected = 1;
    for (int view = 0; view < 3; view++) {
        list->view = (enum du_view)view; list->scroll = list->hscroll = 0; du_invalidate(&u,list->box);
        char name[16]; snprintf(name, sizeof(name), "view%d", view); frame(name);
    }
    /* Resize preserves focus, text/selection, timers and dialogs; anchors
     * grow the body and move fixed-height footer controls. */
    list->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
    status->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    struct du_rect old_list = list->box, old_status = status->box;
    int old_focus = u.focus; unsigned old_active = list->active;
    du_input_box(&u, 101, "Resize", "Keep this text:", "Ciuki");
    uint32_t *large = calloc(600 * 440 + 1, 4); assert(large);
    large[600 * 440] = 0x5a5a5a5au;
    du_resize(&u, large, 600, 440);
    assert(list->box.w == old_list.w + 200 && list->box.h == old_list.h + 140);
    assert(status->box.y == old_status.y + 140 && status->box.w == 600 && status->box.h == 20);
    assert(u.focus == old_focus && list->active == old_active && rows[1].selected);
    assert(u.dialog.kind == DU_DLG_INPUT && !strcmp(u.dialog.items[2].text, "Ciuki"));
    du_render(&u); assert(large[600 * 440] == 0x5a5a5a5au);
    du_resize(&u, pixels, W, H);
    assert(!memcmp(&list->box, &old_list, sizeof(old_list)) && !memcmp(&status->box, &old_status, sizeof(old_status)));
    du_dialog_close(&u); free(large);
    test_wheel();
    free(pixels);
    rt_client_tests();
    puts("PASS deskui: geometry, hit-testing, edit/clipboard/UTF-8, list/tree/scroll/columns, focus/release, menus/accelerators/mnemonics, dialogs, timers, frames");
    return 0;
}
