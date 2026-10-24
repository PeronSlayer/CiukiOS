/* F1 LFB presenter and f1-23 quiesced runtime remapping.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
/* Batch slice timing reads the TSC directly instead of including cpu.h, whose
 * P2V/irq_save macros collide with host harness stubs that include this file
 * (test_boot_modes); same unserialized P6 RDTSC as cpu.h. Lead fix 2026-10-23. */
static inline uint64_t fb_read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/registry.h>
#include <ciuki/fbdev.h>
#include <ciuki/accel.h>
#include <ciuki/display_driver.h>
#include <ciuki/swdib.h>
#include <ciuki/display_batch.h>
#include <ciuki/task.h>
extern void console_framebuffer_revoke(void) __attribute__((weak));
extern void console_framebuffer_changed(const struct fb_device *) __attribute__((weak));
/* f2-92: the display grant's preemption point (grants.c), absent in
 * fixtures that run without the scheduler. */
extern void grants_display_yield(void) __attribute__((weak));

#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
/* G14: the accelerated display probe is the ASD agent's display class. */
#include <ciuki/bootstat.h>
#pragma weak bootstat_begin
#pragma weak bootstat_end
static void accel_class_end(void) { if (bootstat_end) bootstat_end(BOOTSTAT_DISPLAY); }

static struct fb_device device;
static struct kmutex presenter;
static const struct accel_ops *acceleration;
static const struct display_driver *adapter;
static bool initialized, changing;
static struct vfs *accel_vfs;
static bool accel_queued, accel_initializing;
static bool accel_blocked;
/* One reusable normal-priority thread owns marker I/O and initialization.
 * Synchronous serial trace must stay outside the 1-ms device-work budget. */
static struct task *accel_marker_task;
static struct kwait accel_marker_wait;
static enum { ACCEL_MARKER_IDLE, ACCEL_MARKER_BEGIN, ACCEL_MARKER_INIT, ACCEL_MARKER_CLEAR, ACCEL_MARKER_STATUS } accel_marker_request;
static void *change_window;
static uint32_t change_capacity;
static unsigned console_rows;
static unsigned drawing;
/* f2-92: a display-service engine request is between its submission and
 * its proven completion; it may have released the presenter lock between
 * 1 ms wait slices while keeping its `drawing` claim. Mode changes wait. */
static bool accel_inflight;
static struct display_batch *batch_owner;
static bool batch_cursor_shape;
static uint64_t batch_slice_start;
static bool batch_recovery_active;
static struct task *batch_recovery_task;
static uint32_t batch_video_first, batch_video_end, batch_video_generation;
static uint32_t offscreen_end(uint32_t *first);
static uint64_t full_count, full_bytes, full_total_us, full_last_us, full_max_us, full_log_tick;
enum { CONSOLE_BUSY = 1, PRESENT_BUSY = 2 };

static void presenter_unlock(void)
{
    if (!accel_initializing && display_driver_refresh()) accel_blocked=true;
    batch_video_first=batch_video_end=0;
    batch_video_generation=device.generation;
    if (!accel_initializing && !accel_blocked) batch_video_end=offscreen_end(&batch_video_first);
    kmutex_unlock(&presenter);
}

void fbdev_console_region(unsigned rows)
{
    __atomic_store_n(&console_rows, rows, __ATOMIC_RELEASE);
}

unsigned fbdev_native_capabilities(void)
{ return acceleration && !accel_initializing ? acceleration->capabilities(acceleration->ctx) : 0; }

bool fbdev_cpu_ready(void)
{ return !acceleration || acceleration->cpu_ready(acceleration->ctx); }

