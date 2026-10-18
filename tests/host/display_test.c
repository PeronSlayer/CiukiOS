/* f1-23: real display service and framebuffer transaction, fake firmware,
 * registry/MMIO hardware boundaries. ASan/UBSan; no QEMU.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <ciuki/kernel.h>
#include <ciuki/display.h>
#include <ciuki/registry.h>
#include <ciuki/biosvm.h>
#include <ciuki/supervisor.h>
#include <ciuki/bootlog.h>
#include <ciuki/cpu.h>
#include <ciuki/supersavage_display.h>
#include <ciuki/dos_video_modes.h>
#undef irq_save
#undef irq_restore
#define irq_save() 0x200u
#define irq_restore(flags) ((void)(flags))

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
static unsigned log_requests,log_errors;
static unsigned blackbox_syncs;
static int blackbox_error;
int blackbox_checkpoint(void) { blackbox_syncs++; return blackbox_error; }
static bool native_supported;
static int native_begin_error,native_end_error;
static unsigned native_prepares,native_begins,native_ends,native_completes;
static bool native_recovered;
static char native_results[256],return_timing[256],frame_timing[256];
void bootlog_snapshot(struct bootlog_stats *s) { memset(s,0,sizeof(*s));s->active=log_active; }
int bootlog_sync(bool (*cancel)(void *),void (*pause)(void *),void *arg)
{ assert(cancel && pause && !cancel(arg)); log_syncs++; return log_sync_error; }
int bootlog_try_sync(void) { log_requests++; return log_sync_error; }
static bool owner = true, busy, fail_firmware, fail_restore, bad_info, fail_window, check_boot;
static bool fail_framebuffer_claim;
static uint8_t *aperture, *window;
static uint32_t window_size;
static uint16_t called[16];
static unsigned sequence[64], sequence_count;
static unsigned char user[1024];
static void event(unsigned n) { assert(sequence_count < 64); sequence[sequence_count++] = n; }
bool supersavage_display_supported(const struct fb_device *f,uint16_t mode)
{ (void)f;(void)mode;return is_supersavage && native_supported; }
int supersavage_display_write(uint32_t offset,const void *p,uint32_t bytes)
{ (void)offset;(void)p;(void)bytes;return 0; }
int supersavage_display_prepare(const struct fb_device *f,uint16_t mode)
{ assert(f->present && (mode==3 || mode==0x13 || dos_video_mode(mode))); native_prepares++; return native_supported ? 0 : -EOPNOTSUPP; }
int supersavage_display_begin(uint16_t mode)
{ (void)mode; assert(suspended && !fbdev_get()->present && !accel_ports_owned); native_begins++; event(11); return native_begin_error; }
int supersavage_display_end(void)
{ assert(suspended && !fbdev_get()->present); native_ends++; event(12); return native_end_error; }
void supersavage_display_complete(bool recovered)
{ native_completes++;native_recovered=recovered; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void mode_info(uint16_t id, uint8_t m[256])
{
    memset(m, 0, 256); wr16(m, 0x91); m[24] = 1; m[27] = 6;
    uint16_t w = id == 0x118 || id == 0x117 ? 1024 : 640, h = id == 0x118 || id == 0x117 ? 768 : 480;
    m[25] = id == 0x118 ? 32 : 16;
    wr16(m + 18, w); wr16(m + 20, h); wr16(m + 50, w * (m[25] / 8) + (id==0x117 ? 0 : 16));
    wr32(m + 40, id == 0x118 ? 0xE0000000 : 0xE1000003);
    if (m[25] == 32) { m[54] = m[56] = m[58] = m[60] = 8; m[55] = 16; m[57] = 8; m[61] = 24; }
    else { m[54] = 5; m[55] = 11; m[56] = 6; m[57] = 5; m[58] = 5; }
}
int biosvm_video_prepare(void) { return prepare_failure ? -EPERM : 0; }
int biosvm_video_switch(uint16_t id, uint8_t m[256])
{
    assert(!accel_ports_owned);
    if (is_supersavage) assert(native_ends==1 && suspended);
    event(4); assert(!fbdev_get()->present && !fbdev_get()->mapped);
    assert(resources[fbdev_get()->handle].state == RS_QUIESCING || resources[fbdev_get()->handle].state == RS_RELEASED);
    assert(switches < 16); called[switches++] = id;
    if (fail_restore || fail_firmware) { fail_firmware = false; return -EIO; }
    mode_info(id, m);
    if (bad_info) { bad_info = false; wr32(m + 40, 0x100000); }
    return 0;
}
void klog(const char *fmt, ...) {
    if (strstr(fmt,"fullscreen_log error=")) log_errors++;
    char *out=strstr(fmt,"native_result=") ? native_results :
        strstr(fmt,"lease_return_ms=") ? return_timing :
        strstr(fmt,"desktop_frame_ms=") ? frame_timing : NULL;
    if (out) { va_list ap;va_start(ap,fmt);vsnprintf(out,256,fmt,ap);va_end(ap); }
}
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
    if (fail_framebuffer_claim) { fail_framebuffer_claim=false;return -ENOMEM; }
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
    fullscreen_display=fullscreen_native=awaiting_native_frame=suspended=fs_begin_failure=fs_end_failure=false;
    native_supported=false;native_begin_error=native_end_error=0;
    fullscreen_return_timed=false;fullscreen_return_tick=0;
    fullscreen_recovering=fullscreen_log_failed=false;
    native_prepares=native_begins=native_ends=native_completes=blackbox_syncs=0;native_recovered=false;blackbox_error=0;
    native_results[0]=return_timing[0]=frame_timing[0]=0;
    is_supersavage=false;log_active=true;log_sync_error=0;log_syncs=0;
    log_requests=log_errors=0;
    initialized = changing = switching = awaiting_frame = check_display = check_boot = check_sweeping = false;
    check_result=0; check_deadline=0;
    acceleration=0; accel_ports_owned=false; accel_stops=0;
    change_window = 0; change_capacity = changes = drawing = 0; watchdog = 0;
    resource_count = generation = 1; sequence_count = switches = copies = frames = 0;
    g_ticks = 100; current = &desktop; owner = true;
    prepare_failure = busy = fail_firmware = fail_restore = bad_info = fail_window = false;
    fail_framebuffer_claim=false;
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
    assert(display_fullscreen_begin(0x13)==DISPLAY_FULLSCREEN_WINDOWED && !suspended && !fullscreen_display);
    assert(!sequence_count && !switches && !accel_stops && accel_ports_owned && fbdev_get()->present && log_syncs==2);
    fixture();log_active=false;
    assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !sequence_count && fbdev_get()->present);
    fixture();log_sync_error=-EIO;
    assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !sequence_count && fbdev_get()->present);
    fixture();blackbox_error=-EIO;
    assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !sequence_count && blackbox_syncs==1 && fbdev_get()->present);
    fixture();g_boot.flags|=CBI_F_SMBIOS_QEMU;log_active=false;
    assert(!display_fullscreen_begin(3) && !log_syncs);
    assert(!display_fullscreen_end());
    puts("PASS f3-26 display: SuperSavage refusal before hardware mutation, durable hardware checkpoints, missing/failed log refusal, QEMU serial-only timing");
    fixture();is_supersavage=native_supported=prepare_failure=true;
    acceleration=&test_acceleration;accel_ports_owned=true;
    assert(!display_fullscreen_begin(0x13) && suspended && fullscreen_native && native_prepares==1 && native_begins==1);
    assert(!switches && accel_stops==1 && !fbdev_get()->present);
    display_fullscreen_return_request();
    g_ticks+=10;display_fullscreen_return_request();assert(fullscreen_return_tick==100);
    /* A disabled BIOS and failed persistence do not prevent native success. */
    log_sync_error=-EIO;
    assert(!display_fullscreen_end() && !suspended && fbdev_get()->present && native_ends==1 && !switches);
    const unsigned native_sequence[]={0,1,2,3,11,12,5,6,7,8};
    assert(sequence_count==ARRAY_SIZE(native_sequence) && !memcmp(sequence,native_sequence,sizeof(native_sequence)));
    g_ticks+=10000; display_watchdog_poll(); assert(!switches && fbdev_get()->present);
    fixture();is_supersavage=native_supported=true;native_begin_error=-EOPNOTSUPP;
    assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !suspended && fbdev_get()->present && native_ends==1 && !switches);
    fixture();is_supersavage=native_supported=true;
    assert(!display_fullscreen_begin(0x101)); native_end_error=-EIO;
    display_fullscreen_return_request();g_ticks+=1180;
    assert(!display_fullscreen_end() && fbdev_get()->present && fbdev_get()->mapped && switches==1);
    assert(!suspended && !fullscreen_display && !changing && !switching && awaiting_frame);
    assert(strstr(native_results,"native_result=-5 fallback_attempted=1 fallback_result=0 result=0"));
    assert(native_completes==1 && native_recovered);
    assert(strstr(return_timing,"lease_return_ms=1180 budget_ms=2000 overrun=0 result=0"));
    uint32_t rgb=0x00123456;
    struct fb_surface frame={&rgb,1,1,4,4};struct fb_rect rect={0,0,1,1};
    assert(!fbdev_present(&frame,&rect) && fbdev_get()->mapped[0]==0x56 && fbdev_get()->mapped[2]==0x12);
    display_frame_complete(fbdev_get()->generation);
    assert(!awaiting_frame && strstr(frame_timing,"budget_ms=2000 overrun=0"));
    g_ticks+=10000;display_watchdog_poll();assert(switches==1);
    fixture();is_supersavage=native_supported=true;
    assert(!display_fullscreen_begin(0x13));native_end_error=-EIO;fail_restore=true;
    assert(display_fullscreen_end()==-EIO && !fbdev_get()->present && !fbdev_get()->mapped && switches==1);
    assert(!suspended && !fullscreen_display && !awaiting_frame);
    assert(native_completes==1 && !native_recovered);
    assert(strstr(native_results,"native_result=-5 fallback_attempted=1 fallback_result=-5 result=-5"));
    g_ticks+=10000;display_watchdog_poll();assert(switches==1);
    fixture();is_supersavage=native_supported=true;
    assert(!display_fullscreen_begin(0x13));native_end_error=-EIO;bad_info=true;
    assert(display_fullscreen_end()==-EIO && !fbdev_get()->present && switches==1 && !suspended);
    fixture();is_supersavage=native_supported=true;native_begin_error=-EIO;native_end_error=-EIO;
    assert(display_fullscreen_begin(0x13)==DISPLAY_FULLSCREEN_WINDOWED && fbdev_get()->present && !suspended && switches==1);
    fixture();is_supersavage=native_supported=true;
    assert(!display_fullscreen_begin(0x13));display_fullscreen_return_request();g_ticks+=1180;
    assert(!display_fullscreen_end() && strstr(return_timing,"budget_ms=2000 overrun=0 result=0"));
    g_ticks+=821;display_frame_complete(fbdev_get()->generation);
    assert(strstr(frame_timing,"desktop_frame_ms=2001 budget_ms=2000 overrun=1") && !awaiting_frame);
    fixture();is_supersavage=native_supported=true;
    assert(!display_fullscreen_begin(0x13));fail_framebuffer_claim=true;
    assert(!display_fullscreen_end() && fbdev_get()->present && switches==1 && !suspended);
    assert(strstr(native_results,"native_result=0 fallback_attempted=1 fallback_result=0 result=0"));
    const unsigned packed_modes[]={0x101,0x103,0x111,0x114};
    for (unsigned i=0;i<ARRAY_SIZE(packed_modes);i++) for (unsigned failure=0;failure<2;failure++) {
        fixture();is_supersavage=native_supported=true;
        assert(display_fullscreen_supported(packed_modes[i]));
        assert(display_fullscreen_write(0,"x",1)==-EIO);
        assert(!display_fullscreen_begin(packed_modes[i]));
        assert(!display_fullscreen_write(0,"x",1));
        native_end_error=failure ? -EIO : 0;
        assert(!display_fullscreen_end() && fbdev_get()->present && fbdev_get()->mapped && !suspended);
        assert(switches==failure && native_ends==1);
        assert(display_fullscreen_write(0,"x",1)==-EIO);
        display_frame_complete(fbdev_get()->generation);
        assert(!awaiting_frame && !awaiting_native_frame);
    }
    puts("PASS f3-37 display: all four packed modes retain native success / mediated VBE fallback, returned grant and first desktop frame; writes confined to active transaction");
    puts("PASS f3-39 display: safe refusal/recovered entry returns windowed; final recovery result reaches native strike policy");
    puts("PASS f3-36 display: native failure -> bounded mediated VBE fallback, both results logged, lease/remap/presents/frame resume, invalid/failed fallback stays unavailable without retry, entry rollback, measured return/frame budgets");
    fixture(); acceleration=&test_acceleration; accel_ports_owned=true;
    assert(!display_fullscreen_begin(0x13) && suspended && display_fullscreen_active());
    assert(!fbdev_get()->present && !fbdev_get()->mapped && accel_stops==1);
    assert(display_set(&desktop,0x111)==-EBUSY && display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED);
    assert(!display_fullscreen_end() && !suspended && fbdev_get()->present && awaiting_frame);
    const unsigned expected[]={0,1,2,3,9,10,5,6,7,8};
    assert(sequence_count==ARRAY_SIZE(expected) && !memcmp(sequence,expected,sizeof(expected)));
    assert(!display_fullscreen_end());
    display_frame_complete(fbdev_get()->generation); assert(!awaiting_frame);
    fixture(); fs_begin_failure=true;
    assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !suspended && fbdev_get()->present);
    fixture(); assert(!display_fullscreen_begin(3)); fs_end_failure=true;
    assert(!display_fullscreen_end() && !suspended && fbdev_get()->present && switches==1);
    fixture(); assert(!display_fullscreen_begin(3)); fs_end_failure=fail_restore=true;
    assert(display_fullscreen_end()==-EIO && !fbdev_get()->present && !fbdev_get()->mapped);
    fixture(); busy=true; assert(display_fullscreen_begin(3)==DISPLAY_FULLSCREEN_WINDOWED && !suspended);
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
static void return_logging_tests(void)
{
    for (unsigned failure=0;failure<3;failure++) {
        fixture();is_supersavage=native_supported=true;
        assert(!display_fullscreen_begin(0x13));
        unsigned syncs=log_syncs,raws=blackbox_syncs;
        /* A busy worker queues, a failed/inactive sink reports once. */
        log_sync_error=failure==0 ? -EAGAIN : -EIO;
        if (failure==2) log_active=false;
        display_fullscreen_return_request();
        uint64_t start=g_ticks;
        assert(display_fullscreen_checkpoint("watchdog-return",0)==(failure ? -EIO : 0));
        assert(display_fullscreen_checkpoint("guest-revoke",0)==(failure ? -EIO : 0));
        assert(!display_fullscreen_end() && fbdev_get()->present && !suspended);
        assert(!fullscreen_recovering && !fullscreen_display);
        assert(log_syncs==syncs && blackbox_syncs==raws);
        assert(log_requests==(failure==2 ? 0u : 4u));
        assert(log_errors==(failure ? 1u : 0u));
        assert(g_ticks-start<=50 && g_ticks-start<=NATIVE_RETURN_MS);
        printf("F1-80 display return log failure=%u watchdog-return->desktop-restored=%llums synchronous_io=0 errors=%u PASS\n",
               failure,(unsigned long long)(g_ticks-start),log_errors);
        /* The next entry must regain its synchronous durability gate. */
        log_sync_error=0;log_active=true;
        assert(!display_fullscreen_checkpoint("vm-park",0));
        assert(log_syncs==syncs+1 && blackbox_syncs==raws+1);
    }
}
int main(void)
{
    return_logging_tests();
    fixture(); assert(display_check_begin(0x111)==-EPERM && !switches);
    g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111) && fbdev_get()->bpp==16);
    assert(watchdog && check_display && !switching);
    assert(!display_check_end() && fbdev_get()->mode_id==0x118 && fbdev_get()->bpp==32);
    assert(!check_display && !switching && !awaiting_frame);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    g_boot.display.mode_count=3;
    g_boot.display.modes[2]=(struct ciuki_boot_display_mode){0x117,1024,768,16,2048,1,0};
    assert(!display_check_begin(0x117) && !display_check_sweep_begin());
    assert(fbdev_get()->width==1024 && fbdev_get()->height==768 && fbdev_get()->pitch==2048 && fbdev_get()->bpp==16);
    display_check_sweep_end(true);
    assert(!display_check_end() && fbdev_get()->mode_id==0x118 && fbdev_get()->pitch==4112 && fbdev_get()->bpp==32);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    fail_firmware=true;
    assert(display_check_begin(0x111)==-EIO && !check_display && fbdev_get()->present && switches==2);
    assert(fbdev_get()->mode_id==0x118 && !changing && !switching);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    fail_firmware=fail_restore=true;
    assert(display_check_begin(0x111)==-EIO && !check_display && !fbdev_get()->present);
    unsigned attempts=switches;
    g_ticks+=10000; display_watchdog_poll(); assert(switches==attempts);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111));
    g_ticks+=CHECK_MODE_MS-1; display_watchdog_poll(); assert(switches==1);
    g_ticks++; display_watchdog_poll();
    assert(switches==2 && !check_display && fbdev_get()->mode_id==0x118);
    assert(display_check_end()==-ETIMEDOUT && display_check_sweep_begin()==-ETIMEDOUT);
    display_watchdog_poll(); assert(switches==2);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111) && !display_check_sweep_begin());
    g_ticks+=CHECK_MODE_MS; display_watchdog_poll();
    assert(switches==1 && check_sweeping && display_check_end()==-EBUSY);
    display_check_sweep_end(true);
    assert(switches==2 && !check_display && fbdev_get()->mode_id==0x118);
    assert(display_check_end()==-ETIMEDOUT);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111) && !display_check_sweep_begin());
    display_check_sweep_end(false); g_ticks+=CHECK_MODE_MS;
    display_watchdog_poll(); assert(switches==1 && display_check_end()==-EIO);
    fixture(); g_boot.flags|=CBI_F_HW_CHECK; check_boot=true;
    assert(!display_check_begin(0x111)); fail_restore=true;
    g_ticks+=CHECK_MODE_MS; display_watchdog_poll(); attempts=switches;
    assert(!check_display && display_check_end()==-EIO);
    g_ticks+=CHECK_MODE_MS; display_watchdog_poll(); assert(switches==attempts);
    puts("PASS f1-69 display check watchdog: independent restore, late sweep refusal, lease pin, unsafe writer refusal, failed restore not retried");
    puts("PASS hardware display check: H-only access, no owner, 16bpp switch and verified original mode restore");
    worker_tests();
    fullscreen_tests(); query_tests(); switch_tests(); watchdog_tests(); free(aperture); free(window);
    puts("display host tests: PASS"); return 0;
}
