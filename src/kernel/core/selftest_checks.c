/* fx-selftest checks: bounded, read-mostly observations of the ordinary
 * drivers. No check writes a file (the report is written by selftest.c),
 * changes a saved setting, powers off or reboots. Absent devices SKIP.
 * Sources and exceptions: docs/design/self-test.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/abi.h>
#include <ciuki/storage.h>
#include <ciuki/ata.h>
#include <ciuki/usb.h>
#include <ciuki/net.h>
#include <ciuki/audio.h>
#include <ciuki/audio_backend.h>
#include <ciuki/fbdev.h>
#include <ciuki/display.h>
#include <ciuki/display_driver.h>
#include <ciuki/biosvm.h>
#include <ciuki/thinkpad_ec.h>
#include <ciuki/power.h>
#include <ciuki/power_source.h>
#include <ciuki/mm.h>
#include <ciuki/ledger.h>
#include <ciuki/supervisor.h>
#include <ciuki/com.h>
#include <ciuki/selftest.h>
#include <ciuki/pbkdf2.h> /* sec-k08: security.kdf */

/* Optional subsystems: an image or configuration without them reads as an
 * absent device, never as a link error. */
#pragma weak render3d_qualification_report
#pragma weak w32_render3d_selftest
#pragma weak apm_platform_selftest
#pragma weak biosvm_video_state_snapshot
#pragma weak display_selftest_info
#pragma weak power_s5_fallback_qualified
#pragma weak power_s5_fallback_state
#pragma weak power_s5_fallback_data
#pragma weak power_source_snapshot
#pragma weak thinkpad_ec_smbios_snapshot
#pragma weak thinkpad_ec_poll_state
#pragma weak net_state_snapshot
#pragma weak usb_count
#pragma weak usb_get
#pragma weak ata_inventory_get
#pragma weak com_snapshot
void net_settings_get(unsigned, struct ciuki_net_config *) __attribute__((weak));
int net_stack_dhcp_error(unsigned) __attribute__((weak));

static struct serial_stats serial_start;
static struct kheap_ledger heap_start;
static uint32_t pmm_refused_start, pmm_free_start;

void selftest_checks_begin(void)
{
    serial_snapshot(&serial_start);
    kheap_snapshot(&heap_start);
    pmm_refused_start = pmm_refusals();
    pmm_free_start = pmm_free_count();
}

#define CHECK(name) static void name(void *ctx, const struct selftest_check *c, uint64_t until, struct selftest_result *r)
#define UNUSED (void)ctx; (void)c; (void)until
static void pass(struct selftest_result *r) { selftest_set(r, SELFTEST_PASS, 0); }
static void skip(struct selftest_result *r, const char *reason) { selftest_set(r, SELFTEST_SKIP, reason); }
static void fail(struct selftest_result *r, const char *reason) { selftest_set(r, SELFTEST_FAIL, reason); }
static uint64_t now(void) { return g_ticks; }
/* Sleep in 10 ms steps until done() or reserve ms before until. */
static bool wait_for(bool (*done)(void), uint64_t until, uint32_t reserve)
{
    while (!done()) {
        if (now() + reserve >= until) return false;
        task_sleep_ms(10);
    }
    return true;
}
/* ---- serial ---------------------------------------------------------- */
static const char *com_origin(uint32_t origin)
{ return origin == CIUKI_COM_BDA ? "bda" : origin == CIUKI_COM_PNP ? "pnp" : origin == CIUKI_COM_PCI ? "pci" : "other"; }
/* The kernel UART (loader and kernel probe results) and the ports the COM
 * driver admitted from the BIOS data area 0040:0000 (com_bda_discover at
 * boot), PnP and PCI: number:base:irq:origin:state for each. */
CHECK(check_serial)
{
    UNUSED;
    struct serial_stats st; serial_snapshot(&st);
    selftest_figures(r, "loader_state=%u base=0x%x divisor=%u kernel_present=%u probe_result=%u fifo=%u",
                     g_boot.uart_state, g_boot.uart_base, g_boot.uart_divisor, st.present,
                     (unsigned)st.probe_result, serial_fifo_size());
    char ports[100]; size_t used = 0; unsigned count = 0, bda = 0;
    ports[0] = 0;
    for (unsigned i = 0; com_snapshot && i < CIUKI_COM_MAX; i++) {
        struct ciuki_com_status com; const char *why = 0;
        if (!com_snapshot(i, &com, &why)) continue;
        count++; bda += com.origin == CIUKI_COM_BDA;
        int w = ksnprintf(ports + used, sizeof(ports) - used, "%s%u:%x:%u:%s:%s", used ? "," : "", com.number, com.base,
                          com.irq, com_origin(com.origin), com.flags & CIUKI_COM_READY ? "ready" :
                          com.flags & CIUKI_COM_QUARANTINED ? "quarantined" : "present");
        if (w > 0 && used + (size_t)w < sizeof(ports)) used += (size_t)w;
        else { ports[used] = 0; selftest_figures(r, "com_list_truncated=1"); break; }
    }
    selftest_figures(r, "com_ports=%u bda_ports=%u", count, bda);
    selftest_token(r->figures, sizeof(r->figures), "com", used ? ports : "-");
    if (!bda && !st.present) skip(r, "no_uart");
    else if (st.present && st.probe_result == SERIAL_PROBE_OK) pass(r);
    else fail(r, st.present ? "probe" : "listed_but_absent");
}
CHECK(check_serial_records)
{
    UNUSED;
    struct serial_stats st; serial_snapshot(&st);
    selftest_figures(r, "accepted=%llu sent=%llu queued=%u dropped=%u periodic_dropped=%u evidence_dropped=%u resent=%u",
                     (unsigned long long)(st.accepted - serial_start.accepted),
                     (unsigned long long)(st.sent - serial_start.sent), st.queued,
                     st.dropped - serial_start.dropped, st.periodic_dropped - serial_start.periodic_dropped,
                     st.evidence_dropped - serial_start.evidence_dropped, st.resent - serial_start.resent);
    if (!st.present) skip(r, "no_uart");
    else if (st.evidence_dropped != serial_start.evidence_dropped || st.resent != serial_start.resent)
        fail(r, "records_lost");
    else pass(r);
}

