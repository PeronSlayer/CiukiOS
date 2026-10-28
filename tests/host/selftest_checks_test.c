/* fx-selftest check table on the host: the production checks
 * (core/selftest_checks.c, vm/dosvm_selftest.c) and model run against
 * fake subsystems on a fake millisecond clock, in two machines:
 *   bare - no framebuffer, desktop, UART, ACPI/APM, optical, audio, NIC,
 *          USB or ATA: every device check SKIPs, only desktop.start fails;
 *   t23  - the owner's ThinkPad T23 profile (S3 SuperSavage 5333:8c2e,
 *          1024x768 panel, ATAPI CD on the secondary channel without a
 *          disc, ICH3-M UHCI, AC'97, APM 1.2 + ACPI 1.0, a DOS box that
 *          runs FSTEST.COM): every check PASSes but optical.disc (no disc).
 * Every check stays inside its bound, opens no file writable and keeps the
 * DOS box's overlay rule. argv[1] receives the t23 SELFTEST.LOG and serial
 * capture for the parser tests. SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
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
#include <ciuki/dosvm.h>
#include <ciuki/process.h>
#include <ciuki/selftest.h>
#include <ciuki/security.h>
#include <ciuki/desktop.h>
#include "dosvm_internal.h"

/* <stdlib.h> clashes with the kernel ABI's wait-status names. */
void exit(int) __attribute__((noreturn));

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
struct audio_state audio;
static bool t23, desktop_running, no_3d;

/* ---- report sinks (as probes/selftest.c, without the disk) -------------- */
static char log_text[65536], serial_out[131072];
static unsigned seq;
static uint64_t op_now(void *ctx) { (void)ctx; return g_ticks; }
static void append(char *buf, size_t size, const char *s)
{
    size_t used = strlen(buf), n = strlen(s);
    CHECK(used + n + 1 < size);
    memcpy(buf + used, s, n + 1);
}
static void op_line(void *ctx, const char *line) { (void)ctx; append(log_text, sizeof(log_text), line); append(log_text, sizeof(log_text), "\n"); }
static void op_record(void *ctx, const char *event, const char *fields)
{
    (void)ctx;
    char line[300];
    int n = snprintf(line, sizeof(line), "CIUKI_TEST v=1 run=0badf00d seq=%06u probe=selftest event=%s%s%s",
                     ++seq, event, *fields ? " " : "", fields);
    CHECK(n > 0 && n <= 240);
    append(serial_out, sizeof(serial_out), line);
    append(serial_out, sizeof(serial_out), "\n");
}
static const struct selftest_ops ops = { 0, op_now, op_line, op_record, 0 };
static char details[SELFTEST_DETAIL_BYTES];
static size_t details_used;
void selftest_detail(const char *id, const char *fmt, ...)
{
    char tokens[SELFTEST_FIGURES];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(tokens, sizeof(tokens), fmt, ap);
    va_end(ap);
    selftest_model_detail(&ops, details, sizeof(details), &details_used, id, tokens);
}

/* ---- fake DOS box: COMMAND.COM prompt and FSTEST.COM's three stages ---- */
static struct dosvm fake_vm;
static bool dosbox_requested, vm_live_flag, vm_created, fs_active, overlay_exists;
static uint64_t dosbox_at;
static enum { DOS_PROMPT, DOS_STAGE1, DOS_STAGE2 } dos_stage;
static uint8_t dos_mode;
static char typed[64];
static unsigned typed_len, fullscreen_entries, fsprog_runs;
static uint16_t text_cells[80 * 25];
static const struct dosvm_key vm_key = { .id = 1, .generation = 7 };
static void draw_prompt(const char *before)
{
    for (unsigned i = 0; i < 80 * 25; i++) text_cells[i] = 0x0720;
    const char *rows[2] = { before, "C:\\APPS>" };
    for (unsigned r = 0; r < 2; r++)
        for (unsigned c = 0; rows[r] && rows[r][c]; c++) text_cells[r * 80 + c] = (uint16_t)(0x0700 | (uint8_t)rows[r][c]);
}
void selftest_request_dosbox(void) { dosbox_requested = true; dosbox_at = g_ticks; }
void selftest_device_running(void) { dosbox_requested = false; }
static void machine_tick(void)
{
    if (t23 && desktop_running && dosbox_requested && !vm_created && g_ticks - dosbox_at >= 1500) {
        vm_created = vm_live_flag = overlay_exists = true;
        memset(&fake_vm, 0, sizeof(fake_vm));
        fake_vm.info.key = vm_key;
        fake_vm.kernel = 2; fake_vm.window_input = true; fake_vm.overlay_created = true;
        snprintf(fake_vm.overlay_path, sizeof(fake_vm.overlay_path), "C:\\SYSTEM\\DOS\\VM%08x.COW", vm_key.generation);
        dos_stage = DOS_PROMPT; dos_mode = 3;
        draw_prompt("Ciuki DOS");
    }
}
void task_sleep_ms(uint32_t ms) { g_ticks += ms ? ms : 1; machine_tick(); }

