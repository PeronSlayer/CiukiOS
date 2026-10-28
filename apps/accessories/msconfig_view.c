/* SPDX-License-Identifier: MIT
 * System Configuration window. General and Advanced are stored by Apply/OK
 * (call 99 SET and BOOT.CFG) and take effect at the next start; Enable on
 * the Automatic Skip Driver page and the Tools act at once. */
#include "msconfig.h"
#include <ciuki/recovery.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const tab_labels[MC_PAGES] = { "General", "Advanced", "Automatic Skip Driver", "Tools" };
static const int page_widgets[MC_PAGES][16] = {
    { MC_G_LABEL, MC_G_NORMAL, MC_G_DIAGNOSTIC, MC_G_SELECTIVE, MC_G_CLASS0, MC_G_CLASS0 + 1, MC_G_CLASS0 + 2,
      MC_G_CLASS0 + 3, MC_G_CLASS0 + 4, MC_G_CLASS0 + 5, MC_G_CLASS0 + 6, 0 },
    { MC_A_MENU, MC_A_NOSCAN, MC_A_QUIETSCAN, MC_A_SERIAL, MC_A_LABEL, 0 },
    { MC_D_LABEL, MC_D_LIST, MC_D_ENABLE, 0 },
    { MC_T_REG_GROUP, MC_T_REG_LIST, MC_T_REG_BACKUP, MC_T_REG_LABEL, MC_T_SFC_GROUP, MC_T_SFC_LIST,
      MC_T_SFC_SCAN, MC_T_SFC_UPDATE, MC_T_SFC_RESTORE, 0 } };
static const struct du_column asd_columns[] = { { "Driver class", 250, 0 }, { "Failed starts", 90, 1 }, { "Status", 100, 0 } };
static const struct du_column backup_columns[] = { { "Backup", 90, 0 }, { "Date", 130, 0 }, { "Status", 80, 0 }, { "Comment", 136, 0 } };
static const struct du_column sfc_columns[] = { { "File", 340, 0 }, { "Status", 96, 0 } };

static struct du_widget *add(struct du_ui *u, enum du_kind k, int id, int x, int y, int w, int h, const char *text)
{ return du_add(u, k, id, (struct du_rect){x, y, w, h}, text); }
static void status(struct msconfig_app *a, const char *text)
{ du_set_text(du_get(a->ui, MC_STATUS), text); du_invalidate_widget(a->ui, MC_STATUS); }
static int read_bootcfg(char *out, size_t cap, size_t *n)
{
    FILE *f = fopen("/system/BOOT.CFG", "rb");
    *n = 0;
    if (!f) return -errno;
    *n = fread(out, 1, cap, f); fclose(f);
    return 0;
}

