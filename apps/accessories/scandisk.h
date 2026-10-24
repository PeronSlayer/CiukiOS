/* SPDX-License-Identifier: MIT
 * G14 /Programs/scandisk: ScanDisk (docs/design/startup-recovery.md 2.4;
 * Windows 98 Resource Kit "Using ScanDisk"). */
#ifndef CIUKI_ACCESSORIES_SCANDISK_H
#define CIUKI_ACCESSORIES_SCANDISK_H
#include "acc.h"
#include <ciuki/abi.h>
#define SD_W 420
#define SD_H 318
enum { SD_DRIVES = 1, SD_LABEL, SD_TYPE, SD_STANDARD, SD_THOROUGH, SD_AUTOFIX, SD_SAVE_LOST, SD_COPY_CROSS,
       SD_START, SD_CLOSE, SD_STATUS, SD_PROGRESS, SD_RESULT, SD_TIMER, SD_M_ABOUT, SD_M_EXIT };
struct scandisk_app {
    struct du_ui *ui;
    struct du_item items[26]; char labels[26][24]; unsigned drives[26], count;
    unsigned pending;                      /* drives still to check, as a bit mask */
    struct ciuki_scandisk last;
    char report[DU_DIALOG_TEXT];
    struct du_menu_item file_items[1], help_items[1]; struct du_menu menus[2];
    int checking, test;
    void (*quit)(void *); void *host;      /* closes the window (main) */
};
/* drive: the drive to select first (0 = A:), or -1 for C:. */
void scandisk_attach(struct scandisk_app *, struct du_ui *, int drive);
void scandisk_event(struct du_ui *, const struct du_event *, void *);
/* Drive letter index of an absolute path ("/" C:, "/mnt/d/..." D:), or -1. */
int scandisk_drive_of(const char *path);
#endif