/* ---- sec-user-06: the security figures ---------------------------------
 * The desktop answers the registry-backup request 1.5 s later (an
 * administrator's backup of 1180 ms); the secure attention conceals after
 * 400 ms (neutral 35 ms, trusted frame 410 ms) and LOGON's automatic menu
 * return restores the session 3.6 s after it (restore 220 ms). The session
 * activation at logon is restoration 1. */
static bool backup_requested, attention_sent;
static uint64_t backup_at, attention_at;
static struct grants_console_timing gate;
bool security_roles_active(void) { return t23; }
int input_selftest_attention(void) { CHECK(t23 && desktop_running); attention_sent = true; attention_at = g_ticks; return 0; }
void grants_console_timing(struct grants_console_timing *out)
{
    if (attention_sent && g_ticks - attention_at >= 400 && !gate.concealments) {
        gate.concealments = 1; gate.last_neutral_ms = gate.max_neutral_ms = 35; gate.last_trusted_ms = gate.max_trusted_ms = 410;
    }
    if (attention_sent && g_ticks - attention_at >= 3600 && gate.restorations == 1) {
        gate.restorations = 2; gate.last_restore_ms = 220; if (gate.max_restore_ms < 220) gate.max_restore_ms = 220;
    }
    *out = gate;
}
void selftest_request_registry_backup(void) { CHECK(t23 && desktop_running); backup_requested = true; backup_at = g_ticks; }
bool selftest_figure(const char *name, char *tokens, size_t size)
{
    if (!backup_requested || g_ticks - backup_at < 1500 || strcmp(name, "registry_backup")) return false;
    snprintf(tokens, size, "backup_ms=1180 result=0 bytes=262144 files=9");
    return true;
}
struct dosvm *dosvm_client_first(uint32_t *owner) { if (!vm_live_flag) return 0; if (owner) *owner = 42; return &fake_vm; }
void dosvm_client_poll(void) {}
struct dosvm_key dosvm_key(const struct dosvm *v) { return v->info.key; }
bool dosvm_valid(struct dosvm_key k) { return vm_live_flag && k.id == vm_key.id && k.generation == vm_key.generation; }
struct dosvm_key dosvm_focused_key(void) { return vm_live_flag ? vm_key : (struct dosvm_key){0}; }
void dosvm_info(const struct dosvm *v, struct dosvm_info *out)
{
    memset(out, 0, sizeof(*out));
    out->key = v->info.key; out->dos_ready = true; out->dos_major = 7; out->dos_minor = 10; out->free_paragraphs = 0x9000;
}
bool dosvm_video_info(struct dosvm_key k, struct dosvm_video_info *out)
{
    if (!dosvm_valid(k)) return false;
    memset(out, 0, sizeof(*out)); out->mode = dos_mode; return true;
}
const uint16_t *dosvm_text(const struct dosvm *v, unsigned *cursor) { (void)v; if (cursor) *cursor = 88; return dos_mode == 3 ? text_cells : 0; }
bool dosvm_keyboard(struct dosvm_key k, uint16_t bios_key)
{
    if (!dosvm_valid(k)) return false;
    if (bios_key != 0x1C0D) {
        CHECK(dos_stage == DOS_PROMPT && typed_len + 1 < sizeof(typed));
        typed[typed_len++] = (char)bios_key; typed[typed_len] = 0;
        return true;
    }
    if (dos_stage == DOS_STAGE1) { dos_stage = DOS_STAGE2; fake_vm.vbe_mode = 0x101; }
    else if (dos_stage == DOS_STAGE2) { dos_stage = DOS_PROMPT; fake_vm.vbe_mode = 0; dos_mode = 3; draw_prompt("FSTEST done"); }
    else if (!strcmp(typed, "\\FSTEST.COM")) { dos_stage = DOS_STAGE1; dos_mode = 0x13; fsprog_runs++; }
    else if (!strcmp(typed, "EXIT")) { vm_live_flag = false; overlay_exists = false; }
    typed_len = 0; typed[0] = 0;
    return true;
}
int dosvm_fullscreen_enter(struct dosvm_key k)
{
    if (!dosvm_valid(k)) return -ESRCH;
    CHECK(dos_mode == 0x13);           /* FSTEST holds 13h or VBE 101h */
    fs_active = true; fullscreen_entries++;
    return 0;
}
int dosvm_fullscreen_leave(struct dosvm_key k, bool background) { (void)k; CHECK(!background); fs_active = false; return 0; }
bool dosvm_fullscreen_active(struct dosvm_key k) { return dosvm_valid(k) && fs_active; }
void dosvm_snapshot(struct dosvm_ledger *out) { memset(out, 0, sizeof(*out)); out->vms = vm_live_flag; out->pages = vm_live_flag ? 160 : 0; }
struct process *proc_find(uint32_t pid) { (void)pid; return 0; }
void proc_stop(struct process *p, int raw, uint32_t signal) { (void)p; (void)raw; (void)signal; CHECK(!"forced stop"); }