static void show_state(struct msconfig_app *a)
{
    struct du_ui *u = a->ui;
    du_get(u, MC_G_NORMAL)->value = a->edit.mode == MC_NORMAL;
    du_get(u, MC_G_DIAGNOSTIC)->value = a->edit.mode == MC_DIAGNOSTIC;
    du_get(u, MC_G_SELECTIVE)->value = a->edit.mode == MC_SELECTIVE;
    for (unsigned c = 0; c < MC_CLASSES; c++) {
        struct du_widget *w = du_get(u, MC_G_CLASS0 + (int)c);
        w->value = (a->edit.enabled >> c) & 1;
        w->disabled = a->edit.mode != MC_SELECTIVE || !a->have_status;
    }
    du_get(u, MC_A_MENU)->value = a->edit.advanced.menu;
    du_get(u, MC_A_NOSCAN)->value = a->edit.advanced.autoscan == 0;
    du_get(u, MC_A_QUIETSCAN)->value = a->edit.advanced.autoscan == 2;
    du_get(u, MC_A_QUIETSCAN)->disabled = a->edit.advanced.autoscan == 0;
    du_get(u, MC_A_SERIAL)->value = a->edit.advanced.serial;
    du_get(u, MC_APPLY)->disabled = !a->dirty;
    u->dirty = 1;
}
static void asd_refresh(struct msconfig_app *a)
{
    struct ciuki_recovery_status *s = &a->status;
    a->have_status = !ciuki_recovery_status(s) && (s->flags & CIUKI_RECOVERY_VALID);
    for (unsigned c = 0; c < MC_CLASSES; c++) {
        static const char *const names[MC_CLASSES] = { "Accelerated display driver", "Sound drivers", "Network drivers",
            "USB drivers", "Serial, parallel, floppy and game ports", "Power management", "Startup programs" };
        snprintf(a->asd_text[c][0], sizeof(a->asd_text[c][0]), "%s", names[c]);
        snprintf(a->asd_text[c][1], sizeof(a->asd_text[c][1]), "%u", a->have_status ? s->fail_count[c] : 0);
        int skipped = a->have_status && (s->skipped >> c) & 1, off = a->have_status && (s->disabled >> c) & 1;
        if (skipped) snprintf(a->asd_text[c][2], sizeof(a->asd_text[c][2]), "Skipped (start %u)", (unsigned)s->skipped_at[c]);
        else snprintf(a->asd_text[c][2], sizeof(a->asd_text[c][2]), "%s", off ? "Turned off" : "Loaded");
        memset(&a->asd_items[c], 0, sizeof(a->asd_items[c]));
        for (unsigned k = 0; k < 3; k++) a->asd_items[c].text[k] = a->asd_text[c][k];
    }
    struct du_widget *w = du_get(a->ui, MC_D_LIST);
    w->count = MC_CLASSES; du_invalidate_widget(a->ui, MC_D_LIST);
    du_set_text(du_get(a->ui, MC_D_LABEL), !a->have_status ?
        "This system drive has no boot status record: the Automatic Skip Driver agent is not available." :
        a->status.skipped ? "These driver classes stopped CiukiOS from starting twice and are skipped. Select one and click Enable to load it again." :
        "No driver class has stopped CiukiOS from starting. Classes that fail twice are listed here and skipped.");
}
static void backups_refresh(struct msconfig_app *a)
{
    a->backup_count = rc_list("", a->backups, RC_MAX_BACKUPS + 4);
    if (a->backup_count < 0) a->backup_count = 0;
    for (int i = 0; i < a->backup_count; i++) {
        time_t t = (time_t)a->backups[i].created; struct tm tm;
        snprintf(a->backup_text[i][0], 40, "%s", a->backups[i].name);
        if (localtime_r(&t, &tm)) strftime(a->backup_text[i][1], 40, "%Y-%m-%d %H:%M", &tm);
        else snprintf(a->backup_text[i][1], 40, "?");
        snprintf(a->backup_text[i][2], 40, "%s", !a->backups[i].valid ? "Damaged" : a->backups[i].started ? "Started" : "Not started");
        memset(&a->backup_items[i], 0, sizeof(a->backup_items[i]));
        for (unsigned k = 0; k < 3; k++) a->backup_items[i].text[k] = a->backup_text[i][k];
        a->backup_items[i].text[3] = a->backups[i].comment;
    }
    du_get(a->ui, MC_T_REG_LIST)->count = (unsigned)a->backup_count;
    du_invalidate_widget(a->ui, MC_T_REG_LIST);
}
static void sfc_show(struct msconfig_app *a)
{
    free(a->sfc_items); free(a->sfc_text); a->sfc_items = NULL; a->sfc_text = NULL;
    struct du_widget *w = du_get(a->ui, MC_T_SFC_LIST);
    w->count = 0;
    if (a->sfc.count) {
        a->sfc_items = calloc(a->sfc.count, sizeof(*a->sfc_items));
        a->sfc_text = calloc(a->sfc.count, sizeof(*a->sfc_text));
        if (a->sfc_items && a->sfc_text) {
            for (unsigned i = 0; i < a->sfc.count; i++) {
                snprintf(a->sfc_text[i][0], 160, "%s", a->sfc.problems[i].path);
                snprintf(a->sfc_text[i][1], 160, "%s", a->sfc.problems[i].state == SFC_DELETED ? "Deleted" : "Changed");
                a->sfc_items[i].text[0] = a->sfc_text[i][0]; a->sfc_items[i].text[1] = a->sfc_text[i][1];
            }
            w->items = a->sfc_items; w->count = a->sfc.count;
        }
    }
    du_get(a->ui, MC_T_SFC_UPDATE)->disabled = du_get(a->ui, MC_T_SFC_RESTORE)->disabled = !w->count;
    du_invalidate_widget(a->ui, MC_T_SFC_LIST);
    a->ui->dirty = 1;
}
static void page(struct msconfig_app *a, int p)
{
    if (p < 0 || p >= MC_PAGES) p = 0;
    a->page = p;
    for (int k = 0; k < MC_PAGES; k++)
        for (unsigned i = 0; page_widgets[k][i]; i++) du_get(a->ui, page_widgets[k][i])->hidden = k != p;
    du_get(a->ui, MC_TABS)->active = (unsigned)p;
    a->ui->dirty = 1;
}

