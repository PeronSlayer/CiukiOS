/* G30 system tools models against synthetic inventories and counters.
 * SPDX-License-Identifier: MIT */
#include "systools.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct ciuki_devices_info info;
static struct si_tree tree;
static struct sm_monitor mon;
static struct dc_store store, loaded;
static struct st_store sched, sched2;
static char text[8192];

static struct ciuki_device_record *dev(unsigned i, uint32_t id, unsigned cls, unsigned state, const char *name, const char *driver)
{
    struct ciuki_device_record *d = &info.devices[i];
    memset(d, 0, sizeof(*d));
    d->id = id; d->class_id = cls; d->state = state;
    snprintf(d->name, sizeof(d->name), "%s", name); snprintf(d->driver, sizeof(d->driver), "%s", driver);
    if (i >= info.count) info.count = i + 1;
    return d;
}
static void res(struct ciuki_device_record *d, unsigned type, uint64_t start, uint64_t end)
{ d->resources[d->resource_count++] = (struct ciuki_device_resource){ .type = type, .start = start, .end = end }; }

static void test_sysinfo(void)
{
    memset(&info, 0, sizeof(info));
    info.version = CIUKI_DEVICES_VERSION; info.size = sizeof(info);
    snprintf(info.cpu_brand, sizeof(info.cpu_brand), "Pentium III"); info.cpu_family = 6;
    struct ciuki_device_record *a = dev(0, 1, CIUKI_DEVICE_SOUND, CIUKI_DEVICE_READY, "AC97 audio", "ac97");
    res(a, CIUKI_RESOURCE_IO, 0xd000, 0xd100); res(a, CIUKI_RESOURCE_IRQ, 11, 12);
    struct ciuki_device_record *b = dev(1, 2, CIUKI_DEVICE_NETWORK, CIUKI_DEVICE_QUARANTINED, "NE2000", "");
    snprintf(b->reason, sizeof(b->reason), "no driver"); res(b, CIUKI_RESOURCE_IRQ, 11, 12); res(b, CIUKI_RESOURCE_IO, 0xd0f0, 0xd110);
    dev(2, 3, CIUKI_DEVICE_SYSTEM, CIUKI_DEVICE_READY, "Timer", "pit");
    info.driver_count = 1; snprintf(info.drivers[0].name, sizeof(info.drivers[0].name), "ac97"); snprintf(info.drivers[0].build, sizeof(info.drivers[0].build), "b1");
    struct si_input in = { .devices = &info, .memory_total_kib = 131072, .memory_free_kib = 90000, .processes = 7, .user_free_pct = 80, .gdi_free_pct = 64 };
    si_build(&tree, &in);
    assert(!tree.overflow && tree.conflicts == 1);               /* I/O d0f0-d100 overlaps; IRQ 11 is sharing */
    int c = si_find(&tree, "Conflicts/Sharing"); assert(c >= 0 && si_children(&tree, c) == 2);
    assert(strstr(tree.n[c].value, "1 conflict(s), 1 shared IRQ(s)"));
    assert(si_find(&tree, "Hardware Resources") >= 0 && si_find(&tree, "Components") >= 0 && si_find(&tree, "Software Environment") >= 0);
    int p = si_find(&tree, "Problem Devices"); assert(p >= 0 && si_children(&tree, p) == 1 && strstr(tree.n[p].value, "1 device"));
    int n = si_find(&tree, "NE2000"); assert(n >= 0);
    assert(tree.n[si_find(&tree, "System resources")].value[0] == '6');
    assert(si_export(&tree, text, sizeof(text)) > 100 && strstr(text, "Pentium III"));
    assert(si_export(&tree, text, 20) == 0);
    in.devices = NULL; si_build(&tree, &in); assert(tree.conflicts == 0 && si_find(&tree, "No conflicts") < 0);
    /* disabling the card frees its resources: the conflict disappears */
    dc_init(&store);
    struct dc_result r; dc_apply(&store, b, DC_DISABLE, NULL, &r); assert(!r.error);
    struct ciuki_devices_info live = info;
    for (unsigned i = 0; i < live.count; i++) live.devices[i].resource_count = dc_resource_count(&store, &info.devices[i]);
    in.devices = &live; si_build(&tree, &in); assert(tree.conflicts == 0);
    /* two records of the same PCI function share its resources without being a conflict */
    struct ciuki_devices_info twin = info;
    for (unsigned k = 0; k < 2; k++) { struct ciuki_device_record *t = &twin.devices[k];
        t->vendor = 0x8086; t->device = 0x2415; t->bus = 0; t->slot = 4; t->function = 0; }
    in.devices = &twin; si_build(&tree, &in); assert(tree.conflicts == 0);
    twin.devices[1].function = 1; si_build(&tree, &in); assert(tree.conflicts == 1);
    puts("PASS G30 System Information");
}
static void test_meter(void)
{
    struct rm_state s;
    rm_compute(&s, &(struct rm_sample){ 20, 100, 50, 100 });
    assert(s.user_free == 80 && s.gdi_free == 50 && s.system_free == 50 && s.level == RM_OK);
    rm_compute(&s, &(struct rm_sample){ 50, 100, 80, 100 }); assert(s.system_free == 20 && s.level == RM_LOW);
    rm_compute(&s, &(struct rm_sample){ 95, 100, 10, 100 }); assert(s.system_free == 5 && s.level == RM_CRITICAL);
    rm_compute(&s, &(struct rm_sample){ 200, 100, 0, 0 }); assert(s.user_free == 0 && s.gdi_free == 0 && s.level == RM_CRITICAL);
    rm_compute(&s, &(struct rm_sample){ 25, 100, 75, 100 });
    assert(rm_gfsr(&s, 0) == 25 && rm_gfsr(&s, 1) == 25 && rm_gfsr(&s, 2) == 75 && rm_gfsr(&s, 9) == 0);
    char tip[96]; rm_tooltip(tip, sizeof(tip), &s);
    assert(strstr(tip, "System Resources: 25%") && strstr(tip, "GDI Resources: 25%") && strstr(tip, "User Resources: 75%"));
    puts("PASS G30 Resource Meter");
}
static struct sm_raw raw(uint64_t total, uint64_t idle, uint64_t rd, uint64_t rx)
{
    return (struct sm_raw){ .total_ticks = total, .idle_ticks = idle, .disk_read_bytes = rd, .net_rx_packets = rx, .usable_pages = 1000,
        .free_pages = 400, .kheap_bytes = 2048000, .processes = 5, .threads = 9, .tick_hz = 1000,
        .have = (1u << SM_CPU_USAGE) | (1u << SM_MEM_FREE) | (1u << SM_MEM_USED) | (1u << SM_KHEAP) | (1u << SM_PROCESSES) | (1u << SM_THREADS) | (1u << SM_DISK_READ) };
}
static void test_monitor(void)
{
    sm_init(&mon);
    struct sm_raw first = raw(0, 0, 0, 0), second = raw(5000, 3750, 500000, 100);
    assert(mon.count == 1 && mon.items[0].counter == SM_CPU_USAGE && mon.interval_ms == 5000 && mon.chart == SM_LINE);
    assert(sm_add(&mon, SM_MEM_FREE, &first) == 0 && sm_add(&mon, SM_MEM_FREE, &first) == -EEXIST);
    assert(sm_add(&mon, SM_NET_RX, &first) == -ENODEV && sm_add(&mon, 99, &first) == -EINVAL);
    assert(sm_add(&mon, SM_DISK_READ, &first) == 0);
    assert(sm_set_interval(&mon, 0) == -EINVAL && sm_set_interval(&mon, 301) == -EINVAL && sm_set_interval(&mon, 1) == 0 && mon.interval_ms == 1000);
    assert(sm_set_chart(&mon, 3) == -EINVAL && sm_set_chart(&mon, SM_NUMERIC) == 0);
    size_t used = 0; char log[1024];
    assert(sm_log_header(&mon, log, sizeof(log), &used) == 0 && strstr(log, "\"Processor usage\",\"Free memory\",\"Bytes read/second\""));
    assert(sm_sample(&mon, &first, log, sizeof(log), &used) == 0 && mon.log_rows == 0);   /* baseline only */
    assert(sm_sample(&mon, &second, log, sizeof(log), &used) == 0 && mon.log_rows == 1);
    assert(mon.items[0].value == 25);                 /* 1250 busy of 5000 ticks */
    assert(mon.items[1].value == 1600 && mon.items[2].value == 100000);   /* 400 pages, 500000 B over 5 s */
    char f[64]; sm_format(&mon, 0, f, sizeof(f)); assert(!strcmp(f, "Processor usage: 25%"));
    sm_format(&mon, 2, f, sizeof(f)); assert(!strcmp(f, "Bytes read/second: 100000 B/s"));
    assert(mon.items[0].history[0] == 25 && mon.items[0].count == 1);
    assert(mon.items[2].history[0] == 100);           /* auto scale: first value is the peak */
    assert(strstr(log + strlen(log) - 40, "5,25,1600,100000"));
    /* counter wrap or a clock that went backwards never produces a huge rate */
    struct sm_raw back = raw(4000, 3000, 100, 0);
    assert(sm_sample(&mon, &back, log, sizeof(log), &used) == 0 && mon.items[2].value == 0);
    assert(sm_edit(&mon, SM_CPU_USAGE, 0xff00ff, 50) == 0 && mon.items[0].count == 0 && mon.items[0].color == 0xff00ff);
    assert(sm_remove(&mon, SM_MEM_FREE) == 0 && sm_remove(&mon, SM_MEM_FREE) == -ENOENT && mon.count == 2);
    for (int c = SM_MEM_FREE; mon.count < SM_CHART_ITEMS; c++) assert(sm_add(&mon, c, NULL) == 0 || sm_add(&mon, c, NULL) == -EEXIST);
    assert(mon.count == SM_CHART_ITEMS && sm_add(&mon, SM_NET_ERRORS, NULL) == -ENOSPC);
    /* a full log stops logging while the charts continue */
    char tiny[24]; size_t u2 = 0;
    assert(sm_log_header(&mon, tiny, sizeof(tiny), &u2) == -ENOSPC);   /* the header does not fit either */
    mon.logging = 1; u2 = 0;
    struct sm_raw later = raw(9000, 6000, 900, 0);
    assert(sm_sample(&mon, &later, tiny, sizeof(tiny), &u2) == -ENOSPC && !mon.logging && u2 == 0);
    for (unsigned i = 0; i < SM_HISTORY + 5; i++) { later.total_ticks += 1000; later.idle_ticks += 500; assert(sm_sample(&mon, &later, NULL, 0, NULL) == 0); }
    assert(mon.items[0].count == SM_HISTORY);
    assert(strlen(sm_explain(SM_NET_RX)) > 20 && !strcmp(sm_category(SM_DISK_READ), "File System") && !strcmp(sm_name(99), ""));
    puts("PASS G30 System Monitor");
}
static void test_devices(void)
{
    memset(&info, 0, sizeof(info)); info.count = 0;
    struct ciuki_device_record *snd = dev(0, 10, CIUKI_DEVICE_SOUND, CIUKI_DEVICE_READY, "AC97", "ac97");
    struct ciuki_device_record *tim = dev(1, 11, CIUKI_DEVICE_SYSTEM, CIUKI_DEVICE_READY, "Timer", "pit");
    struct ciuki_device_record *hd = dev(2, 12, CIUKI_DEVICE_DISK, CIUKI_DEVICE_READY, "IDE", "ata");
    struct ciuki_device_record *gone = dev(3, 13, CIUKI_DEVICE_PORT, CIUKI_DEVICE_ABSENT, "COM3", "");
    struct ciuki_device_record *nic = dev(4, 14, CIUKI_DEVICE_NETWORK, CIUKI_DEVICE_FIRMWARE_FIRST, "RTL8139", "");
    res(snd, CIUKI_RESOURCE_IRQ, 5, 6);
    dc_init(&store);
    struct dc_result r;
    dc_apply(&store, tim, DC_DISABLE, NULL, &r); assert(r.error == -EPERM && dc_status_of(&store, 11) == DC_ACTIVE);
    dc_apply(&store, hd, DC_REMOVE, NULL, &r); assert(r.error == -EPERM && store.count == 0);
    dc_apply(&store, gone, DC_DISABLE, NULL, &r); assert(r.error == -ENODEV);
    dc_apply(&store, snd, 77, NULL, &r); assert(r.error == -EINVAL);
    dc_apply(&store, snd, DC_DISABLE, NULL, &r);
    assert(!r.error && r.needs_restart && r.warn && store.restart_needed && dc_status_of(&store, 10) == DC_DISABLED && dc_resource_count(&store, snd) == 0);
    dc_apply(&store, snd, DC_DISABLE, NULL, &r); assert(!r.error && !r.needs_restart && strstr(r.message, "already"));
    dc_apply(&store, snd, DC_ENABLE, NULL, &r); assert(!r.error && r.needs_restart && dc_status_of(&store, 10) == DC_ACTIVE && store.count == 0);
    dc_apply(&store, snd, DC_ENABLE, NULL, &r); assert(!r.error && !r.needs_restart && strstr(r.message, "already"));
    dc_apply(&store, nic, DC_DISABLE, NULL, &r); assert(!r.error && !r.needs_restart);       /* nothing running to stop */
    dc_apply(&store, snd, DC_REMOVE, NULL, &r); assert(!r.error && dc_status_of(&store, 10) == DC_REMOVED);
    dc_apply(&store, snd, DC_UPDATE_DRIVER, "bad name", &r); assert(r.error == -EINVAL);
    dc_apply(&store, snd, DC_UPDATE_DRIVER, NULL, &r); assert(r.error == -EINVAL);
    dc_apply(&store, snd, DC_UPDATE_DRIVER, "ac97-new", &r); assert(!r.error && !strcmp(dc_driver_override(&store, 10), "ac97-new"));
    assert(dc_save(&store, text, sizeof(text)) > 0 && dc_save(&store, text + 4000, 12) == -ENOSPC);
    dc_load(&loaded, "garbage\ndevice=zz disabled=1 removed=0 driver=-\ndevice=5 disabled=7 removed=0 driver=-\n");
    assert(loaded.count == 0);
    assert(dc_save(&store, text, sizeof(text)) > 0 && dc_load(&loaded, text) == (int)store.count);
    assert(dc_status_of(&loaded, 10) == DC_REMOVED && dc_status_of(&loaded, 14) == DC_DISABLED && !strcmp(dc_driver_override(&loaded, 10), "ac97-new"));
    dc_restart(&loaded);                                         /* removed device is detected again; disabled stays; override stays */
    assert(dc_status_of(&loaded, 10) == DC_ACTIVE && dc_status_of(&loaded, 14) == DC_DISABLED && !strcmp(dc_driver_override(&loaded, 10), "ac97-new") && !loaded.restart_needed);
    for (unsigned i = 0; i < DC_MAX; i++) { struct ciuki_device_record x = *snd; x.id = 100 + i; dc_apply(&loaded, &x, DC_DISABLE, NULL, &r); }
    assert(r.error == -ENOSPC);
    puts("PASS G30 Device Manager actions");
}
static const struct aw_model catalog[] = {
    { CIUKI_DEVICE_NETWORK, "Realtek RTL8139", "rtl8139" }, { CIUKI_DEVICE_NETWORK, "NE2000 compatible", "ne2k" },
    { CIUKI_DEVICE_SOUND, "Sound Blaster 16", "sb16" },
};
static void test_wizard(void)
{
    memset(&info, 0, sizeof(info));
    struct ciuki_device_record *nic = dev(0, 20, CIUKI_DEVICE_NETWORK, CIUKI_DEVICE_FIRMWARE_FIRST, "PCI network controller", "");
    (void)nic;
    dev(1, 21, CIUKI_DEVICE_SOUND, CIUKI_DEVICE_READY, "AC97", "ac97");
    dc_init(&store);
    struct aw_wizard w; unsigned list[8];
    aw_init(&w, catalog, 3, &store, &info);
    assert(w.page == AW_WELCOME && aw_back(&w) == -EINVAL && aw_next(&w) == 0 && w.page == AW_SEARCH);
    aw_choose_search(&w, 1); assert(aw_next(&w) == 0 && w.page == AW_FOUND && w.found_count == 1 && w.found[0] == 20);
    assert(aw_back(&w) == 0 && w.page == AW_SEARCH && aw_next(&w) == 0 && w.page == AW_FOUND);
    assert(aw_next(&w) == 0 && w.page == AW_MODEL && w.class_id == CIUKI_DEVICE_NETWORK);
    assert(aw_next(&w) == -EINVAL);                                    /* nothing chosen */
    assert(aw_models(&w, list, 8) == 2 && list[0] == 0 && list[1] == 1);
    assert(aw_select_model(&w, 2) == -EINVAL && aw_select_model(&w, 1) == 0 && aw_next(&w) == 0 && w.page == AW_FINISH);
    assert(aw_next(&w) == 0 && w.page == AW_DONE && w.restart && !strcmp(dc_driver_override(&store, 20), "ne2k") && strstr(w.summary, "ne2k"));
    assert(aw_back(&w) == -EINVAL);
    /* nothing new: the manual path through the hardware types */
    aw_init(&w, catalog, 3, &store, &info);
    dev(0, 20, CIUKI_DEVICE_NETWORK, CIUKI_DEVICE_READY, "PCI network controller", "ne2k");
    aw_next(&w); aw_choose_search(&w, 1); aw_next(&w);
    assert(w.page == AW_NONE_FOUND && aw_next(&w) == 0 && w.page == AW_CLASS && aw_next(&w) == -EINVAL);
    assert(aw_select_class(&w, 99) == -EINVAL && aw_select_class(&w, CIUKI_DEVICE_SOUND) == 0 && aw_next(&w) == 0);
    assert(aw_models(&w, list, 8) == 1 && aw_select_model(&w, (int)list[0]) == 0 && aw_next(&w) == 0 && w.page == AW_FINISH);
    assert(aw_next(&w) == -ENODEV && w.page == AW_FINISH);            /* no sound device waits for a driver */
    assert(aw_back(&w) == 0 && aw_back(&w) == 0 && aw_back(&w) == 0 && aw_back(&w) == 0 && w.page == AW_SEARCH);
    aw_choose_search(&w, 0); assert(aw_next(&w) == 0 && w.page == AW_CLASS);
    assert(strlen(aw_title(AW_FOUND)) > 5 && !strcmp(aw_title(99), ""));
    puts("PASS G30 Add New Hardware");
}
static struct st_task task(const char *name, int kind, unsigned minute, struct st_date start, int64_t created)
{
    struct st_task t; memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name); snprintf(t.command, sizeof(t.command), "/bin/%s now", name);
    t.kind = kind; t.minute_of_day = minute; t.start = start; t.created = created; t.last_run = -1;
    return t;
}
static void test_scheduler(void)
{
    struct st_date epoch = { 1970, 1, 1 }, d20 = { 2026, 10, 23 };
    assert(st_weekday(epoch) == 4 && st_weekday(d20) == 5 && st_weekday((struct st_date){ 2000, 2, 29 }) == 2);
    assert(st_minutes(epoch, 61) == 61 && st_minutes((struct st_date){ 1969, 12, 31 }, 0) == -1440);
    int64_t t0 = st_minutes(d20, 9 * 60);                              /* Friday 09:00 */
    st_init(&sched);
    struct st_task daily = task("daily", ST_DAILY, 8 * 60, d20, t0);
    assert(st_add(&sched, &daily) == 0 && st_add(&sched, &daily) == -EEXIST);
    assert(st_next(&sched.t[0], t0) == st_minutes((struct st_date){ 2026, 10, 24 }, 480));
    assert(st_next(&sched.t[0], t0 - 600) == st_minutes(d20, 480));
    struct st_task weekly = task("weekly", ST_WEEKLY, 60, d20, t0); weekly.weekday_mask = 1 | 4;  /* Sunday, Tuesday */
    assert(st_add(&sched, &weekly) == 0);
    assert(st_next(&sched.t[1], t0) == st_minutes((struct st_date){ 2026, 10, 25 }, 60));
    assert(st_weekday((struct st_date){ 2026, 10, 25 }) == 0);
    struct st_task monthly = task("monthly", ST_MONTHLY, 0, d20, t0); monthly.day_of_month = 31;
    assert(st_add(&sched, &monthly) == 0);
    assert(st_next(&sched.t[2], t0) == st_minutes((struct st_date){ 2026, 10, 31 }, 0));
    assert(st_next(&sched.t[2], st_minutes((struct st_date){ 2026, 10, 31 }, 0)) == st_minutes((struct st_date){ 2026, 11, 30 }, 0));
    struct st_task feb = monthly; snprintf(feb.name, sizeof(feb.name), "feb"); feb.start = (struct st_date){ 2028, 2, 1 };
    assert(st_add(&sched, &feb) == 0 && st_next(&sched.t[3], 0) == st_minutes((struct st_date){ 2028, 2, 29 }, 0));
    struct st_task once = task("once", ST_ONCE, 600, (struct st_date){ 2026, 10, 24 }, t0);
    assert(st_add(&sched, &once) == 0);
    int64_t when = st_minutes((struct st_date){ 2026, 10, 24 }, 600);
    assert(st_next(&sched.t[4], t0) == when && st_next(&sched.t[4], when) == -1);
    struct st_task boot = task("boot", ST_AT_START, 0, epoch, t0);
    assert(st_add(&sched, &boot) == 0 && st_next(&sched.t[5], 0) == -1);
    /* due rules */
    unsigned due[ST_MAX];
    assert(st_due(&sched, t0, 0, due, ST_MAX) == 0);                  /* created after today's runs */
    assert(st_due(&sched, t0, 1, due, ST_MAX) == 1 && due[0] == 5);   /* only the At Start task */
    int64_t sat = st_minutes((struct st_date){ 2026, 10, 24 }, 500);
    assert(st_due(&sched, sat, 0, due, ST_MAX) == 1 && due[0] == 0);
    int64_t later = st_minutes((struct st_date){ 2026, 10, 31 }, 700);   /* a week off: missed daily runs happen once */
    unsigned n = st_due(&sched, later, 0, due, ST_MAX); assert(n == 4 && due[0] == 0 && due[1] == 1 && due[2] == 2 && due[3] == 4);
    st_mark_run(&sched, 0, later, 0); st_mark_run(&sched, 4, later, 7);
    assert(!sched.t[4].enabled && sched.t[4].last_result == 7 && st_due(&sched, later, 0, due, ST_MAX) == 2);
    sched.t[1].enabled = 0; assert(st_next(&sched.t[1], 0) == -1);
    /* validation */
    struct st_task bad = daily; snprintf(bad.name, sizeof(bad.name), "bad"); bad.minute_of_day = 1440; assert(st_add(&sched, &bad) == -EINVAL);
    bad = weekly; snprintf(bad.name, sizeof(bad.name), "bad"); bad.weekday_mask = 0x80; assert(st_add(&sched, &bad) == -EINVAL);
    bad = monthly; snprintf(bad.name, sizeof(bad.name), "bad"); bad.start.month = 13; assert(st_add(&sched, &bad) == -EINVAL);
    bad = daily; snprintf(bad.name, sizeof(bad.name), "a=b"); assert(st_add(&sched, &bad) == -EINVAL);
    bad = daily; snprintf(bad.name, sizeof(bad.name), "n"); bad.created = -1; assert(st_add(&sched, &bad) == -EINVAL);
    /* persistence */
    int saved = st_save(&sched, text, sizeof(text)); assert(saved > 0 && st_save(&sched, text + 4000, 20) == -ENOSPC);
    assert(st_load(&sched2, text) == (int)sched.count);
    for (unsigned i = 0; i < sched.count; i++) {
        assert(!strcmp(sched.t[i].name, sched2.t[i].name) && !strcmp(sched.t[i].command, sched2.t[i].command));
        assert(sched.t[i].enabled == sched2.t[i].enabled && sched.t[i].last_run == sched2.t[i].last_run && sched.t[i].kind == sched2.t[i].kind);
        assert(st_next(&sched.t[i], 0) == st_next(&sched2.t[i], 0));
    }
    assert(st_remove(&sched2, "daily") == 0 && st_remove(&sched2, "daily") == -ENOENT);
    st_init(&sched2); assert(st_load(&sched2, "task=x|1\ntask=\ngarbage\n") == 0);
    for (unsigned i = 0; i < ST_MAX; i++) { struct st_task f = daily; snprintf(f.name, sizeof(f.name), "f%u", i); st_add(&sched2, &f); }
    snprintf(daily.name, sizeof(daily.name), "over"); assert(st_add(&sched2, &daily) == -ENOSPC);
    puts("PASS G30 Task Scheduler");
}
static void test_maintenance(void)
{
    struct st_date start = { 2026, 10, 24 }; int64_t now = st_minutes(start, 0);
    st_init(&sched);
    assert(mw_apply(&sched, &(struct mw_choice){ .express = 1, .slot = MW_NIGHT }, start, now) == 3 && sched.count == 3);
    assert(!strcmp(sched.t[0].name, "Disk Defragmenter") && sched.t[0].minute_of_day == 0 && sched.t[1].minute_of_day == 30 && sched.t[2].minute_of_day == 60);
    assert(sched.t[0].kind == ST_WEEKLY && sched.t[0].weekday_mask == 1 && !strcmp(mw_tool_name(MW_SCANDISK), "ScanDisk") && !strcmp(mw_tool_name(9), ""));
    /* running the wizard again replaces its schedule */
    assert(mw_apply(&sched, &(struct mw_choice){ .slot = MW_EVENING, .tools = 1u << MW_SCANDISK }, start, now) == 1 && sched.count == 3);
    unsigned found = 0;
    for (unsigned i = 0; i < sched.count; i++) if (!strcmp(sched.t[i].name, "ScanDisk")) { found++; assert(sched.t[i].minute_of_day == 20 * 60); }
    assert(found == 1);
    assert(mw_apply(&sched, &(struct mw_choice){ .slot = 5, .tools = 1 }, start, now) == -EINVAL);
    assert(mw_apply(&sched, &(struct mw_choice){ .slot = MW_DAY, .tools = 0 }, start, now) == -EINVAL);
    assert(mw_apply(&sched, &(struct mw_choice){ .slot = MW_DAY, .tools = 1 }, (struct st_date){ 2026, 2, 30 }, now) == -EINVAL);
    unsigned due[ST_MAX]; assert(st_due(&sched, st_minutes((struct st_date){ 2026, 10, 25 }, 20 * 60 + 5), 0, due, ST_MAX) >= 1);
    puts("PASS G30 Maintenance Wizard");
}