/* ---- memory ---------------------------------------------------------- */
static void memory_figures(struct selftest_result *r, bool *ok)
{
    *ok = ledger_reconcile();
    struct mm_ledger v; ledger_snapshot(&v);
    struct kheap_ledger h; kheap_snapshot(&h);
    selftest_figures(r, "free_pages=%u total_pages=%u normal_free=%u dma_free=%u heap_pages=%u heap_bytes=%u heap_peak=%u ledger_mismatches=%u",
                     pmm_free_count(), pmm_total_usable(), v.zone[ZONE_NORMAL].free, v.zone[ZONE_DMA].free,
                     h.pages, (unsigned)h.in_use, (unsigned)h.peak, v.mismatches);
}
CHECK(check_memory_start)
{
    UNUSED;
    bool ok; memory_figures(r, &ok);
    selftest_figures(r, "heap_refused=%u pmm_refused=%u", heap_start.refused, pmm_refused_start);
    if (ok) pass(r); else fail(r, "ledger");
}
CHECK(check_memory_end)
{
    UNUSED;
    bool ok; memory_figures(r, &ok);
    struct kheap_ledger h; kheap_snapshot(&h);
    uint32_t heap_refused = h.refused - heap_start.refused, pmm_refused = pmm_refusals() - pmm_refused_start;
    selftest_figures(r, "run_heap_refused=%u run_pmm_refused=%u free_delta=%d",
                     heap_refused, pmm_refused, (int)pmm_free_count() - (int)pmm_free_start);
    if (!ok) fail(r, "ledger");
    else if (heap_refused || pmm_refused) fail(r, "allocation_refused");
    else pass(r);
}

/* ---- display --------------------------------------------------------- */
static const char *driver_name(void)
{
    const struct display_driver *d = display_driver_bound();
    return d && d->id ? d->id : "none";
}
CHECK(check_display_info)
{
    UNUSED;
    const struct fb_device *fb = fbdev_get();
    const struct pci_func *pci = display_driver_owner();
    unsigned panel_w = 0, panel_h = 0;
    bool edid = (g_boot.flags & CBI_F_EDID_VALID) != 0;
    if (edid) { /* EDID 1.3 first detailed timing descriptor (bytes 54..71) */
        panel_w = g_boot.edid[56] | ((g_boot.edid[58] & 0xF0u) << 4);
        panel_h = g_boot.edid[59] | ((g_boot.edid[61] & 0xF0u) << 4);
    }
    selftest_figures(r, "mode=0x%x width=%u height=%u bpp=%u pitch=%u vbe=0x%x vram_kib=%u modes=%u edid=%u panel=%ux%u",
                     fb->mode_id, fb->width, fb->height, fb->bpp, fb->pitch, g_boot.display.vbe_version,
                     g_boot.display.vram_bytes / 1024u, g_boot.display.mode_count, edid, panel_w, panel_h);
    selftest_token(r->figures, sizeof(r->figures), "driver", driver_name());
    if (pci) selftest_figures(r, "pci=%04x:%04x", pci->vendor, pci->device);
    char oem[3][40];
    const char *src[3] = { g_boot.display.oem, g_boot.display.vendor, g_boot.display.product };
    for (unsigned i = 0; i < 3; i++) {
        unsigned k = 0;
        for (; k < sizeof(oem[i]) - 1 && src[i][k]; k++) oem[i][k] = src[i][k];
        oem[i][k] = 0;
    }
    char tokens[SELFTEST_FIGURES] = "";
    selftest_token(tokens, sizeof(tokens), "oem", oem[0]);
    selftest_token(tokens, sizeof(tokens), "vendor", oem[1]);
    selftest_token(tokens, sizeof(tokens), "product", oem[2]);
    selftest_detail(c->id, "%s", tokens);
    /* Mode lists in groups whose detail record (prefix, "line=N modes=")
     * stays one serial record: 30 + 1 + 8 + 6 + 112 < SELFTEST_RECORD_FIELDS. */
    for (unsigned i = 0, line = 0; i < g_boot.display.mode_count && i < CBI_DISPLAY_MAX; line++) {
        char list[112]; size_t used = 0; list[0] = 0;
        for (; i < g_boot.display.mode_count && i < CBI_DISPLAY_MAX && used + 24 < sizeof(list); i++) {
            const struct ciuki_boot_display_mode *m = &g_boot.display.modes[i];
            int w = ksnprintf(list + used, sizeof(list) - used, "%s%x:%ux%ux%u%s", used ? "," : "",
                              m->id, m->width, m->height, m->bpp, m->linear ? "" : "b");
            if (w > 0) used += (size_t)w;
        }
        selftest_detail(c->id, "line=%u modes=%s", line, list);
    }
    if (!fb->present) skip(r, "no_framebuffer"); else pass(r);
}
static uint16_t listed_mode(unsigned width, unsigned height, unsigned bpp)
{
    for (unsigned i = 0; i < g_boot.display.mode_count && i < CBI_DISPLAY_MAX; i++) {
        const struct ciuki_boot_display_mode *m = &g_boot.display.modes[i];
        if (m->width == width && m->height == height && m->bpp == bpp && m->linear) return m->id;
    }
    return 0;
}
static bool same_layout(const struct fb_device *a, const struct fb_device *b)
{
    return a->present && b->present && a->width == b->width && a->height == b->height &&
           a->pitch == b->pitch && a->bpp == b->bpp && a->phys == b->phys && a->mode_id == b->mode_id;
}
/* The desktop's own mode-switch path (VBE 4F02 through the display
 * service, or the bound native provider), set and restored, before the
 * desktop starts. display_check_* arms its five-second watchdog. */
static void mode_check(unsigned bpp, struct selftest_result *r)
{
    const struct fb_device original = *fbdev_get();
    uint16_t id = listed_mode(640, 480, bpp);
    selftest_figures(r, "mode=0x%x desktop_mode=0x%x", id, original.mode_id);
    if (!original.present) { skip(r, "no_framebuffer"); return; }
    if (!id) { skip(r, "not_listed"); return; }
    if (id == original.mode_id) { skip(r, "desktop_mode"); return; }
    uint64_t t0 = now();
    int e = display_check_begin(id);
    uint64_t set_ms = now() - t0;
    const struct fb_device *fb = fbdev_get();
    bool geometry = !e && fb->present && fb->width == 640 && fb->height == 480 && fb->bpp == bpp;
    if (!e) task_sleep_ms(300);
    t0 = now();
    int restore = e ? 0 : display_check_end();
    uint64_t restore_ms = now() - t0;
    bool restored = same_layout(fbdev_get(), &original);
    selftest_figures(r, "set_ms=%llu result=%d geometry=%u restore_ms=%llu restore=%d restored=%u",
                     (unsigned long long)set_ms, e, geometry, (unsigned long long)restore_ms, restore, restored);
    if (!e && geometry && !restore && restored) pass(r);
    else fail(r, e ? "mode_set" : !geometry ? "geometry" : restore ? "restore" : "not_restored");
}
CHECK(check_mode_8) { UNUSED; mode_check(8, r); }
CHECK(check_mode_32) { UNUSED; mode_check(32, r); }

