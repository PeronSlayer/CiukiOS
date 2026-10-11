/* SPDX-License-Identifier: MIT */
#include "view.h"
#include <stdio.h>
#include <string.h>
static const char *const tab_labels[] = { "Applications", "Processes", "Performance" };
static const struct du_column app_columns[] = { { "Task", 70, 0 }, { "Status", 30, 0 } };
static const struct du_column proc_columns[] = {
    { "Image Name", 36, 0 }, { "PID", 9, 1 }, { "PPID", 9, 1 },
    { "Threads", 13, 1 }, { "Mem Usage", 20, 1 }, { "CPU", 13, 1 } };
static const struct du_menu_item file_items[] = {
    { "New Task (Run...)", M_NEW, 0, 0, 0 }, { "Exit Task Manager", M_EXIT, 0, 0, 0 } };
static const struct du_menu_item help_items[] = { { "About Task Manager", M_ABOUT, 0, 0, 0 } };
static const char *const speed_names[] = { "High", "Normal", "Low", "Paused" };
static const int page_widgets[TV_PAGES][14] = {
    { A_APPS, A_END_TASK, A_SWITCH, A_NEW, 0 },
    { A_PROCS, A_PROC_SCROLL, A_END_PROCESS, 0 },
    { A_CPU_GROUP, A_CPU_TEXT, A_CPU_BAR, A_HISTORY_GROUP, A_HISTORY, A_MEM_GROUP, A_MEM_TOTAL,
      A_MEM_FREE, A_MEM_USED, A_KERNEL_GROUP, A_KERNEL_HEAP, A_KERNEL_PEAK, A_KERNEL_POOL, 0 } };
