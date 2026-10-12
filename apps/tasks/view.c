/* SPDX-License-Identifier: MIT */
#include "view.h"
#include <stdio.h>
#include <string.h>
static const char *const tab_labels[TV_PAGES] = { "Applications", "Processes", "Performance" };
static const struct du_column app_columns[] = { { "Task", 250, 0 }, { "Status", 106, 0 } };
static const struct du_column proc_columns[TM_COLUMNS] = {
    { "Image Name", 112, 0 }, { "PID", 40, 1 }, { "PPID", 40, 1 },
    { "Threads", 54, 1 }, { "Mem Usage", 70, 1 }, { "CPU", 40, 1 } };
static const int page_widgets[TV_PAGES][18] = {
    { A_APPS, A_END_TASK, A_SWITCH, A_NEW, 0 },
    { A_PROCS, A_END_PROCESS, 0 },
    { A_CPU_GROUP, A_CPU_TEXT, A_CPU_BAR, A_HISTORY_GROUP, A_HISTORY, A_MEM_GROUP, A_MEM_TOTAL, A_MEM_FREE,
      A_MEM_USED, A_KERNEL_GROUP, A_KERNEL_HEAP, A_KERNEL_PEAK, A_KERNEL_POOL, A_TOTALS_GROUP,
      A_TOTAL_PROCESSES, A_TOTAL_THREADS, A_TOTAL_UPTIME, 0 } };
