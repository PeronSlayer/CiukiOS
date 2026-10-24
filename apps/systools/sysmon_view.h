/* System Monitor window (Windows 98, Resource Kit ch. 26): Line, Bar and Numeric charts of kernel counters,
 * Add/Remove/Edit Item, Explain, update interval and logging to a CSV file. The model is sm_* (monitor.c);
 * effects go through the injected host so tests drive the production code. SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSMON_VIEW_H
#define CIUKI_SYSMON_VIEW_H
#include "systools.h"
#include "../libdeskui/deskui.h"
#define SMV_WIDTH 560
#define SMV_HEIGHT 420
#define SMV_MIN_WIDTH 420
#define SMV_MIN_HEIGHT 400
#define SMV_ROW_BYTES 1024
enum { SMV_CHART = 1, SMV_LEGEND, SMV_STATUS, SMV_TIMER = 1 };
enum {
    SMM_LOG_START = 100, SMM_LOG_STOP, SMM_EXIT, SMM_ADD, SMM_REMOVE, SMM_EDIT, SMM_EXPLAIN,
    SMM_LINE, SMM_BAR, SMM_NUMERIC, SMM_OPTIONS,
    SMD_LOG = 200, SMD_ADD, SMD_EDIT, SMD_INTERVAL, SMD_BAD_INTERVAL, SMD_MESSAGE
};
struct sysmon_host {
    void *ctx;
    int (*sample)(void *ctx, struct sm_raw *raw);                       /* 0 or -errno */
    int (*log_open)(void *ctx, const char *path);                       /* create/truncate; 0 or -errno */
    int (*log_write)(void *ctx, const char *text, size_t length);       /* 0 or -errno */
    void (*log_close)(void *ctx);
    void (*quit)(void *ctx);
};
struct sysmon_app {
    struct du_ui *ui; struct sysmon_host host;
    struct sm_monitor mon; struct sm_raw raw; int have_raw;
    int selected;                          /* legend row, -1 none */
    int edit_counter, bad_seconds;
    size_t log_capacity;                   /* row buffer size used for sm_sample (tests lower it) */
    char log_row[SMV_ROW_BYTES], log_path[DU_TEXT_MAX], folder[DU_TEXT_MAX], message[96], explain[400];
    struct du_item legend_items[SM_CHART_ITEMS];
    char cells[SM_CHART_ITEMS][3][64];
    uint32_t swatch[SM_CHART_ITEMS][256];
    struct du_item add_items[SM_COUNTERS], color_items[8];
    char add_cells[SM_COUNTERS][2][72];
    uint32_t color_swatch[8][256];
    struct du_menu_item file_items[4], edit_items[5], view_items[3], options_items[1];
    struct du_menu menus[4];
};
extern const uint32_t smv_palette[8];
extern const char *const smv_palette_names[8];
int smv_init(struct sysmon_app *, struct du_ui *, const struct sysmon_host *);
void smv_sample(struct sysmon_app *);                 /* one timer step: sample, log, repaint the chart */
void smv_stop_logging(struct sysmon_app *);
void smv_event(struct du_ui *, const struct du_event *, void *app);
void smv_layout(struct du_ui *, void *app);
void smv_paint(struct du_ui *, void *app);
#endif
