/* F1 LFB presenter and f1-23 quiesced runtime remapping.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/registry.h>
#include <ciuki/fbdev.h>
#include <ciuki/accel.h>
#include <ciuki/task.h>
extern const struct accel_ops *savage2d_init(const struct fb_device *, struct vfs *) __attribute__((weak));
extern const struct accel_ops *savage2d_fallback_ops(void) __attribute__((weak));
extern bool savage2d_eligible(const struct fb_device *) __attribute__((weak));
extern int savage2d_attempt_begin(struct vfs *) __attribute__((weak));
extern int savage2d_attempt_end(struct vfs *) __attribute__((weak));
extern int savage2d_attempt_failed(struct vfs *) __attribute__((weak));
extern void savage2d_quarantine(const char *) __attribute__((weak));
extern void savage2d_record(void) __attribute__((weak));
extern void savage2d_snapshot(struct ciuki_accel_info *) __attribute__((weak));
extern void console_framebuffer_revoke(void) __attribute__((weak));
extern void console_framebuffer_changed(const struct fb_device *) __attribute__((weak));

#include <ciuki/mtrr.h>
#include <ciuki/timing.h>

static struct fb_device device;
static struct kmutex presenter;
static const struct accel_ops *acceleration;
static bool initialized, changing;
static struct vfs *accel_vfs;
static bool accel_queued, accel_initializing;
/* One reusable normal-priority thread owns marker I/O and initialization.
 * Synchronous serial trace must stay outside the 1-ms device-work budget. */
static struct task *accel_marker_task;
static struct kwait accel_marker_wait;
static enum { ACCEL_MARKER_IDLE, ACCEL_MARKER_BEGIN, ACCEL_MARKER_INIT, ACCEL_MARKER_CLEAR, ACCEL_MARKER_STATUS } accel_marker_request;
static void *change_window;
static uint32_t change_capacity;
static unsigned console_rows;
static unsigned drawing;
static uint64_t full_count, full_bytes, full_total_us, full_last_us, full_max_us, full_log_tick;
enum { CONSOLE_BUSY = 1, PRESENT_BUSY = 2 };

void fbdev_console_region(unsigned rows)
{
    __atomic_store_n(&console_rows, rows, __ATOMIC_RELEASE);
}

bool fbdev_console_begin(void)
{
    if (acceleration && !acceleration->cpu_ready(acceleration->ctx)) return false;
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
    kmutex_unlock(&presenter);
    if (run) {
        /* No engine command is submitted during setup. CPU rendering and
         * console output remain available across reset, polls and logging. */
        const struct accel_ops *ops = savage2d_init(&device, 0);
        ready = ops != 0;
        if (!ready && savage2d_fallback_ops) ops = savage2d_fallback_ops();
        kmutex_lock(&presenter);
        acceleration = ops;
        accel_initializing = false;
        kmutex_unlock(&presenter);
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
            if (err && savage2d_fallback_ops) acceleration = savage2d_fallback_ops();
            __atomic_store_n(&drawing, 0, __ATOMIC_RELEASE);
        }
    }
    kmutex_unlock(&presenter);
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
        if (savage2d_attempt_failed) savage2d_attempt_failed(accel_vfs);
        accel_queued = false;
        return;
    }
    bool clear = accel_marker_request == ACCEL_MARKER_CLEAR;
    accel_marker_request = ACCEL_MARKER_IDLE;
    if (!clear) {
        /* VFS/ATA locks must never nest under the presenter mutex. */
        if (!savage2d_attempt_begin || savage2d_attempt_begin(accel_vfs)) {
            accel_queued = false;
            return;
        }
        accel_marker_request = ACCEL_MARKER_INIT;
        return;
    }
    if (!savage2d_attempt_end || savage2d_attempt_end(accel_vfs)) {
        /* The clear did not reach the disk: the next boot would skip the
         * accelerator anyway; keep the software presenter now as well. */
        kmutex_lock(&presenter);
        if (acceleration) { acceleration->stop(acceleration->ctx); acceleration = 0; }
        kmutex_unlock(&presenter);
        if (savage2d_quarantine) savage2d_quarantine("marker_clear_failed");
    }
    accel_queued = false;
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
    if (g_boot.flags & CBI_F_SAFE_MODE) return;
    if (accel_queued) return; /* keep the pending attempt's volume stable */
    accel_vfs = vfs;
    if (!initialized || !device.present || !savage2d_init || acceleration) return;
    if (!savage2d_eligible || !savage2d_eligible(&device)) return;
    if (!accel_marker_task) {
        kwait_init(&accel_marker_wait);
        accel_marker_task = task_create_kernel("savage-marker", accel_marker_main, 0, P_NORMAL);
        if (!accel_marker_task) {
            klog("[accel] savage quarantined reason=marker_worker_unavailable");
            return;
        }
        task_start(accel_marker_task);
    }
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