void msconfig_attach(struct msconfig_app *a, struct du_ui *u)
{
    a->ui = u; u->event = msconfig_event; u->user = a;
    a->file_items[0] = (struct du_menu_item){ "E&xit", MC_M_EXIT, 0, 0, 0 };
    a->help_items[0] = (struct du_menu_item){ "&About System Configuration", MC_M_ABOUT, 0, 0, 0 };
    a->menus[0] = (struct du_menu){ "&File", a->file_items, 1 };
    a->menus[1] = (struct du_menu){ "&Help", a->help_items, 1 };
    u->menus = a->menus; u->menu_count = 2;
    for (unsigned i = 0; i < MC_PAGES; i++) a->tabs[i].text[0] = tab_labels[i];
    struct du_widget *w = add(u, DU_TABS, MC_TABS, 4, 22, MC_W - 8, 338, "");
    w->items = a->tabs; w->count = MC_PAGES;
    /* General */
    add(u, DU_LABEL, MC_G_LABEL, 16, 52, 448, 16, "Startup selection");
    w = add(u, DU_RADIO, MC_G_NORMAL, 20, 72, 440, 18, "&Normal startup - load all device drivers"); w->total = 1;
    w = add(u, DU_RADIO, MC_G_DIAGNOSTIC, 20, 94, 440, 18, "&Diagnostic startup - show the Startup menu at the next start"); w->total = 1;
    w = add(u, DU_RADIO, MC_G_SELECTIVE, 20, 116, 440, 18, "&Selective startup"); w->total = 1;
    for (unsigned c = 0; c < MC_CLASSES; c++) add(u, DU_CHECKBOX, MC_G_CLASS0 + (int)c, 40, 140 + 22 * (int)c, 420, 18, msconfig_class_label(c));
    /* Advanced */
    add(u, DU_CHECKBOX, MC_A_MENU, 20, 56, 440, 18, "&Enable Startup menu (shown at every start)");
    add(u, DU_CHECKBOX, MC_A_NOSCAN, 20, 80, 440, 18, "Disable Scan&Disk after bad shutdown");
    add(u, DU_CHECKBOX, MC_A_QUIETSCAN, 20, 104, 440, 18, "Run ScanDisk after bad shutdown &without asking");
    add(u, DU_CHECKBOX, MC_A_SERIAL, 20, 128, 440, 18, "Copy the boot log to the &serial port (COM1)");
    add(u, DU_LABEL, MC_A_LABEL, 20, 160, 440, 48,
        "These settings are kept in C:\\SYSTEM\\BOOT.CFG. Hold Ctrl or press F8 while the computer starts for the Startup menu.");
    /* Automatic Skip Driver */
    add(u, DU_LABEL, MC_D_LABEL, 16, 52, 448, 44, "");
    w = add(u, DU_LISTVIEW, MC_D_LIST, 16, 100, 448, 220, "");
    w->view = DU_DETAILS; w->items = a->asd_items; w->column_count = 3; memcpy(w->columns, asd_columns, sizeof(asd_columns));
    add(u, DU_BUTTON, MC_D_ENABLE, 384, 326, 80, 23, "&Enable");
    /* Tools */
    add(u, DU_GROUP, MC_T_REG_GROUP, 12, 50, 456, 150, "Registry Checker");
    w = add(u, DU_LISTVIEW, MC_T_REG_LIST, 20, 68, 440, 92, "No registry backup yet.");
    w->view = DU_DETAILS; w->items = a->backup_items; w->column_count = 4; memcpy(w->columns, backup_columns, sizeof(backup_columns));
    add(u, DU_BUTTON, MC_T_REG_BACKUP, 20, 168, 96, 23, "&Back Up Now");
    add(u, DU_LABEL, MC_T_REG_LABEL, 124, 166, 336, 30, "To restore a backup, type SCANREG /RESTORE in the Terminal, then restart.");
    add(u, DU_GROUP, MC_T_SFC_GROUP, 12, 204, 456, 150, "System File Checker");
    w = add(u, DU_LISTVIEW, MC_T_SFC_LIST, 20, 222, 440, 94, "Click Scan Files to check the system files.");
    w->view = DU_DETAILS; w->column_count = 2; memcpy(w->columns, sfc_columns, sizeof(sfc_columns));
    add(u, DU_BUTTON, MC_T_SFC_SCAN, 20, 322, 96, 23, "Scan &Files");
    add(u, DU_BUTTON, MC_T_SFC_UPDATE, 258, 322, 96, 23, "&Update")->disabled = 1;
    add(u, DU_BUTTON, MC_T_SFC_RESTORE, 364, 322, 96, 23, "&Restore...")->disabled = 1;
    /* Sheet buttons */
    add(u, DU_BUTTON, MC_OK, 230, 366, 78, 23, "OK")->def = 1;
    add(u, DU_BUTTON, MC_CANCEL, 314, 366, 78, 23, "Cancel");
    add(u, DU_BUTTON, MC_APPLY, 398, 366, 78, 23, "&Apply");
    add(u, DU_STATUS, MC_STATUS, 0, MC_H - 20, MC_W, 20, "");
    asd_refresh(a);
    char cfg[BOOTCFG_MAX + 1]; size_t n = 0;
    read_bootcfg(cfg, sizeof(cfg) - 1, &n);
    msconfig_from_status(a->have_status ? &a->status : NULL, cfg, n, &a->saved);
    a->edit = a->saved;
    backups_refresh(a);
    show_state(a);
    page(a, 0);
    status(a, a->have_status ? "Changes on General and Advanced apply at the next start." :
           "Selective and Diagnostic startup need the boot status record (reinstall from the Setup CD).");
    if (!a->have_status) { du_get(u, MC_G_DIAGNOSTIC)->disabled = du_get(u, MC_G_SELECTIVE)->disabled = 1; }
}

