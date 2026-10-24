/* System Information window (Windows 98 MSInfo model): tree of categories, list of items,
 * File/View/Tools menus. Built on the pure si_* model; every effect goes through the
 * injected host so tests render and drive the production code (sysinfo_view.c).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSINFO_VIEW_H
#define CIUKI_SYSINFO_VIEW_H
#include "systools.h"
#include "../libdeskui/deskui.h"
#define SIV_WIDTH 640
#define SIV_HEIGHT 480
#define SIV_MIN_WIDTH 480
#define SIV_MIN_HEIGHT 320
enum { SIV_TREE = 1, SIV_LIST, SIV_STATUS };
enum {
    SIM_SAVE = 100, SIM_EXIT, SIM_REFRESH, SIM_HWREPORT, SIM_SYSMON, SIM_RESMETER, SIM_TASKS, SIM_CONTROL,
    SID_SAVE = 200, SID_MESSAGE
};
struct sysinfo_host {
    void *ctx;
    /* Fills the (zeroed, versioned by the callee) inventory and the memory/process figures. 0 or -errno. */
    int (*collect)(void *ctx, struct ciuki_devices_info *devices, struct si_input *input);
    /* Asks the desktop to start a catalogue program by name; the serial (>0) is answered by siv_launch_result, or -errno. */
    int (*launch)(void *ctx, const char *program);
    int (*write_file)(void *ctx, const char *path, const char *text, size_t length);   /* 0 or -errno */
    void (*quit)(void *ctx);
};
struct sysinfo_app {
    struct du_ui *ui;
    struct sysinfo_host host;
    struct ciuki_devices_info *devices;       /* caller-owned, heap (45 KiB) */
    struct si_tree *tree;                     /* caller-owned, heap (30 KiB) */
    struct si_input input;
    int selected;                             /* node shown in the list */
    uint32_t open_hash[SI_NODES]; unsigned open_count;   /* expanded categories, by label path */
    int visible[SI_NODES]; unsigned visible_count;
    int list_node[SI_NODES]; unsigned list_count;
    struct du_item tree_items[SI_NODES], list_items[SI_NODES];
    char summary[SI_NODES][32];
    struct du_menu_item file_items[4], view_items[1], tools_items[5];
    struct du_menu menus[3];
    const uint32_t *folder_icon, *warning_icon, *error_icon, *info_icon;
    char folder[DU_TEXT_MAX];                 /* Save As starting folder */
    char message[96];
    uint32_t launch_serial; char launch_label[32];
};
/* Builds the widgets and the first tree. `devices` and `tree` must outlive the app. */
int siv_init(struct sysinfo_app *, struct du_ui *, const struct sysinfo_host *, struct ciuki_devices_info *devices, struct si_tree *tree);
/* View > Refresh: collects again and rebuilds, keeping the selection and the expanded categories. */
void siv_refresh(struct sysinfo_app *);
void siv_event(struct du_ui *, const struct du_event *, void *app);
void siv_layout(struct du_ui *, void *app);
/* TASK_RESULT of a launch started through host.launch. */
void siv_launch_result(struct sysinfo_app *, uint32_t serial, int result);
/* Node (index into the si_tree) of the visible tree row, or -1. */
int siv_node_at(const struct sysinfo_app *, unsigned row);
#endif