bool fbdev_console_begin(void)
{
    if (!fbdev_cpu_ready()) return false;
    unsigned idle = 0;
    return __atomic_compare_exchange_n(&drawing, &idle, CONSOLE_BUSY, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void fbdev_console_end(void)
{
    framebuffer_write_drain();
    __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
}

static bool present_begin(const struct fb_device *d, const struct fb_rect *c)
{
    if (d == &device && batch_owner) return false;
    if (d != &device || (!acceleration && (unsigned)c->y >= __atomic_load_n(&console_rows, __ATOMIC_ACQUIRE)))
        return true;
    unsigned idle = 0;
    return __atomic_compare_exchange_n(&drawing, &idle, PRESENT_BUSY, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static uint64_t present_end(const struct fb_device *d, const struct fb_rect *c, bool timed)
{
    if (d == &device) framebuffer_write_drain();
    uint64_t end = timed ? framebuffer_cycles() : 0;
    if (d == &device && (acceleration || (unsigned)c->y < __atomic_load_n(&console_rows, __ATOMIC_ACQUIRE)))
        __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
    return end;
}

/* VESA VBE Core Functions 3.0 (1998-09-16), pp. 14, 30-31, 37-39:
 * https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf
 * 24/32 bpp occupy 3/4 bytes. Pitch is a logical scanline byte count,
 * not width*bytes. VBE 3.0 LFB pitch/masks use the Lin* fields, whereas
 * VBE 2.x uses the ordinary fields. Consume the loader's normalized pitch
 * and RGB masks; the full copy supplies the omitted reserved position.
 * Reserved bits are written zero; X in the source is never a colour. */
static bool layout_valid(const struct fb_device *d)
{
    if (!d->width || !d->height || d->width > INT32_MAX ||
        d->height > INT32_MAX || (d->bpp != 16 && d->bpp != 24 && d->bpp != 32))
        return false;
    if ((uint64_t)d->width * (d->bpp / 8) > d->pitch ||
        (uint64_t)d->pitch * d->height > d->size)
        return false;
    uint32_t used = 0;
    const uint8_t fields[4][2] = {
        { d->red_size, d->red_pos }, { d->green_size, d->green_pos },
        { d->blue_size, d->blue_pos }, { d->rsvd_size, d->rsvd_pos },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(fields); i++) {
        uint32_t n = fields[i][0], p = fields[i][1];
        if ((!n && i < 3) || n > 8 || (n && p + n > d->bpp))
            return false;
        uint32_t mask = n ? ((1u << n) - 1u) << p : 0;
        if (used & mask)
            return false;
        used |= mask;
    }
    return true;
}

static bool geometry_valid(const struct fb_device *d)
{
    return layout_valid(d) && d->mapped && (uintptr_t)d->mapped <= UINTPTR_MAX - d->size;
}

int fbdev_init(void)
{
    if (initialized)
        return 0;
    if ((g_boot.flags & CBI_F_TEXT_MODE) || !g_boot.fb_phys) {
        device.handle = -1;
        initialized = true;
        kmutex_init(&presenter);
        return 0;
    }
    uint64_t size = (uint64_t)g_boot.fb_pitch * g_boot.fb_height;
    if (!size || size > UINT32_MAX || size > UINT32_MAX - g_boot.fb_phys)
        return -EINVAL;
    struct fb_device d = {
        .width = g_boot.fb_width, .height = g_boot.fb_height,
        .pitch = g_boot.fb_pitch, .size = (uint32_t)size, .phys = g_boot.fb_phys,
        .bpp = g_boot.fb_bpp, .red_size = g_boot.fb_red_size, .red_pos = g_boot.fb_red_pos,
        .green_size = g_boot.fb_green_size, .green_pos = g_boot.fb_green_pos,
        .blue_size = g_boot.fb_blue_size, .blue_pos = g_boot.fb_blue_pos,
        .rsvd_size = g_boot.fb_rsvd_size, .mode_id = g_boot.vbe_mode, .handle = -1,
    };
    const uint8_t *m = g_boot.vbe_mode_info;
    bool linear = (g_boot.flags & CBI_F_VBE_CTRL_VALID) &&
                  (g_boot.vbe_ctrl[4] | (uint32_t)g_boot.vbe_ctrl[5] << 8) >= 0x300;
    unsigned fields = linear ? 54 : 31;
    d.rsvd_pos = m[fields + 7];
    /* Reject disagreement with the copied mode description before claiming.
     * Attribute bits: supported, graphics, LFB; memory model 6 = direct. */
    unsigned pitch_off = linear ? 50 : 16;
    uint32_t copied_pitch = m[pitch_off] | (uint32_t)m[pitch_off + 1] << 8;
    uint32_t copied_width = m[18] | (uint32_t)m[19] << 8;
    uint32_t copied_height = m[20] | (uint32_t)m[21] << 8;
    uint32_t copied_phys = m[40] | (uint32_t)m[41] << 8 | (uint32_t)m[42] << 16 | (uint32_t)m[43] << 24;
    if ((m[0] & 0x91) != 0x91 || m[27] != 6 || copied_pitch != d.pitch ||
        copied_width != d.width || copied_height != d.height || m[25] != d.bpp || copied_phys != d.phys ||
        m[fields] != d.red_size || m[fields + 1] != d.red_pos ||
        m[fields + 2] != d.green_size || m[fields + 3] != d.green_pos ||
        m[fields + 4] != d.blue_size || m[fields + 5] != d.blue_pos ||
        m[fields + 6] != d.rsvd_size)
        return -EINVAL;
    if (!layout_valid(&d))
        return -EINVAL;
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (r && r->type == RES_MMIO && r->start == d.phys && r->end == d.phys + d.size &&
            r->owner && !strncmp(r->owner, "boot-framebuffer", sizeof("boot-framebuffer"))) {
            d.handle = (int)i;
            int rc = registry_claim_reserved(d.handle, r->generation, "fbdev");
            if (rc)
                return rc;
            d.generation = registry_get(i)->generation;
            break;
        }
    }
    /* A missing reservation is an ownership error, not an absent display.
     * registry_init currently leaves this reservation CLAIMED: the lead
     * must publish it as FIRMWARE/DISCOVERED for the transfer API above. */
    if (d.handle < 0)
        return -ENODEV;
    d.mapped = vmm_map_framebuffer(d.phys, d.size);
    if (!d.mapped) {
        registry_quarantine(d.handle, d.generation);
        return -ENOMEM;
    }
    int rc = registry_activate(d.handle, d.generation);
    if (rc) {
        registry_quarantine(d.handle, d.generation);
        return rc;
    }
    d.present = true;
    kmutex_init(&presenter);
    device = d;
    initialized = true;
    return 0;
}

static void accel_initialize(void)
{
    bool ready = false;
    /* Entered only after this thread committed and closed the
     * attempt marker. This phase owns register access, never file I/O. */
    kmutex_lock(&presenter);
    bool run = !changing && device.present && !acceleration;
    if (run) accel_initializing = true;
    presenter_unlock();
    if (run) {
        /* No engine command is submitted during setup. CPU rendering and
         * console output remain available across reset, polls and logging. */
        const struct accel_ops *ops = adapter->start(&device, 0);
        ready = ops != 0;
        if (!ready && adapter && adapter->fallback) ops = adapter->fallback();
        kmutex_lock(&presenter);
        acceleration = ops;
        accel_initializing = ready;
        presenter_unlock();
    }
    accel_marker_request = ready ? ACCEL_MARKER_CLEAR : ACCEL_MARKER_STATUS;
    kwait_wake_all(&accel_marker_wait);
}

int fbdev_accel_probe(const struct fb_device *d, int (*probe)(void *), void *ctx)
{
    int err = -EBUSY;
    kmutex_lock(&presenter);
    if (d == &device && accel_initializing && !changing && device.present && !acceleration) {
        unsigned idle = 0;
        if (__atomic_compare_exchange_n(&drawing, &idle, PRESENT_BUSY, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            err = probe(ctx);
            /* Before releasing the console, publish a rejecting guard if
             * the probe left a writer whose completion cannot be proved. */
            if (err && adapter && adapter->fallback) acceleration = adapter->fallback();
            framebuffer_write_drain();
            __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
        }
    }
    presenter_unlock();
    return err;
}

static void accel_marker_run(void)
{
    if (accel_marker_request == ACCEL_MARKER_IDLE) return;
    if (accel_marker_request == ACCEL_MARKER_INIT) {
        accel_marker_request = ACCEL_MARKER_IDLE;
        accel_initialize();
        return;
    }
    if (accel_marker_request == ACCEL_MARKER_STATUS) {
        accel_marker_request = ACCEL_MARKER_IDLE;
        if (adapter->attempt_failed) adapter->attempt_failed(accel_vfs);
        accel_blocked = true;
        display_driver_software("start_failed");
        accel_queued = false;
        accel_class_end();
        return;
    }
    bool clear = accel_marker_request == ACCEL_MARKER_CLEAR;
    accel_marker_request = ACCEL_MARKER_IDLE;
    if (!clear) {
        /* VFS/ATA locks must never nest under the presenter mutex. The
         * registry's keyed status (DM8, including an explicit user retry)
         * comes first: a retained attempt never reaches the driver. */
        bool retained = false;
        int status = display_attempt_prepare(accel_vfs, adapter, display_driver_owner(), &retained);
        if (status || retained || !adapter->attempt_begin || adapter->attempt_begin(accel_vfs)) {
            accel_queued = false;
            accel_class_end();
            accel_blocked = true;
            display_driver_software(!status && retained ? DISPLAY_ATTEMPT_RETAINED : "marker_unavailable");
            return;
        }
        accel_marker_request = ACCEL_MARKER_INIT;
        return;
    }
    if (!adapter->attempt_end || adapter->attempt_end(accel_vfs)) {
        /* The clear did not reach the disk: the next boot would skip the
         * accelerator anyway; keep the software presenter now as well. */
        accel_blocked = true;
        kmutex_lock(&presenter);
        if (acceleration) {
            /* Failed stop retains the rejecting guard and console exclusion. */
            int e = acceleration->stop(acceleration->ctx);
            if (!e) acceleration = 0;
        }
        if (adapter->quarantine) adapter->quarantine("marker_clear_failed");
        if (!acceleration && adapter->fallback) acceleration = adapter->fallback();
        presenter_unlock();
    }
    kmutex_lock(&presenter);
    accel_initializing = false;
    presenter_unlock();
    accel_queued = false;
    accel_class_end();
}

static bool accel_marker_pending(void *arg)
{ (void)arg; return accel_marker_request != ACCEL_MARKER_IDLE; }
static void accel_marker_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (kwait_wait_until(&accel_marker_wait, accel_marker_pending, 0, deadline_after_ms(1000)))
            accel_marker_run();
    }
}

void fbdev_accel_schedule(struct vfs *vfs)
{
    if (accel_queued) return; /* keep the pending volume and binding stable */
    accel_vfs = vfs;
    if (!initialized || !device.present || acceleration || accel_blocked) return;
    adapter = display_driver_bind(&device);
    const char *reason = 0;
    if (display_accel_disabled(&reason)) { display_driver_software(reason); return; }
    if (!adapter) { display_driver_software("no_driver"); return; }
    display_driver_software(0);
    if (!adapter->eligible || !adapter->eligible(&device)) return;
    if (!accel_marker_task) {
        kwait_init(&accel_marker_wait);
        accel_marker_task = task_create_kernel("display-marker", accel_marker_main, 0, P_NORMAL);
        if (!accel_marker_task) {
            accel_blocked=true;
            display_driver_software("marker_worker_unavailable");
            klog("[accel] driver quarantined reason=marker_worker_unavailable");
            return;
        }
        task_start(accel_marker_task);
    }
    if (bootstat_begin) bootstat_begin(BOOTSTAT_DISPLAY);
    accel_queued = true;
    accel_marker_request = ACCEL_MARKER_BEGIN;
    kwait_wake_all(&accel_marker_wait);
}

const struct fb_device *fbdev_get(void) { return &device; }

bool fbdev_rect_valid(const struct fb_rect *r)
{
    return r && r->width >= 0 && r->height >= 0 &&
           r->x <= INT32_MAX - r->width && r->y <= INT32_MAX - r->height;
}

static bool clip(const struct fb_device *d, const struct fb_rect *r, struct fb_rect *c)
{
    int32_t x1 = r->x + r->width, y1 = r->y + r->height;
    c->x = r->x > 0 ? r->x : 0;
    c->y = r->y > 0 ? r->y : 0;
    if (x1 > (int32_t)d->width)
        x1 = (int32_t)d->width;
    if (y1 > (int32_t)d->height)
        y1 = (int32_t)d->height;
    if (!r->width || !r->height || x1 <= c->x || y1 <= c->y)
        return false;
    c->width = x1 - c->x;
    c->height = y1 - c->y;
    return true;
}


int fbdev_present_to(const struct fb_device *d, const struct fb_surface *s, const struct fb_rect *r)
{
    if (!d || !d->present)
        return -ENODEV;
    if (!geometry_valid(d) || !fbdev_rect_valid(r) || !s || s->width > INT32_MAX ||
        s->height > INT32_MAX || (uint32_t)r->width > s->width || (uint32_t)r->height > s->height ||
        (uint64_t)s->width * 4 > s->pitch || (uint64_t)s->pitch * s->height > s->size ||
        (s->size && (!s->pixels || (uintptr_t)s->pixels > UINTPTR_MAX - s->size)))
        return -EINVAL;
    struct fb_rect c;
    if (!clip(d, r, &c))
        return 0;
    if (!present_begin(d, &c))
        return -EBUSY;
    if (d == &device && acceleration && (acceleration->sync(acceleration->ctx) || !acceleration->cpu_ready(acceleration->ctx))) {
        (void)present_end(d, &c, false); return -EIO;
    }
    bool timed = d == &device && g_cpu_tsc && g_tsc_per_ms &&
                 !c.x && !c.y && c.width == (int32_t)d->width && c.height == (int32_t)d->height;
    uint64_t start = timed ? framebuffer_cycles() : 0;
    swdib_present(d, s, r, &c);
    uint64_t end = present_end(d, &c, timed);
    if (timed) {
        uint64_t cycles = end - start;
        /* Avoid overflowing cycles*1000; calibration is fixed after boot. */
        full_last_us = (cycles / g_tsc_per_ms) * 1000 +
                       (cycles % g_tsc_per_ms) * 1000 / g_tsc_per_ms;
        full_count++;
        full_bytes += (uint64_t)(uint32_t)c.width * (uint32_t)c.height * (d->bpp / 8);
        full_total_us += full_last_us;
        if (full_last_us > full_max_us) full_max_us = full_last_us;
        uint64_t now = g_ticks;
        if ((g_boot.flags & CBI_F_VERBOSE) && (full_count == 1 || now - full_log_tick >= 1000)) {
            full_log_tick = now;
            /* bytes/us is decimal MB/s; count pixels, excluding pitch padding.
             * Accumulate bytes so mode changes cannot distort the average. */
            (full_count == 1 ? klog : klog_periodic)("[display] present full count=%llu total_us=%llu last_us=%llu max_us=%llu tsc_khz=%llu lfb_mb_s=%llu",
                 full_count, full_total_us, full_last_us, full_max_us, g_tsc_per_ms,
                 full_total_us ? full_bytes / full_total_us : 0);
        }
    }
    return 0;
}

int fbdev_fill_to(const struct fb_device *d, const struct fb_rect *r, uint32_t colour)
{
    if (!d || !d->present)
        return -ENODEV;
    if (!geometry_valid(d) || !fbdev_rect_valid(r))
        return -EINVAL;
    struct fb_rect c;
    if (!clip(d, r, &c))
        return 0;
    if (!present_begin(d, &c))
        return -EBUSY;
    colour = swdib_pack(d, colour);
    if (d == &device) framebuffer_write_drain();
    if (d == &device && acceleration && !accel_initializing && (acceleration->capabilities(acceleration->ctx)&1u)) {
        int err = acceleration->fill_rect(acceleration->ctx, &c, colour);
        if (!err) { display_driver_count(CIUKI_ENGINE_FILL, true); (void)present_end(d, &c, false); return 0; }
    }
    if (d == &device && acceleration && (acceleration->sync(acceleration->ctx) || !acceleration->cpu_ready(acceleration->ctx))) {
        (void)present_end(d,&c,false); return -EIO;
    }
    swdib_fill(d, &c, colour);
    if (d == &device) display_driver_count(CIUKI_ENGINE_FILL, false);
    (void)present_end(d, &c, false);
    return 0;
}

static bool live(void)
{
    if (!initialized || changing || !device.present || !registry_valid(device.handle, device.generation))
        return false;
    const struct resource *r = registry_get((unsigned)device.handle);
    return r && r->state == RS_ACTIVE;
}

int fbdev_present(const struct fb_surface *src, const struct fb_rect *dst)
{
    if (!initialized || !device.present)
        return -ENODEV;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int rc = live() ? fbdev_present_to(&device, src, dst) : -ENODEV;
    presenter_unlock();
    display_driver_record();
    return rc;
}

int fbdev_fill(const struct fb_rect *rect, uint32_t colour)
{
    if (!initialized || !device.present)
        return -ENODEV;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int rc = live() ? fbdev_fill_to(&device, rect, colour) : -ENODEV;
    presenter_unlock();
    display_driver_record();
    return rc;
}

int fbdev_present_native(uint32_t generation, unsigned x, unsigned y,
                          const void *pixels, unsigned count)
{
    if (!pixels || !count || count > 256) return -EINVAL;
    kmutex_lock(&presenter);
    int e = 0;
    struct fb_rect r = {(int32_t)x,(int32_t)y,(int32_t)count,1};
    if (!live() || generation != device.generation) e = -EIO;
    else if (x >= device.width || count > device.width-x || y >= device.height) e = -EINVAL;
    else if (!present_begin(&device,&r)) e = -EBUSY;
    else {
        if (acceleration && (acceleration->sync(acceleration->ctx) || !acceleration->cpu_ready(acceleration->ctx))) e = -EIO;
        if (!e) {
            unsigned bytes = device.bpp/8;
            uint8_t *dst = device.mapped+y*device.pitch+x*bytes;
            const uint8_t *src = pixels;
            for (unsigned i=0;i<count*bytes;++i) *(volatile uint8_t *)(dst+i)=src[i];
        }
        (void)present_end(&device,&r,false);
    }
    presenter_unlock();
    return e;
}

/* Source and destination already clipped together by the grant service. */
int fbdev_copy_to(const struct fb_device *d, const struct fb_rect *r, int32_t x, int32_t y)
{
    if (!d || !d->present) return -ENODEV;
    struct fb_rect dst = {x,y,r ? r->width : 0,r ? r->height : 0};
    if (!geometry_valid(d) || !fbdev_rect_valid(r) || !fbdev_rect_valid(&dst) ||
        r->x < 0 || r->y < 0 || x < 0 || y < 0 ||
        (uint32_t)(r->x+r->width) > d->width || (uint32_t)(r->y+r->height) > d->height ||
        (uint32_t)(x+r->width) > d->width || (uint32_t)(y+r->height) > d->height) return -EINVAL;
    if (!r->width || !r->height) return 0;
    struct fb_rect busy = dst;
    if (r->y < busy.y) busy.y = r->y;
    if (!present_begin(d, &busy)) return -EBUSY;
    if (d == &device && acceleration && (acceleration->capabilities(acceleration->ctx) & 1u)) {
        framebuffer_write_drain();
        int e = acceleration->copy_rect(acceleration->ctx,r,x,y);
        if (!e) display_driver_count(CIUKI_ENGINE_COPY, true);
        if (e != -EOPNOTSUPP) {
        (void)present_end(d,&busy,false);
        /* A partially executed overlapping blit has lost its original source.
         * Caller must rebuild destination from its authoritative RAM surface. */
        return e ? -EAGAIN : 0;
        }
    }
    if (d == &device && acceleration && (acceleration->sync(acceleration->ctx) || !acceleration->cpu_ready(acceleration->ctx))) {
        (void)present_end(d,&busy,false); return -EIO;
    }
    if (d == &device && acceleration && !acceleration->cpu_ready(acceleration->ctx)) {
        (void)present_end(d,&busy,false); return -EIO;
    }
    swdib_copy(d, r, x, y);
    if (d == &device) display_driver_count(CIUKI_ENGINE_COPY, false);
    (void)present_end(d,&busy,false);
    return 0;
}

int fbdev_accel_info(uint32_t generation, struct ciuki_accel_info *out)
{
    if (!out) return -EINVAL;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int err = !live() ? -ENODEV : generation != device.generation ? -EAGAIN : 0;
    if (!err) {
        memset(out, 0, sizeof(*out));
        out->generation = device.generation;
        out->capabilities = fbdev_native_capabilities();
        if (!accel_initializing && adapter && adapter->snapshot) adapter->snapshot(out);
    }
    presenter_unlock();
    return err;
}

int fbdev_engine_info(uint32_t generation, struct ciuki_display_engine_info *out)
{
    if (!out) return -EINVAL;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int e = !live() ? -ENODEV : generation != device.generation ? -EAGAIN : 0;
    if (!e) {
        display_driver_info(out);
        out->generation = device.generation;
        out->capabilities = fbdev_native_capabilities();
        out->cpu_ready = !acceleration || acceleration->cpu_ready(acceleration->ctx);
        if (accel_initializing) out->ready = out->native_used = out->capabilities = 0;
    }
    presenter_unlock();
    return e;
}

/* f2-92: the display service's engine requests wait for completion in
 * 1 ms slices. Between slices the presenter lock is released and the task
 * may be preempted; the request keeps its `drawing` claim (no CPU writer,
 * no console pixels) and accel_inflight (no mode change), so no other
 * writer can reach the LFB or the BCI meanwhile. */
static uint64_t validate_deadline;
static bool (*validate_cancel)(void *);
static void *validate_context;
static int accel_pause(void *arg)
{
    (void)arg;
    presenter_unlock();
    if (grants_display_yield) grants_display_yield();
    kmutex_lock(&presenter);
    if (batch_owner && g_cpu_tsc && g_tsc_per_ms) batch_slice_start=fb_read_tsc();
    if (validate_deadline && g_ticks >= validate_deadline) return -ETIMEDOUT;
    if (validate_cancel && validate_cancel(validate_context)) return -EINTR;
    return 0;
}
static void service_begin(void)
{
    framebuffer_write_drain();
    accel_inflight = true;
    if (acceleration && acceleration->set_pause) acceleration->set_pause(acceleration->ctx, accel_pause, 0);
}
static void service_end(void)
{
    if (acceleration && acceleration->set_pause) acceleration->set_pause(acceleration->ctx, 0, 0);
    accel_inflight = false;
}
/* The first write or offscreen use proves both through verify_write; caller holds the presenter lock and a drawing claim. */
static void verify_writes(void)
{
    if (!acceleration || !acceleration->verify_write || !(acceleration->capabilities(acceleration->ctx) & 1u)) return;
    service_begin();
    (void)acceleration->verify_write(acceleration->ctx);
    service_end();
}
/* Engine-only rows: [*first, end), 0 without an active offscreen lease. */
static uint32_t offscreen_end(uint32_t *first)
{
    if (!acceleration || !acceleration->offscreen || !(acceleration->capabilities(acceleration->ctx) & 1u))
        return 0;
    return acceleration->offscreen(acceleration->ctx, first);
}
/* Visible rows, or a rectangle wholly inside the offscreen rows. */
static int row_band(int32_t y, uint32_t height, uint32_t first, uint32_t end)
{
    if (y < 0 || !height || height > INT32_MAX) return -1;
    uint64_t bottom = (uint64_t)(uint32_t)y + height;
    if (bottom <= device.height) return 0;
    if (end && (uint32_t)y >= first && bottom <= end) return 1;
    return -1;
}

int fbdev_accel_request(const struct ciuki_accel_request *q)
{
    if (!q || q->size != sizeof(*q) || q->operation > CIUKI_ACCEL_OFFSCREEN ||
        q->operation == CIUKI_ACCEL_WRITE) return -EINVAL;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int err = -ENODEV;
    if (!live()) goto done;
    if ((accel_inflight || accel_initializing) && q->operation != CIUKI_ACCEL_QUERY) { err = -EBUSY; goto done; }
    if (q->generation != device.generation) { err = -EAGAIN; goto done; }
    if (q->operation == CIUKI_ACCEL_QUERY) {
        err = (int)fbdev_native_capabilities(); goto done;
    }
    if (q->operation == CIUKI_ACCEL_OFFSCREEN) {
        struct fb_rect busy = {0,0,(int32_t)device.width,(int32_t)device.height};
        if (!present_begin(&device,&busy)) { err = -EBUSY; goto done; }
        verify_writes();
        (void)present_end(&device,&busy,false);
        uint32_t first = 0, end = offscreen_end(&first);
        err = end > first && end < 65536u ? (int)((end << 16) | first) : 0; goto done;
    }
    if (q->operation == CIUKI_ACCEL_COPY) {
        struct ciuki_rect c = q->rect;
        if (!c.width || !c.height) {
            /* Empty copies keep the original strict screen validation. */
            if (c.width > INT32_MAX || c.height > INT32_MAX || c.src_x < 0 || c.src_y < 0 ||
                c.dst_x < 0 || c.dst_y < 0 || (uint64_t)(uint32_t)c.src_x+c.width > device.width ||
                (uint64_t)(uint32_t)c.src_y+c.height > device.height || (uint64_t)(uint32_t)c.dst_x+c.width > device.width ||
                (uint64_t)(uint32_t)c.dst_y+c.height > device.height) { err=-EINVAL; goto done; }
            struct fb_rect r = {c.src_x,c.src_y,(int32_t)c.width,(int32_t)c.height};
            err = fbdev_copy_to(&device,&r,c.dst_x,c.dst_y); goto done;
        }
        uint32_t first = 0, end = offscreen_end(&first);
        /* Avoid a dependency on process code: strict bounded rectangles. */
        int sb = c.width > INT32_MAX || c.src_x < 0 || (uint64_t)(uint32_t)c.src_x + c.width > device.width ?
            -1 : row_band(c.src_y, c.height, first, end);
        int db = c.width > INT32_MAX || c.dst_x < 0 || (uint64_t)(uint32_t)c.dst_x + c.width > device.width ?
            -1 : row_band(c.dst_y, c.height, first, end);
        if (sb < 0 || db < 0) { err = -EINVAL; goto done; }
        struct fb_rect r = {c.src_x,c.src_y,(int32_t)c.width,(int32_t)c.height};
        service_begin();
        if (!sb && !db) err = fbdev_copy_to(&device,&r,c.dst_x,c.dst_y);
        else {
            /* An engine-only endpoint has no CPU path: the caller
             * reconstructs from its RAM frame after -EAGAIN. */
            struct fb_rect busy = {0,0,(int32_t)device.width,(int32_t)device.height};
            if (!present_begin(&device,&busy)) err = -EBUSY;
            else {
                err = acceleration->copy_rect(acceleration->ctx,&r,c.dst_x,c.dst_y) ? -EAGAIN : 0;
                if (!err) display_driver_count(CIUKI_ENGINE_COPY,true);
                (void)present_end(&device,&busy,false);
            }
        }
        service_end();
        goto done;
    }
    if (q->operation == CIUKI_ACCEL_FILL || q->operation == CIUKI_ACCEL_PATTERN) {
        if (q->rect.width > INT32_MAX || q->rect.height > INT32_MAX) { err=-EINVAL; goto done; }
        struct fb_rect r = {q->rect.dst_x,q->rect.dst_y,(int32_t)q->rect.width,(int32_t)q->rect.height};
        if (q->operation == CIUKI_ACCEL_FILL) {
            service_begin(); err=fbdev_fill_to(&device,&r,q->colour); service_end(); goto done;
        }
        struct fb_rect c;
        if (!fbdev_rect_valid(&r)) { err=-EINVAL; goto done; }
        if (!clip(&device,&r,&c)) { err=0; goto done; }
        if (!present_begin(&device,&c)) { err=-EBUSY; goto done; }
        service_begin();
        err=acceleration && (acceleration->capabilities(acceleration->ctx)&1u) && acceleration->pattern_rect ? acceleration->pattern_rect(acceleration->ctx,&c,q->pattern,swdib_pack(&device,q->colour),swdib_pack(&device,q->background)) : -EOPNOTSUPP;
        if (!err) display_driver_count(CIUKI_ENGINE_PATTERN, true);
        service_end();
        if (err && (!acceleration || (!acceleration->sync(acceleration->ctx) && acceleration->cpu_ready(acceleration->ctx)))) {
            swdib_pattern(&device,&c,q->pattern,swdib_pack(&device,q->colour),swdib_pack(&device,q->background));
            display_driver_count(CIUKI_ENGINE_PATTERN, false);
            err=0;
        }
        (void)present_end(&device,&c,false); goto done;
    }
    if (!acceleration || !(acceleration->capabilities(acceleration->ctx)&2u)) { err=-EOPNOTSUPP; goto done; }
    struct fb_rect busy = {0,0,(int32_t)device.width,(int32_t)device.height};
    if (!present_begin(&device,&busy)) { err=-EBUSY; goto done; }
    if (q->operation == CIUKI_ACCEL_CURSOR_SET) {
        struct accel_cursor cursor;
        memcpy(cursor.mask,q->cursor_mask,sizeof(cursor.mask)); memcpy(cursor.image,q->cursor_image,sizeof(cursor.image));
        cursor.foreground=swdib_pack(&device,q->colour); cursor.background=swdib_pack(&device,q->background);
        err=acceleration->cursor_show(acceleration->ctx,false);
        if (!err) err=acceleration->cursor_set(acceleration->ctx,&cursor);
        if (!err) batch_cursor_shape=true;
    } else if (q->operation == CIUKI_ACCEL_CURSOR_MOVE)
        err=acceleration->cursor_move(acceleration->ctx,q->rect.dst_x,q->rect.dst_y);
    else err=acceleration->cursor_show(acceleration->ctx,q->colour!=0);
    (void)present_end(&device,&busy,false);
done:
    presenter_unlock();
    display_driver_record();
    return err;
}

/* f2-92 CIUKI_ACCEL_WRITE, one band: `rows` holds dst->height rows of
 * dst->width 0xXXRRGGBB pixels, `stride` bytes apart, in kernel memory the
 * caller filled outside every lock; it is converted in place to the native
 * format. Only pixels inside `limit` (already clipped by the caller) are
 * written. Returns 0 when the engine wrote the band, 1 when the CPU did
 * (visible rows, engine absent or failed after a proven idle), -EAGAIN for
 * offscreen rows the engine could not write. */
int fbdev_accel_write(uint32_t generation, const struct fb_rect *dst, const struct fb_rect *limit,
                      void *rows, uint32_t stride)
{
    if (!dst || !limit || !rows || !fbdev_rect_valid(dst) || !fbdev_rect_valid(limit) ||
        dst->x < 0 || dst->y < 0 || dst->width <= 0 || dst->height <= 0 ||
        (uint64_t)(uint32_t)dst->width * 4u > stride || (stride & 3u)) return -EINVAL;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int err = -ENODEV;
    if (!live()) goto done;
    if (accel_inflight || accel_initializing) { err = -EBUSY; goto done; }
    if (generation != device.generation) { err = -EAGAIN; goto done; }
    struct fb_rect busy = {0,0,(int32_t)device.width,(int32_t)device.height};
    if (!present_begin(&device,&busy)) { err = -EBUSY; goto done; }
    verify_writes();
    uint32_t first = 0, end = offscreen_end(&first);
    int band = (uint64_t)(uint32_t)dst->x + (uint32_t)dst->width > device.width ? -1 :
        row_band(dst->y, (uint32_t)dst->height, first, end);
    if (band < 0) { err = -EINVAL; (void)present_end(&device,&busy,false); goto done; }
    struct fb_rect c = *dst;
    {
        int64_t x1 = (int64_t)c.x + c.width, y1 = (int64_t)c.y + c.height;
        if (limit->x > c.x) c.x = limit->x;
        if (limit->y > c.y) c.y = limit->y;
        if ((int64_t)limit->x + limit->width < x1) x1 = (int64_t)limit->x + limit->width;
        if ((int64_t)limit->y + limit->height < y1) y1 = (int64_t)limit->y + limit->height;
        if (x1 <= c.x || y1 <= c.y) { err = 0; (void)present_end(&device,&busy,false); goto done; }
        c.width = (int32_t)(x1 - c.x); c.height = (int32_t)(y1 - c.y);
    }
    swdib_convert(&device, dst, rows, stride);
    if (acceleration && acceleration->write_rect && (acceleration->capabilities(acceleration->ctx) & 1u) &&
        (!acceleration->verify_write || !acceleration->verify_write(acceleration->ctx))) {
        service_begin();
        err = acceleration->write_rect(acceleration->ctx, dst, &c, rows, stride);
        service_end();
        if (!err) { display_driver_count(CIUKI_ENGINE_WRITE, true); (void)present_end(&device,&busy,false); goto done; }
    }
    if (band) { err = -EAGAIN; (void)present_end(&device,&busy,false); goto done; }
    if (acceleration && (acceleration->sync(acceleration->ctx) || !acceleration->cpu_ready(acceleration->ctx))) {
        err = -EIO; (void)present_end(&device,&busy,false); goto done;
    }
    swdib_write(&device, dst, &c, rows, stride);
    display_driver_count(CIUKI_ENGINE_WRITE, false);
    (void)present_end(&device,&busy,false);
    err = 1;
done:
    presenter_unlock();
    display_driver_record();
    return err;
}

/* No calls to the software-dispatch wrappers below: a pass proves native
 * execution. Only a small saved visible block is read, under exclusive
 * drawing ownership and after idle. Cursor evidence belongs to selftest. */
int fbdev_accel_validate(uint64_t deadline, bool (*cancel)(void *), void *ctx, uint32_t *tested)
{
    if (!tested) return -EINVAL;
    *tested=0;
    if (g_ticks>=deadline) return -ETIMEDOUT;
    if (cancel && cancel(ctx)) return -EINTR;
    const char *why;
    if (display_accel_disabled(&why)) return -EOPNOTSUPP;
    kmutex_lock(&presenter);
    int e=-EOPNOTSUPP;
    if (!live() || accel_initializing || accel_inflight || !acceleration ||
        !(acceleration->capabilities(acceleration->ctx)&1u) || device.width<16 || device.height<16) goto unlock;
    struct fb_rect block={0,0,16,16};
    if (!present_begin(&device,&block)) { e=-EBUSY; goto unlock; }
    validate_deadline=deadline; validate_cancel=cancel; validate_context=ctx;
    service_begin();
    uint8_t saved[16*16*4], reference[16*16*4];
    uint32_t upload[16*16];
    unsigned bytes=device.bpp/8;
    struct fb_device ref=device; ref.width=ref.height=16; ref.pitch=16*bytes;
    ref.size=16*ref.pitch; ref.mapped=reference;
    bool saved_ok=false;
    e=acceleration->sync(acceleration->ctx);
    if (e || !fbdev_cpu_ready()) { e=-EIO; goto finish; }
    for (unsigned y=0;y<16;y++) memcpy(saved+y*ref.pitch,device.mapped+y*device.pitch,ref.pitch);
    memcpy(reference,saved,ref.size); saved_ok=true;
    for (unsigned op=0;op<5;op++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        framebuffer_write_drain();
        unsigned kind;
        struct fb_rect source={0,0,12,12};
        if (!op) {
            uint32_t colour=swdib_pack(&device,0x36a5e7);
            swdib_fill(&ref,&block,colour); kind=CIUKI_ENGINE_FILL;
            e=acceleration->fill_rect(acceleration->ctx,&block,colour);
            if (!e) {
                struct fb_rect seed={0,0,4,4};
                uint32_t contrasting=swdib_pack(&device,0xcb7123);
                swdib_fill(&ref,&seed,contrasting);
                e=acceleration->fill_rect(acceleration->ctx,&seed,contrasting);
            }
        } else if (op==1 || op==2) {
            if (op==2) source.x=source.y=4;
            int dst=op==1 ? 4 : 0;
            swdib_copy(&ref,&source,dst,dst); kind=CIUKI_ENGINE_COPY;
            e=acceleration->copy_rect(acceleration->ctx,&source,dst,dst);
        } else if (op==3) {
            if (!acceleration->pattern_rect) continue;
            const uint8_t pattern[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
            uint32_t fg=swdib_pack(&device,0x12c456), bg=swdib_pack(&device,0xd73491);
            swdib_pattern(&ref,&block,pattern,fg,bg); kind=CIUKI_ENGINE_PATTERN;
            e=acceleration->pattern_rect(acceleration->ctx,&block,pattern,fg,bg);
        } else {
            if (!acceleration->write_rect) continue;
            if (acceleration->verify_write && acceleration->verify_write(acceleration->ctx)) { e=-EIO; break; }
            for (unsigned i=0;i<ARRAY_SIZE(upload);i++) upload[i]=i*34571u;
            swdib_convert(&ref,&block,upload,16*4);
            swdib_write(&ref,&block,&block,upload,16*4); kind=CIUKI_ENGINE_WRITE;
            e=acceleration->write_rect(acceleration->ctx,&block,&block,upload,16*4);
        }
        if (e) break; /* refusal is recorded as unsupported, never replayed */
        e=acceleration->sync(acceleration->ctx);
        if (e || !fbdev_cpu_ready()) { e=-EIO; break; }
        for (unsigned y=0;y<16;y++) if (memcmp(reference+y*ref.pitch,device.mapped+y*device.pitch,ref.pitch)) { e=-EIO; break; }
        if (e) break;
        *tested|=1u<<kind;
    }
    if (saved_ok && !acceleration->sync(acceleration->ctx) && fbdev_cpu_ready()) {
        for (unsigned y=0;y<16;y++) memcpy(device.mapped+y*device.pitch,saved+y*ref.pitch,ref.pitch);
        framebuffer_write_drain();
    } else e=-EIO;
finish:
    service_end(); validate_deadline=0; validate_cancel=0; validate_context=0;
    if (e && e!=-EOPNOTSUPP && adapter && adapter->quarantine) {
        accel_blocked=true;
        adapter->quarantine("native_comparison_failed");
        const struct accel_ops *guard=adapter->fallback ? adapter->fallback() : acceleration;
        if (guard) acceleration=guard;
    }
    (void)present_end(&device,&block,false);
unlock:
    presenter_unlock();
    display_driver_record();
    return e;
}
int fbdev_accel_diagnostic(struct vfs *vfs, uint64_t deadline, bool (*cancel)(void *), void *ctx, uint32_t *tested)
{
    if (!(g_boot.flags & CBI_F_HW_CHECK)) return -EPERM;
    const char *why;
    if (display_accel_disabled(&why)) return -EOPNOTSUPP;
    if (!tested) return -EINVAL;
    *tested=0;
    if (g_ticks>=deadline) return -ETIMEDOUT;
    if (cancel && cancel(ctx)) return -EINTR;
    /* No cursor-state export exists for restoring an arbitrary live owner's
     * shape/position. Diagnostics use only their own fresh activation. */
    if (acceleration) return -EBUSY;
    if (!acceleration) {
        if (accel_marker_task || accel_queued || accel_initializing || accel_blocked) return -EBUSY;
        adapter=display_driver_bind(&device);
        if (!adapter || !adapter->eligible || !adapter->eligible(&device)) return -EOPNOTSUPP;
        accel_vfs=vfs;
        if (!adapter->attempt_begin || adapter->attempt_begin(vfs)) return -EIO;
        kwait_init(&accel_marker_wait);
        accel_queued=true;
        accel_initialize();
        accel_marker_run(); /* clear/status I/O, outside presenter */
    }
    int e=fbdev_accel_validate(deadline,cancel,ctx,tested);
    kmutex_lock(&presenter);
    if (!e && acceleration && (acceleration->capabilities(acceleration->ctx)&2u)) {
        struct fb_rect busy={0,0,(int32_t)device.width,(int32_t)device.height};
        if (!present_begin(&device,&busy)) e=-EBUSY;
        else {
            service_begin();
            e=adapter->cursor_test ? adapter->cursor_test(acceleration,deadline,cancel,ctx) : -EOPNOTSUPP;
            service_end();
            if (!e) *tested|=DISPLAY_CHECK_CURSOR;
            (void)present_end(&device,&busy,false);
        }
    }
    if (acceleration) {
        int stop=acceleration->stop(acceleration->ctx);
        if (!stop) acceleration=0;
        else { accel_blocked=true; e=-EIO; }
    }
    presenter_unlock();
    return e;
}

void fbdev_cursor_hide(void)
{
    kmutex_lock(&presenter);
    if (acceleration && !changing && !accel_inflight && (acceleration->capabilities(acceleration->ctx)&2u))
        (void)acceleration->cursor_show(acceleration->ctx,false);
    presenter_unlock();
}

/* VBE 3.0 functions 01h/02h: a mode can change its LFB address. Reject RAM
 * aliases and invalid RGB masks before claiming or mapping the result. */
int fbdev_mode_decode(uint16_t id, const uint8_t m[256], struct fb_device *d)
{
    unsigned p = g_boot.display.vbe_version >= 0x300 ? 50 : 16;
    unsigned f = g_boot.display.vbe_version >= 0x300 ? 54 : 31;
    *d = (struct fb_device){.mode_id = id, .handle = -1, .present = true,
        .width = m[18] | (uint32_t)m[19] << 8, .height = m[20] | (uint32_t)m[21] << 8,
        .pitch = m[p] | (uint32_t)m[p + 1] << 8, .bpp = m[25],
        .phys = m[40] | (uint32_t)m[41] << 8 | (uint32_t)m[42] << 16 | (uint32_t)m[43] << 24,
        .red_size = m[f], .red_pos = m[f + 1], .green_size = m[f + 2], .green_pos = m[f + 3],
        .blue_size = m[f + 4], .blue_pos = m[f + 5], .rsvd_size = m[f + 6], .rsvd_pos = m[f + 7]};
    d->size = d->pitch * d->height;
    if ((m[0] & 0x91) != 0x91 || m[24] != 1 || m[27] != 6 || !layout_valid(d) ||
        d->width < 640 || d->width > 1600 || d->height < 480 || d->height > 1200 ||
        d->phys < 0x100000 || d->size > UINT32_MAX - d->phys ||
        d->size > g_boot.display.vram_bytes) return -EINVAL;
    uint64_t end = (uint64_t)d->phys + d->size;
    if (d->phys < g_boot.kernel_end && end > g_boot.kernel_start) return -EINVAL;
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type == CBI_E820_RAM && d->phys < e->base + e->length && end > e->base) return -EINVAL;
    }
    return 0;
}

static bool change_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    if (drawing) return false;
    framebuffer_write_drain();
    console_framebuffer_revoke();
    vmm_revoke_mmio(device.mapped, device.size);
    device.mapped = 0;
    device.present = false;
    return true;
}

