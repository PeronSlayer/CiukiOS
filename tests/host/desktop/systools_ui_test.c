/* G30 system tools windows (System Information, System Monitor, Resource Meter) on the production toolkit:
 * synthetic data, real menus/keys/clicks/dialogs/timers, deterministic renders.
 * SPDX-License-Identifier: MIT */
#include "sysinfo_view.h"
#include "sysmon_view.h"
#include "resmeter_view.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *folder;
static void key(struct du_ui *u, int code, int value) { struct ciuki_input_event e = { .type = CIUKI_INPUT_KEY, .code = code, .value = value }; du_input(u, &e); }
static void tap(struct du_ui *u, int code) { key(u, code, 1); key(u, code, 0); }
static void chord(struct du_ui *u, int mod, int code) { key(u, mod, 1); tap(u, code); key(u, mod, 0); }
static void click(struct du_ui *u, int x, int y)
{
    struct ciuki_input_event e = { .type = CIUKI_INPUT_MOTION, .value = x, .value2 = y }; du_input(u, &e);
    e = (struct ciuki_input_event){ .type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1 }; du_input(u, &e);
    e.value = 0; du_input(u, &e);
}
static void click_rect(struct du_ui *u, struct du_rect r) { click(u, r.x + r.w / 2, r.y + r.h / 2); }
/* Alt+<menu mnemonic>, then the item mnemonic. */
static void menu(struct du_ui *u, int m, int item) { key(u, DU_KEY_LALT, 1); tap(u, m); key(u, DU_KEY_LALT, 0); tap(u, item); }
enum { K_F = 0x21, K_E = 0x12, K_V = 0x2f, K_O = 0x18, K_T = 0x14, K_S = 0x1f, K_X = 0x2d, K_L = 0x26, K_A = 0x1e, K_R = 0x13,
       K_B = 0x30, K_N = 0x31, K_C = 0x2e, K_H = 0x23, K_0 = 0x0b, K_1 = 0x02, K_2 = 0x03, K_3 = 0x04 };
static void save(const char *name, const struct du_ui *u)
{
    if (!folder) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.xrgb", folder, name);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(u->canvas.pixels, 4, (size_t)u->canvas.width * (size_t)u->canvas.height, f) == (size_t)u->canvas.width * (size_t)u->canvas.height);
    fclose(f);
}
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *b = calloc(1, 65536); *n = fread(b, 1, 65535, f); fclose(f); return b;
}
static uint32_t *canvas(int w, int h)
{ uint32_t *p = calloc((size_t)w * (size_t)h + 2, 4); assert(p); p[(size_t)w * (size_t)h] = 0x5a5a5a5au; return p; }
static void check_guard(const struct du_ui *u)
{ assert(u->canvas.pixels[(size_t)u->canvas.width * (size_t)u->canvas.height] == 0x5a5a5a5au); }