static bool desktop_ready(void)
{
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    return supervisor_desktop_pid() && d.desktop_frames > 0;
}
CHECK(check_desktop)
{
    UNUSED;
    uint64_t t0 = now();
    bool ok = wait_for(desktop_ready, until, 200);
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    selftest_figures(r, "pid=%u frames=%u wait_ms=%llu deaths=%u restarts=%u",
                     supervisor_desktop_pid(), d.desktop_frames, (unsigned long long)(now() - t0),
                     supervisor_desktop_deaths(), supervisor_desktop_restarts());
    if (ok) pass(r); else fail(r, supervisor_desktop_pid() ? "no_frame" : "no_desktop");
}
static bool desktop_up(void) { return supervisor_desktop_pid() != 0; }

static struct render3d_qualification_report report3d;
static bool report3d_finished(void)
{
    render3d_qualification_report(&report3d);
    return report3d.finished;
}
static bool report3d_scheduled(void)
{
    render3d_qualification_report(&report3d);
    return report3d.scheduled || report3d.finished;
}
/* The 2D activation schedules the qualification (the driver's boot
 * diagnostic) and the qualification waits for the desktop's first frame; a run that is
 * not scheduled within this grace never will be on this start. */
#define SCHEDULE_GRACE_MS 15000u
CHECK(check_3d)
{
    UNUSED;
    if (!render3d_qualification_report) { skip(r, "not_built"); return; }
    render3d_qualification_report(&report3d);
    if (!report3d.present) { skip(r, "no_3d_backend"); return; }
    uint64_t t0 = now();
    uint64_t grace = t0 + SCHEDULE_GRACE_MS < until ? t0 + SCHEDULE_GRACE_MS : until;
    bool scheduled = wait_for(report3d_scheduled, grace, 0);
    bool done = scheduled && wait_for(report3d_finished, until, 300);
    struct render3d_qualification_report *p = &report3d;
    selftest_figures(r, "wait_ms=%llu scheduled=%u active=%u", (unsigned long long)(now() - t0), p->scheduled, p->active);
    if (!p->scheduled && !p->finished) {
        struct ciuki_display_engine_info engine = { .version = 1, .size = sizeof(engine) };
        display_driver_info(&engine);
        selftest_figures(r, "engine_kind=%u engine_ready=%u engine_quarantined=%u", engine.kind, engine.ready,
                         engine.quarantined);
        fail(r, "not_scheduled");
        return;
    }
    if (!done) { fail(r, "timeout"); return; }
    selftest_token(r->figures, sizeof(r->figures), "result", p->result);
    selftest_token(r->figures, sizeof(r->figures), "why", p->reason);
    selftest_token(r->figures, sizeof(r->figures), "marker", p->marker);
    selftest_figures(r, "error=%d elapsed_us=%llu failed_phase=%u triangles=%llu packets=%llu copies=%llu",
                     p->error, (unsigned long long)p->elapsed_us, p->failed_phase,
                     (unsigned long long)p->triangles, (unsigned long long)p->packets, (unsigned long long)p->copies);
    selftest_figures(r, "fullsize=%u perspective=%u d24s8=%u rgb565=%u heap=%u required=%u free_vram=%u largest_vram=%u texture=%u",
                     p->fullsize, p->perspective, p->depth24, p->color565, p->heap_bytes, p->required,
                     p->free_vram, p->largest_vram, p->texture_bytes);
    for (unsigned i = 0; i < p->phase_count && i < RENDER3D_QUALIFICATION_PHASES; i += 4) {
        char line[SELFTEST_FIGURES] = "";
        for (unsigned k = i; k < i + 4 && k < p->phase_count; k++) {
            char key[8], value[64];
            ksnprintf(key, sizeof(key), "p%u", k);
            ksnprintf(value, sizeof(value), "%s:%u:%d", p->phases[k].name ? p->phases[k].name : "?",
                      p->phases[k].us, p->phases[k].error);
            selftest_token(line, sizeof(line), key, value);
        }
        selftest_detail(c->id, "%s", line);
    }
    if (p->qualified) pass(r); else fail(r, p->lost ? "device_lost" : "not_qualified");
}
CHECK(check_gl)
{
    UNUSED;
    if (!w32_render3d_selftest) { skip(r, "not_built"); return; }
    bool present = false;
    if (render3d_qualification_report) {
        render3d_qualification_report(&report3d);
        present = report3d.present;
        if (present && !report3d.qualified) { skip(r, "3d_not_qualified"); return; }
    }
    struct render3d_selftest g = {0};
    uint64_t budget = until > now() + 500 ? until - now() - 500 : 0;
    int e = w32_render3d_selftest(&g, budget > 8000 ? 8000 : (uint32_t)budget);
    selftest_token(r->figures, sizeof(r->figures), "stage", e == -ETIMEDOUT && !g.stage ? "worker" : g.stage);
    selftest_figures(r, "error=%d hardware=%u expected=0x%08x actual=0x%08x size=%ux%u draw_ms=%u",
                     e, g.hardware, g.expected, g.actual, g.width, g.height, g.elapsed_ms);
    if (!e && g.hardware) pass(r);
    else if (e == -EOPNOTSUPP && !present) skip(r, "no_3d_backend");
    else fail(r, !e ? "software_context" : "draw");
}
CHECK(check_2d)
{
    UNUSED;
    struct ciuki_display_engine_info info = { .version = 1, .size = sizeof(info) };
    display_driver_info(&info);
    uint64_t native = 0, software = 0;
    char ops[160] = "";
    for (unsigned i = 0; i < CIUKI_ENGINE_OPERATIONS; i++) {
        native += info.native[i]; software += info.software[i];
        char key[8], value[48];
        ksnprintf(key, sizeof(key), "op%u", i);
        ksnprintf(value, sizeof(value), "%llu/%llu", (unsigned long long)info.native[i], (unsigned long long)info.software[i]);
        selftest_token(ops, sizeof(ops), key, value);
    }
    char id[20]; unsigned k = 0;
    for (; k < sizeof(id) - 1 && k < sizeof(info.driver_id) && info.driver_id[k]; k++) id[k] = info.driver_id[k];
    id[k] = 0;
    char why[64]; k = 0;
    for (; k < sizeof(why) - 1 && k < sizeof(info.reason) && info.reason[k]; k++) why[k] = info.reason[k];
    why[k] = 0;
    selftest_token(r->figures, sizeof(r->figures), "driver", id);
    selftest_figures(r, "kind=%u ready=%u native_used=%u cpu_ready=%u quarantined=%u native_ops=%llu software_ops=%llu",
                     info.kind, info.ready, info.native_used, info.cpu_ready, info.quarantined,
                     (unsigned long long)native, (unsigned long long)software);
    selftest_token(r->figures, sizeof(r->figures), "state", why);
    selftest_detail(c->id, "%s", ops);
    const char *disabled = 0;
    if (display_accel_disabled(&disabled)) skip(r, "disabled");
    else if (info.kind == CIUKI_ENGINE_NONE) skip(r, "no_driver");
    else if (info.quarantined) fail(r, "quarantined");
    else if (!info.ready) fail(r, "not_ready");
    else if (!info.native_used) fail(r, "unused");
    else pass(r);
}
static bool repainted(void)
{
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    return !d.frame_pending;
}
/* The DOS full-screen display transaction without a guest: the current
 * policy (native provider or video BIOS with 4F04 state) sets the mode on
 * the panel, then restores the desktop and waits for its repaint. */