int fbdev_change_begin(void)
{
    if (batch_owner) { batch_owner->cancel=true; return -EBUSY; }
    if (!initialized || changing || accel_initializing || accel_inflight || !live()) return -EBUSY;
    if (!console_framebuffer_revoke || !console_framebuffer_changed) return -ENODEV;
    changing = true; /* stop submissions before waiting for the last presenter */
    kmutex_lock(&presenter);
    if (accel_initializing || accel_inflight) {
        changing = false; presenter_unlock(); return -EBUSY;
    }
    if (acceleration && acceleration->stop(acceleration->ctx)) {
        changing = false; presenter_unlock(); return -EIO;
    }
    acceleration=0; /* successful stop may have revoked register mappings */
    batch_cursor_shape=false;
    if (!change_window) {
        uint32_t capacity = device.size;
        for (unsigned i = 0; i < g_boot.display.mode_count; i++) {
            uint32_t size = (uint32_t)g_boot.display.modes[i].pitch * g_boot.display.modes[i].height;
            if (size > capacity) capacity = size;
        }
        change_capacity = PAGE_ALIGN_UP(capacity + PAGE_SIZE); /* nonaligned physical base */
        change_window = vmm_reserve_mmio(change_capacity);
        if (!change_window) { changing = false; presenter_unlock(); return -ENOMEM; }
    }
    int e = registry_quiesce(device.handle, device.generation, change_idle);
    presenter_unlock();
    if (e) { changing = false; device.present = false; }
    return e;
}

