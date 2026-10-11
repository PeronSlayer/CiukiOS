/* f2-33 Task Manager widgets: layout, hit tests, accelerators and
 * deterministic renders of every tab with fixed model data.
 * SPDX-License-Identifier: MIT */
#include "view.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct du_event events[64]; static unsigned event_count;
static void record(struct du_ui *u, const struct du_event *e, void *arg) { (void)u; (void)arg; if (event_count < 64) events[event_count++] = *e; }
static void key(struct du_ui *u, int code, int value) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = code, .value = value }; du_input(u, &e); }
static void click(struct du_ui *u, int x, int y)
{
    struct ciuki_input_event e = { .type = CIUKI_INPUT_MOTION, .value = x, .value2 = y }; du_input(u, &e);
    e = (struct ciuki_input_event){ .type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1 }; du_input(u, &e);
    e.value = 0; du_input(u, &e);
}
static const struct du_event *last(void) { assert(event_count); return &events[event_count - 1]; }
static void save(const char *prefix, const char *name, const uint32_t *pixels)
{
    if (!prefix) return;
    char path[1024]; snprintf(path, sizeof(path), "%s-%s.xrgb", prefix, name);
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
    const char *prefix = argc > 1 ? argv[1] : NULL;
    uint32_t *pixels = calloc(TV_WIDTH * TV_HEIGHT + 2, 4); assert(pixels);
    pixels[TV_WIDTH * TV_HEIGHT] = 0x5a5a5a5au;
    static struct du_ui ui; static struct tasks_view v; static struct tm_model m;
    static uint32_t icon[256];
    for (unsigned i = 0; i < 256; i++) icon[i] = 0xff000000u | (i * 0x010203u);
    du_init(&ui, pixels, TV_WIDTH, TV_HEIGHT); ui.event = record;
    tm_init(&m); tv_init(&v, &ui, icon);
    assert(ui.menu_count == 3 && !strcmp(ui.menus[1].label, "View") && ui.menus[1].count == 5);
    assert(!strcmp(v.view_items[2].label, "* Update Speed: Normal") && v.speed == TV_NORMAL);
    assert(tv_interval_ns(TV_HIGH) == 500000000ull && tv_interval_ns(TV_NORMAL) == 1000000000ull && tv_interval_ns(TV_LOW) == 4000000000ull && !tv_interval_ns(TV_PAUSED));
    const uint64_t first[5] = { 0, 900, 150, 600, 50 }, second[5] = { 0, 1000, 210, 1600, 90 };
    snapshot(&m, 10000, 7000, first); snapshot(&m, 12000, 7600, second);
    for (unsigned i = 0; i < 60; i++) {
        struct ciuki_proc_system s = { .version = 1, .size = 64, .tick_hz = 1000, .total_ticks = (i + 1) * 1000,
            .idle_ticks = 0, .usable_pages = 1, .free_pages = 1 };
        static uint64_t idle; idle += 1000 - (uint64_t)((i * 37) % 100) * 10; s.idle_ticks = idle;
        assert(tm_sample(&m, &s, sizeof(s)) == 1);
    }
    tm_windows_begin(&m, 1);
    assert(tm_window_info(&m, 1, 0, 3, 1, 7, DESK_WINDOW_FOCUSED, "Task Manager") == 0);
    assert(tm_window_info(&m, 1, 1, 3, 2, 13, 0, "Ciuki Demo") == 0);
    assert(tm_window_info(&m, 1, 2, 3, 3, 21, DESK_WINDOW_UNRESPONSIVE, "Files") == 1);
    tv_update(&v, &m);
    struct du_widget *apps = du_get(&ui, A_APPS), *procs = du_get(&ui, A_PROCS);
    assert(apps->count == 3 && !strcmp(v.app_cells[1], "Running") && !strcmp(v.app_cells[3], "Not Responding"));
    assert(du_get(&ui, A_END_TASK)->disabled && du_get(&ui, A_SWITCH)->disabled);
    /* Nothing selected: the End Task accelerator is inert. */
    event_count = 0; key(&ui, 0x38, 1); key(&ui, 0x12, 1); key(&ui, 0x12, 0); key(&ui, 0x38, 0);
    for (unsigned i = 0; i < event_count; i++) assert(events[i].id != A_END_TASK);
    tv_select_window(&v, &m, 2);
    assert(apps->active == 0 && v.app_rows[0].selected && !du_get(&ui, A_END_TASK)->disabled && tv_window(&v, &m)->pid == 13);
    event_count = 0; key(&ui, 0x38, 1); key(&ui, 0x12, 1); assert(last()->type == DU_ACTIVATE && last()->id == A_END_TASK);
    key(&ui, 0x12, 0); key(&ui, 0x38, 0);
    key(&ui, 0x38, 1); key(&ui, 0x31, 1); assert(last()->id == A_NEW); key(&ui, 0x31, 0); key(&ui, 0x38, 0);
    key(&ui, 0x38, 1); key(&ui, 0x1f, 1); assert(last()->id == A_SWITCH); key(&ui, 0x1f, 0); key(&ui, 0x38, 0);
    /* Alt+V opens View (label mnemonic), Alt+H opens Help; F5 is Refresh. */
    key(&ui, 0x38, 1); key(&ui, 0x2f, 1); assert(ui.menu == 1); key(&ui, 0x2f, 0); key(&ui, 0x38, 0); key(&ui, 1, 1); key(&ui, 1, 0);
    key(&ui, 0x38, 1); key(&ui, 0x23, 1); assert(ui.menu == 2); key(&ui, 0x23, 0); key(&ui, 0x38, 0); key(&ui, 1, 1); key(&ui, 1, 0);
    key(&ui, 0x3f, 1); assert(last()->id == M_REFRESH); key(&ui, 0x3f, 0);
    du_render(&ui); assert(pixels[TV_WIDTH * TV_HEIGHT] == 0x5a5a5a5au); save(prefix, "applications", pixels);
    /* Tabs: click hit-test, Ctrl+Tab, Ctrl+Shift+Tab. */
    event_count = 0; click(&ui, 6 + 2 + 116 + 10, 36);
    assert(last()->type == DU_SELECT && last()->id == A_TABS && last()->index == 1);
    tv_page(&v, last()->index); tv_update(&v, &m);
    assert(!procs->hidden && apps->hidden && du_get(&ui, A_END_TASK)->hidden && !du_get(&ui, A_END_PROCESS)->hidden);
    assert(procs->count == m.count && !strcmp(v.proc_cells[1 * TM_COLUMNS + TM_NAME], "supervisor"));
    /* Header click sorts by the column under the pointer (PID column ~30%). */
    event_count = 0; click(&ui, 8 + 408 * 40 / 100, 66); assert(last()->type == DU_SORT && last()->index == TM_PID);
    click(&ui, 8 + 408 * 92 / 100, 66); assert(last()->type == DU_SORT && last()->index == TM_CPU);
    tm_sort(&m, TM_CPU, 1); tv_select_pid(&v, &m, 9); tv_update(&v, &m);
    assert(procs->sort_column == TM_CPU && procs->sort_descending && v.proc_rows[procs->active].selected && m.processes[procs->active].pid == 9);
    assert(!du_get(&ui, A_END_PROCESS)->disabled && !strcmp(v.proc_cells[0], "lua") && !strcmp(v.proc_cells[TM_COLUMNS], "System Idle Process"));
    event_count = 0; key(&ui, 0x38, 1); key(&ui, 0x12, 1); assert(last()->id == A_END_PROCESS); key(&ui, 0x12, 0); key(&ui, 0x38, 0);
    du_render(&ui); save(prefix, "processes", pixels);
    key(&ui, 0x1d, 1); key(&ui, 0x0f, 1); assert(last()->type == DU_SELECT && last()->index == 2); key(&ui, 0x0f, 0);
    tv_page(&v, 2); tv_update(&v, &m);
    assert(du_get(&ui, A_HISTORY)->sample_count == TM_HISTORY && !du_get(&ui, A_HISTORY)->hidden && procs->hidden);
    assert(!strcmp(du_get(&ui, A_CPU_TEXT)->text, " 70 %"));
    assert(!strcmp(du_get(&ui, A_MEM_TOTAL)->text, "Total          507,904"));
    du_render(&ui); save(prefix, "performance", pixels);
    key(&ui, 0x2a, 1); key(&ui, 0x0f, 1); assert(last()->index == 1); key(&ui, 0x0f, 0); key(&ui, 0x2a, 0); key(&ui, 0x1d, 0);
    tv_page(&v, TV_APPS); tv_speed(&v, TV_PAUSED); tv_update(&v, &m);
    assert(strstr(du_get(&ui, A_STATUS)->text, "Paused") && !strcmp(v.view_items[4].label, "* Update Speed: Paused"));
    du_dialog_show(&ui, DU_INPUT, D_NEW, "Create New Task", "Type the name of a program in /bin, and\nCiukiOS will open it for you.\nOpen:", "demo");
    du_render(&ui); save(prefix, "newtask", pixels);
    assert(pixels[TV_WIDTH * TV_HEIGHT] == 0x5a5a5a5au);
    free(pixels);
    puts("PASS tasks view: tabs, Details columns/sort, selection identity, accelerators, label mnemonics, deterministic renders");
    return 0;
}