static int apply(struct msconfig_app *a)
{
    int e = 0;
    if (a->have_status && (a->edit.mode != a->saved.mode || a->edit.enabled != a->saved.enabled)) {
        uint8_t disabled, request;
        msconfig_to_request(&a->edit, a->status.request, &disabled, &request);
        if (ciuki_recovery_configure(disabled, request)) e = -errno;
    }
    if (!e && memcmp(&a->edit.advanced, &a->saved.advanced, sizeof(a->edit.advanced))) e = bootcfg_update("", &a->edit.advanced);
    if (e) {
        snprintf(a->message, sizeof(a->message), "The settings could not be saved: %s.", strerror(-e));
        du_message(a->ui, MC_DLG_INFO, "System Configuration", a->message, DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
        return e;
    }
    a->saved = a->edit; a->dirty = 0;
    asd_refresh(a); show_state(a);
    du_message(a->ui, MC_DLG_RESTART, "System Configuration",
               "You must restart your computer before the new settings will take effect.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    return 0;
}
static void changed(struct msconfig_app *a, int id, int value)
{
    if (id == MC_G_NORMAL && value) a->edit.mode = MC_NORMAL;
    else if (id == MC_G_DIAGNOSTIC && value) a->edit.mode = MC_DIAGNOSTIC;
    else if (id == MC_G_SELECTIVE && value) a->edit.mode = MC_SELECTIVE;
    else if (id >= MC_G_CLASS0 && id < MC_G_CLASS0 + MC_CLASSES) {
        uint8_t bit = (uint8_t)(1u << (id - MC_G_CLASS0));
        a->edit.enabled = value ? (uint8_t)(a->edit.enabled | bit) : (uint8_t)(a->edit.enabled & ~bit);
    } else if (id == MC_A_MENU) a->edit.advanced.menu = value;
    else if (id == MC_A_NOSCAN) a->edit.advanced.autoscan = value ? 0 : 1;
    else if (id == MC_A_QUIETSCAN) a->edit.advanced.autoscan = value ? 2 : 1;
    else if (id == MC_A_SERIAL) a->edit.advanced.serial = value;
    else return;
    a->dirty = memcmp(&a->edit, &a->saved, sizeof(a->edit)) != 0;
    show_state(a);
}
static void tool(struct msconfig_app *a, int id)
{
    struct du_ui *u = a->ui;
    if (id == MC_T_REG_BACKUP) {
        struct rc_backup made;
        int e = rc_backup("", "System Configuration", (int64_t)time(NULL),
                          a->have_status && (a->status.flags & CIUKI_RECOVERY_COMPLETE), &made);
        if (e == -EPERM) snprintf(a->message, sizeof(a->message), "Only administrators can back up the registry."); /* sec-sysvc2 */
        else if (e) snprintf(a->message, sizeof(a->message), "Registry Checker could not back up the registry: %s.", strerror(-e));
        else snprintf(a->message, sizeof(a->message), "The registry and configuration files were backed up to C:\\SYSTEM\\SYSBCKUP\\%s.", made.name);
        backups_refresh(a);
        du_message(u, MC_DLG_INFO, "Registry Checker", a->message, e ? DU_DLG_ICON_ERROR : DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    } else if (id == MC_T_SFC_SCAN) {
        sfc_free(&a->sfc);
        status(a, "System File Checker is scanning the system files...");
        int e = sfc_scan("", &a->sfc);
        if (!e) (void)sfc_log("", &a->sfc, (int64_t)time(NULL));
        sfc_show(a);
        if (e) snprintf(a->message, sizeof(a->message), "System File Checker could not read its baseline: %s.", strerror(-e));
        else snprintf(a->message, sizeof(a->message), "%u files checked: %u changed, %u deleted.", a->sfc.checked, a->sfc.changed, a->sfc.deleted);
        status(a, a->message);
    } else if (id == MC_T_SFC_UPDATE || id == MC_T_SFC_RESTORE) {
        struct du_widget *w = du_get(u, MC_T_SFC_LIST);
        if (w->active >= a->sfc.count) return;
        if (id == MC_T_SFC_RESTORE) { du_input_box(u, MC_DLG_SOURCE, "Restore File", "Restore from (folder holding the original files):", "/mnt/d"); return; }
        int e = sfc_update("", a->sfc.problems[w->active].path);
        snprintf(a->message, sizeof(a->message), e ? "The verification information could not be updated: %s." : "%s is now the verified version.",
                 e ? strerror(-e) : a->sfc.problems[w->active].path);
        status(a, a->message);
    }
}
void msconfig_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct msconfig_app *a = user;
    switch (e->type) {
    case DU_EV_SELECT:
        if (e->id == MC_TABS) page(a, e->index);
        break;
    case DU_EV_CHANGE: changed(a, e->id, e->index); break;
    case DU_EV_ACTIVATE:
        if (e->id == MC_OK) {
            if (!a->dirty) { if (a->quit) a->quit(a->host); }
            else if (!apply(a)) a->close_after = 1;   /* closes once the restart notice is read */
        }
        else if (e->id == MC_CANCEL || e->id == MC_M_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == MC_APPLY) { if (a->dirty) apply(a); }
        else if (e->id == MC_D_ENABLE) {
            unsigned c = du_get(u, MC_D_LIST)->active;
            if (c < MC_CLASSES && a->have_status && ((a->status.skipped >> c) & 1)) {
                int r = ciuki_recovery_enable((uint8_t)(1u << c));
                snprintf(a->message, sizeof(a->message), r ? "The driver class could not be enabled: %s." :
                         "%s will be loaded again at the next start.", r ? strerror(errno) : a->asd_text[c][0]);
                asd_refresh(a); status(a, a->message);
            }
        } else if (e->id == MC_M_ABOUT)
            du_message(u, 0, "About System Configuration", "CiukiOS System Configuration - Classic computing, one woof at a time.\n"
                       "Startup selection, BOOT.CFG options, the Automatic Skip Driver agent, Registry Checker and "
                       "System File Checker.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else tool(a, e->id);
        break;
    case DU_EV_DIALOG:
        if (e->id == MC_DLG_SOURCE && e->index == 0 && e->text && e->text[0] == '/') {
            struct du_widget *w = du_get(u, MC_T_SFC_LIST);
            if (w->active >= a->sfc.count) break;
            int r = sfc_restore("", a->sfc.problems[w->active].path, strcmp(e->text, "/") ? e->text : "");
            snprintf(a->message, sizeof(a->message), r == -EILSEQ ? "That file is not the original CiukiOS file." :
                     r ? "The file could not be restored: %s." : "The file was restored; the previous one is in C:\\SYSTEM\\HELPDESK\\SFC.",
                     strerror(-r));
            status(a, a->message);
        } else if (e->id == MC_DLG_RESTART && a->close_after && a->quit) a->quit(a->host);
        break;
    default: break;
    }
}
