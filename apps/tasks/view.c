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
    v->file_items[1] = (struct du_menu_item){ "&Shut Down...", M_SHUTDOWN, 0, 0, 0 };   /* G3: the Close Program dialog's Shut Down */
    v->file_items[2] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    v->file_items[3] = (struct du_menu_item){ "E&xit Task Manager", M_EXIT, 0, 0, 0 };
    v->view_items[0] = (struct du_menu_item){ "&Refresh Now", M_REFRESH, DU_KEY_F5, 0, 0 };
    v->view_items[1] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    v->view_items[2] = (struct du_menu_item){ "Update Speed: &High", M_HIGH, 0, 0, DU_ITEM_RADIO };
    v->view_items[3] = (struct du_menu_item){ "Update Speed: &Normal", M_NORMAL, 0, 0, DU_ITEM_RADIO };
    v->view_items[4] = (struct du_menu_item){ "Update Speed: &Low", M_LOW, 0, 0, DU_ITEM_RADIO };
    v->view_items[5] = (struct du_menu_item){ "Update Speed: &Paused", M_PAUSED, 0, 0, DU_ITEM_RADIO };
    v->help_items[0] = (struct du_menu_item){ "&About Task Manager", M_ABOUT, 0, 0, 0 };
    v->menus[0] = (struct du_menu){ "&File", v->file_items, 4 };
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
    add(u, DU_LABEL, A_SYSTEM, 20, 290, 368, 16, "System: collecting display statistics...");
    add(u, DU_LABEL, A_SYSTEM_TIME, 20, 312, 368, 16, "");
    add(u, DU_LABEL, A_SYSTEM_IO, 20, 334, 368, 16, "");
    w = add(u, DU_STATUS, A_STATUS, 0, TV_HEIGHT - 22, TV_WIDTH, 22, "");
    w->panels[0] = 100; w->panels[1] = 110;
    /* Lists fill the page; buttons/status keep their height and bottom gap. */
    du_get(u, A_TABS)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
    du_get(u, A_APPS)->anchors = du_get(u, A_PROCS)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
    static const int buttons[] = {A_END_TASK, A_SWITCH, A_NEW, A_END_PROCESS};
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        du_get(u, buttons[i])->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    du_get(u, A_STATUS)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    du_get(u, A_HISTORY_GROUP)->anchors = du_get(u, A_HISTORY)->anchors = DU_ANCHOR_GROW_X;
    du_get(u, A_TOTALS_GROUP)->anchors = DU_ANCHOR_GROW_X;
    tv_page(v, TV_APPS);
}
void tv_page(struct tasks_view *v, int page)
{
    if (page < 0 || page >= TV_PAGES) page = TV_APPS;
    v->page = page;
    du_get(v->ui,A_SYSTEM)->hidden=du_get(v->ui,A_SYSTEM_TIME)->hidden=du_get(v->ui,A_SYSTEM_IO)->hidden=page!=TV_PERFORMANCE;
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
static void changed_text(struct du_ui *u, int id, const char *text)
{
    struct du_widget *w=du_get(u,id);
    if (strcmp(w->text,text)) { du_set_text(w,text); du_invalidate_widget(u,id); }
}
static void changed_value(struct du_ui *u, int id, int value, int disabled)
{
    struct du_widget *w=du_get(u,id);
    if (w->value!=value || w->disabled!=disabled) { w->value=value; w->disabled=(uint8_t)disabled; du_invalidate_widget(u,id); }
}
static uint64_t hash_bytes(uint64_t h, const void *data, size_t n)
{ const unsigned char *p=data; while(n--) h=(h^*p++)*1099511628211ull; return h; }
static uint64_t list_hash(const struct du_widget *w)
{
    uint64_t h=1469598103934665603ull;
    h=hash_bytes(h,&w->count,sizeof(w->count)); h=hash_bytes(h,&w->active,sizeof(w->active));
    h=hash_bytes(h,&w->sort,sizeof(w->sort)); h=hash_bytes(h,&w->descending,sizeof(w->descending));
    for (unsigned i=0;i<w->count;i++) {
        const struct du_item *it=&w->items[i];
        h=hash_bytes(h,&it->selected,sizeof(it->selected));
        for (unsigned c=0;c<w->column_count;c++) if (it->text[c]) h=hash_bytes(h,it->text[c],strlen(it->text[c])+1);
    }
    return h;
}
static void line(struct du_ui *u, int id, const char *label, const char *value)
{ char text[64]; snprintf(text, sizeof(text), "%-10s%14s", label, value); changed_text(u,id,text); }
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
    changed_value(u,A_END_TASK,0,!v->selected_window); changed_value(u,A_SWITCH,0,!v->selected_window);
    changed_value(u,A_END_PROCESS,0,!v->selected_pid);
    uint64_t ah=list_hash(apps),ph=list_hash(procs);
    if (ah!=v->app_hash) { v->app_hash=ah; du_invalidate_widget(u,A_APPS); }
    if (ph!=v->proc_hash) { v->proc_hash=ph; du_invalidate_widget(u,A_PROCS); }
    char a[32], b[32], c[32];
    snprintf(a, sizeof(a), "%3u %%", m->cpu); changed_text(u,A_CPU_TEXT,a);
    changed_value(u,A_CPU_BAR,(int)m->cpu,0);
    struct du_widget *history = du_get(u, A_HISTORY);
    history->samples=m->history; history->sample_count=m->history_count;
    uint64_t hh=hash_bytes(1469598103934665603ull,m->history,m->history_count*sizeof(*m->history));
    if (hh!=v->history_hash) { v->history_hash=hh; du_invalidate_widget(u,A_HISTORY); }
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
    snprintf(text, sizeof(text), "Processes  %u", s->processes); changed_text(u,A_TOTAL_PROCESSES,text);
    snprintf(text, sizeof(text), "Threads  %u", m->threads); changed_text(u,A_TOTAL_THREADS,text);
    tm_format_uptime(a, sizeof(a), s->total_ticks, s->tick_hz);
    snprintf(text, sizeof(text), "Up Time  %s", a); changed_text(u,A_TOTAL_UPTIME,text);
    const uint64_t *p=m->performance;
    if (p[8]) {
        snprintf(text,sizeof(text),"System: %llu fps  %llu px/frame  %llu presents",
                 (unsigned long long)(p[0]*1000000000ull/p[8]),
                 (unsigned long long)(p[1]/(p[0]?p[0]:1)),(unsigned long long)p[2]);
        changed_text(u,A_SYSTEM,text);
        snprintf(text,sizeof(text),"Composite %llu us/frame  Present %llu us/frame",
                 (unsigned long long)(p[4]/(p[0]?p[0]:1)/1000),
                 (unsigned long long)(p[5]/(p[0]?p[0]:1)/1000));
        changed_text(u,A_SYSTEM_TIME,text);
        snprintf(text,sizeof(text),"LFB %llu KiB  Input %llu us  Lag %llu us",
                 (unsigned long long)(p[3]/1024),(unsigned long long)(p[6]/1000),(unsigned long long)(p[7]/1000));
        changed_text(u,A_SYSTEM_IO,text);
    }
    snprintf(text, sizeof(text), v->speed == TV_PAUSED ? "Processes: %u\tCPU Usage: %u%%\tMem: %uM / %uM (paused)" :
             "Processes: %u\tCPU Usage: %u%%\tMem Usage: %uM / %uM", s->processes, m->cpu,
             (s->usable_pages - s->free_pages) / 256, s->usable_pages / 256);
    changed_text(u,A_STATUS,text);

}