static const int totals_widgets[] = { A_TOTALS_GROUP, A_TOTAL_PROCESSES, A_TOTAL_THREADS, A_TOTAL_UPTIME };
static struct du_widget *add(struct du_ui *u, enum du_kind kind, int id, int x, int y, int w, int h, const char *text)
{ return du_add(u, kind, id, (struct du_rect){ x, y, w, h }, text); }
uint64_t tv_interval_ns(enum tv_speed s)
{ return s == TV_HIGH ? 500000000ull : s == TV_NORMAL ? 1000000000ull : s == TV_LOW ? 4000000000ull : 0; }
void tv_speed(struct tasks_view *v, enum tv_speed s)
{
    v->speed = s;
    for (unsigned i = 0; i < 4; i++) {
        snprintf(v->view_labels[i], sizeof(v->view_labels[i]), "%s Update Speed: %s", (unsigned)s == i ? "*" : " ", speed_names[i]);
        v->view_items[i + 1] = (struct du_menu_item){ v->view_labels[i], M_HIGH + (int)i, 0, 0, 0 };
    }
    if (v->ui) v->ui->dirty = 1;
}
void tv_init(struct tasks_view *v, struct du_ui *u, const uint32_t *app_icon)
{
    memset(v, 0, sizeof(*v));
    v->ui = u; v->app_icon = app_icon;
    v->view_items[0] = (struct du_menu_item){ "Refresh Now", M_REFRESH, 0x3f, 0, 0 };
    tv_speed(v, TV_NORMAL);
    v->menus[0] = (struct du_menu){ "File", file_items, 2 };
    v->menus[1] = (struct du_menu){ "View", v->view_items, 5 };
    v->menus[2] = (struct du_menu){ "Help", help_items, 1 };
    u->menus = v->menus; u->menu_count = 3;
    struct du_widget *w = add(u, DU_TABS, A_TABS, 6, 28, 428, 24, "");
    w->labels = tab_labels; w->label_count = TV_PAGES;
    w = add(u, DU_LIST, A_APPS, 8, 58, 424, 334, "");
    w->columns = app_columns; w->column_count = 2; w->sort_column = -1; w->cells = v->app_cells; w->rows = v->app_rows;
    w = add(u, DU_BUTTON, A_END_TASK, 150, 400, 90, 28, "End Task"); w->key = 0x12; w->key_modifiers = DU_ALT;
    w = add(u, DU_BUTTON, A_SWITCH, 246, 400, 90, 28, "Switch To"); w->key = 0x1f; w->key_modifiers = DU_ALT;
    w = add(u, DU_BUTTON, A_NEW, 342, 400, 90, 28, "New Task"); w->key = 0x31; w->key_modifiers = DU_ALT;
    w = add(u, DU_LIST, A_PROCS, 8, 58, 408, 334, "");
    w->columns = proc_columns; w->column_count = TM_COLUMNS; w->cells = v->proc_cells; w->rows = v->proc_rows;
    add(u, DU_SCROLLBAR, A_PROC_SCROLL, 418, 58, 14, 334, "");
    w = add(u, DU_BUTTON, A_END_PROCESS, 320, 400, 112, 28, "End Process"); w->key = 0x12; w->key_modifiers = DU_ALT;
    add(u, DU_GROUP, A_CPU_GROUP, 8, 58, 100, 112, "CPU Usage");
    add(u, DU_LABEL, A_CPU_TEXT, 30, 92, 64, 16, "");
    add(u, DU_PROGRESS, A_CPU_BAR, 16, 140, 84, 18, "")->total = 100;
    add(u, DU_GROUP, A_HISTORY_GROUP, 116, 58, 316, 112, "CPU Usage History");
    add(u, DU_GRAPH, A_HISTORY, 124, 76, 300, 86, "")->total = TM_HISTORY;
    add(u, DU_GROUP, A_MEM_GROUP, 8, 178, 208, 92, "Physical Memory (K)");
    add(u, DU_LABEL, A_MEM_TOTAL, 16, 198, 192, 16, "");
    add(u, DU_LABEL, A_MEM_FREE, 16, 220, 192, 16, "");
    add(u, DU_LABEL, A_MEM_USED, 16, 242, 192, 16, "");
    add(u, DU_GROUP, A_KERNEL_GROUP, 224, 178, 208, 92, "Kernel Memory (K)");
    add(u, DU_LABEL, A_KERNEL_HEAP, 232, 198, 192, 16, "");
    add(u, DU_LABEL, A_KERNEL_PEAK, 232, 220, 192, 16, "");
    add(u, DU_LABEL, A_KERNEL_POOL, 232, 242, 192, 16, "");
    add(u, DU_GROUP, A_TOTALS_GROUP, 8, 278, 424, 48, "Totals");
    add(u, DU_LABEL, A_TOTAL_PROCESSES, 16, 298, 136, 16, "");
    add(u, DU_LABEL, A_TOTAL_THREADS, 156, 298, 120, 16, "");
    add(u, DU_LABEL, A_TOTAL_UPTIME, 280, 298, 148, 16, "");
    add(u, DU_STATUS, A_STATUS, 2, TV_HEIGHT - 26, TV_WIDTH - 4, 24, "");
    tv_page(v, TV_APPS);
}
void tv_page(struct tasks_view *v, int page)
{
    if (page < 0 || page >= TV_PAGES) page = TV_APPS;
    v->page = page;
    for (int p = 0; p < TV_PAGES; p++)
        for (unsigned i = 0; page_widgets[p][i]; i++) du_get(v->ui, page_widgets[p][i])->hidden = p != page;
    for (unsigned i = 0; i < sizeof(totals_widgets) / sizeof(totals_widgets[0]); i++)
        du_get(v->ui, totals_widgets[i])->hidden = page != TV_PERFORMANCE;
    du_get(v->ui, A_TABS)->value = (unsigned)page;
    du_focus(v->ui, page == TV_APPS ? A_APPS : page == TV_PROCESSES ? A_PROCS : A_TABS);
}
static void plain_k(char *out, size_t size, uint64_t kib)
{
    char text[40]; tm_format_k(text, sizeof(text), kib);
    size_t n = strlen(text); if (n >= 2) text[n - 2] = 0;
    snprintf(out, size, "%s", text);
}
static void line(struct du_ui *u, int id, const char *label, const char *value)
{ snprintf(du_get(u, id)->text, DU_TEXT_MAX, "%-10s%12s", label, value); }
static unsigned window_index(const struct tm_model *m, uint32_t id)
{ for (unsigned i = 0; i < m->window_count; i++) if (m->windows[i].id == id) return i; return UINT32_MAX; }
static unsigned process_index(const struct tm_model *m, uint32_t pid)
{ for (unsigned i = 0; i < m->count; i++) if (m->processes[i].pid == pid) return i; return UINT32_MAX; }
static void keep_visible(struct du_widget *w)
{
    unsigned page = (unsigned)(w->box.h > 22 ? w->box.h - 22 : 20) / 20;
    if (!page) page = 1;
    if (w->active >= w->count) w->active = w->count ? w->count - 1 : 0;
    if (w->first > w->active) w->first = w->active;
    if (w->active >= w->first + page) w->first = w->active - page + 1;
    if (w->first + page > w->count) w->first = w->count > page ? w->count - page : 0;
}
void tv_select_window(struct tasks_view *v, const struct tm_model *m, uint32_t id)
{
    v->selected_window = window_index(m, id) == UINT32_MAX ? 0 : id;
    tv_update(v, m);
}
void tv_select_pid(struct tasks_view *v, const struct tm_model *m, uint32_t pid)
{
    v->selected_pid = process_index(m, pid) == UINT32_MAX ? 0 : pid;
    tv_update(v, m);
}
const struct tm_window *tv_window(const struct tasks_view *v, const struct tm_model *m)
{
    unsigned i = window_index(m, v->selected_window);
    return i == UINT32_MAX ? 0 : &m->windows[i];
}
void tv_update(struct tasks_view *v, const struct tm_model *m)
{
    struct du_ui *u = v->ui;
    struct du_widget *apps = du_get(u, A_APPS), *procs = du_get(u, A_PROCS);
    unsigned app_selected = window_index(m, v->selected_window);
    for (unsigned i = 0; i < m->window_count; i++) {
        const struct tm_window *w = &m->windows[i];
        v->app_rows[i] = (struct du_row){ w->title, "", "", "", v->app_icon, i == app_selected, 0, 0 };
        v->app_cells[i * 2] = w->title;
        v->app_cells[i * 2 + 1] = (w->flags & DESK_WINDOW_UNRESPONSIVE) ? "Not Responding" : "Running";
    }
    apps->count = m->window_count;
    if (app_selected != UINT32_MAX) apps->active = app_selected;
    else v->selected_window = 0;
    keep_visible(apps);
    unsigned proc_selected = process_index(m, v->selected_pid);
    for (unsigned i = 0; i < m->count; i++) {
        const struct tm_process *p = &m->processes[i];
        v->proc_rows[i] = (struct du_row){ p->cells[TM_NAME], "", "", "", 0, i == proc_selected, 0, 0 };
        for (unsigned c = 0; c < TM_COLUMNS; c++) v->proc_cells[i * TM_COLUMNS + c] = p->cells[c];
    }
    procs->count = m->count;
    procs->sort_column = m->sort; procs->sort_descending = m->descending;
    if (proc_selected != UINT32_MAX) procs->active = proc_selected;
    else v->selected_pid = 0;
    keep_visible(procs);
    struct du_widget *scroll = du_get(u, A_PROC_SCROLL);
    scroll->total = procs->count; scroll->page = (unsigned)(procs->box.h - 22) / 20; scroll->value = procs->first;
    du_get(u, A_END_TASK)->disabled = du_get(u, A_SWITCH)->disabled = !v->selected_window;
    du_get(u, A_END_PROCESS)->disabled = !v->selected_pid;
    snprintf(du_get(u, A_CPU_TEXT)->text, DU_TEXT_MAX, "%3u %%", m->cpu);
    du_get(u, A_CPU_BAR)->value = m->cpu;
    struct du_widget *history = du_get(u, A_HISTORY);
    history->samples = m->history; history->sample_count = m->history_count;
    const struct ciuki_proc_system *s = &m->system;
    char a[32], b[32], c[32];
    plain_k(a, sizeof(a), (uint64_t)s->usable_pages * 4);
    plain_k(b, sizeof(b), (uint64_t)s->free_pages * 4);
    plain_k(c, sizeof(c), (uint64_t)(s->usable_pages - s->free_pages) * 4);
    line(u, A_MEM_TOTAL, "Total", a); line(u, A_MEM_FREE, "Available", b); line(u, A_MEM_USED, "Used", c);
    plain_k(a, sizeof(a), s->kheap_bytes / 1024);
    plain_k(b, sizeof(b), s->kheap_high_water / 1024);
    plain_k(c, sizeof(c), (uint64_t)s->kheap_pages * 4);
    line(u, A_KERNEL_HEAP, "Heap", a); line(u, A_KERNEL_PEAK, "Peak", b); line(u, A_KERNEL_POOL, "Pool", c);
    snprintf(du_get(u, A_TOTAL_PROCESSES)->text, DU_TEXT_MAX, "Processes %u", s->processes);
    snprintf(du_get(u, A_TOTAL_THREADS)->text, DU_TEXT_MAX, "Threads %u", m->threads);
    tm_format_uptime(a, sizeof(a), s->total_ticks, s->tick_hz);
    snprintf(du_get(u, A_TOTAL_UPTIME)->text, DU_TEXT_MAX, "Up Time %s", a);
    snprintf(du_get(u, A_STATUS)->text, DU_TEXT_MAX, "Processes: %u   CPU Usage: %u%%   Mem: %uM / %uM%s",
             s->processes, m->cpu, (s->usable_pages - s->free_pages) / 256, s->usable_pages / 256,
             v->speed == TV_PAUSED ? "   Paused" : "");
    u->dirty = 1;
}
