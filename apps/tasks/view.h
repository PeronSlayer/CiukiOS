/* Task Manager widgets on libdeskui: built from the model, no syscalls
 * (host-rendered by tests/host/desktop/tasks_view_test.c).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TASKS_VIEW_H
#define CIUKI_TASKS_VIEW_H
#include "model.h"
#include "../libdeskui/deskui.h"
#define TV_WIDTH 400
#define TV_HEIGHT 420
enum tv_page { TV_APPS, TV_PROCESSES, TV_PERFORMANCE, TV_PAGES };
enum tv_speed { TV_HIGH, TV_NORMAL, TV_LOW, TV_PAUSED };
enum {
    A_TABS = 1, A_APPS, A_PROCS, A_END_TASK, A_SWITCH, A_NEW, A_END_PROCESS, A_STATUS,
    A_CPU_GROUP, A_CPU_TEXT, A_CPU_BAR, A_HISTORY_GROUP, A_HISTORY,
    A_MEM_GROUP, A_MEM_TOTAL, A_MEM_FREE, A_MEM_USED,
    A_KERNEL_GROUP, A_KERNEL_HEAP, A_KERNEL_PEAK, A_KERNEL_POOL,
    A_TOTALS_GROUP, A_TOTAL_PROCESSES, A_TOTAL_THREADS, A_TOTAL_UPTIME,
    A_SYSTEM, A_SYSTEM_TIME, A_SYSTEM_IO,
    M_NEW = 100, M_EXIT, M_REFRESH, M_HIGH, M_NORMAL, M_LOW, M_PAUSED, M_ABOUT, M_SHUTDOWN,
    D_NEW = 200, D_INFO, D_CONFIRM
};
struct tasks_view {
    struct du_ui *ui;
    int page;
    enum tv_speed speed;
    uint32_t selected_window, selected_pid;
    uint64_t app_hash, proc_hash, history_hash;
    struct du_item tab_items[TV_PAGES];
    struct du_item app_items[TM_WINDOWS];
    struct du_item proc_items[TM_PROCESSES];
    struct du_menu_item file_items[4], view_items[6], help_items[1];
    struct du_menu menus[3];
    const uint32_t *app_icon;
};
void tv_init(struct tasks_view *, struct du_ui *, const uint32_t *app_icon);
void tv_page(struct tasks_view *, int page);
void tv_speed(struct tasks_view *, enum tv_speed);
uint64_t tv_interval_ns(enum tv_speed);
void tv_update(struct tasks_view *, const struct tm_model *);
/* Selection by identity (refresh, new task); list events pass row indices. */
void tv_select_window(struct tasks_view *, const struct tm_model *, uint32_t id);
void tv_select_pid(struct tasks_view *, const struct tm_model *, uint32_t pid);
const struct tm_window *tv_window(const struct tasks_view *, const struct tm_model *);
#endif