static void fullscreen_check(uint16_t mode, uint64_t until, struct selftest_result *r)
{
    selftest_figures(r, "mode=0x%x", mode);
    if (!desktop_up()) { skip(r, "no_desktop"); return; }
    if (mode > 0x7F && !display_fullscreen_supported(mode)) { skip(r, "unsupported"); return; }
    (void)wait_for(repainted, until, 8000);
    const struct fb_device original = *fbdev_get();
    struct biosvm_video_state before = {0}, after = {0};
    if (biosvm_video_state_snapshot) biosvm_video_state_snapshot(&before);
    uint64_t t0 = now();
    int e = display_fullscreen_begin(mode);
    uint64_t enter_ms = now() - t0, leave_ms = 0, repaint_ms = 0;
    struct display_selftest_info d = {0};
    if (display_selftest_info) display_selftest_info(&d);
    int end = 0;
    if (!e) {
        const struct fb_device *fb = fbdev_get();
        selftest_figures(r, "lfb=%u", fb->present);
        task_sleep_ms(500);
        t0 = now();
        end = display_fullscreen_end();
        leave_ms = now() - t0;
    }
    t0 = now();
    bool painted = wait_for(repainted, until, 300);
    repaint_ms = now() - t0;
    bool restored = same_layout(fbdev_get(), &original);
    if (biosvm_video_state_snapshot) biosvm_video_state_snapshot(&after);
    selftest_token(r->figures, sizeof(r->figures), "provider",
                   d.last_provider == 1 ? "native" : d.last_provider == 2 ? "firmware" : "none");
    selftest_figures(r, "result=%d refusal=%d enter_ms=%llu leave_ms=%llu end=%d restored=%u repaint_ms=%llu repainted=%u",
                     e, e == DISPLAY_FULLSCREEN_WINDOWED ? d.last_refusal : 0, (unsigned long long)enter_ms,
                     (unsigned long long)leave_ms, end, restored, (unsigned long long)repaint_ms, painted);
    if (after.queries != before.queries)
        selftest_figures(r, "state_bytes=%u state_query=%d state_save=%d state_restore=%d",
                         after.bytes, after.query_result, after.save_result, after.restore_result);
    if (!e && !end && restored && painted) pass(r);
    else fail(r, e == DISPLAY_FULLSCREEN_WINDOWED ? "refused" : e ? "recovery" : end ? "restore" :
                 !restored ? "not_restored" : "no_repaint");
}
CHECK(check_fs_101) { UNUSED; fullscreen_check(0x101, until, r); }
CHECK(check_fs_13) { UNUSED; fullscreen_check(0x13, until, r); }
CHECK(check_vbe_state)
{
    UNUSED;
    struct biosvm_video_state st = {0};
    if (biosvm_video_state_snapshot) biosvm_video_state_snapshot(&st);
    selftest_figures(r, "bytes=%u area=%u queries=%u saves=%u restores=%u query=%d save=%d restore=%d",
                     st.bytes, BIOSVM_VIDEO_STATE_SIZE, st.queries, st.saves, st.restores,
                     st.query_result, st.save_result, st.restore_result);
    if (!st.queries) skip(r, "firmware_state_unused");
    else if (!st.bytes || st.bytes > BIOSVM_VIDEO_STATE_SIZE) fail(r, "state_size");
    else if (st.query_result || st.save_result || st.restore_result || !st.saves || !st.restores) fail(r, "round_trip");
    else pass(r);
}