/* ---- storage: read-only views only ------------------------------------ */
static struct storage fake_storage;
static struct fat_volume *fake_c = (struct fat_volume *)&fake_storage;
static unsigned opens;
struct storage *storage_get(void) { return &fake_storage; }
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive) { (void)v; (void)t; CHECK(drive == 2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{
    (void)t; (void)share; (void)attr;
    CHECK(flags == VFS_READ);          /* no check opens a file writable */
    opens++;
    return !strcmp(path, fake_vm.overlay_path) && overlay_exists ? 5 : -FS_ENOENT;
}
int vfs_close(struct vfs_table *t, int fd) { (void)t; (void)fd; return 0; }
int vfs_read(struct vfs_table *t, int fd, void *b, size_t n, size_t *done) { (void)t; (void)fd; (void)b; (void)n; *done = 0; return -FS_EIO; }
int vfs_find_first(struct vfs_table *t, const char *p, uint8_t m, struct vfs_find *f, struct fat_entry *e)
{ (void)t; (void)p; (void)m; (void)f; (void)e; return -FS_ENOENT; }
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *e) { (void)t; (void)f; (void)e; return -FS_ENOENT; }
uint32_t fnv1a32(const void *data, size_t len, uint32_t h)
{ const uint8_t *p = data; for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u; return h; }
static struct ata_channel primary = { .index = 0 }, secondary = { .index = 1 };
static struct ata_device disk = { .channel = &primary, .present = true, .lba48 = true, .dma_enabled = true, .udma_mode = 5,
                                  .firmware_udma = 5, .model = "IBM-DJSA-220", .commands = 4000 };
static struct ata_device cd = { .channel = &secondary, .present = true, .atapi = true, .model = "TOSHIBA_DVD-ROM_SD-C2502",
                                .firmware = "1105", .packet_bytes = 12, .media = ATAPI_NO_MEDIUM, .ready_stage = "tur",
                                .sense_key = 2, .asc = 0x3a };
const struct ata_device *ata_inventory_get(unsigned ch, unsigned unit)
{
    if (!t23 || unit) return 0;
    return ch == 0 ? &disk : ch == 1 ? &cd : 0;
}

/* ---- display service --------------------------------------------------- */
static struct fb_device fb;
static struct fb_device saved_fb;
static struct biosvm_video_state vbe_state;
static bool frame_pending;
static const struct display_driver savage = { .id = "s3savage", .name = "S3 SuperSavage" };
static struct pci_func savage_pci = { .vendor = 0x5333, .device = 0x8c2e };
const struct fb_device *fbdev_get(void) { return &fb; }
const struct display_driver *display_driver_bound(void) { return t23 ? &savage : 0; }
const struct pci_func *display_driver_owner(void) { return t23 ? &savage_pci : 0; }
int display_check_begin(uint16_t id)
{
    CHECK(!desktop_running);           /* mode checks run before the desktop */
    for (unsigned i = 0; i < g_boot.display.mode_count; i++)
        if (g_boot.display.modes[i].id == id) {
            saved_fb = fb;
            fb.width = g_boot.display.modes[i].width; fb.height = g_boot.display.modes[i].height;
            fb.bpp = (uint8_t)g_boot.display.modes[i].bpp; fb.pitch = g_boot.display.modes[i].pitch;
            fb.mode_id = id; fb.generation++;
            g_ticks += 120;
            return 0;
        }
    return -EOPNOTSUPP;
}
int display_check_end(void) { uint32_t generation = fb.generation; fb = saved_fb; fb.generation = generation + 1; g_ticks += 90; return 0; }
bool display_fullscreen_supported(uint16_t mode) { return t23 && (mode == 0x101 || mode == 0x13); }
int display_fullscreen_begin(uint16_t mode)
{
    CHECK(desktop_running && t23 && (mode == 0x101 || mode == 0x13));
    vbe_state.queries++; vbe_state.saves++; vbe_state.bytes = 3776;
    g_ticks += 180;
    return 0;
}
int display_fullscreen_end(void) { vbe_state.restores++; frame_pending = true; g_ticks += 150; return 0; }
void display_selftest_info(struct display_selftest_info *out)
{
    if (frame_pending) frame_pending = false;  /* the desktop repaints on the next look */
    *out = (struct display_selftest_info){ .desktop_frames = desktop_running ? 40 : 0, .last_provider = 2 };
}
void biosvm_video_state_snapshot(struct biosvm_video_state *out) { *out = vbe_state; }
void biosvm_last_failure(struct biosvm_failure *out) { memset(out, 0, sizeof(*out)); }
void display_driver_info(struct ciuki_display_engine_info *info)
{
    uint32_t version = info->version, size = info->size;
    memset(info, 0, sizeof(*info)); info->version = version; info->size = size;
    if (!t23) return;
    info->kind = CIUKI_ENGINE_NATIVE; info->ready = info->native_used = info->cpu_ready = 1;
    snprintf(info->driver_id, sizeof(info->driver_id), "s3savage");
    snprintf(info->reason, sizeof(info->reason), "active");
    for (unsigned i = 0; i < CIUKI_ENGINE_OPERATIONS; i++) info->native[i] = 100 + i;
}
bool display_accel_disabled(const char **reason) { (void)reason; return false; }
void render3d_qualification_report(struct render3d_qualification_report *out)
{
    memset(out, 0, sizeof(*out));
    out->present = t23;                  /* the T23's adapter has the qualified backend */
    if (!t23 || no_3d) return;           /* no_3d: the 2D activation never scheduled it */
    *out = (struct render3d_qualification_report){ .present = true, .scheduled = true, .finished = true, .qualified = true, .fullsize = true,
        .perspective = true, .depth24 = true, .color565 = true, .result = "qualified",
        .reason = "pixel_depth_copy_guards_restore", .marker = "clear", .elapsed_us = 2400000, .packets = 3100,
        .triangles = 1800, .copies = 12, .heap_bytes = 4194304, .required = 3145728, .free_vram = 5242880,
        .largest_vram = 4194304, .texture_bytes = 1048576, .phase_count = 5 };
    static const char *const names[] = { "prepare", "marker_begin", "pixel", "depth", "marker_clear" };
    for (unsigned i = 0; i < 5; i++) { out->phases[i].name = names[i]; out->phases[i].us = 1000 * (i + 1); }
}
int w32_render3d_selftest(struct render3d_selftest *out, uint32_t timeout_ms)
{
    CHECK(timeout_ms && timeout_ms <= 8000);
    *out = (struct render3d_selftest){ .width = 64, .height = 64, .expected = 0xFF00FF00u, .stage = t23 ? "done" : "context" };
    if (!t23) return out->error = -EOPNOTSUPP;
    out->hardware = true; out->actual = out->expected; out->elapsed_ms = 18; g_ticks += 18;
    return 0;
}

/* ---- serial, memory, desktop ------------------------------------------ */
void serial_snapshot(struct serial_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->present = t23; out->probe_result = t23 ? SERIAL_PROBE_OK : SERIAL_PROBE_UNKNOWN;
    out->accepted = out->sent = seq * 180u;
}
unsigned serial_fifo_size(void) { return t23 ? 16 : 0; }
/* COM1 from the BIOS data area, as com_bda_discover admits it on the T23. */
bool com_snapshot(unsigned i, struct ciuki_com_status *out, const char **reason)
{
    if (!t23 || i) return false;
    *out = (struct ciuki_com_status){ .number = 1, .base = 0x3f8, .irq = 4, .origin = CIUKI_COM_BDA, .flags = CIUKI_COM_READY };
    if (reason) *reason = "Qualified UART; exclusive native COM available.";
    return true;
}
bool ledger_reconcile(void) { return true; }
void ledger_snapshot(struct mm_ledger *out) { memset(out, 0, sizeof(*out)); out->zone[ZONE_NORMAL].free = 30000; out->zone[ZONE_DMA].free = 2000; }
void kheap_snapshot(struct kheap_ledger *out) { *out = (struct kheap_ledger){ .pages = 96, .in_use = 200000, .peak = 260000 }; }
uint32_t pmm_free_count(void) { return 32000; }
uint32_t pmm_total_usable(void) { return 65000; }
uint32_t pmm_refusals(void) { return 0; }
uint32_t supervisor_desktop_pid(void) { return desktop_running ? 77 : 0; }
uint32_t supervisor_desktop_deaths(void) { return 0; }
uint32_t supervisor_desktop_restarts(void) { return 0; }

/* ---- power ------------------------------------------------------------- */
void apm_platform_selftest(struct apm_selftest *out)
{
    *out = (struct apm_selftest){ .attempted = true, .finished = true, .state = t23 ? 4 : 1, .connected = t23,
        .capabilities_valid = t23, .version = t23 ? 0x0102 : 0, .flags = t23 ? 0x0003 : 0, .capabilities = t23 ? 0x000F : 0,
        .units = t23 ? 1 : 0, .reason = t23 ? "connected" : "no_apm_bios" };
}
bool power_s5_fallback_qualified(void) { return t23; }
const char *power_s5_fallback_state(void) { return t23 ? "qualified" : "absent"; }
bool power_s5_fallback_data(struct acpi_soft_off *out)
{
    if (!t23) return false;
    *out = (struct acpi_soft_off){ .pm1a_cnt = 0x1004, .slp_typa = 7, .slp_typb = 7, .fadt_revision = 1,
                                   .smi_cmd = 0xB2, .acpi_enable = 0xF0, .sci_int = 9, .flags = 0x2D };
    return true;
}
void power_source_snapshot(struct power_source_snapshot *out)
{ memset(out, 0, sizeof(*out)); out->provider = 1; out->active = t23; out->status.ac = 1; out->status.percent = 87; }
void thinkpad_ec_smbios_snapshot(struct thinkpad_ec_smbios *out)
{
    memset(out, 0, sizeof(*out));
    out->verdict = t23 ? "thinkpad" : "not_thinkpad"; out->valid = t23; out->eligible = t23;
    if (t23) { strcpy(out->vendor, "IBM"); strcpy(out->product, "2647NG1"); strcpy(out->bios_version, "1AET69WW (1.09 )"); }
}
int thinkpad_ec_poll_state(void) { return t23 ? 500 : -ENODEV; }

/* ---- audio ------------------------------------------------------------- */
static struct audio_stream stream;
static int midi_token;
int audio_stream_open(struct audio_stream **out) { if (!audio.ready) return -ENODEV; memset(&stream, 0, sizeof(stream)); *out = &stream; return 0; }
void audio_stream_close(struct audio_stream *s) { CHECK(s == &stream && !s->midi); }
int audio_stream_enqueue(struct audio_stream *s, const void *b, uint32_t bytes)
{
    (void)b; CHECK(bytes % 4 == 0);
    if (bytes > 2048) bytes = 2048;
    s->accepted += bytes / 4;
    audio.played_output += bytes / 4;
    if (s->draining && s->accepted >= s->drain_goal) { s->drain_latched = true; s->drain_output = audio.played_output; }
    return (int)bytes;
}
void audio_backend_snapshot(struct audio_transport *t) { static uint64_t irqs; memset(t, 0, sizeof(*t)); t->irqs = irqs += 12; }
int audio_midi_write(struct audio_stream *s, struct ciuki_midi_data *m)
{ s->midi = (struct audio_midi *)&midi_token; m->accepted = m->bytes; return 0; }
void audio_midi_close(struct audio_stream *s) { s->midi = 0; }
bool audio_midi_active(struct audio_stream *s) { (void)s; return false; }

/* ---- network and USB ---------------------------------------------------- */
static uint64_t lease_at;
void net_state_snapshot(struct ciuki_net_state *out)
{
    memset(out, 0, sizeof(*out));
    if (!t23) return;
    out->present = 1; out->state = CIUKI_NET_RUNNING; out->link = CIUKI_NET_LINK_UP;
    snprintf(out->driver, sizeof(out->driver), "e100");
    memcpy(out->mac, "\x00\x02\xb3\x11\x22\x33", 6);
    out->address_configured = g_ticks >= lease_at;
    if (out->address_configured) memcpy(out->ipv4, "\xc0\xa8\x01\x17", 4);
}
void net_settings_get(unsigned i, struct ciuki_net_config *cfg) { (void)i; memset(cfg, 0, sizeof(*cfg)); cfg->enabled = cfg->dhcp = 1; }
int net_stack_dhcp_error(unsigned i) { (void)i; return 0; }
static struct usb_host hosts[3];
unsigned usb_count(void) { return t23 ? 3 : 0; }
const struct usb_host *usb_get(unsigned i) { return t23 && i < 3 ? &hosts[i] : 0; }

/* ---- the run ------------------------------------------------------------ */
static void machine(bool profile)
{
    t23 = profile; desktop_running = false;
    g_ticks = 5000; seq = 0; log_text[0] = serial_out[0] = 0; details_used = 0; details[0] = 0;
    memset(&g_boot, 0, sizeof(g_boot)); memset(&audio, 0, sizeof(audio)); memset(&fb, 0, sizeof(fb));
    memset(&fake_storage, 0, sizeof(fake_storage)); memset(&vbe_state, 0, sizeof(vbe_state));
    memset(hosts, 0, sizeof(hosts));
    vm_created = vm_live_flag = dosbox_requested = fs_active = overlay_exists = false;
    fullscreen_entries = fsprog_runs = opens = 0;
    backup_requested = attention_sent = false;   /* sec-user-06 */
    memset(&gate, 0, sizeof(gate)); gate.restorations = 1; gate.last_restore_ms = gate.max_restore_ms = 180;
    fake_storage.ready = true; fake_storage.vfs.volumes[2] = fake_c;
    if (!profile) return;
    g_boot.uart_state = 1; g_boot.uart_base = 0x3F8; g_boot.uart_divisor = 1;
    g_boot.flags = CBI_F_EDID_VALID | CBI_F_SELF_TEST;
    g_boot.edid[56] = 0x00; g_boot.edid[58] = 0x40; g_boot.edid[59] = 0x00; g_boot.edid[61] = 0x30; /* 1024x768 */
    g_boot.acpi_rsdp_phys = 0xF6B50;
    g_boot.display.vbe_version = 0x300; g_boot.display.vram_bytes = 8u << 20;
    snprintf(g_boot.display.oem, sizeof(g_boot.display.oem), "S3 Incorporated. M7 BIOS");
    snprintf(g_boot.display.vendor, sizeof(g_boot.display.vendor), "S3 Incorporated.");
    snprintf(g_boot.display.product, sizeof(g_boot.display.product), "SuperSavage/IXC 1014");
    static const struct ciuki_boot_display_mode modes[] = {
        { 0x101, 640, 480, 8, 640, 1, 0 }, { 0x112, 640, 480, 32, 2560, 1, 0 },
        { 0x111, 640, 480, 16, 1280, 1, 0 }, { 0x105, 1024, 768, 8, 1024, 1, 0 },
        { 0x117, 1024, 768, 16, 2048, 1, 0 }, { 0x118, 1024, 768, 32, 4096, 1, 0 },
        { 0x103, 800, 600, 8, 800, 0, 0 },
    };
    memcpy(g_boot.display.modes, modes, sizeof(modes));
    g_boot.display.mode_count = ARRAY_SIZE(modes);
    fb = (struct fb_device){ .width = 1024, .height = 768, .pitch = 2048, .size = 1536u << 10, .phys = 0xF0000000u,
                             .bpp = 16, .mode_id = 0x117, .present = true, .generation = 3 };
    fake_storage.optical_bindings[1].device = &cd;
    audio.ready = audio.backend_touched = true; audio.backend_name = "ac97"; audio.reason = "running";
    audio.backend_pci.vendor = 0x8086; audio.backend_pci.device = 0x2485;
    static const uint16_t uhci[3] = { 0x2482, 0x2484, 0x2487 };
    for (unsigned i = 0; i < 3; i++) {
        hosts[i].pci.vendor = 0x8086; hosts[i].pci.device = uhci[i]; hosts[i].pci.prog_if = 0x00;
        hosts[i].ready = hosts[i].scanned = hosts[i].irq_attached = true; hosts[i].ports = 2;
    }
    hosts[0].device_count = 1;
    hosts[0].devices[0] = (struct usb_device){ .present = true, .configured = true, .address = 1,
                                               .vendor = 0x046d, .product = 0xc00e, .class_code = 0 };
    lease_at = g_ticks + 30000;        /* the lease arrives during the run */
}
static void run(bool profile, bool unscheduled_3d, const char *folder)
{
    machine(profile);
    no_3d = unscheduled_3d;
    static struct selftest_result r1[32], r2[32];
    struct selftest_summary s;
    selftest_model_begin(&ops, &s, "0badf00d", "12345678", "1.0.1 alpha");
    selftest_checks_begin();
    uint64_t began = g_ticks;
    CHECK(selftest_phase1_count <= 32 && selftest_phase2_count <= 32);
    selftest_model_run(&ops, selftest_phase1, selftest_phase1_count, r1, &s);
    desktop_running = profile;         /* supervisor_bootstrap starts the desktop */
    selftest_model_run(&ops, selftest_phase2, selftest_phase2_count, r2, &s);
    selftest_model_end(&ops, &s, details);
    uint64_t took = g_ticks - began;
    unsigned limit_total = 0;
    const struct { const struct selftest_check *table; unsigned count; const struct selftest_result *results; } phases[] = {
        { selftest_phase1, selftest_phase1_count, r1 }, { selftest_phase2, selftest_phase2_count, r2 } };
    for (unsigned p = 0; p < 2; p++)
        for (unsigned i = 0; i < phases[p].count; i++) {
            const struct selftest_check *c = &phases[p].table[i];
            const struct selftest_result *r = &phases[p].results[i];
            limit_total += c->limit_ms;
            if (r->ms > c->limit_ms || selftest_has_token(r->figures, "deadline_exceeded"))
                fprintf(stderr, "%s overran: %u > %u\n", c->id, r->ms, c->limit_ms);
            CHECK(r->ms <= c->limit_ms);
            const char *expected = "SKIP";
            if (!strncmp(c->id, "memory.", 7) || !strcmp(c->id, "security.kdf")) expected = "PASS";  /* the CPU alone */
            else if (profile && no_3d && !strcmp(c->id, "display.3d_qualification")) expected = "FAIL";
            else if (profile && no_3d && !strcmp(c->id, "display.gl_draw")) expected = "SKIP";
            else if (profile) expected = !strcmp(c->id, "optical.disc") ? "SKIP" : "PASS";
            else if (!strcmp(c->id, "desktop.start")) expected = "FAIL";
            if (strcmp(selftest_status_text(r->status), expected))
                fprintf(stderr, "%s: %s %s (expected %s)\n", c->id, selftest_status_text(r->status), r->figures, expected);
            CHECK(!strcmp(selftest_status_text(r->status), expected));
            if (r->status == SELFTEST_SKIP) CHECK(selftest_has_token(r->figures, "reason"));
        }
    CHECK(took <= limit_total);
    printf("%s: %u passed, %u failed, %u skipped in %llu ms (bounds %u ms), %u records\n",
           profile ? no_3d ? "t23 without 3D" : "t23" : "bare",
           s.passed, s.failed, s.skipped, (unsigned long long)took, limit_total, seq);
    if (profile && no_3d) {
        /* An unscheduled qualification fails after its 15 s grace, not 60 s. */
        CHECK(strstr(log_text, "FAIL display.3d_qualification ms=15000 wait_ms=15000 scheduled=0 active=0 engine_kind=2 "));
        CHECK(strstr(log_text, " reason=not_scheduled\n") && strstr(log_text, "SKIP display.gl_draw ms=0 reason=3d_not_qualified"));
        return;
    }
    if (profile) {
        /* About five minutes in the worst case, far less on the expected path. */
        CHECK(took < 120000 && limit_total <= 600000);
        CHECK(fsprog_runs == 1 && fullscreen_entries == 2 && !vm_live_flag && !overlay_exists && !fs_active);
        CHECK(strstr(log_text, "PASS dos.return ") && strstr(log_text, " overlay_left=0"));
        CHECK(strstr(log_text, "PASS display.fs_640x480x8 ") && strstr(log_text, " provider=firmware"));
        CHECK(strstr(log_text, "PASS display.vbe_state ms=0 bytes=3776 "));
        CHECK(strstr(log_text, "SKIP optical.disc ms=0 media=no_medium reason=no_disc"));
        CHECK(strstr(log_text, "display.info line=0 modes=101:640x480x8,112:640x480x32,"));
        CHECK(strstr(log_text, "display.3d_qualification p0=prepare:1000:0 "));
        CHECK(strstr(log_text, "PASS network.dhcp ") && strstr(log_text, " address=192.168.1.23 "));
        CHECK(strstr(log_text, " kernel_present=1 probe_result=1 fifo=16 com_ports=1 bda_ports=1 com=1:3f8:4:bda:ready"));
        CHECK(strstr(log_text, "PASS audio.silent_stream ") && strstr(log_text, " frames=24000 stream_ms="));
        CHECK(strstr(log_text, " consumed=1 midi_ms="));
        CHECK(strstr(log_text, " model=TOSHIBA_DVD-ROM_SD-C2502\n"));
        CHECK(fb.width == 1024 && fb.height == 768 && fb.mode_id == 0x117);
        /* sec-user-06 */
        CHECK(strstr(log_text, "PASS security.kdf ") && strstr(log_text, " algorithm=pbkdf2_hmac_sha256 iterations=10000 runs=3 "));
        CHECK(strstr(log_text, "PASS security.registry_backup ") && strstr(log_text, " answered=1 wait_ms=1500 backup_ms=1180 result=0 bytes=262144 deadline_ms=5000"));
        CHECK(strstr(log_text, "PASS security.lock_latency ") &&
              strstr(log_text, " path=attention concealed=1 restored=1 wait_ms=3600 neutral_ms=35 trusted_ms=410 restore_ms=220 "));
    } else {
        CHECK(s.passed == 3 && s.failed == 1 && s.skipped == selftest_phase1_count + selftest_phase2_count - 4);
        CHECK(strstr(log_text, "FAIL desktop.start ") && strstr(log_text, " reason=no_desktop"));
        CHECK(opens == 0 && !dosbox_requested && !vm_created);
    }
    if (folder) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.LOG", folder, profile ? "T23" : "BARE");
        FILE *out = fopen(path, "wb"); CHECK(out); fputs(log_text, out); fclose(out);
        snprintf(path, sizeof(path), "%s/%s-serial.txt", folder, profile ? "t23" : "bare");
        out = fopen(path, "wb"); CHECK(out); fputs(serial_out, out); fclose(out);
    }
}

int main(int argc, char **argv)
{
    run(false, false, argc > 1 ? argv[1] : 0);
    run(true, true, 0);
    run(true, false, argc > 1 ? argv[1] : 0);
    printf("selftest checks: 0 failures\n");
    return 0;
}