struct pixel_layout {
    unsigned bytes, red_shift, red_pos, green_shift, green_pos, blue_shift, blue_pos;
};

static struct pixel_layout pixel_layout(const struct fb_device *d)
{
    return (struct pixel_layout){ d->bpp / 8, 8 - d->red_size, d->red_pos,
                                 8 - d->green_size, d->green_pos, 8 - d->blue_size, d->blue_pos };
}

static uint32_t pack_layout(const struct pixel_layout *l, uint32_t rgb)
{
    return ((((rgb >> 16) & 255u) >> l->red_shift) << l->red_pos) |
           ((((rgb >> 8) & 255u) >> l->green_shift) << l->green_pos) |
           (((rgb & 255u) >> l->blue_shift) << l->blue_pos);
}

static uint32_t pack(const struct fb_device *d, uint32_t rgb)
{
    const struct pixel_layout l = pixel_layout(d);
    return pack_layout(&l, rgb);
}

/* Unaligned RAM loads and generic integer stores are valid on i686. The
 * lowered C alignment and alias permission also cover arbitrary row pitch. */
typedef uint32_t fb_word __attribute__((aligned(1), may_alias));
typedef uint16_t fb_half __attribute__((aligned(1), may_alias));

static void pixel(uint8_t *p, unsigned bytes, uint32_t colour)
{
    if (bytes == 2)
        *(fb_half *)p = (uint16_t)colour;
    else if (bytes == 4)
        *(fb_word *)p = colour;
    else {
        p[0] = (uint8_t)colour;
        p[1] = (uint8_t)(colour >> 8);
        p[2] = (uint8_t)(colour >> 16);
    }
}

/* Intel SDM Vol. 3A, 253668-084US, sections 12.3/12.3.1 and 9.2.5:
 * https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf
 * Consecutive WC writes combine; partially filled buffers drain at the next
 * serializing event. A compiler barrier per row preserves generated stores,
 * without volatile per pixel or a hardware fence per row. Keep the existing
 * framebuffer_write_drain() before ownership release and the end timestamp. */
static void row_written(void) { __asm__ volatile("" ::: "memory"); }

/* Both pointers must be dword aligned; count is in dwords, not bytes. */
static void row_copy(uint8_t *dst, const void *src, size_t count)
{
    __asm__ volatile("cld\n\trep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory", "cc");
}

static void row_fill(uint8_t *dst, uint32_t colour, size_t count)
{
    __asm__ volatile("cld\n\trep stosl" : "+D"(dst), "+c"(count) : "a"(colour) : "memory", "cc");
}

static void present_xrgb_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    uint32_t used = 0;
    for (unsigned x = 0; x < width; x++) used |= ((const fb_word *)src)[x];
    if (!(used & 0xff000000u)) {
        row_copy(dst, src, width);
        return;
    }
    /* X is ignored by the public surface contract. Normalize in RAM, never
     * by reading or masking a destination word in the WC framebuffer. */
    uint32_t pixels[64];
    while (width) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width;
        for (unsigned x = 0; x < n; x++) pixels[x] = ((const fb_word *)src)[x] & 0xffffffu;
        row_copy(dst, pixels, n);
        src += n * 4; dst += n * 4; width -= n;
    }
}

