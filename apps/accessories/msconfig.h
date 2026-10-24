/* SPDX-License-Identifier: MIT
 * G14 /Programs/msconfig: System Configuration Utility (docs/design/
 * startup-recovery.md 2.7; Windows 98 Resource Kit Tables 27.6-27.7):
 * General (Normal, Diagnostic, Selective startup), Advanced (BOOT.CFG
 * options), Automatic Skip Driver and Tools (Registry Checker, System File
 * Checker). The Startup tab (Run keys, StartUp folder) is G21's. */
#ifndef CIUKI_ACCESSORIES_MSCONFIG_H
#define CIUKI_ACCESSORIES_MSCONFIG_H
#include "acc.h"
#include "../sysrecovery/recovery.h"
#include <ciuki/abi.h>
#define MC_W 480
#define MC_H 410
#define MC_CLASSES 7
enum { MC_NORMAL, MC_DIAGNOSTIC, MC_SELECTIVE };
struct msconfig_state {
    int mode;                       /* MC_NORMAL, MC_DIAGNOSTIC, MC_SELECTIVE */
    uint8_t enabled;                /* selective: classes that load (bootstat.h bits) */
    struct bootcfg_options advanced;
};
const char *msconfig_class_label(unsigned c);
/* The saved state: requests and disabled classes from call 99, BOOT.CFG. */
void msconfig_from_status(const struct ciuki_recovery_status *, const char *bootcfg, size_t n, struct msconfig_state *);
/* What SET stores: scheduled ScanDisk requests are kept. */
void msconfig_to_request(const struct msconfig_state *, uint8_t current_request, uint8_t *disabled, uint8_t *request);

enum { MC_TABS = 1, MC_OK, MC_CANCEL, MC_APPLY,
       MC_G_LABEL, MC_G_NORMAL, MC_G_DIAGNOSTIC, MC_G_SELECTIVE, MC_G_CLASS0,
       MC_A_MENU = MC_G_CLASS0 + MC_CLASSES, MC_A_NOSCAN, MC_A_QUIETSCAN, MC_A_SERIAL, MC_A_LABEL,
       MC_D_LABEL, MC_D_LIST, MC_D_ENABLE,
       MC_T_REG_GROUP, MC_T_REG_LIST, MC_T_REG_BACKUP, MC_T_REG_LABEL, MC_T_SFC_GROUP, MC_T_SFC_LIST,
       MC_T_SFC_SCAN, MC_T_SFC_UPDATE, MC_T_SFC_RESTORE,
       MC_STATUS, MC_DLG_RESTART, MC_DLG_SOURCE, MC_DLG_INFO, MC_M_ABOUT, MC_M_EXIT };
#define MC_PAGES 4
struct msconfig_app {
    struct du_ui *ui;
    struct ciuki_recovery_status status; int have_status;
    struct msconfig_state saved, edit;
    int page, dirty, close_after;
    struct du_item tabs[MC_PAGES];
    struct du_item asd_items[MC_CLASSES]; char asd_text[MC_CLASSES][3][48];
    struct rc_backup backups[RC_MAX_BACKUPS + 4]; int backup_count;
    struct du_item backup_items[RC_MAX_BACKUPS + 4]; char backup_text[RC_MAX_BACKUPS + 4][3][40];
    struct sfc_result sfc; struct du_item *sfc_items; char (*sfc_text)[2][160];
    struct du_menu_item file_items[1], help_items[1]; struct du_menu menus[2];
    char message[DU_DIALOG_TEXT];
    void (*quit)(void *); void *host;
};
void msconfig_attach(struct msconfig_app *, struct du_ui *);
void msconfig_event(struct du_ui *, const struct du_event *, void *);
#endif