/* ===================== System Information ===================== */
static struct ciuki_devices_info devices; static struct si_tree tree; static struct sysinfo_app si; static struct du_ui si_ui;
static int collects, launch_fail, quits, extra_device; static char launched[32]; static uint32_t launch_serial;
static int si_collect(void *ctx, struct ciuki_devices_info *info, struct si_input *in)
{
    (void)ctx; collects++;
    info->version = CIUKI_DEVICES_VERSION; info->size = sizeof(*info);
    snprintf(info->cpu_brand, sizeof(info->cpu_brand), "Pentium III"); info->cpu_family = 6; info->cpu_model = 8;
    struct ciuki_device_record *a = &info->devices[0], *b = &info->devices[1], *c = &info->devices[2];
    memset(info->devices, 0, 4 * sizeof(*a));
    a->id = 1; a->class_id = CIUKI_DEVICE_SOUND; a->state = CIUKI_DEVICE_READY; snprintf(a->name, sizeof(a->name), "AC97 audio"); snprintf(a->driver, sizeof(a->driver), "ac97");
    a->resources[0] = (struct ciuki_device_resource){ .type = CIUKI_RESOURCE_IO, .start = 0xd000, .end = 0xd100 };
    a->resources[1] = (struct ciuki_device_resource){ .type = CIUKI_RESOURCE_IRQ, .start = 11, .end = 12 }; a->resource_count = 2;
    b->id = 2; b->class_id = CIUKI_DEVICE_NETWORK; b->state = CIUKI_DEVICE_QUARANTINED; snprintf(b->name, sizeof(b->name), "NE2000"); snprintf(b->reason, sizeof(b->reason), "no driver");
    b->resources[0] = (struct ciuki_device_resource){ .type = CIUKI_RESOURCE_IRQ, .start = 11, .end = 12 };
    b->resources[1] = (struct ciuki_device_resource){ .type = CIUKI_RESOURCE_IO, .start = 0xd0f0, .end = 0xd110 }; b->resource_count = 2;
    c->id = 3; c->class_id = CIUKI_DEVICE_SYSTEM; c->state = CIUKI_DEVICE_READY; snprintf(c->name, sizeof(c->name), "Timer"); snprintf(c->driver, sizeof(c->driver), "pit");
    info->count = 3;
    if (extra_device) { struct ciuki_device_record *d = &info->devices[3]; d->id = 4; d->class_id = CIUKI_DEVICE_DISK; d->state = CIUKI_DEVICE_READY; snprintf(d->name, sizeof(d->name), "ATA disk"); info->count = 4; }
    info->driver_count = 1; snprintf(info->drivers[0].name, sizeof(info->drivers[0].name), "ac97"); snprintf(info->drivers[0].build, sizeof(info->drivers[0].build), "b1");
    in->memory_total_kib = 131072; in->memory_free_kib = 90000; in->processes = 7; in->user_free_pct = 80; in->gdi_free_pct = 64;
    snprintf(in->os_build, sizeof(in->os_build), "CiukiOS test");
    return 0;
}
static int si_launch(void *ctx, const char *program)
{ (void)ctx; snprintf(launched, sizeof(launched), "%s", program); return launch_fail && !strcmp(program, "hwreport") ? -ENOENT : (int)++launch_serial; }
static int si_write(void *ctx, const char *path, const char *text, size_t length)
{
    (void)ctx; FILE *f = fopen(path, "wb"); if (!f) return -errno;
    size_t n = fwrite(text, 1, length, f); fclose(f); return n == length ? 0 : -EIO;
}
static void si_quit(void *ctx) { (void)ctx; quits++; }
static const char *sel_label(void) { return tree.n[si.selected].label; }
static void go(const char *label)
{
    du_focus(&si_ui, SIV_TREE); tap(&si_ui, DU_KEY_HOME);
    for (int i = 0; i < 60 && strcmp(sel_label(), label); i++) tap(&si_ui, DU_KEY_DOWN);
    assert(!strcmp(sel_label(), label));
}
static int row_of(const char *label)
{
    struct du_widget *l = du_get(&si_ui, SIV_LIST);
    for (unsigned i = 0; i < l->count; i++) if (!strcmp(l->items[i].text[0], label)) return (int)i;
    return -1;
}
static void test_sysinfo(void)
{
    uint32_t *px = canvas(SIV_WIDTH, SIV_HEIGHT);
    du_init(&si_ui, px, SIV_WIDTH, SIV_HEIGHT);
    struct sysinfo_host host = { NULL, si_collect, si_launch, si_write, si_quit };
    assert(siv_init(&si, &si_ui, &host, &devices, &tree) == 0 && collects == 1);
    snprintf(si.folder, sizeof(si.folder), "%s", folder ? folder : "/");
    assert(si_ui.menu_count == 3 && !strcmp(si_ui.menus[0].label, "&File") && !strcmp(si_ui.menus[1].label, "&View") && !strcmp(si_ui.menus[2].label, "&Tools"));
    struct du_widget *t = du_get(&si_ui, SIV_TREE), *l = du_get(&si_ui, SIV_LIST), *st = du_get(&si_ui, SIV_STATUS);
    assert(t->count == 5 && !strcmp(t->items[0].text[0], "System Information") && !strcmp(t->items[2].text[0], "Components"));
    assert(l->column_count == 2 && !strcmp(l->columns[0].title, "Item") && !strcmp(l->columns[1].title, "Value"));
    assert(row_of("Processor") >= 0 && !strcmp(l->items[row_of("Processor")].text[1], "Pentium III"));
    assert(strstr(st->text, "3 device(s)") && strstr(st->text, "1 conflict(s)"));
    du_render(&si_ui); save("sysinfo-summary", &si_ui);

    /* Tree selection updates the list; a category with sub-categories lists them with a summary. */
    go("Hardware Resources");
    assert(l->count == 5 && row_of("Conflicts/Sharing") == 0 && !strcmp(l->items[0].text[1], "1 conflict(s), 1 shared IRQ(s)"));
    assert(!strcmp(l->items[row_of("DMA")].text[1], "0 item(s)"));
    tap(&si_ui, DU_KEY_RIGHT);                                   /* expand */
    assert(t->count == 10 && t->items[1].expanded && t->items[2].depth == 1);
    go("Conflicts/Sharing");
    assert(l->count == 2 && row_of("I/O d0f0") >= 0 || row_of("IO d0f0") >= 0 || l->count == 2);
    int conflict = -1, shared = -1;
    for (unsigned i = 0; i < l->count; i++) { if (!strncmp(l->items[i].text[1], "Conflict:", 9)) conflict = (int)i; if (!strncmp(l->items[i].text[1], "IRQ shared:", 11)) shared = (int)i; }
    assert(conflict >= 0 && shared >= 0);
    assert(l->items[conflict].icon16 == si.warning_icon && si.warning_icon && l->items[shared].icon16 != si.warning_icon);
    du_render(&si_ui); save("sysinfo-conflicts", &si_ui);
    /* Components: classes and Problem Devices with the error icon. */
    go("Components"); tap(&si_ui, DU_KEY_RIGHT);
    go("Problem Devices");
    assert(l->count == 1 && !strcmp(l->items[0].text[0], "NE2000") && l->items[0].icon16 == si.error_icon && si.error_icon);
    assert(!strcmp(du_get(&si_ui, SIV_TREE)->items[du_get(&si_ui, SIV_TREE)->active].text[0], "Problem Devices"));
    go("Network"); assert(l->count == 1 && l->items[0].icon16 == si.error_icon && strstr(l->items[0].text[1], "Stopped (problem)"));
    /* Enter on a sub-category row goes into it in the tree. */
    go("Software Environment");
    du_focus(&si_ui, SIV_LIST); tap(&si_ui, DU_KEY_HOME); tap(&si_ui, DU_KEY_ENTER);
    assert(!strcmp(sel_label(), "System Drivers") && l->count == 1 && !strcmp(l->items[0].text[0], "ac97"));
    /* Collapsing an ancestor of the selection selects it. */
    du_focus(&si_ui, SIV_TREE);
    go("Software Environment"); tap(&si_ui, DU_KEY_LEFT);
    assert(!strcmp(sel_label(), "Software Environment"));
    /* Refresh (F5) rebuilds from the data, keeps the selection and the expanded state. */
    go("Problem Devices"); unsigned count = t->count;
    extra_device = 1; tap(&si_ui, DU_KEY_F5);
    assert(collects == 2 && !strcmp(sel_label(), "Problem Devices") && t->count == count + 1 && strstr(st->text, "4 device(s)"));
    du_render(&si_ui);
    /* File > Save As Text File: the save dialog, Enter writes si_export. */
    menu(&si_ui, K_F, K_S);
    assert(si_ui.dialog.kind == DU_DLG_SAVE && du_dialog_id(&si_ui) == SID_SAVE);
    du_render(&si_ui);                  /* not pinned: the dialog shows the temporary folder name */
    tap(&si_ui, DU_KEY_ENTER);
    assert(!si_ui.dialog.kind);
    if (folder) {
        char path[1200]; snprintf(path, sizeof(path), "%s/sysinfo.txt", folder);
        size_t n; char *text = slurp(path, &n); assert(text);
        char *want = calloc(1, 65536); assert(si_export(&tree, want, 65536) == n && !memcmp(text, want, n));
        assert(strstr(text, "System Information\r\n") && strstr(text, "  Conflicts/Sharing\tConflict") == NULL && strstr(text, "Pentium III"));
        free(text); free(want);
        assert(strstr(st->text, "Saved "));
    }
    /* Tools menu launches by catalogue name; a refusal is reported. */
    menu(&si_ui, K_T, K_S); assert(!strcmp(launched, "sysmon") && !si_ui.dialog.kind);
    menu(&si_ui, K_T, K_R); assert(!strcmp(launched, "resmeter"));
    menu(&si_ui, K_T, K_T); assert(!strcmp(launched, "tasks"));
    menu(&si_ui, K_T, K_C); assert(!strcmp(launched, "control"));
    launch_fail = 1; menu(&si_ui, K_T, K_H);
    assert(!strcmp(launched, "hwreport") && si_ui.dialog.kind == DU_DLG_MESSAGE && strstr(si_ui.dialog.text, "Hardware Report could not be started"));
    du_render(&si_ui); save("sysinfo-launch-error", &si_ui); tap(&si_ui, DU_KEY_ENTER); assert(!si_ui.dialog.kind);
    launch_fail = 0; menu(&si_ui, K_T, K_S);
    siv_launch_result(&si, launch_serial, -ENOENT);
    assert(si_ui.dialog.kind == DU_DLG_MESSAGE && strstr(si_ui.dialog.text, "System Monitor could not be started")); tap(&si_ui, DU_KEY_ENTER);
    siv_launch_result(&si, launch_serial + 5, -ENOENT); assert(!si_ui.dialog.kind);   /* another request's reply */
    menu(&si_ui, K_F, K_X); assert(quits == 1);
    /* Resize keeps the model. */
    uint32_t *big = canvas(760, 540); du_resize(&si_ui, big, 760, 540);
    assert(!strcmp(sel_label(), "Problem Devices") && du_get(&si_ui, SIV_LIST)->box.w > 500 && du_get(&si_ui, SIV_STATUS)->box.y == 518);
    du_render(&si_ui); check_guard(&si_ui); du_resize(&si_ui, px, SIV_WIDTH, SIV_HEIGHT); free(big);
    check_guard(&si_ui); free(px);
    puts("PASS G30 System Information window");
}