static uint16_t pack_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

static void present_565_row(uint8_t *dst, const uint8_t *src, unsigned width)
{
    if ((uintptr_t)dst & 2u) {
        pixel(dst, 2, pack_565(*(const fb_word *)src));
        dst += 2; src += 4; width--;
    }
    uint16_t pixels[64] __attribute__((aligned(4)));
    while (width >= 2) {
        unsigned n = width > ARRAY_SIZE(pixels) ? ARRAY_SIZE(pixels) : width & ~1u;
        for (unsigned x = 0; x < n; x++) pixels[x] = pack_565(((const fb_word *)src)[x]);
        row_copy(dst, pixels, n / 2);
        src += n * 4; dst += n * 2; width -= n;
    }
    if (width) pixel(dst, 2, pack_565(*(const fb_word *)src));
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
    if (d == &device && acceleration && acceleration->sync(acceleration->ctx)) {
        (void)present_end(d, &c, false); return -EIO;
    }
    bool timed = d == &device && g_cpu_tsc && g_tsc_per_ms &&
                 !c.x && !c.y && c.width == (int32_t)d->width && c.height == (int32_t)d->height;
    uint64_t start = timed ? framebuffer_cycles() : 0;
    uint32_t sx = (uint32_t)((int64_t)c.x - r->x), sy = (uint32_t)((int64_t)c.y - r->y);
    const struct pixel_layout l = pixel_layout(d);
    bool xrgb = l.bytes == 4 && !l.red_shift && l.red_pos == 16 &&
                !l.green_shift && l.green_pos == 8 && !l.blue_shift && !l.blue_pos;
    bool rgb565 = l.bytes == 2 && l.red_shift == 3 && l.red_pos == 11 &&
                  l.green_shift == 2 && l.green_pos == 5 && l.blue_shift == 3 && !l.blue_pos;
    for (int32_t y = 0; y < c.height; y++) {
        const uint8_t *sp = (const uint8_t *)s->pixels + (sy + (uint32_t)y) * s->pitch + sx * 4;
        uint8_t *dp = d->mapped + (c.y + (uint32_t)y) * d->pitch + (uint32_t)c.x * l.bytes;
        if (xrgb && !(((uintptr_t)sp | (uintptr_t)dp) & 3u))
            present_xrgb_row(dp, sp, (unsigned)c.width);
        else if (rgb565 && !((uintptr_t)dp & 1u))
            present_565_row(dp, sp, (unsigned)c.width);
        else for (int32_t x = 0; x < c.width; x++)
            pixel(dp + (uint32_t)x * l.bytes, l.bytes, pack_layout(&l, ((const fb_word *)sp)[x]));
        row_written();
    }
    uint64_t end = present_end(d, &c, timed);
    if (timed) {
        uint64_t cycles = end - start;
        /* Avoid overflowing cycles*1000; calibration is fixed after boot. */
        full_last_us = (cycles / g_tsc_per_ms) * 1000 +
                       (cycles % g_tsc_per_ms) * 1000 / g_tsc_per_ms;
        full_count++;
        full_bytes += (uint64_t)(uint32_t)c.width * (uint32_t)c.height * l.bytes;
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
    unsigned bytes = d->bpp / 8;
    colour = pack(d, colour);
    if (d == &device && acceleration) {
        int err = acceleration->fill_rect(acceleration->ctx, &c, colour);
        if (!err) { (void)present_end(d, &c, false); return 0; }
        if (acceleration->sync(acceleration->ctx)) { (void)present_end(d, &c, false); return -EIO; }
    }
    for (int32_t y = 0; y < c.height; y++) {
        uint8_t *dp = d->mapped + (c.y + (uint32_t)y) * d->pitch + (uint32_t)c.x * bytes;
        unsigned width = (unsigned)c.width;
        if (bytes == 4 && !((uintptr_t)dp & 3u))
            row_fill(dp, colour, width);
        else if (bytes == 2 && !((uintptr_t)dp & 1u)) {
            if ((uintptr_t)dp & 2u) { pixel(dp, 2, colour); dp += 2; width--; }
            row_fill(dp, colour | (colour << 16), width / 2);
            if (width & 1u) pixel(dp + (width & ~1u) * 2, 2, colour);
        } else for (int32_t x = 0; x < c.width; x++)
            pixel(dp + (uint32_t)x * bytes, bytes, colour);
        row_written();
    }
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
    kmutex_unlock(&presenter);
    if (savage2d_record) savage2d_record();
    return rc;
}

int fbdev_fill(const struct fb_rect *rect, uint32_t colour)
{
    if (!initialized || !device.present)
        return -ENODEV;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int rc = live() ? fbdev_fill_to(&device, rect, colour) : -ENODEV;
    kmutex_unlock(&presenter);
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
        if (acceleration && acceleration->sync(acceleration->ctx)) e = -EIO;
        if (!e) {
            unsigned bytes = device.bpp/8;
            uint8_t *dst = device.mapped+y*device.pitch+x*bytes;
            const uint8_t *src = pixels;
            for (unsigned i=0;i<count*bytes;++i) *(volatile uint8_t *)(dst+i)=src[i];
        }
        (void)present_end(&device,&r,false);
    }
    kmutex_unlock(&presenter);
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
        int e = acceleration->copy_rect(acceleration->ctx,r,x,y);
        (void)present_end(d,&busy,false);
        /* A partially executed overlapping blit has lost its original source.
         * Caller must rebuild destination from its authoritative RAM surface. */
        return e ? -EAGAIN : 0;
    }
    if (d == &device && acceleration && acceleration->sync(acceleration->ctx)) {
        (void)present_end(d,&busy,false); return -EIO;
    }
    /* LFB reads are uncached, even when WC accelerates its stores. The
     * desktop's screen_copy/scroll_copy recovery rebuilds scene damage from
     * the authoritative RAM surface and re-presents it after -EAGAIN. */
    (void)present_end(d,&busy,false);
    return -EAGAIN;
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
        out->capabilities = acceleration ? acceleration->capabilities(acceleration->ctx) : 0;
        if (!accel_initializing && savage2d_snapshot) savage2d_snapshot(out);
    }
    kmutex_unlock(&presenter);
    return err;
}

