/* f1-23: real display service and framebuffer transaction, fake firmware,
 * registry/MMIO hardware boundaries. ASan/UBSan; no QEMU.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ciuki/kernel.h>
#include <ciuki/display.h>
#include <ciuki/registry.h>
#include <ciuki/biosvm.h>
#include <ciuki/supervisor.h>
#include <ciuki/bootlog.h>

struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
static struct process desktop = {.pid = 2}, other = {.pid = 3};
static struct process *current = &desktop;
static struct task timer;
static struct resource resources[16];
static unsigned resource_count, generation, frames, switches, copies;
static bool prepare_failure, suspended, fs_begin_failure, fs_end_failure;
static bool accel_ports_owned;
static unsigned accel_stops;
static bool is_supersavage,log_active=true;
static int log_sync_error;
static unsigned log_syncs;
void bootlog_snapshot(struct bootlog_stats *s) { memset(s,0,sizeof(*s));s->active=log_active; }
int bootlog_sync(bool (*cancel)(void *),void (*pause)(void *),void *arg)
{ assert(cancel && pause && !cancel(arg)); log_syncs++; return log_sync_error; }
static bool owner = true, busy, fail_firmware, fail_restore, bad_info, fail_window, check_boot;
static uint8_t *aperture, *window;
static uint32_t window_size;
static uint16_t called[16];
static unsigned sequence[64], sequence_count;
static unsigned char user[1024];
static void event(unsigned n) { assert(sequence_count < 64); sequence[sequence_count++] = n; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void mode_info(uint16_t id, uint8_t m[256])
{
    memset(m, 0, 256); wr16(m, 0x91); m[24] = 1; m[27] = 6;
    uint16_t w = id == 0x118 ? 1024 : 640, h = id == 0x118 ? 768 : 480;
    m[25] = id == 0x118 ? 32 : 16;
    wr16(m + 18, w); wr16(m + 20, h); wr16(m + 50, w * (m[25] / 8) + 16);
    wr32(m + 40, id == 0x118 ? 0xE0000000 : 0xE1000003);
    if (m[25] == 32) { m[54] = m[56] = m[58] = m[60] = 8; m[55] = 16; m[57] = 8; m[61] = 24; }
    else { m[54] = 5; m[55] = 11; m[56] = 6; m[57] = 5; m[58] = 5; }
}
int biosvm_video_prepare(void) { return prepare_failure ? -EPERM : 0; }
int biosvm_video_switch(uint16_t id, uint8_t m[256])
{
    assert(!accel_ports_owned);
    event(4); assert(!fbdev_get()->present && !fbdev_get()->mapped);
    assert(resources[fbdev_get()->handle].state == RS_QUIESCING || resources[fbdev_get()->handle].state == RS_RELEASED);
    assert(switches < 16); called[switches++] = id;
    if (fail_restore || fail_firmware) { fail_firmware = false; return -EIO; }
    mode_info(id, m);
    if (bad_info) { bad_info = false; wr32(m + 40, 0x100000); }
    return 0;
}
void klog(const char *fmt, ...) { (void)fmt; }
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
void kmutex_init(struct kmutex *m) { m->owner = 0; }
void kmutex_lock(struct kmutex *m) { assert(!m->owner); m->owner = &timer; }
void kmutex_unlock(struct kmutex *m) { assert(m->owner == &timer); m->owner = 0; }
unsigned registry_count(void) { return resource_count; }
const struct resource *registry_get(unsigned i) { return i < resource_count ? &resources[i] : 0; }
bool registry_valid(int h, gen_t g) { return h >= 0 && (unsigned)h < resource_count && resources[h].generation == g && resources[h].state != RS_QUARANTINED && resources[h].state != RS_RELEASED; }
int registry_claim_reserved(int h, gen_t g, const char *o) { assert(registry_valid(h, g)); resources[h].generation = ++generation; resources[h].owner = o; resources[h].state = RS_CLAIMED; return 0; }
int registry_activate(int h, gen_t g) { assert(registry_valid(h, g) && resources[h].state == RS_CLAIMED); resources[h].state = RS_ACTIVE; return 0; }
int registry_quarantine(int h, gen_t g) { assert(registry_valid(h, g)); resources[h].state = RS_QUARANTINED; return 0; }
int registry_quiesce(int h, gen_t g, registry_idle_proof_fn proof) {
    assert(registry_valid(h, g) && resources[h].state == RS_ACTIVE); event(1);
    resources[h].state = RS_QUIESCING; return proof(h, g) ? 0 : -EIO;
}
int registry_release(int h, uint32_t g) { assert(registry_valid(h, g) && resources[h].state == RS_QUIESCING); event(5); resources[h].state = RS_RELEASED; return 0; }
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner_name, bool share) {
    assert(!share && resource_count < 16); event(6);
    for (unsigned i = 0; i < resource_count; i++)
        assert(resources[i].state == RS_RELEASED || resources[i].end <= start || resources[i].start >= end);
    int h = (int)resource_count++;
    resources[h] = (struct resource){type, start, end, owner_name, ++generation, RS_CLAIMED, false}; return h;
}
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached) { (void)phys; assert(uncached); aperture = calloc(1, bytes); assert(aperture); return aperture; }
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
void framebuffer_write_drain(void) {}
uint64_t framebuffer_cycles(void) { return 0; }
void *vmm_map_framebuffer(uint32_t phys, uint32_t bytes) { return vmm_map_mmio(phys, bytes, true); }
void *vmm_reserve_mmio(uint32_t cap) { if (fail_window) return 0; window_size = cap; window = calloc(1, cap); assert(window); return window; }
void vmm_revoke_mmio(void *p, uint32_t bytes) { assert(p && bytes); event(3); }
void *vmm_replace_framebuffer(void *p, uint32_t cap, uint32_t phys, uint32_t bytes) { assert(p == window && cap == window_size && (uint64_t)bytes + (phys & 4095) <= cap); event(7); return window + (phys & 4095); }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t deadline)
{ (void)q; (void)deadline; return cond(arg); }
void kwait_wake_all(struct kwait *q) { (void)q; }
bool deadline_passed(uint64_t d) { return g_ticks - d < (UINT64_C(1) << 63); }
void task_sleep_ms(uint32_t ms) { (void)ms; assert(0); }
static unsigned workers_created, workers_started;
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *a, enum task_prio p) { assert(!strcmp(n, "display-watchdog") && fn && !a && p == P_NORMAL); workers_created++; return &timer; }
void task_start(struct task *t) { assert(t == &timer); workers_started++; }
struct process *proc_current(void) { return current; }
struct process *proc_find(uint32_t pid) { return pid==desktop.pid ? &desktop : 0; }
int grants_display_suspend(bool on) { if (on && (busy || suspended)) return -EBUSY; suspended=on; return 0; }
int biosvm_video_fullscreen_begin(uint8_t mode) {
    assert(suspended && !fbdev_get()->present && !accel_ports_owned && (mode==3 || mode==0x13));
    event(9); return fs_begin_failure ? -EIO : 0;
}
int biosvm_video_fullscreen_end(uint16_t mode,uint8_t info[256]) {
    assert(suspended && !fbdev_get()->present && mode==0x118); event(10);
    if (fs_end_failure) return -EIO;
    mode_info(mode,info); return 0;
}
uint32_t supervisor_desktop_pid(void) { return 2; }
bool grants_display_owner(const struct process *p) { return owner && p == &desktop; }
bool grants_display_busy(void) { return busy; }
uint32_t grants_display_pid(void) { return check_boot ? 0 : desktop.pid; }
static const struct pci_func gpu = {.vendor = 0x5333, .device = 0x8C2E, .class_code = 3};
static const struct pci_func standard_gpu = {.vendor=0x1234,.device=0x1111,.class_code=3};
unsigned pci_count(void) { return 1; }
const struct pci_func *pci_get(unsigned i) { return i ? 0 : is_supersavage ? &gpu : &standard_gpu; }
int copy_from_user(void *dst, uint32_t src, uint32_t n) { copies++; if (src > sizeof(user) || n > sizeof(user) - src) return -EFAULT; memcpy(dst, user + src, n); return 0; }
int copy_to_user(uint32_t dst, const void *src, uint32_t n) { copies++; if (dst > sizeof(user) || n > sizeof(user) - dst) return -EFAULT; memcpy(user + dst, src, n); return 0; }

#include "../../src/kernel/drivers/fbdev.c"
#include "../../src/kernel/core/display.c"

static int stop_accel(void *ctx)
{
    assert(ctx==&accel_ports_owned && changing && presenter.owner && accel_ports_owned);
    event(0); accel_stops++; accel_ports_owned=false;
    return 0;
}
static const struct accel_ops test_acceleration={.ctx=&accel_ports_owned,.stop=stop_accel};

void console_framebuffer_revoke(void) { event(2); }
void console_framebuffer_changed(const struct fb_device *f) { assert(f->mapped && f->present); event(8); }


static void fixture(void)
{
    free(aperture); free(window); aperture = window = 0;
    fullscreen_display=suspended=fs_begin_failure=fs_end_failure=false;
    is_supersavage=false;log_active=true;log_sync_error=0;log_syncs=0;
    initialized = changing = switching = awaiting_frame = check_display = check_boot = false;
    acceleration=0; accel_ports_owned=false; accel_stops=0;
    change_window = 0; change_capacity = changes = drawing = 0; watchdog = 0;
    resource_count = generation = 1; sequence_count = switches = copies = frames = 0;
    g_ticks = 100; current = &desktop; owner = true;
    prepare_failure = busy = fail_firmware = fail_restore = bad_info = fail_window = false;
    g_boot = (struct ciuki_boot_info){.version = 2, .flags = CBI_F_VBE_CTRL_VALID, .vbe_mode = 0x118,
        .fb_phys = 0xE0000000, .fb_width = 1024, .fb_height = 768, .fb_bpp = 32, .fb_pitch = 4112,
        .fb_red_size = 8, .fb_red_pos = 16, .fb_green_size = 8, .fb_green_pos = 8, .fb_blue_size = 8, .fb_rsvd_size = 8,
        .kernel_start = 0x100000, .kernel_end = 0x400000, .e820_count = 1};
    g_boot.e820[0] = (struct ciuki_e820){0x100000, 0x7F00000, CBI_E820_RAM, 1};
    g_boot.vbe_ctrl[5] = 3; mode_info(0x118, g_boot.vbe_mode_info);
    g_boot.display = (struct ciuki_boot_display){.tag = CBI_DISPLAY_TAG, .size = sizeof(g_boot.display),
        .vbe_version = 0x300, .vram_bytes = 16 * 1024 * 1024, .oem = "S3 BIOS", .vendor = "S3", .product = "SuperSavage", .mode_count = 2,
        .modes = {{0x118, 1024, 768, 32, 4112, 1, 0}, {0x111, 640, 480, 16, 1296, 1, 0}}};
    resources[0] = (struct resource){RES_MMIO, g_boot.fb_phys, g_boot.fb_phys + g_boot.fb_pitch * g_boot.fb_height, "boot-framebuffer", 1, RS_FIRMWARE, false};
    assert(!fbdev_init());
}
static void query_tests(void)
{
    fixture();
    is_supersavage=true;
    struct ciuki_display_info q = {.version = 1, .size = sizeof(q)};
    assert(!display_query(&q) && q.mode_count == 2 && q.width == 1024 && q.bpp == 32 && q.pitch == 4112);
    assert(q.modes[0].current && !q.modes[1].current && q.pci_vendor == 0x5333 && q.pci_device == 0x8C2E);
    assert(!strcmp(q.oem, "S3 BIOS") && q.vram_bytes == 16 * 1024 * 1024 && q.vbe_version == 0x300);
    q.version = 2; assert(display_query(&q) == -EINVAL);
    q.version = 1; q.size--; assert(display_query(&q) == -EINVAL);
    struct trap_frame t = {.ebx = CIUKI_DISPLAY_QUERY, .ecx = 16};
    memset(user, 0xA5, sizeof(user)); q.size = sizeof(q); memcpy(user + 16, &q, 8);
    current = &other; assert(!display_syscall(&t)); memcpy(&q, user + 16, sizeof(q)); assert(q.mode_count == 2);
    t.ecx = sizeof(user) - 4; assert(display_syscall(&t) == -EFAULT);
    t.ecx = sizeof(user) - 8; memcpy(user + t.ecx, &q, 8); assert(display_syscall(&t) == -EFAULT);
    t.ebx = 2; assert(display_syscall(&t) == -EINVAL);
    t.ebx = CIUKI_DISPLAY_SET; t.ecx = 0x10000; assert(display_syscall(&t) == -EINVAL);
    t.ecx = 0x111; assert(display_syscall(&t) == -EPERM);
    current = &desktop; t.edx = 1; assert(display_syscall(&t) == -EINVAL);
    g_boot.version = 1; q.version = 1; q.size = sizeof(q); assert(!display_query(&q) && !q.mode_count && !q.oem[0]);
    puts("PASS display query/syscall: v1/v2 bootinfo, PCI, exact wire layout, header/pointers/operation/rights");
}
static void switch_tests(void)
{
    fixture(); assert(display_set(&other, 0x111) == -EPERM);
    owner = false; assert(display_set(&desktop, 0x111) == -EPERM); owner = true;
    assert(display_set(&desktop, 0x4111) == -EINVAL && !switches);
    busy = true; assert(display_set(&desktop, 0x111) == -EBUSY && !switches); busy = false;
    assert(!display_set(&desktop, 0x118) && !switches);
    assert(!display_set(&desktop, 0x111) && switches == 1 && called[0] == 0x111);
    const unsigned expected[] = {1,2,3,4,5,6,7,8};
    assert(sequence_count == 8 && !memcmp(sequence, expected, sizeof(expected)));
    assert(fbdev_get()->mode_id == 0x111 && fbdev_get()->phys == 0xE1000003 && fbdev_get()->bpp == 16 && display_change_counter() == 1);
    struct ciuki_display_info q = {.version = 1, .size = sizeof(q)}; assert(!display_query(&q) && q.modes[1].current);
    assert(display_set(&desktop, 0x118) == -EBUSY);
    display_frame_complete(fbdev_get()->generation - 1); assert(awaiting_frame);
    display_frame_complete(fbdev_get()->generation); assert(!awaiting_frame);
    g_ticks += 10001; display_watchdog_poll(); assert(switches == 1);
    uint32_t rgb = 0x00FF8000;
    struct fb_surface s = {&rgb, 1, 1, 4, 4}; struct fb_rect r = {0,0,1,1};
    assert(!fbdev_present(&s, &r)); assert(fbdev_get()->mapped[0] == 0 && fbdev_get()->mapped[1] == 0xFC);
    fixture(); fail_firmware = true;
    assert(display_set(&desktop, 0x111) == -EIO && switches == 2 && called[1] == 0x118);
    assert(fbdev_get()->present && fbdev_get()->mode_id == 0x118 && display_change_counter() == 1 && !awaiting_frame);
    fixture(); bad_info = true;
    assert(display_set(&desktop, 0x111) == -EIO && switches == 2 && fbdev_get()->mode_id == 0x118);
    fixture(); fail_restore = true;
    assert(display_set(&desktop, 0x111) == -EIO && switches == 2 && !fbdev_get()->present && !fbdev_get()->mapped);
    fixture(); prepare_failure = true;
    assert(display_set(&desktop, 0x111) == -EIO && !switches && !sequence_count && fbdev_get()->present);
    fixture(); fail_window = true;
    assert(display_set(&desktop, 0x111) == -EIO && !switches && fbdev_get()->present);
    fixture(); acceleration=&test_acceleration; accel_ports_owned=true;
    assert(!display_set(&desktop,0x111) && accel_stops==1 && !accel_ports_owned);
    const unsigned with_accel[]={0,1,2,3,4,5,6,7,8};
    assert(sequence_count==ARRAY_SIZE(with_accel) && !memcmp(sequence,with_accel,sizeof(with_accel)));
    puts("PASS display switch: desktop-only, ids/busy, revoke-before-firmware, changed base/pitch, RGB565, rollback and failed restore");
}
static void watchdog_tests(void)
{
    fixture(); assert(!display_set(&desktop, 0x111));
    g_ticks += 9999; display_watchdog_poll(); assert(switches == 1);
    g_ticks++; display_frame_complete(fbdev_get()->generation); assert(awaiting_frame);
    display_watchdog_poll(); assert(switches == 2 && fbdev_get()->mode_id == 0x118 && display_change_counter() == 2);
    display_watchdog_poll(); assert(switches == 2);
    fixture(); g_ticks = UINT64_MAX - 5000; assert(!display_set(&desktop, 0x111));
    g_ticks += 10000; display_watchdog_poll(); assert(switches == 2 && fbdev_get()->present);
    puts("PASS display watchdog: independent 10 s rollback, stale/late frame refusal, successful frame cancellation, tick wrap");
}
static void fullscreen_tests(void)
{
    fixture();is_supersavage=true;
    acceleration=&test_acceleration;accel_ports_owned=true;
    assert(display_fullscreen_begin(0x13)==-EOPNOTSUPP && !suspended && !fullscreen_display);
    assert(!sequence_count && !switches && !accel_stops && accel_ports_owned && fbdev_get()->present && log_syncs==2);
    fixture();log_active=false;
    assert(display_fullscreen_begin(3)==-EIO && !sequence_count && fbdev_get()->present);
    fixture();log_sync_error=-EIO;
    assert(display_fullscreen_begin(3)==-EIO && !sequence_count && fbdev_get()->present);
    fixture();g_boot.flags|=CBI_F_SMBIOS_QEMU;log_active=false;
    assert(!display_fullscreen_begin(3) && !log_syncs);
    assert(!display_fullscreen_end());
    puts("PASS f3-26 display: SuperSavage refusal before hardware mutation, durable hardware checkpoints, missing/failed log refusal, QEMU serial-only timing");
    fixture(); acceleration=&test_acceleration; accel_ports_owned=true;
    assert(!display_fullscreen_begin(0x13) && suspended && display_fullscreen_active());
    assert(!fbdev_get()->present && !fbdev_get()->mapped && accel_stops==1);
    assert(display_set(&desktop,0x111)==-EBUSY && display_fullscreen_begin(3)==-EBUSY);
    assert(!display_fullscreen_end() && !suspended && fbdev_get()->present && awaiting_frame);
    const unsigned expected[]={0,1,2,3,9,10,5,6,7,8};
    assert(sequence_count==ARRAY_SIZE(expected) && !memcmp(sequence,expected,sizeof(expected)));
    assert(!display_fullscreen_end());
    display_frame_complete(fbdev_get()->generation); assert(!awaiting_frame);
    fixture(); fs_begin_failure=true;
    assert(display_fullscreen_begin(3)==-EIO && !suspended && fbdev_get()->present);
    fixture(); assert(!display_fullscreen_begin(3)); fs_end_failure=true;
    assert(!display_fullscreen_end() && !suspended && fbdev_get()->present && switches==1);
    fixture(); assert(!display_fullscreen_begin(3)); fs_end_failure=fail_restore=true;
    assert(display_fullscreen_end()==-EIO && !fbdev_get()->present && !fbdev_get()->mapped);
    fixture(); busy=true; assert(display_fullscreen_begin(3)==-EBUSY && !suspended);
    puts("PASS full-screen lease: accelerator stop, grant suspend, LFB revoke, firmware save, restore/remap/repaint, rollback and failed restore");
}
/* f3-21: the worker that polls queued DOS full-screen requests must exist
 * before any mode switch (a window VM start calls it); one worker only. */
static void worker_tests(void)
{
    fixture(); workers_created = workers_started = 0;
    assert(!display_worker_start() && workers_created == 1 && workers_started == 1);
    assert(!display_worker_start() && workers_created == 1);
    assert(!display_fullscreen_begin(0x13) && workers_created == 1);
    assert(!display_fullscreen_end());
    puts("PASS display worker: started once before any mode switch, reused by full-screen entry");
}
int main(void)
{
    fixture(); assert(display_check_begin(0x111)==-EPERM && !switches);
    g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111) && fbdev_get()->bpp==16);
    assert(!display_check_end() && fbdev_get()->mode_id==0x118 && fbdev_get()->bpp==32);
    assert(!check_display && !switching && !awaiting_frame);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    fail_firmware=fail_restore=true;
    assert(display_check_begin(0x111)==-EIO && check_display && !fbdev_get()->present);
    puts("PASS hardware display check: H-only access, no owner, 16bpp switch and verified original mode restore");
    worker_tests();
    fullscreen_tests(); query_tests(); switch_tests(); watchdog_tests(); free(aperture); free(window);
    puts("display host tests: PASS"); return 0;
}