int fbdev_change_commit(const struct fb_device *next)
{
    if (!changing || !layout_valid(next) || (uint64_t)next->size + (next->phys & 0xfff) > change_capacity) return -EINVAL;
    struct fb_device d = *next;
    const struct resource *old = registry_get((unsigned)device.handle);
    if (old && old->state == RS_QUIESCING) {
        int e = registry_release(device.handle, device.generation);
        if (e) return e;
    }
    d.handle = registry_claim(RES_MMIO, d.phys, d.phys + d.size, "fbdev", false);
    if (d.handle < 0) return d.handle;
    d.generation = registry_get((unsigned)d.handle)->generation;
    d.mapped = vmm_replace_framebuffer(change_window, change_capacity, d.phys, d.size);
    if (!d.mapped || registry_activate(d.handle, d.generation)) {
        registry_quarantine(d.handle, d.generation);
        return -EIO;
    }
    d.present = true;
    device = d;
    console_framebuffer_changed(&device);
    return 0;
}

void fbdev_change_end(void)
{
    kmutex_lock(&presenter);
    changing = false;
    presenter_unlock();
    if (accel_vfs) fbdev_accel_schedule(accel_vfs);
}

/* Batch ownership is deliberately stronger than presenter ownership. begin
 * claims the entire drawing domain even on a software-only boot; that claim
 * and accel_inflight survive every CPU/native wait slice and unsafe return. */