int fbdev_accel_request(const struct ciuki_accel_request *q)
{
    if (!q || q->size != sizeof(*q) || q->operation > CIUKI_ACCEL_CURSOR_SHOW) return -EINVAL;
    if (changing) return -EBUSY;
    kmutex_lock(&presenter);
    int err = -ENODEV;
    if (!live()) goto done;
    if (q->generation != device.generation) { err = -EAGAIN; goto done; }
    if (q->operation == CIUKI_ACCEL_QUERY) {
        err = acceleration ? (int)acceleration->capabilities(acceleration->ctx) : 0; goto done;
    }
    if (q->operation == CIUKI_ACCEL_COPY) {
        struct ciuki_surface_info s = {.width=device.width,.height=device.height};
        struct ciuki_rect c;
        /* Avoid a dependency on process code: strict bounded screen rectangles. */
        c = q->rect;
        if (c.width > INT32_MAX || c.height > INT32_MAX || c.src_x < 0 || c.src_y < 0 ||
            c.dst_x < 0 || c.dst_y < 0 || (uint64_t)(uint32_t)c.src_x+c.width > s.width ||
            (uint64_t)(uint32_t)c.src_y+c.height > s.height || (uint64_t)(uint32_t)c.dst_x+c.width > s.width ||
            (uint64_t)(uint32_t)c.dst_y+c.height > s.height) { err=-EINVAL; goto done; }
        struct fb_rect r = {c.src_x,c.src_y,(int32_t)c.width,(int32_t)c.height};
        err = fbdev_copy_to(&device,&r,c.dst_x,c.dst_y); goto done;
    }
    if (q->operation == CIUKI_ACCEL_FILL || q->operation == CIUKI_ACCEL_PATTERN) {
        if (q->rect.width > INT32_MAX || q->rect.height > INT32_MAX) { err=-EINVAL; goto done; }
        struct fb_rect r = {q->rect.dst_x,q->rect.dst_y,(int32_t)q->rect.width,(int32_t)q->rect.height};
        if (q->operation == CIUKI_ACCEL_FILL) { err=fbdev_fill_to(&device,&r,q->colour); goto done; }
        struct fb_rect c;
        if (!fbdev_rect_valid(&r)) { err=-EINVAL; goto done; }
        if (!clip(&device,&r,&c)) { err=0; goto done; }
        if (!present_begin(&device,&c)) { err=-EBUSY; goto done; }
        err=acceleration ? acceleration->pattern_rect(acceleration->ctx,&c,q->pattern,pack(&device,q->colour),pack(&device,q->background)) : -ENODEV;
        if (err && (!acceleration || !acceleration->sync(acceleration->ctx))) {
            unsigned bytes=device.bpp/8;
            uint32_t fg=pack(&device,q->colour), bg=pack(&device,q->background);
            for (int32_t y=c.y; y<c.y+c.height; y++) {
                for (int32_t x=c.x; x<c.x+c.width; x++)
                    pixel(device.mapped+(uint32_t)y*device.pitch+(uint32_t)x*bytes,bytes,
                          q->pattern[y&7] & (0x80u >> (x&7)) ? fg : bg);
                row_written();
            }
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
        cursor.foreground=pack(&device,q->colour); cursor.background=pack(&device,q->background);
        err=acceleration->cursor_set(acceleration->ctx,&cursor);
    } else if (q->operation == CIUKI_ACCEL_CURSOR_MOVE)
        err=acceleration->cursor_move(acceleration->ctx,q->rect.dst_x,q->rect.dst_y);
    else err=acceleration->cursor_show(acceleration->ctx,q->colour!=0);
    (void)present_end(&device,&busy,false);
done:
    kmutex_unlock(&presenter);
    if (savage2d_record) savage2d_record();
    return err;
}

void fbdev_cursor_hide(void)
{
    kmutex_lock(&presenter);
    if (acceleration && !changing && (acceleration->capabilities(acceleration->ctx)&2u))
        (void)acceleration->cursor_show(acceleration->ctx,false);
    kmutex_unlock(&presenter);
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
    if (!initialized || changing || accel_initializing || !live()) return -EBUSY;
    if (!console_framebuffer_revoke || !console_framebuffer_changed) return -ENODEV;
    changing = true; /* stop submissions before waiting for the last presenter */
    kmutex_lock(&presenter);
    if (accel_initializing) {
        changing = false; kmutex_unlock(&presenter); return -EBUSY;
    }
    if (acceleration && acceleration->stop(acceleration->ctx)) {
        changing = false; kmutex_unlock(&presenter); return -EIO;
    }
    acceleration=0; /* successful stop may have revoked register mappings */
    if (!change_window) {
        uint32_t capacity = device.size;
        for (unsigned i = 0; i < g_boot.display.mode_count; i++) {
            uint32_t size = (uint32_t)g_boot.display.modes[i].pitch * g_boot.display.modes[i].height;
            if (size > capacity) capacity = size;
        }
        change_capacity = PAGE_ALIGN_UP(capacity + PAGE_SIZE); /* nonaligned physical base */
        change_window = vmm_reserve_mmio(change_capacity);
        if (!change_window) { changing = false; kmutex_unlock(&presenter); return -ENOMEM; }
    }
    int e = registry_quiesce(device.handle, device.generation, change_idle);
    kmutex_unlock(&presenter);
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
    kmutex_unlock(&presenter);
    if (accel_vfs) fbdev_accel_schedule(accel_vfs);
}