/* ---- optical --------------------------------------------------------- */
static const char *media_text(enum atapi_media m)
{
    return m == ATAPI_READY ? "ready" : m == ATAPI_NO_MEDIUM ? "no_medium" : m == ATAPI_NOT_READY ? "not_ready" :
           m == ATAPI_CHANGED ? "changed" : "unknown";
}
static int optical_slot(struct storage *s)
{
    for (unsigned i = 0; s && i < STORAGE_DISKS; i++) if (s->optical_bindings[i].device) return (int)i;
    return -1;
}
static struct ata_device *optical_device;
static bool media_known(void)
{
    return optical_device && optical_device->media != ATAPI_UNKNOWN && optical_device->media != ATAPI_CHANGED;
}
CHECK(check_optical_drive)
{
    UNUSED;
    struct storage *s = storage_get();
    int slot = optical_slot(s);
    unsigned drives = 0;
    for (unsigned i = 0; s && i < STORAGE_DISKS; i++) drives += s->optical_bindings[i].device != 0;
    if (slot < 0) { selftest_figures(r, "drives=0"); skip(r, "no_optical_drive"); return; }
    optical_device = s->optical_bindings[slot].device;
    struct ata_device *d = optical_device;
    bool known = wait_for(media_known, until, 300);
    selftest_figures(r, "drives=%u slot=%d channel=%u unit=%u packet=%u dma=%u quarantined=%u",
                     drives, slot, d->channel ? d->channel->index : 0, d->unit, d->packet_bytes,
                     d->dma_enabled, d->block.quarantined);
    selftest_token(r->figures, sizeof(r->figures), "model", d->model);
    selftest_token(r->figures, sizeof(r->figures), "firmware", d->firmware);
    /* fx-emptydrive: the letter the drive owns with or without a disc. */
    unsigned letter = s->optical_bindings[slot].drive;
    selftest_figures(r, "letter=%c", letter >= 3 && letter < 26 ? 'A' + letter : '-');
    selftest_token(r->figures, sizeof(r->figures), "media", media_text(d->media));
    selftest_token(r->figures, sizeof(r->figures), "ready_stage", d->ready_stage ? d->ready_stage : "-");
    selftest_figures(r, "ready_error=%d sense=%x/%x/%x packets=%llu",
                     d->ready_error, d->sense_key, d->asc, d->ascq, (unsigned long long)d->packet_commands);
    if (d->block.quarantined) fail(r, "quarantined");
    else if (!known) fail(r, "media_state_unknown");
    else pass(r);
}
static struct vfs_table optical_table;
static uint8_t optical_buffer[32768];
static int optical_drive_letter(struct storage *s, int slot)
{
    for (unsigned drive = 3; drive < 26; drive++)
        if (s->vfs.ro_volumes[drive] == &s->optical[slot]) return (int)drive;
    return -1;
}
static int mounted_slot = -1;
static bool optical_mounted(void) { return optical_drive_letter(storage_get(), mounted_slot) >= 0; }
CHECK(check_optical_disc)
{
    UNUSED;
    struct storage *s = storage_get();
    int slot = optical_slot(s);
    if (slot < 0) { skip(r, "no_optical_drive"); return; }
    struct ata_device *d = s->optical_bindings[slot].device;
    selftest_token(r->figures, sizeof(r->figures), "media", media_text(d->media));
    if (d->media != ATAPI_READY) { skip(r, "no_disc"); return; }
    const struct atapi_toc *t = &d->toc;
    selftest_figures(r, "toc=%u tracks=%u first=%u last=%u leadout=%u audio_only=%u first_audio=%u session=%u",
                     t->valid, t->count, t->first, t->last, t->leadout, t->audio_only, t->first_audio, d->session_lba);
    if (t->valid && t->first_audio) { pass(r); selftest_token(r->figures, sizeof(r->figures), "disc", "audio_cd"); return; }
    mounted_slot = slot;
    bool mounted = wait_for(optical_mounted, until, 5000);
    const struct fs_ro_volume *ro = &s->optical[slot];
    selftest_token(r->figures, sizeof(r->figures), "label", ro->volume_label);
    if (!mounted) {
        selftest_token(r->figures, sizeof(r->figures), "mount", ro->diagnostic ? ro->diagnostic : "not_mounted");
        selftest_figures(r, "poll_result=%d", s->optical_bindings[slot].result);
        fail(r, "iso_mount");
        return;
    }
    int drive = optical_drive_letter(s, slot);
    char letter[2] = { (char)('A' + drive), 0 };
    selftest_token(r->figures, sizeof(r->figures), "drive", letter);
    int e = vfs_table_init(&s->vfs, &optical_table, (unsigned)drive);
    if (e) { selftest_figures(r, "table=%d", e); fail(r, "vfs"); return; }
    char pattern[8] = "X:\\*.*", path[FS_NAME_BYTES + 8];
    pattern[0] = letter[0];
    struct vfs_find find; struct fat_entry entry;
    e = vfs_find_first(&optical_table, pattern, 0x21, &find, &entry);
    while (!e && (entry.attr & 0x10 || !entry.size)) e = vfs_find_next(&optical_table, &find, &entry);
    uint64_t total = 0, t0 = now();
    uint32_t sum = 2166136261u;
    if (!e) {
        ksnprintf(path, sizeof(path), "%c:\\%s", letter[0], entry.name);
        selftest_token(r->figures, sizeof(r->figures), "file", entry.name);
        int fd = vfs_open(&optical_table, path, VFS_READ, VFS_DENY_NONE, 0);
        if (fd < 0) e = fd;
        while (!e && total < 262144u && now() + 1000 < until) {
            size_t done = 0;
            e = vfs_read(&optical_table, fd, optical_buffer, sizeof(optical_buffer), &done);
            if (!e && !done) break;
            sum = fnv1a32(optical_buffer, done, sum);
            total += done;
        }
        if (fd >= 0) (void)vfs_close(&optical_table, fd);
    }
    uint64_t ms = now() - t0;
    vfs_table_destroy(&optical_table);
    selftest_figures(r, "read_error=%d bytes=%llu read_ms=%llu kib_s=%llu fnv=%08x", e, (unsigned long long)total,
                     (unsigned long long)ms, (unsigned long long)(ms ? total * 1000u / 1024u / ms : 0), sum);
    if (!e && total) pass(r); else fail(r, "file_read");
}