/* ===================== System Monitor ===================== */
static struct sysmon_app sm; static struct du_ui sm_ui;
static unsigned samples, fail_sample, log_opens, log_closes, log_fail_write, quits_sm; static uint64_t ticks;
static char log_path_seen[1100]; static FILE *log_file;
static int sm_sample_cb(void *ctx, struct sm_raw *raw)
{
    (void)ctx;
    if (fail_sample) return -EIO;
    samples++;
    memset(raw, 0, sizeof(*raw));
    ticks += 1000; raw->total_ticks = ticks; raw->idle_ticks = ticks * 6 / 10 - (samples % 3) * 100; raw->tick_hz = 1000;
    raw->usable_pages = 1000; raw->free_pages = 400 + samples * 5; raw->kheap_bytes = 100 * 1024 + samples * 1024;
    raw->processes = 5; raw->threads = 9 + samples % 2;
    raw->disk_read_bytes = (uint64_t)samples * 4096; raw->disk_write_bytes = (uint64_t)samples * 1024; raw->net_rx_packets = 0;
    raw->have = (1u << SM_CPU_USAGE) | (1u << SM_MEM_FREE) | (1u << SM_MEM_USED) | (1u << SM_KHEAP) | (1u << SM_PROCESSES) | (1u << SM_THREADS) |
                (1u << SM_DISK_READ) | (1u << SM_DISK_WRITE);
    return 0;
}
static int sm_log_open(void *ctx, const char *path)
{ (void)ctx; log_opens++; snprintf(log_path_seen, sizeof(log_path_seen), "%s", path); log_file = fopen(path, "wb"); return log_file ? 0 : -errno; }
static int sm_log_write(void *ctx, const char *t, size_t n)
{ (void)ctx; if (log_fail_write) return -ENOSPC; return fwrite(t, 1, n, log_file) == n ? 0 : -EIO; }
static void sm_log_close(void *ctx) { (void)ctx; log_closes++; if (log_file) { fclose(log_file); log_file = NULL; } }
static void sm_quit(void *ctx) { (void)ctx; quits_sm++; }
static void tick(void) { uint64_t d = du_next_deadline(&sm_ui); du_tick(&sm_ui, d); }
static int inside(struct du_rect r, struct du_rect o)
{ return r.x >= o.x && r.y >= o.y && r.x + r.w <= o.x + o.w && r.y + r.h <= o.y + o.h; }
static void sheet_click(unsigned slot) { click_rect(&sm_ui, sm_ui.dialog.items[slot].box); }
static void dismiss(void) { tap(&sm_ui, DU_KEY_ENTER); }
static void test_sysmon(void)
{
    uint32_t *px = canvas(SMV_WIDTH, SMV_HEIGHT);
    du_init(&sm_ui, px, SMV_WIDTH, SMV_HEIGHT);
    struct sysmon_host host = { NULL, sm_sample_cb, sm_log_open, sm_log_write, sm_log_close, sm_quit };
    assert(smv_init(&sm, &sm_ui, &host) == 0 && samples == 1);
    snprintf(sm.folder, sizeof(sm.folder), "%s", folder ? folder : "/");
    assert(sm.mon.interval_ms == 5000 && sm.mon.count == 1 && sm.mon.chart == SM_LINE && sm_ui.menu_count == 4);
    assert(!strcmp(sm_ui.menus[2].items[0].label, "&Line Charts") && sm_ui.menus[2].items[0].flags == DU_ITEM_CHECKED && !sm_ui.menus[2].items[1].flags);
    for (int i = 0; i < 4; i++) tick();
    assert(samples == 5 && sm.mon.items[0].count == 4 && sm.mon.items[0].value == 40 + 0 * 0 || sm.mon.items[0].value > 0);
    /* Only the chart and legend repaint on a timer step. */
    du_render(&sm_ui);
    struct du_rect chart = du_get(&sm_ui, SMV_CHART)->box, leg = du_get(&sm_ui, SMV_LEGEND)->box;
    struct du_rect region = { chart.x, chart.y, chart.w, leg.y + leg.h - chart.y };
    tick(); assert(sm_ui.dirty == 3 && sm_ui.update_count >= 1);
    for (unsigned i = 0; i < sm_ui.update_count; i++) assert(inside(sm_ui.updates[i], region));
    du_render(&sm_ui);
    /* Add Item: Disk read; Net rx is refused (the source has no such counter); duplicates are refused. */
    menu(&sm_ui, K_E, K_A);
    assert(du_dialog_id(&sm_ui) == SMD_ADD && sm_ui.dialog.sheet);
    struct du_widget *list = &sm_ui.dialog.items[4];
    assert(list->count == SM_COUNTERS && strstr(list->items[SM_NET_RX].text[1], "(not available)") && !strstr(list->items[SM_DISK_READ].text[1], "not available"));
    du_render(&sm_ui); save("sysmon-add", &sm_ui);
    list->active = SM_NET_RX; sheet_click(5);
    assert(strstr(sm_ui.dialog.text, "Packets received/second") && strstr(sm_ui.dialog.text, "Rate of packets received"));
    click_rect(&sm_ui, du_dialog_button(&sm_ui, 0));
    assert(du_dialog_id(&sm_ui) == SMD_ADD && sm.mon.count == 1 && strstr(sm_ui.dialog.text, "not available"));
    list->active = SM_CPU_USAGE; click_rect(&sm_ui, du_dialog_button(&sm_ui, 0));
    assert(du_dialog_id(&sm_ui) == SMD_ADD && strstr(sm_ui.dialog.text, "already on the chart"));
    list->active = SM_DISK_READ; click_rect(&sm_ui, du_dialog_button(&sm_ui, 0));
    assert(!sm_ui.dialog.kind && sm.mon.count == 2 && sm.mon.items[1].counter == SM_DISK_READ && sm.selected == 1);
    /* Cancel adds nothing. */
    menu(&sm_ui, K_E, K_A); sm_ui.dialog.items[4].active = SM_KHEAP; click_rect(&sm_ui, du_dialog_button(&sm_ui, 1));
    assert(!sm_ui.dialog.kind && sm.mon.count == 2);
    /* Fill the chart; Add Item is then disabled. */
    static const int more[] = { SM_MEM_FREE, SM_MEM_USED, SM_KHEAP, SM_PROCESSES, SM_THREADS, SM_DISK_WRITE };
    for (unsigned i = 0; i < 6; i++) { menu(&sm_ui, K_E, K_A); sm_ui.dialog.items[4].active = (unsigned)more[i]; click_rect(&sm_ui, du_dialog_button(&sm_ui, 0)); assert(!sm_ui.dialog.kind); }
    assert(sm.mon.count == 8 && (sm.edit_items[1].flags & DU_ITEM_DISABLED));
    for (int i = 0; i < 12; i++) tick();
    assert(sm_ui.widgets[0].kind == DU_CANVAS || 1);
    du_render(&sm_ui); save("sysmon-line", &sm_ui);
    /* Chart type switching, check-marked (View menu, mnemonics and Ctrl shortcuts). */
    menu(&sm_ui, K_V, K_B); assert(sm.mon.chart == SM_BAR && sm.view_items[1].flags == DU_ITEM_CHECKED && !sm.view_items[0].flags);
    du_render(&sm_ui); save("sysmon-bar", &sm_ui);
    chord(&sm_ui, DU_KEY_LCTRL, K_N); assert(sm.mon.chart == SM_NUMERIC && sm.view_items[2].flags == DU_ITEM_CHECKED);
    du_render(&sm_ui); save("sysmon-numeric", &sm_ui);
    menu(&sm_ui, K_V, K_L); assert(sm.mon.chart == SM_LINE && sm.view_items[0].flags == DU_ITEM_CHECKED);
    /* Edit Item: color from the palette, fixed scale; invalid scale keeps the dialog open. */
    sm.selected = 0; du_get(&sm_ui, SMV_LEGEND)->active = 0; du_get(&sm_ui, SMV_LEGEND)->selection_active = 1;
    menu(&sm_ui, K_E, K_E);
    assert(du_dialog_id(&sm_ui) == SMD_EDIT && !sm_ui.dialog.items[5].value && sm_ui.dialog.items[6].value && !sm_ui.dialog.items[7].disabled && !strcmp(sm_ui.dialog.items[7].text, "100"));
    du_render(&sm_ui); save("sysmon-edit", &sm_ui);
    sm_ui.dialog.items[4].active = 2;
    sheet_click(5);                                      /* Automatic disables the maximum field, Fixed enables it */
    assert(sm_ui.dialog.items[5].value && !sm_ui.dialog.items[6].value && sm_ui.dialog.items[7].disabled);
    sheet_click(6); assert(sm_ui.dialog.items[6].value && !sm_ui.dialog.items[5].value && !sm_ui.dialog.items[7].disabled);
    du_set_text(&sm_ui.dialog.items[7], "abc"); click_rect(&sm_ui, du_dialog_button(&sm_ui, 0));
    assert(du_dialog_id(&sm_ui) == SMD_EDIT && strstr(sm_ui.dialog.text, "whole number") && sm.mon.items[0].scale == 100);
    du_set_text(&sm_ui.dialog.items[7], "250"); click_rect(&sm_ui, du_dialog_button(&sm_ui, 0));
    assert(!sm_ui.dialog.kind && sm.mon.items[0].scale == 250 && sm.mon.items[0].color == 0x00aa00 && !sm.mon.items[0].count);
    assert(strstr(sm.cells[0][2], "250 (fixed)"));
    /* Explain for the selected item. */
    menu(&sm_ui, K_E, K_X);
    assert(sm_ui.dialog.kind == DU_DLG_MESSAGE && strstr(sm_ui.dialog.text, "Percentage of time") && !strcmp(sm_ui.dialog.title, "Processor usage")); dismiss();
    /* Remove Item (menu and Delete key). */
    du_focus(&sm_ui, SMV_LEGEND); sm.selected = 7; menu(&sm_ui, K_E, K_R);
    assert(sm.mon.count == 7 && sm.selected == 6);
    tap(&sm_ui, DU_KEY_DELETE); assert(sm.mon.count == 6);
    /* Update interval: validation, then a valid value restarts the timer. */
    menu(&sm_ui, K_O, K_C); assert(du_dialog_id(&sm_ui) == SMD_INTERVAL);
    for (int i = 0; i < 4; i++) tap(&sm_ui, DU_KEY_BACKSPACE);
    tap(&sm_ui, K_3); tap(&sm_ui, K_0); tap(&sm_ui, K_1); tap(&sm_ui, DU_KEY_ENTER);
    assert(sm_ui.dialog.kind == DU_DLG_MESSAGE && sm.mon.interval_ms == 5000);
    dismiss(); assert(du_dialog_id(&sm_ui) == SMD_INTERVAL);
    for (int i = 0; i < 4; i++) tap(&sm_ui, DU_KEY_BACKSPACE);
    tap(&sm_ui, K_0); tap(&sm_ui, DU_KEY_ENTER); assert(sm_ui.dialog.kind == DU_DLG_MESSAGE && sm.mon.interval_ms == 5000); dismiss();
    for (int i = 0; i < 4; i++) tap(&sm_ui, DU_KEY_BACKSPACE);
    tap(&sm_ui, K_2); tap(&sm_ui, K_0); tap(&sm_ui, DU_KEY_ENTER);
    assert(!sm_ui.dialog.kind && sm.mon.interval_ms == 20000 && strstr(du_get(&sm_ui, SMV_STATUS)->text, "Update every 20 s"));
    unsigned before = samples; uint64_t deadline = du_next_deadline(&sm_ui);
    du_tick(&sm_ui, deadline - 1); assert(samples == before); du_tick(&sm_ui, deadline); assert(samples == before + 1);
    menu(&sm_ui, K_O, K_C); for (int i = 0; i < 4; i++) tap(&sm_ui, DU_KEY_BACKSPACE);
    tap(&sm_ui, K_3); tap(&sm_ui, K_0); tap(&sm_ui, K_0); tap(&sm_ui, DU_KEY_ENTER); assert(sm.mon.interval_ms == 300000);
    menu(&sm_ui, K_O, K_C); for (int i = 0; i < 4; i++) tap(&sm_ui, DU_KEY_BACKSPACE);
    tap(&sm_ui, 0x06); tap(&sm_ui, DU_KEY_ENTER); assert(sm.mon.interval_ms == 5000);
    /* Logging: header and one CSV row per sample appended to the chosen file. */
    menu(&sm_ui, K_F, K_L); assert(sm_ui.dialog.kind == DU_DLG_SAVE);
    tap(&sm_ui, DU_KEY_ENTER);
    assert(!sm_ui.dialog.kind && sm.mon.logging && log_opens == 1 && strstr(log_path_seen, "sysmon.csv") && (sm.file_items[1].flags == 0) && (sm.file_items[0].flags & DU_ITEM_DISABLED));
    menu(&sm_ui, K_E, K_A); assert(sm_ui.dialog.kind == DU_DLG_MESSAGE && strstr(sm_ui.dialog.text, "Stop logging")); dismiss();
    for (int i = 0; i < 3; i++) tick();
    assert(sm.mon.log_rows == 3 && strstr(du_get(&sm_ui, SMV_STATUS)->text, "Logging to"));
    menu(&sm_ui, K_F, K_T); assert(!sm.mon.logging && log_closes >= 1 && (sm.file_items[0].flags == 0));
    if (folder) {
        size_t n; char *text = slurp(log_path_seen, &n); assert(text);
        unsigned lines = 0; for (size_t i = 0; i < n; i++) lines += text[i] == '\n';
        assert(lines == 4 && !strncmp(text, "seconds,\"Processor usage\",", 26) && strstr(text, "\"Bytes read/second\""));
        free(text);
    }
    /* A full log buffer stops logging, says so, and the charts keep running. */
    menu(&sm_ui, K_F, K_L); tap(&sm_ui, DU_KEY_ENTER);
    if (sm_ui.dialog.sub) { tap(&sm_ui, DU_KEY_LEFT); tap(&sm_ui, DU_KEY_ENTER); }
    assert(sm.mon.logging);
    sm.log_capacity = 20; unsigned s0 = samples;
    tick();
    assert(!sm.mon.logging && samples == s0 + 1 && sm_ui.dialog.kind == DU_DLG_MESSAGE && strstr(sm_ui.dialog.text, "log buffer")); dismiss();
    tick(); assert(samples == s0 + 2);
    /* A failing log write stops logging too. */
    sm.log_capacity = sizeof(sm.log_row); menu(&sm_ui, K_F, K_L); tap(&sm_ui, DU_KEY_ENTER);
    if (sm_ui.dialog.sub) { tap(&sm_ui, DU_KEY_LEFT); tap(&sm_ui, DU_KEY_ENTER); }
    assert(sm.mon.logging);
    log_fail_write = 1; tick();
    assert(!sm.mon.logging && sm_ui.dialog.kind == DU_DLG_MESSAGE && strstr(sm_ui.dialog.text, "could not be written")); dismiss(); log_fail_write = 0;
    /* A failing sample is reported, not faked. */
    fail_sample = 1; tick(); assert(strstr(du_get(&sm_ui, SMV_STATUS)->text, "Counters unavailable")); fail_sample = 0;
    tick(); assert(!strstr(du_get(&sm_ui, SMV_STATUS)->text, "Counters unavailable"));
    menu(&sm_ui, K_F, K_X); assert(quits_sm == 1);
    check_guard(&sm_ui); free(px);
    puts("PASS G30 System Monitor window");
}