static void test_sources(void)
{
    struct ciuki_proc_system sys = { .version = CIUKI_PROC_VERSION, .size = sizeof(sys), .tick_hz = 1000, .total_ticks = 5000, .idle_ticks = 4000,
        .usable_pages = 1000, .free_pages = 250, .kheap_bytes = 4096, .processes = 6, .disk_read_kib = 0xfffffff0u, .disk_write_kib = 10 };
    struct sm_source src; memset(&src, 0, sizeof(src));
    struct sm_raw a, b;
    sm_raw_from_proc(&a, &src, &sys, 11, 1, 100, 50);
    assert(a.disk_read_bytes == 0 && a.threads == 11 && (a.have & (1u << SM_NET_RX)) && !(a.have & (1u << SM_NET_ERRORS)));
    sys.total_ticks += 2000; sys.disk_read_kib = 0x20;                 /* the 32-bit KiB counter wrapped */
    sys.disk_write_kib = 74;
    sm_raw_from_proc(&b, &src, &sys, 11, 0, 0, 0);
    assert(b.disk_read_bytes == (0x20u + 0x10u) * 1024ull && b.disk_write_bytes == 64 * 1024ull && !(b.have & (1u << SM_NET_RX)));
    sm_raw_from_proc(&a, NULL, &sys, 1, 0, 0, 0); assert(!(a.have & (1u << SM_DISK_READ)));
    struct sm_monitor m; sm_init(&m);
    assert(sm_add(&m, SM_DISK_WRITE, &b) == 0 && sm_add(&m, SM_NET_ERRORS, &b) == -ENODEV);
    struct rm_sample s; struct rm_state r;
    rm_from_desktop(&s, &(struct rm_desktop){ .clients_used = 12, .clients_cap = 16, .free_pages = 100, .usable_pages = 1000 });
    rm_compute(&r, &s); assert(r.user_free == 25 && r.gdi_free == 10 && r.system_free == 10 && r.level == RM_CRITICAL);
    puts("PASS G30 sources");
}
static void test_profiles(void)
{
    struct hp_store h; hp_init(&h);
    assert(h.count == 1 && !strcmp(h.p[0].name, HP_ORIGINAL) && h.wait_seconds == 30);
    assert(hp_delete(&h, 0) == -EPERM && hp_startup(&h, -1, 0) == 0);
    assert(hp_copy(&h, 0, "Docked") == 0 && hp_copy(&h, 0, "docked") == -EEXIST && hp_copy(&h, 5, "x") == -ENOENT);
    assert(hp_copy(&h, 0, "") == -EINVAL && hp_copy(&h, 0, " lead") == -EINVAL && hp_copy(&h, 0, "a=b") == -EINVAL);
    struct ciuki_device_record snd; memset(&snd, 0, sizeof(snd)); snd.id = 77; snd.class_id = CIUKI_DEVICE_SOUND; snd.state = CIUKI_DEVICE_READY;
    struct dc_result r;
    assert(hp_apply(&h, 1, &snd, DC_DISABLE, NULL, &r) == 0 && dc_status_of(&h.p[1].devices, 77) == DC_DISABLED && dc_status_of(&h.p[0].devices, 77) == DC_ACTIVE);
    assert(hp_apply(&h, 3, &snd, DC_DISABLE, NULL, &r) == -ENOENT);
    assert(hp_copy(&h, 1, "Undocked") == 0 && dc_status_of(&h.p[2].devices, 77) == DC_DISABLED);   /* a copy keeps the device settings */
    assert(hp_rename(&h, 2, "Docked") == -EEXIST && hp_rename(&h, 2, "Travel") == 0);
    assert(hp_delete(&h, 0) == -EBUSY);                               /* the profile in use */
    assert(hp_set_wait(&h, 1000) == -EINVAL && hp_set_wait(&h, -2) == -EINVAL && hp_set_wait(&h, 10) == 0);
    assert(hp_set_default(&h, 2) == 0 && hp_startup(&h, -1, 5) == -1 && hp_startup(&h, -1, 10) == 2 && hp_startup(&h, 1, 0) == 1);
    assert(hp_set_wait(&h, -1) == 0 && hp_startup(&h, -1, 100000) == -1 && hp_startup(&h, 9, 0) == -1);
    assert(hp_move(&h, 2, -2) == 0 && !strcmp(h.p[0].name, "Travel") && h.default_index == 0 && h.active == 1);
    assert(hp_move(&h, 0, -1) == -EINVAL && hp_move(&h, 9, 1) == -ENOENT);
    char text[8192];
    assert(hp_save(&h, text, sizeof(text)) > 0 && hp_save(&h, text + 8000, 20) == -ENOSPC);
    assert(hp_save(&h, text, sizeof(text)) > 0);
    struct hp_store g;
    assert(hp_load(&g, text) == 3 && !strcmp(g.p[0].name, "Travel") && g.wait_seconds == -1 && g.default_index == 0 && g.active == 1);
    assert(dc_status_of(&g.p[0].devices, 77) == DC_DISABLED && dc_status_of(&g.p[1].devices, 77) == DC_ACTIVE &&
           dc_status_of(&g.p[2].devices, 77) == DC_DISABLED);
    struct hp_store junk; assert(hp_load(&junk, "garbage") == 1 && !strcmp(junk.p[0].name, HP_ORIGINAL));
    assert(hp_delete(&g, 2) == 0 && g.count == 2 && hp_delete(&g, 1) == -EBUSY);
    assert(hp_delete(&g, 0) == 0 && g.count == 1 && g.active == 0 && !strcmp(g.p[0].name, HP_ORIGINAL) && g.default_index == 0);
    for (unsigned i = 1; i < HP_MAX; i++) { char n[16]; snprintf(n, sizeof(n), "p%u", i); assert(hp_copy(&g, 0, n) == 0); }
    assert(hp_copy(&g, 0, "overflow") == -ENOSPC);
    puts("PASS G30 Hardware Profiles");
}
static void test_performance(void)
{
    struct pf_status s;
    struct pf_input in = { .memory_kib = 65536, .system_free_pct = 82, .pagefile_present = 1, .pagefile_kib = 98304, .pagefile_used_kib = 2048,
        .accel_active = 1, .accel_name = "Legacy native 2D" };
    pf_summarize(&s, &in);
    assert(!strcmp(s.memory, "64.0 MB of RAM") && !strcmp(s.resources, "82% free") && !strcmp(s.file_system, "32-bit"));
    assert(strstr(s.virtual_memory, "swap file 96.0 MB") && strstr(s.virtual_memory, "2048 KB in use"));
    assert(strstr(s.graphics, "Legacy native 2D") && !strcmp(s.disk_compression, "Not installed") && strstr(s.pc_cards, "No PC Card"));
    assert(!s.degraded && !strcmp(s.verdict, "Your system is configured for optimal performance."));
    in.system_free_pct = 10; pf_summarize(&s, &in); assert(s.degraded && strstr(s.verdict, "System resources are low"));
    in.system_free_pct = 80; in.pagefile_present = 0; pf_summarize(&s, &in); assert(s.degraded && strstr(s.verdict, "Virtual memory is turned off"));
    in.memory_kib = 262144; pf_summarize(&s, &in); assert(!s.degraded && !strcmp(s.virtual_memory, "Swap file not in use"));
    in.accel_active = 0; in.compressed_drives = 1; in.pccard_sockets = 2; pf_summarize(&s, &in);
    assert(strstr(s.graphics, "software") && strstr(s.disk_compression, "1 compressed") && strstr(s.pc_cards, "2 PC Card"));
    puts("PASS G30 Performance");
}
struct fake_host {
    int64_t now; char file[SS_TEXT]; int have_file, saves, loads; unsigned pid; char launched[8][96]; unsigned launch_count; int launch_result;
};
static int64_t fh_now(void *c) { return ((struct fake_host *)c)->now; }
static int fh_load(void *c, char *t, size_t cap, size_t *n)
{ struct fake_host *f = c; f->loads++; if (!f->have_file) return -ENOENT; *n = strlen(f->file); assert(*n < cap); memcpy(t, f->file, *n); return 0; }
static int fh_save(void *c, const char *t, size_t n) { struct fake_host *f = c; f->saves++; memcpy(f->file, t, n); f->file[n] = 0; f->have_file = 1; return 0; }
static int fh_launch(void *c, const char *p, const char *a)
{ struct fake_host *f = c; snprintf(f->launched[f->launch_count++ % 8], 96, "%s|%s", p, a); return f->launch_result ? f->launch_result : (int)++f->pid; }
static struct ss_service service;
static void test_service(void)
{
    char program[48], argument[161];
    assert(ss_split("/Programs/files /mnt/c", program, sizeof(program), argument, sizeof(argument)) == 0 && !strcmp(program, "files") && !strcmp(argument, "/mnt/c"));
    assert(ss_split("  hwreport  ", program, sizeof(program), argument, sizeof(argument)) == 0 && !strcmp(program, "hwreport") && !argument[0]);
    assert(ss_split("", program, sizeof(program), argument, sizeof(argument)) == -EINVAL && ss_split("/Programs/", program, sizeof(program), argument, sizeof(argument)) == -EINVAL);
    assert(ss_split("a b", program, sizeof(program), argument, 1) == -EINVAL);
    struct st_date today = { 2026, 10, 24 };
    int64_t base = st_minutes(today, 8 * 60);
    struct st_store s; st_init(&s);
    struct st_task daily = task("backup", ST_DAILY, 9 * 60, today, base);
    snprintf(daily.command, sizeof(daily.command), "/Programs/files /mnt/c");
    struct st_task boot = task("greeter", ST_AT_START, 0, (struct st_date){ 1970, 1, 1 }, base);
    snprintf(boot.command, sizeof(boot.command), "hwreport");
    struct st_task bad = task("missing", ST_DAILY, 8 * 60 + 30, today, base);
    snprintf(bad.command, sizeof(bad.command), "/bin/nothing");
    assert(st_add(&s, &daily) == 0 && st_add(&s, &boot) == 0 && st_add(&s, &bad) == 0);
    struct fake_host f; memset(&f, 0, sizeof(f)); f.now = base + 10;
    assert(st_save(&s, f.file, sizeof(f.file)) > 0); f.have_file = 1;
    struct ss_host h = { &f, fh_now, fh_load, fh_save, fh_launch, NULL, NULL, NULL, NULL };
    assert(ss_poll(&service, &h) == 1 && f.launch_count == 1 && !strcmp(f.launched[0], "hwreport|") && f.saves == 1);   /* At Start only */
    assert(ss_poll(&service, &h) == 0 && f.saves == 1);                 /* unchanged file, nothing due */
    f.now = base + 40;                                                  /* 08:40: "missing" is due */
    f.launch_result = -ENOENT;
    assert(ss_poll(&service, &h) == 0 && service.failed == 1 && f.saves == 2);
    f.launch_result = 0;
    assert(ss_poll(&service, &h) == 0);                                 /* a failure is recorded, not retried */
    f.now = base + 61;                                                  /* 09:01 */
    assert(ss_poll(&service, &h) == 1 && !strcmp(f.launched[2], "files|/mnt/c"));
    assert(ss_poll(&service, &h) == 0);
    struct st_store check; assert(st_load(&check, f.file) == 3);
    assert(check.t[0].last_run == base + 61 && check.t[2].last_result == -ENOENT && check.t[1].last_run == base + 10);
    f.now += 1440;                                                      /* next day, and the file was edited meanwhile */
    check.t[0].enabled = 0; assert(st_save(&check, f.file, sizeof(f.file)) > 0);
    unsigned before = f.launch_count;
    f.launch_result = -ENOENT;
    assert(ss_poll(&service, &h) == 0 && f.launch_count == before + 1 && !strcmp(f.launched[before % 8], "/bin/nothing|"));   /* the edited file disables 'backup'; 'missing' is due again the next day */
    assert(service.store.t[0].enabled == 0);
    /* a busy launcher (-EAGAIN) leaves the task due; nothing is recorded */
    f.launch_result = -EAGAIN; f.now += 1440;
    unsigned deferred = service.deferred;
    assert(ss_poll(&service, &h) == 0 && service.deferred > deferred);
    int saves = f.saves; f.launch_result = 0;
    assert(ss_poll(&service, &h) == 1 && f.saves == saves + 1);
    /* no file at all: an empty schedule, no error */
    struct ss_service empty; memset(&empty, 0, sizeof(empty)); struct fake_host none; memset(&none, 0, sizeof(none)); none.now = base;
    struct ss_host hn = { &none, fh_now, fh_load, fh_save, fh_launch, NULL, NULL, NULL, NULL };
    assert(ss_poll(&empty, &hn) == 0 && empty.store.count == 0 && none.saves == 0);
    puts("PASS G30 scheduler service");
}
int main(void)
{
    test_sysinfo(); test_meter(); test_monitor(); test_devices(); test_wizard(); test_scheduler(); test_maintenance();
    test_sources(); test_profiles(); test_performance(); test_service();
    puts("PASS G30 system tools");
    return 0;
}