/* ---- power ----------------------------------------------------------- */
static struct apm_selftest apm_view;
static bool apm_finished(void) { apm_platform_selftest(&apm_view); return apm_view.finished || !apm_view.attempted; }
CHECK(check_apm)
{
    UNUSED;
    if (!apm_platform_selftest) { skip(r, "not_built"); return; }
    (void)wait_for(apm_finished, until, 500);
    apm_platform_selftest(&apm_view);
    struct apm_selftest *a = &apm_view;
    selftest_figures(r, "attempted=%u finished=%u state=%u connected=%u version=0x%04x flags=0x%04x caps_valid=%u caps=0x%04x units=%u error=%d bios_error=0x%02x function=0x%04x transport=%d",
                     a->attempted, a->finished, a->state, a->connected, a->version, a->flags, a->capabilities_valid,
                     a->capabilities, a->units, a->error, a->bios_error, a->last_function, a->transport_error);
    selftest_token(r->figures, sizeof(r->figures), "why", a->reason ? a->reason : "-");
    struct biosvm_failure f = {0};
    biosvm_last_failure(&f);
    if (f.error) {
        selftest_token(r->figures, sizeof(r->figures), "fault", f.reason ? f.reason : "error");
        selftest_figures(r, "fault_function=0x%x fault_at=%04x:%04x fault_port=0x%x fault_int=0x%x",
                         f.function, f.cs, f.ip, f.port, f.interrupt);
    }
    if (!a->attempted) skip(r, "not_started");
    else if (!a->finished) fail(r, "discovery_timeout");
    else if (a->connected) pass(r);
    else if (a->state == 1 /* APM_ABSENT */) skip(r, "no_apm_bios");
    else if (a->state == 2 /* APM_EXCLUDED */) skip(r, "acpi_owner");
    else fail(r, "not_connected");
}
CHECK(check_acpi_s5)
{
    UNUSED;
    if (!g_boot.acpi_rsdp_phys) { skip(r, "no_acpi"); return; }
    bool qualified = power_s5_fallback_qualified ? power_s5_fallback_qualified() : false;
    const char *state = power_s5_fallback_state ? power_s5_fallback_state() : "not_built";
    struct acpi_soft_off o = {0};
    bool have = power_s5_fallback_data && power_s5_fallback_data(&o);
    selftest_figures(r, "rsdp=0x%x qualified=%u parsed=%u", g_boot.acpi_rsdp_phys, qualified, have);
    selftest_token(r->figures, sizeof(r->figures), "state", state);
    if (have) selftest_figures(r, "pm1a=0x%x pm1b=0x%x slp_typa=%u slp_typb=%u source=%u fadt_rev=%u smi_cmd=0x%x acpi_enable=0x%x sci=%u fadt_flags=0x%x",
                               o.pm1a_cnt, o.pm1b_cnt, o.slp_typa, o.slp_typb, o.source, o.fadt_revision,
                               o.smi_cmd, o.acpi_enable, o.sci_int, o.flags);
    if (power_source_snapshot) {
        struct power_source_snapshot p; power_source_snapshot(&p);
        selftest_figures(r, "provider=%u active=%u ac=%d battery=%d", p.provider, p.active,
                         (int)p.status.ac, (int)p.status.percent);
    }
    if (have && o.pm1a_cnt) pass(r); /* read-only: never enters S5 */
    else if (!strncmp(state, "policy", 7)) skip(r, "policy");
    else fail(r, "no_s5_data");
}
CHECK(check_ec)
{
    UNUSED;
    if (!thinkpad_ec_smbios_snapshot) { skip(r, "not_built"); return; }
    static struct thinkpad_ec_smbios id;
    thinkpad_ec_smbios_snapshot(&id);
    int poll = thinkpad_ec_poll_state ? thinkpad_ec_poll_state() : -1;
    selftest_token(r->figures, sizeof(r->figures), "verdict", id.verdict ? id.verdict : "-");
    selftest_token(r->figures, sizeof(r->figures), "vendor", id.vendor);
    selftest_token(r->figures, sizeof(r->figures), "product", id.product);
    selftest_token(r->figures, sizeof(r->figures), "bios", id.bios_version);
    selftest_figures(r, "smbios=%u eligible=%u poll_ms=%d volume_in_ec=%u master=%u/%u mute=%u",
                     id.valid, id.eligible, poll, audio.master_in_hardware, audio.master.left, audio.master.right,
                     audio.master.mute);
    if (!id.eligible) skip(r, "not_a_thinkpad");
    else if (poll < 0) fail(r, "ec_absent");
    else pass(r);
}

/* ---- audio ----------------------------------------------------------- */
CHECK(check_audio_device)
{
    UNUSED;
    selftest_token(r->figures, sizeof(r->figures), "backend", audio.backend_name ? audio.backend_name : "-");
    selftest_figures(r, "ready=%u touched=%u pci=%04x:%04x error=%d init_error=%d output_rate=%u quarantined=%u",
                     audio.ready, audio.backend_touched, audio.backend_pci.vendor, audio.backend_pci.device,
                     audio.error, audio.init_error, audio_output_rate(), audio.quarantined);
    selftest_token(r->figures, sizeof(r->figures), "state", audio.reason ? audio.reason : "-");
    if (audio.ready) pass(r);
    else if (!audio.backend_touched) skip(r, "no_device");
    else fail(r, "not_ready");
}
static struct ciuki_audio_frame_zero { int16_t l, r; } silence[512];
CHECK(check_audio_stream)
{
    UNUSED;
    if (!audio.ready) { skip(r, "no_device"); return; }
    struct audio_stream *s = 0;
    int e = audio_stream_open(&s);
    unsigned total = AUDIO_RATE / 2, sent = 0; /* 0.5 s of digital silence */
    uint64_t underruns = audio.underruns, t0 = now();
    struct audio_transport before = {0}, after = {0};
    audio_backend_snapshot(&before);
    if (!e) { s->draining = true; s->drain_goal = total; }
    while (!e && sent < total) {
        if (now() + 1000 >= until) { e = -ETIMEDOUT; break; }
        unsigned count = total - sent;
        if (count > ARRAY_SIZE(silence)) count = ARRAY_SIZE(silence);
        int wrote = audio_stream_enqueue(s, silence, count * sizeof(silence[0]));
        if (wrote > 0) sent += (unsigned)wrote / sizeof(silence[0]);
        else if (wrote != -EAGAIN) e = wrote;
        if (!e && sent < total) task_sleep_ms(1);
    }
    while (!e && (!s->drain_latched || audio.played_output < s->drain_output)) {
        if (audio.error) e = audio.error;
        else if (now() + 500 >= until) e = -ETIMEDOUT;
        else task_sleep_ms(1);
    }
    if (s) audio_stream_close(s);
    audio_backend_snapshot(&after);
    selftest_figures(r, "error=%d frames=%u stream_ms=%llu underruns=%llu irqs=%llu lead=%u",
                     e, sent, (unsigned long long)(now() - t0), (unsigned long long)(audio.underruns - underruns),
                     (unsigned long long)(after.irqs - before.irqs), audio.lead_periods);
    if (!e && sent == total) pass(r); else fail(r, "stream");
}
CHECK(check_midi)
{
    UNUSED;
    if (!audio.ready) { skip(r, "no_device"); return; }
    struct audio_stream *s = 0;
    int e = audio_stream_open(&s);
    /* MIDI 1.0 Channel Mode message: Control Change 123 All Notes Off. */
    struct ciuki_midi_data m = { .version = 1, .size = sizeof(m), .bytes = 3, .data = { 0xB0, 0x7B, 0x00 } };
    uint64_t t0 = now();
    if (!e) e = audio_midi_write(s, &m);
    bool allocated = !e && s && s->midi;
    while (!e && audio_midi_active(s) && now() + 300 < until) task_sleep_ms(1);
    bool consumed = allocated && !audio_midi_active(s);
    if (s) { audio_midi_close(s); audio_stream_close(s); }
    selftest_figures(r, "error=%d allocated=%u accepted=%u consumed=%u midi_ms=%llu output_rate=%u",
                     e, allocated, m.accepted, consumed, (unsigned long long)(now() - t0), audio_output_rate());
    if (e == -EOPNOTSUPP) skip(r, "output_rate");
    else if (!e && allocated && consumed) pass(r);
    else fail(r, e == -ENOMEM ? "allocation" : "synth");
}