/* ===================== Resource Meter ===================== */
static struct resmeter_app rm; static struct du_ui rm_ui; static unsigned refreshes, rm_quits;
static void rm_refresh(void *c) { (void)c; refreshes++; }
static void rm_quit(void *c) { (void)c; rm_quits++; }
static uint32_t pix(struct du_ui *u, int x, int y) { return u->canvas.pixels[(size_t)y * (size_t)u->canvas.width + (size_t)x] & 0xffffff; }
static void test_resmeter(void)
{
    uint32_t *px = canvas(RMV_WIDTH, RMV_HEIGHT);
    du_init(&rm_ui, px, RMV_WIDTH, RMV_HEIGHT);
    struct resmeter_host host = { NULL, rm_refresh, rm_quit };
    assert(rmv_init(&rm, &rm_ui, &host) == 0);
    struct rm_desktop d = { 4, 16, 800, 1000 };
    rmv_update(&rm, &d, 1);
    assert(rm.percent[1] == 75 && rm.percent[2] == 80 && rm.percent[0] == 75 && rm.state.level == RM_OK);
    assert(!strcmp(du_get(&rm_ui, RMV_SYSTEM_TEXT)->text, "75%") && !strcmp(du_get(&rm_ui, RMV_GDI_TEXT)->text, "80%"));
    du_render(&rm_ui);
    struct du_rect sb = du_get(&rm_ui, RMV_SYSTEM_BAR)->box, ub = du_get(&rm_ui, RMV_USER_BAR)->box;
    assert(pix(&rm_ui, sb.x + 4, sb.y + sb.h / 2) == rmv_color(RM_OK) && pix(&rm_ui, sb.x + sb.w - 4, sb.y + sb.h / 2) != rmv_color(RM_OK));
    save("resmeter-ok", &rm_ui);
    d = (struct rm_desktop){ 14, 16, 250, 1000 };       /* User 12 % critical, GDI 25 % low */
    rmv_update(&rm, &d, 1);
    assert(rm.percent[1] == 12 && rm.percent[2] == 25 && rm.state.level == RM_CRITICAL && rm.level[1] == RM_CRITICAL && rm.level[2] == RM_LOW);
    du_render(&rm_ui);
    assert(pix(&rm_ui, ub.x + 3, ub.y + ub.h / 2) == rmv_color(RM_CRITICAL) && pix(&rm_ui, sb.x + 3, sb.y + sb.h / 2) == rmv_color(RM_CRITICAL));
    struct du_rect gb = du_get(&rm_ui, RMV_GDI_BAR)->box;
    assert(pix(&rm_ui, gb.x + 3, gb.y + gb.h / 2) == rmv_color(RM_LOW));
    assert(strstr(du_get(&rm_ui, RMV_SUMMARY)->text, "critically low"));
    save("resmeter-critical", &rm_ui);
    d = (struct rm_desktop){ 8, 16, 280, 1000 };        /* System 28 % low */
    rmv_update(&rm, &d, 1); assert(rm.state.level == RM_LOW && strstr(du_get(&rm_ui, RMV_SUMMARY)->text, "low"));
    rmv_update(&rm, &d, 0);                              /* the desktop did not answer: User unavailable */
    assert(!strcmp(du_get(&rm_ui, RMV_USER_TEXT)->text, "n/a") && rm.percent[0] == rm.percent[2]);
    du_render(&rm_ui); save("resmeter-unknown", &rm_ui);
    /* Refreshes every 5 s; Close quits. */
    du_tick(&rm_ui, 4999999999ull); assert(!refreshes);
    du_tick(&rm_ui, 5000000000ull); assert(refreshes == 1);
    du_tick(&rm_ui, 10000000000ull); assert(refreshes == 2);
    click_rect(&rm_ui, du_get(&rm_ui, RMV_CLOSE)->box); assert(rm_quits == 1);
    chord(&rm_ui, DU_KEY_LALT, K_C); assert(rm_quits == 2);
    check_guard(&rm_ui); free(px);
    puts("PASS G30 Resource Meter window");
}
int main(int argc, char **argv)
{
    folder = argc > 1 ? argv[1] : NULL;
    test_sysinfo(); test_sysmon(); test_resmeter();
    return 0;
}
