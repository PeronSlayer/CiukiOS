/* SPDX-License-Identifier: MIT
 * ScanDisk window: drives, Standard or Thorough, Automatically fix errors,
 * Start; the check runs on a timer turn after the "Checking" frame is
 * painted, one drive per turn, and the ScanDisk Results text follows. */
#include "scandisk.h"
#include "../sysrecovery/recovery.h"
#include <ciuki/recovery.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int scandisk_drive_of(const char *path)
{
    if (!path || path[0] != '/') return -1;
    if (!strncmp(path, "/mnt/", 5) && path[5] >= 'a' && path[5] <= 'z' && (!path[6] || path[6] == '/')) return path[5] - 'a';
    if (!strncmp(path, "/mnt/", 5)) return -1;
    return 2;
}
static void drives(struct scandisk_app *a, int wanted)
{
    a->count = 0;
    for (unsigned d = 2; d < 26 && a->count < 26; d++) {
        char path[16]; struct stat st;
        if (d == 2) snprintf(path, sizeof(path), "/");
        else snprintf(path, sizeof(path), "/mnt/%c", 'a' + d);
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        unsigned i = a->count++;
        a->drives[i] = d;
        snprintf(a->labels[i], sizeof(a->labels[i]), "%s (%c:)", d == 2 ? "Local Disk" : "Drive", 'A' + d);
        memset(&a->items[i], 0, sizeof(a->items[i]));
        a->items[i].text[0] = a->labels[i];
        a->items[i].icon16 = du_icon_argb("drive-harddisk", 16);
        a->items[i].selected = (int)d == (wanted < 0 ? 2 : wanted);
    }
}
void scandisk_attach(struct scandisk_app *a, struct du_ui *u, int drive)
{
    a->ui = u; u->event = scandisk_event; u->user = a;
    a->file_items[0] = (struct du_menu_item){ "E&xit", SD_M_EXIT, 0, 0, 0 };
    a->help_items[0] = (struct du_menu_item){ "&About ScanDisk", SD_M_ABOUT, 0, 0, 0 };
    a->menus[0] = (struct du_menu){ "&File", a->file_items, 1 };
    a->menus[1] = (struct du_menu){ "&Help", a->help_items, 1 };
    u->menus = a->menus; u->menu_count = 2;
    drives(a, drive);
    du_add(u, DU_LABEL, SD_LABEL, (struct du_rect){12, 28, 396, 16}, "Select the drive(s) you want to check for errors:");
    struct du_widget *w = du_add(u, DU_LISTVIEW, SD_DRIVES, (struct du_rect){12, 48, 396, 90}, "No drive to check.");
    w->view = DU_LIST; w->items = a->items; w->count = a->count; w->selection_external = 1;
    for (unsigned i = 0; i < a->count; i++) if (a->items[i].selected) w->active = i;
    du_add(u, DU_GROUP, SD_TYPE, (struct du_rect){12, 146, 396, 70}, "Type of test");
    w = du_add(u, DU_RADIO, SD_STANDARD, (struct du_rect){24, 164, 370, 18}, "&Standard (checks files and folders for errors)");
    w->total = 1; w->value = 1;
    w = du_add(u, DU_RADIO, SD_THOROUGH, (struct du_rect){24, 186, 370, 18}, "&Thorough (also scans the disk surface for errors)");
    w->total = 1;
    du_add(u, DU_CHECKBOX, SD_AUTOFIX, (struct du_rect){12, 222, 396, 18}, "Automatically &fix errors")->value = 1;
    du_add(u, DU_CHECKBOX, SD_SAVE_LOST, (struct du_rect){12, 242, 196, 18}, "Save &lost fragments as files")->value = 1;
    du_add(u, DU_CHECKBOX, SD_COPY_CROSS, (struct du_rect){212, 242, 196, 18}, "&Copy cross-linked files")->value = 1;
    du_add(u, DU_BUTTON, SD_START, (struct du_rect){246, 266, 78, 23}, "&Start")->def = 1;
    du_add(u, DU_BUTTON, SD_CLOSE, (struct du_rect){330, 266, 78, 23}, "Close");
    du_add(u, DU_PROGRESS, SD_PROGRESS, (struct du_rect){12, 270, 226, 16}, "")->total = 1;
    du_add(u, DU_STATUS, SD_STATUS, (struct du_rect){0, SD_H - 20, SD_W, 20}, "Ready");
    u->focus = SD_START;
}
static unsigned selected_mask(struct scandisk_app *a)
{
    unsigned mask = 0;
    for (unsigned i = 0; i < a->count; i++) if (a->items[i].selected) mask |= 1u << i;
    if (!mask && a->count) mask = 1u << du_get(a->ui, SD_DRIVES)->active;
    return mask;
}
static void status(struct scandisk_app *a, const char *text)
{
    du_set_text(du_get(a->ui, SD_STATUS), text); du_invalidate_widget(a->ui, SD_STATUS);
}
static uint32_t flags(struct scandisk_app *a)
{
    struct du_ui *u = a->ui;
    return (du_get(u, SD_THOROUGH)->value ? CIUKI_SCANDISK_THOROUGH : 0) |
           (du_get(u, SD_AUTOFIX)->value ? CIUKI_SCANDISK_FIX : 0) |
           (du_get(u, SD_SAVE_LOST)->value ? 0 : CIUKI_SCANDISK_FREE_LOST) |
           (du_get(u, SD_COPY_CROSS)->value ? 0 : CIUKI_SCANDISK_NO_COPY);
}
static void enable(struct scandisk_app *a, int on)
{
    static const int ids[] = { SD_DRIVES, SD_STANDARD, SD_THOROUGH, SD_AUTOFIX, SD_SAVE_LOST, SD_COPY_CROSS, SD_START };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) { du_get(a->ui, ids[i])->disabled = !on; du_invalidate_widget(a->ui, ids[i]); }
}
/* One drive per timer turn, so each "Checking" status is on screen first. */
static void check_next(struct scandisk_app *a)
{
    unsigned i = 0;
    while (i < a->count && !(a->pending & (1u << i))) i++;
    if (i == a->count) {
        a->checking = 0; enable(a, 1); status(a, "Ready");
        du_get(a->ui, SD_PROGRESS)->value = 0; du_invalidate_widget(a->ui, SD_PROGRESS);
        return;
    }
    a->pending &= ~(1u << i);
    unsigned drive = a->drives[i];
    char title[48];
    snprintf(title, sizeof(title), "ScanDisk Results - (%c:)", 'A' + drive);
    if (ciuki_scandisk(drive, flags(a), &a->last)) {
        snprintf(a->report, sizeof(a->report), "ScanDisk could not check drive %c: (%s).", 'A' + drive, strerror(errno));
        du_message(a->ui, SD_RESULT, title, a->report, DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
        return;
    }
    (void)scandisk_log("", &a->last, (int64_t)time(NULL));
    size_t n = scandisk_text(&a->last, a->report, sizeof(a->report));
    if (a->last.flags & CIUKI_SCANDISK_SCHEDULED && n + 80 < sizeof(a->report))
        snprintf(a->report + n, sizeof(a->report) - n, "\nRestart CiukiOS (Start, Shut Down, Restart) to fix them now.");
    du_message(a->ui, SD_RESULT, title, a->report,
               a->last.flags & CIUKI_SCANDISK_ERRORS ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO, NULL, 0, 0, 0);
}
static void start(struct scandisk_app *a)
{
    if (a->checking || !a->count) return;
    a->pending = selected_mask(a);
    a->checking = 1; enable(a, 0);
    char text[64]; unsigned first = 0;
    while (first < a->count && !(a->pending & (1u << first))) first++;
    snprintf(text, sizeof(text), "Checking drive %c:...", 'A' + a->drives[first < a->count ? first : 0]);
    status(a, text);
    struct du_widget *p = du_get(a->ui, SD_PROGRESS); p->total = 1; p->value = 0; du_invalidate_widget(a->ui, SD_PROGRESS);
    du_timer_start(a->ui, SD_TIMER, 50000000ull, 0);
}
void scandisk_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct scandisk_app *a = user;
    switch (e->type) {
    case DU_EV_ACTIVATE:
        if (e->id == SD_START) start(a);
        else if ((e->id == SD_CLOSE || e->id == SD_M_EXIT) && a->quit) a->quit(a->host);
        else if (e->id == SD_M_ABOUT)
            du_message(u, 0, "About ScanDisk", "CiukiOS ScanDisk - Classic computing, one woof at a time.\n"
                       "Checks FAT drives: files and folders, lost fragments, cross-linked files and, with Thorough, "
                       "the disk surface.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    case DU_EV_SELECT:
        if (e->id == SD_DRIVES && e->index >= 0 && (unsigned)e->index < a->count) {
            if (!(e->modifiers & DU_CTRL)) for (unsigned i = 0; i < a->count; i++) a->items[i].selected = 0;
            a->items[e->index].selected = !a->items[e->index].selected || !(e->modifiers & DU_CTRL);
            du_invalidate_widget(u, SD_DRIVES);
        }
        break;
    case DU_EV_TICK:
        if (e->id == SD_TIMER && a->checking) {
            struct du_widget *p = du_get(u, SD_PROGRESS); p->value = 1; du_invalidate_widget(u, SD_PROGRESS);
            check_next(a);
        }
        break;
    case DU_EV_DIALOG:
        if (e->id == SD_RESULT && a->checking) {
            if (a->pending) {
                unsigned i = 0;
                while (i < a->count && !(a->pending & (1u << i))) i++;
                char text[64]; snprintf(text, sizeof(text), "Checking drive %c:...", 'A' + a->drives[i < a->count ? i : 0]);
                status(a, text);
                du_timer_start(u, SD_TIMER, 50000000ull, 0);
            } else check_next(a);
        }
        break;
    default: break;
    }
}