/* ---- network --------------------------------------------------------- */
static struct ciuki_net_state net_view;
static bool net_addressed(void)
{
    net_state_snapshot(&net_view);
    return net_view.address_configured || net_view.state != CIUKI_NET_RUNNING || net_view.link != CIUKI_NET_LINK_UP;
}
static void net_figures(struct selftest_result *r)
{
    char mac[24];
    ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", net_view.mac[0], net_view.mac[1],
              net_view.mac[2], net_view.mac[3], net_view.mac[4], net_view.mac[5]);
    char driver[20]; unsigned k = 0;
    for (; k < sizeof(driver) - 1 && k < sizeof(net_view.driver) && net_view.driver[k]; k++) driver[k] = net_view.driver[k];
    driver[k] = 0;
    char why[28]; k = 0;
    for (; k < sizeof(why) - 1 && k < sizeof(net_view.reason) && net_view.reason[k]; k++) why[k] = net_view.reason[k];
    why[k] = 0;
    selftest_token(r->figures, sizeof(r->figures), "driver", driver);
    selftest_token(r->figures, sizeof(r->figures), "mac", mac);
    selftest_token(r->figures, sizeof(r->figures), "state_reason", why);
    selftest_figures(r, "present=%u state=%u link=%u rx=%llu tx=%llu", net_view.present, net_view.state,
                     net_view.link, (unsigned long long)net_view.rx_frames, (unsigned long long)net_view.tx_frames);
}
CHECK(check_nic)
{
    UNUSED;
    if (!net_state_snapshot) { skip(r, "not_built"); return; }
    net_state_snapshot(&net_view);
    net_figures(r);
    if (!net_view.present) skip(r, "no_adapter");
    else if (net_view.state == CIUKI_NET_QUARANTINED) fail(r, "quarantined");
    else if (net_view.state != CIUKI_NET_RUNNING) skip(r, "disabled");
    else pass(r);
}
CHECK(check_dhcp)
{
    UNUSED;
    if (!net_state_snapshot) { skip(r, "not_built"); return; }
    struct ciuki_net_config cfg = {0};
    if (net_settings_get) net_settings_get(0, &cfg);
    uint64_t t0 = now();
    net_state_snapshot(&net_view);
    if (net_view.present && net_view.state == CIUKI_NET_RUNNING && net_view.link == CIUKI_NET_LINK_UP && cfg.dhcp)
        (void)wait_for(net_addressed, until, 300);
    net_state_snapshot(&net_view);
    int dhcp_error = net_stack_dhcp_error ? net_stack_dhcp_error(0) : 0;
    selftest_figures(r, "link=%u dhcp=%u wait_ms=%llu address=%u.%u.%u.%u dhcp_error=%d",
                     net_view.link, cfg.dhcp, (unsigned long long)(now() - t0), net_view.ipv4[0], net_view.ipv4[1],
                     net_view.ipv4[2], net_view.ipv4[3], dhcp_error);
    if (!net_view.present || net_view.state != CIUKI_NET_RUNNING) skip(r, "no_adapter");
    else if (net_view.link != CIUKI_NET_LINK_UP) skip(r, "link_down");
    else if (!cfg.dhcp) skip(r, "static_address");
    else if (net_view.address_configured) pass(r);
    else fail(r, "no_lease");
}

/* ---- USB and ATA ----------------------------------------------------- */
static bool usb_scanned(void)
{
    for (unsigned i = 0; i < usb_count(); i++) {
        const struct usb_host *h = usb_get(i);
        if (h && h->ready && !h->quarantined && !h->scanned) return false;
    }
    return true;
}
static const char *usb_kind(const struct usb_host *h)
{ return h->pci.prog_if == 0x00 ? "uhci" : h->pci.prog_if == 0x10 ? "ohci" : h->pci.prog_if == 0x20 ? "ehci" : "other"; }
CHECK(check_usb_controllers)
{
    UNUSED;
    if (!usb_count || !usb_get) { skip(r, "not_built"); return; }
    (void)wait_for(usb_scanned, until, 300);
    unsigned n = usb_count(), ready = 0, quarantined = 0, kinds[4] = {0};
    for (unsigned i = 0; i < n; i++) {
        const struct usb_host *h = usb_get(i);
        if (!h) continue;
        ready += h->ready; quarantined += h->quarantined;
        const char *k = usb_kind(h);
        kinds[k[0] == 'u' ? 0 : k[0] == 'o' ? 1 : k[0] == 'e' ? 2 : 3]++;
        selftest_detail(c->id, "index=%u kind=%s pci=%04x:%04x ready=%u quarantined=%u ports=%u devices=%u scanned=%u irq=%u",
                        i, k, h->pci.vendor, h->pci.device, h->ready, h->quarantined, h->ports, h->device_count,
                        h->scanned, h->irq_attached);
    }
    selftest_figures(r, "controllers=%u uhci=%u ohci=%u ehci=%u other=%u ready=%u quarantined=%u",
                     n, kinds[0], kinds[1], kinds[2], kinds[3], ready, quarantined);
    if (!n) skip(r, "no_controller");
    else if (quarantined) fail(r, "quarantined");
    else pass(r);
}
CHECK(check_usb_devices)
{
    UNUSED;
    if (!usb_count || !usb_get) { skip(r, "not_built"); return; }
    unsigned present = 0, quarantined = 0, configured = 0;
    for (unsigned i = 0; i < usb_count(); i++) {
        const struct usb_host *h = usb_get(i);
        if (!h) continue;
        for (unsigned j = 0; j < h->device_count; j++) {
            const struct usb_device *d = &h->devices[j];
            if (!d->present) continue;
            present++; quarantined += d->quarantined; configured += d->configured;
            selftest_detail(c->id, "host=%u address=%u id=%04x:%04x class=%u speed=%s configured=%u quarantined=%u",
                            i, d->address, d->vendor, d->product, d->class_code,
                            d->high_speed ? "high" : d->low_speed ? "low" : "full", d->configured, d->quarantined);
        }
    }
    selftest_figures(r, "devices=%u configured=%u quarantined=%u", present, configured, quarantined);
    if (!present) skip(r, "no_device");
    else if (quarantined) fail(r, "quarantined");
    else pass(r);
}
CHECK(check_ata)
{
    UNUSED;
    if (!ata_inventory_get) { skip(r, "not_built"); return; }
    unsigned disks = 0, atapi = 0, dma = 0, bad = 0;
    for (unsigned ch = 0; ch < ATA_CHANNELS; ch++)
        for (unsigned unit = 0; unit < ATA_DEVICES; unit++) {
            const struct ata_device *d = ata_inventory_get(ch, unit);
            if (!d) continue;
            disks += !d->atapi; atapi += d->atapi; dma += d->dma_enabled;
            bad += d->dma_quarantined || d->block.quarantined;
            char model[sizeof(d->model)]; unsigned k = 0;
            for (; k < sizeof(model) - 1 && d->model[k]; k++) model[k] = d->model[k];
            model[k] = 0;
            char tokens[SELFTEST_FIGURES] = "";
            selftest_token(tokens, sizeof(tokens), "model", model);
            selftest_detail(c->id, "channel=%u unit=%u atapi=%u lba48=%u dma=%u udma=%d firmware_udma=%d dma_quarantined=%u quarantined=%u multiple=%u commands=%llu dma_errors=%llu dma_retries=%u early_irqs=%u %s",
                            ch, unit, d->atapi, d->lba48, d->dma_enabled, d->dma_enabled ? (int)d->udma_mode : -1,
                            d->firmware_udma, d->dma_quarantined, d->block.quarantined, d->multiple_sectors,
                            (unsigned long long)d->commands, (unsigned long long)d->dma_errors, d->dma_retries,
                            d->early_irqs, tokens);
            if (!d->atapi && disks == 1)
                selftest_figures(r, "disk0_dma=%u disk0_udma=%d disk0_errors=%llu", d->dma_enabled,
                                 d->dma_enabled ? (int)d->udma_mode : -1, (unsigned long long)d->dma_errors);
        }
    selftest_figures(r, "disks=%u atapi=%u dma_devices=%u faulted=%u", disks, atapi, dma, bad);
    if (!disks && !atapi) skip(r, "no_ata_device");
    else if (bad) fail(r, "quarantined");
    else pass(r);
}

