/* SPDX-License-Identifier: MIT
 * CiukiOS Log Viewer (G13, ER-5): a read-only viewer for the system's text
 * logs: the boot log, the NetSurf log, the Setup log and the crash files of
 * native programs and of Win32 programs (the Windows 98 Dr. Watson log
 * viewer's role, RK-tools Table 27.1). Large logs show their last 60 KB.
 * sec-sysvc: system logs for administrators, the user's own logs for
 * everyone (Windows XP Event Viewer model; logview_model.c). */
#ifndef CIUKI_ACC_LOGVIEW_H
#define CIUKI_ACC_LOGVIEW_H
#include "acc.h"
#include "../libdeskui/textarea.h"
#define LV_W 640
#define LV_H 400
#define LV_MAX 40
#define LV_TEXT_MAX DU_TA_LIMIT

enum lv_kind { LV_BOOT, LV_NETSURF, LV_SETUP, LV_CRASH, LV_SYSTEM };
/* sec-sysvc: where an entry lives. 0: a file (path); LV_MACHINE: the system
 * logs, read by administrators through the SYSTEM log service; LV_USER: the
 * user's own logs (ciuki/recovery.h, CIUKI_SYSTEM_LOG_* scopes). */
#define LV_MACHINE 1u
#define LV_USER 2u
#define LV_NAME 48
struct lv_entry { enum lv_kind kind; unsigned long sequence; unsigned scope; char title[64]; char name[LV_NAME]; char path[256]; };
struct lv_list { struct lv_entry entry[LV_MAX]; unsigned count; };
/* Boot, NetSurf and Setup logs first (always listed: a missing file shows what
 * writes it), then the crash files of native and Windows programs, newest sequence
 * first. root prefixes every path ("" on the system, a directory in tests). */
void lv_scan(struct lv_list *, const char *root);
/* The last bytes of a file, capacity-bounded, on a line boundary, with NUL
 * replaced by a space and CR LF by LF. 0 or -errno; *total is the file size. */
int lv_read_tail(const char *path, char *out, size_t capacity, size_t *length, unsigned long *total);
/* The same tail of an entry, from its file or through the log service. */
int lv_read_entry(const struct lv_entry *, char *out, size_t capacity, size_t *length, unsigned long *total);

struct lv_app {
    struct du_ui *ui; struct du_textarea ta;
    struct du_item items[LV_MAX]; struct lv_list list; int selected;
    char status[160];
    void (*quit)(void *host); void *host;
    struct du_menu menus[4]; struct du_menu_item file_items[4], edit_items[2], view_items[1], help_items[1];
};
enum { LV_M_REFRESH = 1, LV_M_EXIT, LV_M_COPY, LV_M_SELALL, LV_M_ABOUT, LV_M_WRAP, LV_LIST = 100, LV_TEXT, LV_STATUS, LV_D_INFO = 200 };
int lv_attach(struct lv_app *, struct du_ui *);
void lv_detach(struct lv_app *);
void lv_refresh(struct lv_app *);
void lv_show(struct lv_app *, int index);
void lv_event(struct du_ui *, const struct du_event *, void *);
void lv_layout(struct lv_app *);
extern const char *lv_root;   /* path prefix, "" unless a test sets it */
#endif