static int video_target_snapshot(uint32_t generation, uint32_t first, uint32_t end, struct swdib_target *out)
{
    if (!live() || generation!=device.generation) return -EAGAIN;
    if (end<=first || first<device.height) return -EAGAIN;
    uint64_t offset=(uint64_t)first*device.pitch, size=(uint64_t)(end-first)*device.pitch;
    if (size>CIUKI_BATCH_MAX_EXTENT || (uint64_t)device.phys+offset+size>UINT32_MAX) return -EOVERFLOW;
    *out=(struct swdib_target){.device=device,.identity=&device,.offset=offset,.rights=PROT_READ|PROT_WRITE,.kind=CIUKI_TARGET_VIDEO};
    out->device.phys+=(uint32_t)offset; out->device.height=end-first;
    out->device.size=(uint32_t)size; out->device.mapped=NULL;
    return 0;
}
static int video_target_locked(uint32_t generation, struct swdib_target *out)
{
    uint32_t first=0, end=offscreen_end(&first);
    return video_target_snapshot(generation,first,end,out);
}
int fbdev_batch_video_target(uint32_t generation, struct swdib_target *out)
{
    /* Cached UP snapshot: preflight takes no display lock and issues no
     * driver call. Admission rechecks the live lease under presenter. */
    if (accel_inflight || accel_initializing) return -EBUSY;
    if (generation!=batch_video_generation) return -EAGAIN;
    return video_target_snapshot(generation,batch_video_first,batch_video_end,out);
}
static int batch_begin(void *arg, struct display_batch *b)
{
    (void)arg;
    if (b->authority) { int e=b->authority(b->interrupted_arg); if (e) return e; }
    if (changing || batch_owner || accel_inflight || accel_initializing) return -EBUSY;
    if (!initialized || !device.present) return -ENODEV;
    if (b->header.generation!=device.generation) return -EAGAIN;
    if (!live()) return -EAGAIN;
    if (!fbdev_cpu_ready()) return -EAGAIN;
    bool shape=batch_cursor_shape;
    for (unsigned i=0;i<b->header.record_count;i++) {
        const struct ciuki_batch_record *r=(const void *)(b->blob+64+i*128);
        if (r->opcode==CIUKI_CMD_CURSOR_MONO_SET || r->opcode==CIUKI_CMD_CURSOR_ARGB_SET) shape=true;
        else if ((r->opcode==CIUKI_CMD_CURSOR_MOVE || r->opcode==CIUKI_CMD_CURSOR_SHOW) && !shape) return -EINVAL;
    }
    for (unsigned i=0;i<b->header.target_count;i++) if (b->targets[i].kind==CIUKI_TARGET_SCANOUT &&
        (b->targets[i].device.generation!=device.generation || b->targets[i].device.mapped!=device.mapped ||
         b->targets[i].device.handle!=device.handle)) return -EAGAIN;
    for (unsigned i=0;i<b->header.target_count;i++) if (b->targets[i].kind==CIUKI_TARGET_VIDEO) {
        struct swdib_target now;
        int e=video_target_locked(b->targets[i].device.generation,&now); if (e) return e;
        if (now.offset!=b->targets[i].offset || now.device.size!=b->targets[i].device.size ||
            now.device.phys!=b->targets[i].device.phys) return -EAGAIN;
    }
    unsigned idle=0;
    if (!__atomic_compare_exchange_n(&drawing,&idle,PRESENT_BUSY,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return -EBUSY;
    batch_owner=b; b->cursor_shape=batch_cursor_shape;
    batch_slice_start=g_cpu_tsc && g_tsc_per_ms ? fb_read_tsc() : 0;
    service_begin();
    return 0;
}
static int batch_native(void *arg, const struct swdib_command *c)
{
    (void)arg;
    if (!acceleration || accel_blocked || !acceleration->command || !acceleration->command_bits ||
        !(acceleration->command_bits(acceleration->ctx)&(1u<<c->record->opcode)) ||
        (c->dst && c->dst->kind==CIUKI_TARGET_SYSTEM) || (c->src && c->src->kind==CIUKI_TARGET_SYSTEM)) return -EOPNOTSUPP;
    if (c->record->opcode==CIUKI_CMD_SCALED && c->record->filter==CIUKI_FILTER_BILINEAR &&
        (!acceleration->command_features || !(acceleration->command_features(acceleration->ctx)&CIUKI_CAP_BILINEAR))) return -EOPNOTSUPP;
    if (c->record->opcode==CIUKI_CMD_CURSOR_ARGB_SET) {
        uint32_t w=0,h=0;
        if (acceleration->cursor_limits) acceleration->cursor_limits(acceleration->ctx,&w,&h);
        if (!w || !h || w>64 || h>64 || c->record->dw>w || c->record->dh>h) return -EOPNOTSUPP;
    }
    framebuffer_write_drain();
    return acceleration->command(acceleration->ctx,c);
}
static int batch_sync(void *arg)
{
    (void)arg;
    int e=acceleration ? acceleration->sync(acceleration->ctx) : 0;
    if (!e && !fbdev_cpu_ready()) e=-EIO;
    framebuffer_write_drain();
    return e;
}
static bool batch_recovery(void *arg)
{
    struct display_batch *b=arg;
    if (batch_recovery_active) return false;
    batch_recovery_active=true; batch_recovery_task=g_current;
    int e=batch_sync(arg);
    if (b->reply.flags&CIUKI_FENCE_PARTIAL_DIRTY) {
        accel_blocked=true;
        if (adapter && adapter->quarantine) adapter->quarantine("batch completion failure");
        /* stop is also the authoritative cursor disable. Quarantine by itself
         * never authorizes CPU pixels or the desktop software cursor. */
        if (acceleration) {
            e=acceleration->stop(acceleration->ctx);
            if (!e && acceleration->cpu_ready(acceleration->ctx)) acceleration=0;
            else if (!e) e=-EIO;
        }
        batch_cursor_shape=false;
        if (!e && !fbdev_cpu_ready()) e=-EIO;
    }
    batch_recovery_active=false; batch_recovery_task=NULL;
    return !e;
}
static void batch_end(void *arg)
{
    struct display_batch *b=arg;
    framebuffer_write_drain();
    batch_cursor_shape=b->cursor_shape && !(b->reply.flags&CIUKI_FENCE_PARTIAL_DIRTY);
    service_end(); batch_owner=0;
    __atomic_store_n(&drawing,0,__ATOMIC_RELEASE);
}
static bool batch_cancelled(void *arg)
{
    struct display_batch *b=arg;
    if (b->interrupted && b->interrupted(b->interrupted_arg)) b->interrupt_error=-EINTR;
    return b->cancel || b->interrupt_error;
}
static void batch_tile(void *arg)
{
    struct display_batch *b=arg;
    /* Check frequently, but yield only when the measured slice is due.
     * Half a millisecond leaves room for the next bounded CPU tile; without
     * calibrated TSC, every eight-sample checkpoint yields conservatively. */
    if (g_cpu_tsc && g_tsc_per_ms>1 && fb_read_tsc()-batch_slice_start<g_tsc_per_ms/2) return;
    framebuffer_write_drain();
    (void)accel_pause(0);
    if (batch_cancelled(b)) b->cancel=true;
}
static void batch_count(void *arg, unsigned opcode, bool native)
{ (void)arg; display_driver_command_count(opcode,native); }
static struct display_batch_ops batch_ops(struct display_batch *b)
{
    return (struct display_batch_ops){b,batch_begin,batch_native,batch_sync,batch_recovery,batch_end,batch_tile,batch_cancelled,batch_count};
}
int fbdev_batch_execute(struct display_batch *b)
{
    kmutex_lock(&presenter);
    struct display_batch_ops ops=batch_ops(b);
    int e=display_batch_execute(b,&ops);
    presenter_unlock();
    display_driver_record();
    return e;
}
bool fbdev_batch_recover(struct display_batch *b)
{
    kmutex_lock(&presenter);
    bool safe=(b->reply.state==CIUKI_FENCE_SUCCEEDED || b->reply.state==CIUKI_FENCE_STOPPED) &&
        (b->reply.flags&CIUKI_FENCE_CPU_SAFE);
    if (b->reply.state==CIUKI_FENCE_RECOVERING && batch_owner==b && !batch_recovery_active) {
        struct display_batch_ops ops=batch_ops(b);
        safe=display_batch_recover(b,&ops);
    }
    presenter_unlock();
    display_driver_record();
    return safe;
}
void fbdev_batch_abort(struct display_batch *b)
{
    kmutex_lock(&presenter);
    if (batch_owner==b) {
        b->cancel=true;
        b->reply.error=-ECANCELED;
        b->reply.first_uncompleted=b->reply.completed;
        b->reply.flags|=CIUKI_FENCE_CANCEL_REQUESTED|CIUKI_FENCE_PARTIAL_DIRTY;
        b->reply.state=CIUKI_FENCE_RECOVERING;
        /* The exiting caller abandons its stack. Recovery is performed by
         * the retained kernel owner, never by resuming or replaying a record. */
        if (batch_recovery_task==g_current) { batch_recovery_active=false; batch_recovery_task=NULL; }
    }
    presenter_unlock();
}
void fbdev_batch_recovery_abandon(struct display_batch *b)
{
    kmutex_lock(&presenter);
    if (batch_owner==b && batch_recovery_task==g_current) { batch_recovery_active=false; batch_recovery_task=NULL; }
    presenter_unlock();
}
int fbdev_command_info(uint32_t generation, struct ciuki_command_info *out)
{
    if (!out) return -EINVAL;
    kmutex_lock(&presenter);
    int e=!live() ? -ENODEV : generation!=device.generation ? -EAGAIN : 0;
    if (!e) {
        uint32_t native=acceleration && !accel_initializing && !accel_blocked && acceleration->command_bits ? acceleration->command_bits(acceleration->ctx) : 0;
        *out=(struct ciuki_command_info){.size=sizeof(*out),.version=1,.software_commands=0x7fe,.native_commands=native,.capabilities=CIUKI_CAP_BATCH};
        display_driver_command_counters(out);
        if (native&(1u<<CIUKI_CMD_MONO)) out->capabilities|=CIUKI_CAP_MONO;
        if (native&(1u<<CIUKI_CMD_INVERT)) out->capabilities|=CIUKI_CAP_INVERT;
        if (native&(1u<<CIUKI_CMD_KEYED)) out->capabilities|=CIUKI_CAP_KEYED;
        if (native&(1u<<CIUKI_CMD_SCALED)) {
            out->capabilities|=CIUKI_CAP_SCALED;
            if (acceleration->command_features) out->capabilities|=acceleration->command_features(acceleration->ctx)&CIUKI_CAP_BILINEAR;
        }
        if (native&(1u<<CIUKI_CMD_LINE)) out->capabilities|=CIUKI_CAP_LINE;
        if (native&(1u<<CIUKI_CMD_CURSOR_ARGB_SET)) {
            if (acceleration->cursor_limits) acceleration->cursor_limits(acceleration->ctx,&out->cursor_width,&out->cursor_height);
            if (out->cursor_width && out->cursor_height && out->cursor_width<=64 && out->cursor_height<=64) out->capabilities|=CIUKI_CAP_CURSOR_ARGB;
            else out->native_commands&=~(1u<<CIUKI_CMD_CURSOR_ARGB_SET);
        }
    }
    presenter_unlock();
    return e;
}