/* Phase 1: before the desktop (display mode sets need the display without a
 * grant). Phase 2: with the desktop; ordered so that the 3D qualification
 * finishes before any mode change, and the APM fault record (biosvm keeps
 * only the last failure) is read before the firmware video calls. */
/* ---- sec-k08: the account verifier KDF on this CPU, for the lead's runtime
 * gate (user-security-kernel.md, account verifiers: target <= 1000 ms per
 * verification on the Pentium III baseline, measured, never tuned here; the
 * PBKDF2 work factor is the ACCOUNT service's). Five derivations of a fixed
 * non-secret input while the desktop runs, 10 ms apart. ---- */
CHECK(check_kdf)
{
    UNUSED;
    static const char key[] = "selftest-not-a-password";
    static const uint8_t salt[16] = {'C','i','u','k','i','O','S','-','s','e','l','f','t','e','s','t'};
    uint32_t ms[5]; uint8_t out[32]; unsigned runs = 0;
    for (; runs < 5; runs++) {
        if (runs) {
            if (now() + 10 + 2u * ms[runs - 1] > until) break;   /* the next one would overrun the bound */
            task_sleep_ms(10);
        }
        uint64_t t0 = now();
        if (pbkdf2_hmac_sha256(key, sizeof(key) - 1, salt, sizeof(salt), CIUKI_ACCOUNT_KDF_ITERATIONS, out)) { fail(r, "kdf_error"); return; }
        ms[runs] = (uint32_t)(now() - t0);
    }
    for (unsigned i = 1; i < runs; i++)
        for (unsigned j = i; j && ms[j - 1] > ms[j]; j--) { uint32_t t = ms[j]; ms[j] = ms[j - 1]; ms[j - 1] = t; }
    selftest_figures(r, "iterations=%u runs=%u min_ms=%u median_ms=%u max_ms=%u target_ms=1000 out=%02x%02x%02x%02x",
                     CIUKI_ACCOUNT_KDF_ITERATIONS, runs, ms[0], ms[runs / 2], ms[runs - 1], out[0], out[1], out[2], out[3]);
    if (runs < 5) fail(r, "bound");
    else if (ms[2] > 1000) fail(r, "over_target");
    else pass(r);
}
/* ---- end sec-k08 ---- */
const struct selftest_check selftest_phase1[] = {
    { "serial.uart", 2000, check_serial },
    { "memory.start", 2000, check_memory_start },
    { "display.info", 2000, check_display_info },
    { "display.mode_640x480x8", 12000, check_mode_8 },
    { "display.mode_640x480x32", 12000, check_mode_32 },
};
const unsigned selftest_phase1_count = ARRAY_SIZE(selftest_phase1);
const struct selftest_check selftest_phase2[] = {
    { "desktop.start", 90000, check_desktop },
    { "power.apm", 25000, check_apm },
    { "power.acpi_s5", 10000, check_acpi_s5 },
    { "power.thinkpad_ec", 3000, check_ec },
    { "display.3d_qualification", 60000, check_3d },
    { "display.gl_draw", 10000, check_gl },
    { "display.2d_engine", 3000, check_2d },
    { "display.fs_640x480x8", 20000, check_fs_101 },
    { "display.fs_320x200x8", 20000, check_fs_13 },
    { "display.vbe_state", 1000, check_vbe_state },
    { "dos.window", 90000, selftest_dos_window },
    { "dos.fullscreen_13h", 25000, selftest_dos_mode13 },
    { "dos.fullscreen_101h", 25000, selftest_dos_vbe101 },
    { "dos.return", 30000, selftest_dos_return },
    { "optical.drive", 25000, check_optical_drive },
    { "optical.disc", 40000, check_optical_disc },
    { "audio.device", 2000, check_audio_device },
    { "audio.silent_stream", 6000, check_audio_stream },
    { "audio.midi", 4000, check_midi },
    { "network.nic", 2000, check_nic },
    { "network.dhcp", 30000, check_dhcp },
    { "usb.controllers", 10000, check_usb_controllers },
    { "usb.devices", 2000, check_usb_devices },
    { "storage.ata", 2000, check_ata },
    { "security.kdf", 15000, check_kdf },   /* sec-k08 */
    { "serial.records", 1000, check_serial_records },
    { "memory.end", 3000, check_memory_end },
};
const unsigned selftest_phase2_count = ARRAY_SIZE(selftest_phase2);
