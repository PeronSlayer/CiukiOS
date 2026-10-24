/* f2-33 Task Manager widgets on libdeskui: layout, hit tests, mnemonics,
 * tabs, sorting, dialogs and deterministic renders with fixed model data.
 * SPDX-License-Identifier: MIT */
#include "view.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct du_event events[64]; static unsigned event_count; static char dialog_text[DU_TEXT_MAX];
static void record(struct du_ui *u, const struct du_event *e, void *arg)
{
    (void)u; (void)arg;
    if (event_count < 64) events[event_count++] = *e;
    if (e->type == DU_EV_DIALOG) snprintf(dialog_text, sizeof(dialog_text), "%s", e->text ? e->text : "");
}
static void key(struct du_ui *u, int code, int value) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = code, .value = value }; du_input(u, &e); }
static void tap(struct du_ui *u, int code) { key(u, code, 1); key(u, code, 0); }
static void chord(struct du_ui *u, int modifier, int code) { key(u, modifier, 1); tap(u, code); key(u, modifier, 0); }
static void click(struct du_ui *u, int x, int y)
{
    struct ciuki_input_event e = { .type = CIUKI_INPUT_MOTION, .value = x, .value2 = y }; du_input(u, &e);
    e = (struct ciuki_input_event){ .type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1 }; du_input(u, &e);
    e.value = 0; du_input(u, &e);
}
static const struct du_event *last(void) { assert(event_count); return &events[event_count - 1]; }
static int fired(int id) { for (unsigned i = 0; i < event_count; i++) if (events[i].id == id && events[i].type == DU_EV_ACTIVATE) return 1; return 0; }
static void save(const char *folder, const char *name, const uint32_t *pixels)
{
    if (!folder) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/tasks-%s.xrgb", folder, name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(pixels, 4, TV_WIDTH * TV_HEIGHT, f) == TV_WIDTH * TV_HEIGHT); fclose(f);
}
static uint8_t buffer[TM_LIST_BYTES];
static void snapshot(struct tm_model *m, uint64_t total, uint64_t idle, const uint64_t cpu[5])
{
    static const char *names[] = { "supervisor", "desktop", "tasks", "lua", "demo" };
    static const uint32_t pids[] = { 1, 5, 7, 9, 13 }, ppids[] = { 0, 1, 5, 1, 5 }, groups[] = { 1, 5, 5, 9, 5 };
    static const uint32_t threads[] = { 1, 2, 1, 3, 1 }, pages[] = { 0, 412, 96, 1300, 60 };
    struct ciuki_proc_system s = { .version = 1, .size = 64, .tick_hz = 1000, .records = 5, .processes = 5,
        .total_ticks = total, .idle_ticks = idle, .usable_pages = 126976, .free_pages = 101234,
        .kheap_pages = 48, .kheap_bytes = 151552, .kheap_high_water = 180224 };
    memcpy(buffer, &s, sizeof(s));
    for (unsigned i = 0; i < 5; i++) {
        struct ciuki_proc_record r = { .version = 1, .size = 80, .pid = pids[i], .ppid = ppids[i], .pgid = groups[i],
            .state = CIUKI_PROC_LIVE, .threads = threads[i], .resident_pages = pages[i], .cpu_ticks = cpu[i], .start_tick = 100 };
        snprintf(r.name, sizeof(r.name), "%s", names[i]);
        memcpy(buffer + 64 + i * 80, &r, sizeof(r));
    }
    assert(tm_update(m, buffer, sizeof(buffer), 5) == 0);
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    uint32_t *pixels = calloc(TV_WIDTH * TV_HEIGHT + 2, 4); assert(pixels);
    pixels[TV_WIDTH * TV_HEIGHT] = 0x5a5a5a5au;
    static struct du_ui ui; static struct tasks_view v; static struct tm_model m;
    static uint32_t icon[256];
    for (unsigned i = 0; i < 256; i++) icon[i] = 0xff000000u | (i * 0x010203u);
    du_init(&ui, pixels, TV_WIDTH, TV_HEIGHT); ui.event = record;
    tm_init(&m); tv_init(&v, &ui, icon);
    for (int id = A_APPS; id <= A_PROCS; id++) {   /* headers must never clip (sort arrow = 14 px) */
        struct du_widget *lw = du_get(&ui, id);
        if (lw->column_count == 0) continue;
        for (unsigned c = 0; c < lw->column_count; c++)
            assert(du_text_width(lw->columns[c].title) + 14 <= lw->columns[c].width);
    }
    m.performance[0]=120; m.performance[1]=44880; m.performance[2]=240; m.performance[3]=179520;
    m.performance[4]=600000; m.performance[5]=1200000; m.performance[7]=2000000; m.performance[8]=2000000000;
    tv_update(&v,&m);
    assert(strstr(du_get(&ui,A_SYSTEM)->text,"60 fps") && strstr(du_get(&ui,A_SYSTEM)->text,"374 px"));
    assert(strstr(du_get(&ui,A_SYSTEM_IO)->text,"2000 us"));
    assert(ui.menu_count == 3 && !strcmp(ui.menus[1].label, "&View") && ui.menus[1].count == 6);
    assert(v.view_items[3].flags == (DU_ITEM_RADIO | DU_ITEM_CHECKED) && v.view_items[2].flags == DU_ITEM_RADIO && v.speed == TV_NORMAL);
    assert(tv_interval_ns(TV_HIGH) == 500000000ull && tv_interval_ns(TV_NORMAL) == 1000000000ull &&
           tv_interval_ns(TV_LOW) == 4000000000ull && !tv_interval_ns(TV_PAUSED));
    const uint64_t first[5] = { 0, 900, 150, 600, 50 }, second[5] = { 0, 1000, 210, 1600, 90 };
    snapshot(&m, 10000, 7000, first); snapshot(&m, 12000, 7600, second);
    uint64_t idle = 0;
    for (unsigned i = 0; i < 60; i++) {
        idle += 1000 - (uint64_t)((i * 37) % 100) * 10;
        struct ciuki_proc_system s = { .version = 1, .size = 64, .tick_hz = 1000, .total_ticks = (i + 1) * 1000,
            .idle_ticks = idle, .usable_pages = 1, .free_pages = 1 };
        assert(tm_sample(&m, &s, sizeof(s)) == 1);
    }
    tm_windows_begin(&m, 1);
    assert(tm_window_info(&m, 1, 0, 3, 1, 7, DESK_WINDOW_FOCUSED, "Task Manager") == 0);
    assert(tm_window_info(&m, 1, 1, 3, 2, 13, 0, "Ciuki Demo") == 0);
    assert(tm_window_info(&m, 1, 2, 3, 3, 21, DESK_WINDOW_UNRESPONSIVE, "Files") == 1);
    tv_update(&v, &m);
    struct du_widget *apps = du_get(&ui, A_APPS), *procs = du_get(&ui, A_PROCS), *tabs = du_get(&ui, A_TABS);
    assert(apps->count == 3 && !strcmp(apps->items[0].text[1], "Running") && !strcmp(apps->items[1].text[1], "Not Responding"));
    assert(du_get(&ui, A_END_TASK)->disabled && du_get(&ui, A_SWITCH)->disabled && du_focused(&ui) == A_APPS);
    /* Nothing selected: Alt+E (End Task) is inert. */
    event_count = 0; chord(&ui, DU_KEY_LALT, 0x12); assert(!fired(A_END_TASK) && ui.menu < 0);
    tv_select_window(&v, &m, 2);
    assert(apps->active == 0 && apps->items[0].selected && !du_get(&ui, A_END_TASK)->disabled && tv_window(&v, &m)->pid == 13);
    event_count = 0; chord(&ui, DU_KEY_LALT, 0x12); assert(fired(A_END_TASK));
    event_count = 0; chord(&ui, DU_KEY_LALT, 0x31); assert(fired(A_NEW));
    event_count = 0; chord(&ui, DU_KEY_LALT, 0x1f); assert(fired(A_SWITCH));
    /* Menu mnemonics come from the labels; F5 is Refresh Now. */
    key(&ui, DU_KEY_LALT, 1); tap(&ui, 0x2f); key(&ui, DU_KEY_LALT, 0); assert(ui.menu == 1); tap(&ui, DU_KEY_ESC); assert(ui.menu < 0);
    key(&ui, DU_KEY_LALT, 1); tap(&ui, 0x23); key(&ui, DU_KEY_LALT, 0); assert(ui.menu == 2); tap(&ui, DU_KEY_ESC);
    event_count = 0; tap(&ui, DU_KEY_F5); assert(fired(M_REFRESH));
    event_count = 0; tap(&ui, DU_KEY_DELETE); assert(last()->type == DU_EV_KEY && last()->key == DU_KEY_DELETE && last()->id == A_APPS);
    du_render(&ui); assert(pixels[TV_WIDTH * TV_HEIGHT] == 0x5a5a5a5au); save(folder, "applications", pixels);
    /* Tabs: strip hit test, then Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PgUp. */
    assert(du_tabs_hit(tabs, 10, 30) == 0 && du_tabs_hit(tabs, 120, 30) == 1 && du_tabs_hit(tabs, 120, 60) == -1);
    event_count = 0; click(&ui, 120, 30);
    assert(last()->type == DU_EV_SELECT && last()->id == A_TABS && last()->index == 1 && tabs->active == 1);
    tv_page(&v, 1); tv_update(&v, &m);
    assert(!procs->hidden && apps->hidden && du_get(&ui, A_END_TASK)->hidden && !du_get(&ui, A_END_PROCESS)->hidden);
    assert(procs->count == m.count && !strcmp(procs->items[1].text[0], "supervisor"));
    /* Header clicks name the column under the pointer. */
    event_count = 0; click(&ui, 130, 60); assert(last()->type == DU_EV_SORT && last()->index == TM_PID);
    click(&ui, 350, 60); assert(last()->type == DU_EV_SORT && last()->index == TM_CPU);
    tm_sort(&m, TM_CPU, 1); tv_select_pid(&v, &m, 9);
    assert(procs->sort == TM_CPU && procs->descending && procs->items[procs->active].selected && m.processes[procs->active].pid == 9);
    assert(!strcmp(procs->items[0].text[0], "lua") && !strcmp(procs->items[1].text[0], "System Idle Process"));
    event_count = 0; chord(&ui, DU_KEY_LALT, 0x12); assert(fired(A_END_PROCESS) && !fired(A_END_TASK));
    du_render(&ui); save(folder, "processes", pixels);
    event_count = 0; key(&ui, DU_KEY_LCTRL, 1); tap(&ui, DU_KEY_TAB); key(&ui, DU_KEY_LCTRL, 0);
    assert(last()->type == DU_EV_SELECT && last()->id == A_TABS && last()->index == 2);
    tv_page(&v, 2); tv_update(&v, &m);
    assert(du_get(&ui, A_HISTORY)->sample_count == TM_HISTORY && !du_get(&ui, A_HISTORY)->hidden && procs->hidden);
    assert(!strcmp(du_get(&ui, A_CPU_TEXT)->text, " 70 %") && strstr(du_get(&ui, A_MEM_TOTAL)->text, "507,904"));
    du_render(&ui); save(folder, "performance", pixels);
    key(&ui, DU_KEY_LCTRL, 1); key(&ui, DU_KEY_LSHIFT, 1); tap(&ui, DU_KEY_TAB); key(&ui, DU_KEY_LSHIFT, 0);
    assert(last()->index == 1); tap(&ui, DU_KEY_PGUP); assert(last()->index == 0); key(&ui, DU_KEY_LCTRL, 0);
    tv_page(&v, TV_APPS); tv_speed(&v, TV_PAUSED); tv_update(&v, &m);
    assert(strstr(du_get(&ui, A_STATUS)->text, "(paused)") && v.view_items[5].flags == (DU_ITEM_RADIO | DU_ITEM_CHECKED));
    /* New Task: typed set-1 keys, Enter reports the edit text. */
    du_input_box(&ui, D_NEW, "Create New Task", "Type the name of a program in /Programs, and CiukiOS\nwill open it for you.", "");
    tap(&ui, 0x20); tap(&ui, 0x12); tap(&ui, 0x32); tap(&ui, 0x18);
    du_render(&ui); save(folder, "newtask", pixels);
    event_count = 0; tap(&ui, DU_KEY_ENTER);
    assert(last()->type == DU_EV_DIALOG && last()->id == D_NEW && !last()->index && !strcmp(dialog_text, "demo") && !ui.dialog.kind);
    assert(pixels[TV_WIDTH * TV_HEIGHT] == 0x5a5a5a5au);
    uint32_t *large = calloc(600 * 540 + 1, 4); assert(large);
    large[600 * 540] = 0x5a5a5a5au;
    uint32_t selected = v.selected_window;
    du_resize(&ui, large, 600, 540);
    assert(v.selected_window == selected && v.page == TV_APPS && v.speed == TV_PAUSED);
    assert(du_get(&ui, A_APPS)->box.h == 420 && du_get(&ui, A_APPS)->box.w == 576);
    assert(du_get(&ui, A_NEW)->box.y == 478 && du_get(&ui, A_NEW)->box.h == 23);
    assert(du_get(&ui, A_STATUS)->box.y == 518 && du_get(&ui, A_STATUS)->box.w == 600);
    du_render(&ui); assert(large[600 * 540] == 0x5a5a5a5au);
    du_resize(&ui, pixels, TV_WIDTH, TV_HEIGHT); free(large);
    free(pixels);
    puts("PASS tasks view: tabs, Details columns/sort, selection identity, button mnemonics, menus, New Task dialog, deterministic renders");
    return 0;
}