static struct du_widget *add(struct du_ui *u, enum du_kind kind, int id, int x, int y, int w, int h, const char *text)
{ return du_add(u, kind, id, (struct du_rect){ x, y, w, h }, text); }
uint64_t tv_interval_ns(enum tv_speed s)
{ return s == TV_HIGH ? 500000000ull : s == TV_NORMAL ? 1000000000ull : s == TV_LOW ? 4000000000ull : 0; }
void tv_speed(struct tasks_view *v, enum tv_speed s)
{
    v->speed = s;
    for (unsigned i = 0; i < 4; i++) {
        unsigned flags = DU_ITEM_RADIO | ((unsigned)s == i ? DU_ITEM_CHECKED : 0);
        v->view_items[2 + i].flags = flags;
    }
    if (v->ui) v->ui->dirty = 1;
}
void tv_init(struct tasks_view *v, struct du_ui *u, const uint32_t *app_icon)
{
    memset(v, 0, sizeof(*v));
    v->ui = u; v->app_icon = app_icon;
    v->file_items[0] = (struct du_menu_item){ "&New Task (Run...)", M_NEW, 0, 0, 0 };
    v->file_items[1] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    v->file_items[2] = (struct du_menu_item){ "E&xit Task Manager", M_EXIT, 0, 0, 0 };
    v->view_items[0] = (struct du_menu_item){ "&Refresh Now", M_REFRESH, DU_KEY_F5, 0, 0 };
    v->view_items[1] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    v->view_items[2] = (struct du_menu_item){ "Update Speed: &High", M_HIGH, 0, 0, DU_ITEM_RADIO };
    v->view_items[3] = (struct du_menu_item){ "Update Speed: &Normal", M_NORMAL, 0, 0, DU_ITEM_RADIO };
    v->view_items[4] = (struct du_menu_item){ "Update Speed: &Low", M_LOW, 0, 0, DU_ITEM_RADIO };
    v->view_items[5] = (struct du_menu_item){ "Update Speed: &Paused", M_PAUSED, 0, 0, DU_ITEM_RADIO };
    v->help_items[0] = (struct du_menu_item){ "&About Task Manager", M_ABOUT, 0, 0, 0 };
    v->menus[0] = (struct du_menu){ "&File", v->file_items, 3 };
    v->menus[1] = (struct du_menu){ "&View", v->view_items, 6 };
    v->menus[2] = (struct du_menu){ "&Help", v->help_items, 1 };
    u->menus = v->menus; u->menu_count = 3;
    tv_speed(v, TV_NORMAL);
    for (unsigned i = 0; i < TV_PAGES; i++) v->tab_items[i].text[0] = tab_labels[i];
    struct du_widget *w = add(u, DU_TABS, A_TABS, 4, 22, TV_WIDTH - 8, TV_HEIGHT - 48, "");
    w->items = v->tab_items; w->count = TV_PAGES;
    w = add(u, DU_LISTVIEW, A_APPS, 12, 50, 376, 300, "There are no running tasks.");
    w->view = DU_DETAILS; w->items = v->app_items; w->column_count = 2;
    memcpy(w->columns, app_columns, sizeof(app_columns));
    add(u, DU_BUTTON, A_END_TASK, 148, 358, 75, 23, "&End Task");
    add(u, DU_BUTTON, A_SWITCH, 229, 358, 75, 23, "&Switch To");
    add(u, DU_BUTTON, A_NEW, 310, 358, 78, 23, "&New Task...");
    w = add(u, DU_LISTVIEW, A_PROCS, 12, 50, 376, 300, "");
    w->view = DU_DETAILS; w->items = v->proc_items; w->column_count = TM_COLUMNS;
    memcpy(w->columns, proc_columns, sizeof(proc_columns));
    add(u, DU_BUTTON, A_END_PROCESS, 300, 358, 88, 23, "&End Process");
    add(u, DU_GROUP, A_CPU_GROUP, 12, 48, 90, 96, "CPU Usage");
    add(u, DU_LABEL, A_CPU_TEXT, 22, 72, 70, 12, "");
    add(u, DU_PROGRESS, A_CPU_BAR, 22, 104, 70, 14, "")->total = 100;
    add(u, DU_GROUP, A_HISTORY_GROUP, 110, 48, 278, 96, "CPU Usage History");
    add(u, DU_GRAPH, A_HISTORY, 118, 62, 262, 74, "")->total = TM_HISTORY;
    add(u, DU_GROUP, A_MEM_GROUP, 12, 152, 184, 70, "Physical Memory (K)");
    add(u, DU_LABEL, A_MEM_TOTAL, 20, 168, 168, 12, "");
    add(u, DU_LABEL, A_MEM_FREE, 20, 184, 168, 12, "");
    add(u, DU_LABEL, A_MEM_USED, 20, 200, 168, 12, "");
    add(u, DU_GROUP, A_KERNEL_GROUP, 204, 152, 184, 70, "Kernel Memory (K)");
    add(u, DU_LABEL, A_KERNEL_HEAP, 212, 168, 168, 12, "");
    add(u, DU_LABEL, A_KERNEL_PEAK, 212, 184, 168, 12, "");
    add(u, DU_LABEL, A_KERNEL_POOL, 212, 200, 168, 12, "");
    add(u, DU_GROUP, A_TOTALS_GROUP, 12, 230, 376, 40, "Totals");
    add(u, DU_LABEL, A_TOTAL_PROCESSES, 20, 248, 110, 12, "");
    add(u, DU_LABEL, A_TOTAL_THREADS, 136, 248, 110, 12, "");
    add(u, DU_LABEL, A_TOTAL_UPTIME, 252, 248, 130, 12, "");
    w = add(u, DU_STATUS, A_STATUS, 0, TV_HEIGHT - 22, TV_WIDTH, 22, "");
    w->panels[0] = 100; w->panels[1] = 110;
    tv_page(v, TV_APPS);
}
void tv_page(struct tasks_view *v, int page)
{
    if (page < 0 || page >= TV_PAGES) page = TV_APPS;
    v->page = page;
    for (int p = 0; p < TV_PAGES; p++)
        for (unsigned i = 0; page_widgets[p][i]; i++) du_get(v->ui, page_widgets[p][i])->hidden = p != page;
    du_get(v->ui, A_TABS)->active = (unsigned)page;
    du_focus(v->ui, page == TV_APPS ? A_APPS : page == TV_PROCESSES ? A_PROCS : A_TABS);
    v->ui->dirty = 1;
}
static void plain_k(char *out, size_t size, uint64_t kib)
{
    char text[40]; tm_format_k(text, sizeof(text), kib);
    size_t n = strlen(text); if (n >= 2) text[n - 2] = 0;
    snprintf(out, size, "%s", text);
}
static void line(struct du_ui *u, int id, const char *label, const char *value)
{ char text[64]; snprintf(text, sizeof(text), "%-10s%14s", label, value); du_set_text(du_get(u, id), text); }
static unsigned window_index(const struct tm_model *m, uint32_t id)
{ for (unsigned i = 0; i < m->window_count; i++) if (id && m->windows[i].id == id) return i; return UINT32_MAX; }
static unsigned process_index(const struct tm_model *m, uint32_t pid)
{ for (unsigned i = 0; i < m->count; i++) if (m->processes[i].pid == pid) return i; return UINT32_MAX; }
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
        struct du_item *it = &v->app_items[i];
        memset(it, 0, sizeof(*it));
        it->text[0] = w->title;
        it->text[1] = (w->flags & DESK_WINDOW_UNRESPONSIVE) ? "Not Responding" : "Running";
        const struct tm_process *owner = tm_find(m, w->pid);   /* the window's /Programs program icon */
        it->icon16 = owner ? du_icon_argb(du_icon_file(owner->name, 0, 1), 16) : NULL;
        if (!it->icon16) it->icon16 = v->app_icon;
        it->selected = i == app_selected;
    }
    apps->count = m->window_count;
    if (app_selected != UINT32_MAX) { apps->active = app_selected; du_list_ensure(apps, app_selected); }
    else { v->selected_window = 0; if (apps->active >= apps->count) apps->active = 0; }
    unsigned proc_selected = process_index(m, v->selected_pid);
    for (unsigned i = 0; i < m->count; i++) {
        const struct tm_process *p = &m->processes[i];
        struct du_item *it = &v->proc_items[i];
        memset(it, 0, sizeof(*it));
        for (unsigned c = 0; c < TM_COLUMNS; c++) it->text[c] = p->cells[c];
        it->selected = i == proc_selected;
    }
    procs->count = m->count;
    procs->sort = m->sort; procs->descending = m->descending;
    if (proc_selected != UINT32_MAX) { procs->active = proc_selected; du_list_ensure(procs, proc_selected); }
    else { v->selected_pid = 0; if (procs->active >= procs->count) procs->active = 0; }
    int page = du_list_page(procs), last = (int)procs->count > page ? (int)procs->count - page : 0;
    if (procs->scroll > last) procs->scroll = last;
    if (procs->scroll < 0) procs->scroll = 0;
    du_get(u, A_END_TASK)->disabled = du_get(u, A_SWITCH)->disabled = !v->selected_window;
    du_get(u, A_END_PROCESS)->disabled = !v->selected_pid;
    char a[32], b[32], c[32];
    snprintf(a, sizeof(a), "%3u %%", m->cpu); du_set_text(du_get(u, A_CPU_TEXT), a);
    du_get(u, A_CPU_BAR)->value = (int)m->cpu;
    struct du_widget *history = du_get(u, A_HISTORY);
    history->samples = m->history; history->sample_count = m->history_count;
    const struct ciuki_proc_system *s = &m->system;
    plain_k(a, sizeof(a), (uint64_t)s->usable_pages * 4);
    plain_k(b, sizeof(b), (uint64_t)s->free_pages * 4);
    plain_k(c, sizeof(c), (uint64_t)(s->usable_pages - s->free_pages) * 4);
    line(u, A_MEM_TOTAL, "Total", a); line(u, A_MEM_FREE, "Available", b); line(u, A_MEM_USED, "Used", c);
    plain_k(a, sizeof(a), s->kheap_bytes / 1024);
    plain_k(b, sizeof(b), s->kheap_high_water / 1024);
    plain_k(c, sizeof(c), (uint64_t)s->kheap_pages * 4);
    line(u, A_KERNEL_HEAP, "Heap", a); line(u, A_KERNEL_PEAK, "Peak", b); line(u, A_KERNEL_POOL, "Pool", c);
    char text[96];
    snprintf(text, sizeof(text), "Processes  %u", s->processes); du_set_text(du_get(u, A_TOTAL_PROCESSES), text);
    snprintf(text, sizeof(text), "Threads  %u", m->threads); du_set_text(du_get(u, A_TOTAL_THREADS), text);
    tm_format_uptime(a, sizeof(a), s->total_ticks, s->tick_hz);
    snprintf(text, sizeof(text), "Up Time  %s", a); du_set_text(du_get(u, A_TOTAL_UPTIME), text);
    snprintf(text, sizeof(text), v->speed == TV_PAUSED ? "Processes: %u\tCPU Usage: %u%%\tMem: %uM / %uM (paused)" :
             "Processes: %u\tCPU Usage: %u%%\tMem Usage: %uM / %uM", s->processes, m->cpu,
             (s->usable_pages - s->free_pages) / 256, s->usable_pages / 256);
    du_set_text(du_get(u, A_STATUS), text);
    u->dirty = 1;
}
