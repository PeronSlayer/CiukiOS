/* Independently implemented from register references in docs/design/savage-2d.md.
 * Firmware retains scanout; no PCI activation, DMA, mode or clock programming.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/registry.h>
#include <ciuki/savage2d.h>
#include <ciuki/work.h>
#include "../fs/vfs.h"
#define RECT 0x48000000u
#define XP 0x01000000u
#define YP 0x02000000u
#define DEST_NEW 0x00000c00u
#define SRC_NEW 0x00000140u
#define COLOR 0x00008000u
#define WAIT_US 20000u
#define WAIT_POLLS 256u
#define POLL_US 5u
#define BCI_CONTROL 0x48c18u
static uint32_t xy(uint32_t x, uint32_t y) { return (y << 16) | x; }
static bool idle_status(struct savage2d *s, uint32_t value)
{
    /* Infer only a complete, empty signature; never weaken X.Org's mask to
     * Linux's a00000. RE046-B assigns PF/idle differently (p. 231). */
    bool xorg = (value & 0x00e1ffffu) == 0x00e00000u;
    bool manual = (value & 0x00cfffffu) == 0x004e0000u;
    if (value == UINT32_MAX) { s->reason = "dead_mmio"; return false; }
    if (!s->idle_mask) {
        if (xorg) { s->idle_mask = 0x00e1ffffu; s->idle_value = 0x00e00000u; }
        else if (manual) { s->idle_mask = 0x00cfffffu; s->idle_value = 0x004e0000u; }
        if (s->idle_mask)
            klog("[accel] savage idle mask=%08x value=%08x", s->idle_mask, s->idle_value);
    } else if ((s->idle_value == 0x00e00000u && manual) ||
               (s->idle_value == 0x004e0000u && xorg)) {
        s->reason = "status_contradiction";
        return false;
    }
    return s->idle_mask && (value & s->idle_mask) == s->idle_value;
}
static int wait_idle(struct savage2d *s)
{
    s->idle_waits++;
    uint64_t start = s->io.now_us(s->io.ctx);
    for (unsigned i = 0; i < WAIT_POLLS; i++) {
        if (s->io.delay_us(s->io.ctx, POLL_US)) { s->reason = "delay_failed"; break; }
        uint32_t status = s->io.read32(s->io.ctx, SAVAGE_STATUS);
        if (!s->status_logged) {
            klog("[accel] savage status reg=48c60 raw=%08x", status);
            s->status_logged = true;
        }
        if (idle_status(s, status)) { s->cpu_safe = true; return 0; }
        if (status == UINT32_MAX || (s->reason && !strncmp(s->reason, "status_contradiction", sizeof("status_contradiction"))) ||
            s->io.now_us(s->io.ctx) - start >= WAIT_US) break;
    }
    if (!s->reason) s->reason = "idle_timeout";
    s->cpu_safe = false;
    return -EIO;
}
static int fault(struct savage2d *s)
{
    if (!s->quarantined) {
        s->quarantined = true; s->active = false; s->timeouts++;
        /* Cursor is not an engine writer; hide it without touching scanout.
         * Never reset/re-enable or release an engine with unproven idle. */
        if (s->touched) s->io.write_cr(s->io.ctx, 0x45, s->saved45 & ~1u);
        if (s->io.fault) s->io.fault(s->io.ctx);
    }
    s->fallbacks++;
    return -EIO;
}
static int fail(struct savage2d *s, const char *reason)
{ s->reason = reason; return fault(s); }

static int disable_bci(struct savage2d *s, uint32_t control)
{
    s->io.write32(s->io.ctx, BCI_CONTROL, control & 0x3ff0u);
    uint32_t check = s->io.read32(s->io.ctx, BCI_CONTROL);
    if (check == UINT32_MAX || (check & 0x0fu)) return fail(s, "bci_disable_failed");
    return 0;
}
static int sync_engine(void *ctx)
{
    struct savage2d *s = ctx;
    s->io.drain(s->io.ctx);
    if (wait_idle(s)) return fault(s);
    return 0;
}
static int submit(struct savage2d *s, const uint32_t *words, unsigned n)
{
    if (!s->active || s->quarantined) { s->fallbacks++; return -ENODEV; }
    s->io.drain(s->io.ctx);
    if (wait_idle(s)) return fault(s);
    s->cpu_safe = false;
    for (unsigned i = 0; i < n; i++) s->io.write32(s->io.ctx, SAVAGE_BCI + i * 4, words[i]);
    /* UC read flushes posted command writes before observing completion. */
    if (wait_idle(s)) return fault(s);
    s->operations++;
    return 0;
}
static bool valid_rect(struct savage2d *s, const struct fb_rect *r)
{
    return fbdev_rect_valid(r) && r->x >= 0 && r->y >= 0 && r->width > 0 && r->height > 0 &&
        (uint32_t)(r->x + r->width) <= s->width && (uint32_t)(r->y + r->height) <= s->height;
}
static int copy_rect(void *ctx, const struct fb_rect *r, int32_t dx, int32_t dy)
{
    struct savage2d *s = ctx;
    struct fb_rect dst = {dx, dy, r ? r->width : 0, r ? r->height : 0};
    if (!valid_rect(s, r) || !valid_rect(s, &dst)) return -EINVAL;
    uint32_t sx = r->x, sy = r->y, x = dx, y = dy, cmd = RECT | DEST_NEW | SRC_NEW | (0xccu << 16);
    if (dx <= r->x) cmd |= XP;
    else { sx += r->width - 1; x += r->width - 1; }
    if (dy <= r->y) cmd |= YP;
    else { sy += r->height - 1; y += r->height - 1; }
    uint32_t packet[] = {cmd, s->offset, s->descriptor, s->offset, s->descriptor, xy(sx,sy), xy(x,y), xy(r->width,r->height)};
    return submit(s, packet, ARRAY_SIZE(packet));
}
static int fill_rect(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct savage2d *s = ctx;
    if (!valid_rect(s, r)) return -EINVAL;
    uint32_t packet[] = {RECT | XP | YP | DEST_NEW | COLOR | (0xccu << 16),
        s->offset, s->descriptor, colour, xy(r->x,r->y), xy(r->width,r->height)};
    return submit(s, packet, ARRAY_SIZE(packet));
}
static int pattern_rect(void *ctx, const struct fb_rect *r, const uint8_t pattern[8], uint32_t fg, uint32_t bg)
{
    struct savage2d *s = ctx;
    if (!valid_rect(s, r) || !pattern) return -EINVAL;
    uint32_t lo = 0, hi = 0;
    for (unsigned i = 0; i < 4; i++) { lo |= (uint32_t)pattern[i] << (i * 8); hi |= (uint32_t)pattern[i+4] << (i * 8); }
    uint32_t packet[] = {RECT | XP | YP | DEST_NEW | COLOR | 3u | (0xf0u << 16),
        s->offset, s->descriptor, fg, bg, xy(r->x,r->y), xy(r->width,r->height), lo, hi};
    return submit(s, packet, ARRAY_SIZE(packet));
}
static int cursor_show(void *ctx, bool show)
{
    struct savage2d *s = ctx;
    if (!s->active || s->quarantined || !s->cursor || (show && !s->cursor_loaded)) return -ENODEV;
    s->io.write_cr(s->io.ctx, 0x45, (s->saved45 & ~0x11u) | (show ? 1u : 0u));
    if ((s->io.read_cr(s->io.ctx, 0x45) & 1u) != (show ? 1u : 0u)) return fault(s);
    s->operations++;
    return 0;
}
static int cursor_set(void *ctx, const struct accel_cursor *c)
{
    struct savage2d *s = ctx;
    if (!c || !s->cursor || !s->active || s->quarantined) return -ENODEV;
    if (sync_engine(s) || cursor_show(s, false)) return -EIO;
    for (unsigned y = 0; y < 64; y++) for (unsigned group = 0; group < 4; group++) {
        uint16_t mask = 0, image = 0;
        if (y < 32 && group < 2) {
            unsigned shift = 16 - group * 16;
            mask = (uint16_t)(c->mask[y] >> shift);
            image = (uint16_t)(c->image[y] >> shift) & mask;
        }
        uint8_t *p = s->cursor + y * 16 + group * 4;
        uint16_t and_bits = ~mask;
        p[0] = (uint8_t)and_bits; p[1] = (uint8_t)(and_bits >> 8);
        p[2] = (uint8_t)image; p[3] = (uint8_t)(image >> 8);
    }
    s->io.drain(s->io.ctx);
    s->io.write_cr(s->io.ctx, 0x55, s->saved55 & ~0x10u);
    unsigned bytes = ((s->descriptor >> 16) & 255u) == 16 ? 2 : 3;
    (void)s->io.read_cr(s->io.ctx, 0x45);
    for (unsigned i = 0; i < bytes; i++) s->io.write_cr(s->io.ctx, 0x4a, c->foreground >> (i * 8));
    (void)s->io.read_cr(s->io.ctx, 0x45);
    for (unsigned i = 0; i < bytes; i++) s->io.write_cr(s->io.ctx, 0x4b, c->background >> (i * 8));
    s->io.write_cr(s->io.ctx, 0x4c, (s->cursor_offset >> 18) & 0x7f);
    s->io.write_cr(s->io.ctx, 0x4d, (s->cursor_offset >> 10) & 0xfc);
    if (s->io.read_cr(s->io.ctx,0x4c) != ((s->cursor_offset >> 18)&0x7f) ||
        s->io.read_cr(s->io.ctx,0x4d) != ((s->cursor_offset >> 10)&0xfc)) return fault(s);
    s->cursor_loaded = true;
    return 0;
}
static int cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct savage2d *s = ctx;
    if (!s->active || s->quarantined || !s->cursor) return -ENODEV;
    if (x < -63 || y < -63 || x > 2047 || y > 2047) return -EINVAL;
    uint32_t px = x < 0 ? 0u : (uint32_t)x, py = y < 0 ? 0u : (uint32_t)y;
    s->io.write_cr(s->io.ctx, 0x4e, x < 0 ? (uint32_t)-x : 0);
    s->io.write_cr(s->io.ctx, 0x4f, y < 0 ? (uint32_t)-y : 0);
    s->io.write_cr(s->io.ctx, 0x46, px >> 8); s->io.write_cr(s->io.ctx, 0x47, px);
    s->io.write_cr(s->io.ctx, 0x49, py); s->io.write_cr(s->io.ctx, 0x48, py >> 8);
    s->operations++;
    return 0;
}
static int stop_engine(void *ctx)
{
    struct savage2d *s = ctx;
    if (!s->touched) return s->cpu_safe ? 0 : sync_engine(s);
    if (sync_engine(s)) return -EIO;
    /* Never touch quarantined resources beyond idle observation. */
    if (s->quarantined) return 0;
    s->io.write_cr(s->io.ctx, 0x45, s->saved45 & ~1u);
    if (disable_bci(s, s->saved_bci)) return -EIO;
    s->io.write32(s->io.ctx, 0x8168, s->saved_gbd[0]);
    s->io.write32(s->io.ctx, 0x816c, s->saved_gbd[1]);
    s->io.write_cr(s->io.ctx, 0x31, s->saved31);
    s->io.write_cr(s->io.ctx, 0x50, s->saved50);
    s->io.write_cr(s->io.ctx, 0x40, s->saved40);
    s->io.write_cr(s->io.ctx, 0x55, s->saved55);
    s->active = false; s->touched = false; s->cursor_loaded = false;
    return 0;
}
static unsigned capabilities(void *ctx)
{
    struct savage2d *s = ctx;
    return s->active && !s->quarantined ? 1u | (s->cursor ? 2u : 0u) : 0u;
}
static bool cpu_ready(void *ctx) { return ((struct savage2d *)ctx)->cpu_safe; }
bool savage2d_supported(const struct pci_func *p)
{
    if (!p || p->class_code != 3 || p->vendor != 0x5333) return false;
    switch (p->device) {
    case 0x8a22: case 0x8c22: case 0x8c24: case 0x8c26:
    case 0x8c2a: case 0x8c2b: case 0x8c2c: case 0x8c2d: case 0x8c2e: case 0x8c2f: return true;
    default: return false;
    }
}
static bool mode_supported(const struct fb_device *d)
{
    return d && d->present && (d->bpp==16 || d->bpp==32) && d->width && d->height &&
        d->width<=2048 && d->height<=2048 && !(d->pitch%(16*(d->bpp/8))) &&
        d->pitch/(d->bpp/8)<=4095 && d->pitch>=d->width*(d->bpp/8) &&
        (uint64_t)d->pitch*d->height<=d->size;
}
int savage2d_start(struct savage2d *s, const struct savage_io *io, const struct fb_device *d,
                   bool supersavage, uint8_t *cursor, uint32_t cursor_offset)
{
    if (s->quarantined) return -EIO;
    if (!io || !io->read32 || !io->write32 || !io->read_cr || !io->write_cr ||
        !io->now_us || !io->delay_us || !io->drain || !io->read8 || !io->write8 || !mode_supported(d) ||
        (cursor && (cursor_offset & 4095u))) return -EINVAL;
    s->io = *io; s->width = d->width; s->height = d->height; s->offset = 0;
    s->supersavage = supersavage; s->cursor = cursor; s->cursor_offset = cursor_offset;
    s->descriptor = 0x10000000u | ((uint32_t)d->bpp << 16) | (d->pitch / (d->bpp / 8));
    s->ops = (struct accel_ops){s, sync_engine, copy_rect, fill_rect, pattern_rect,
        cursor_set, cursor_move, cursor_show, stop_engine, capabilities, cpu_ready};
    s->reason = 0; s->idle_mask = 0; s->status_logged = false;
    /* Nothing in the engine domain, especially MM48C60, before this reset.
     * CR access initially follows MISC[0], then is explicitly set to 3D4. */
    io->write_cr(io->ctx, 0x38, 0x48); io->write_cr(io->ctx, 0x39, 0xa5);
    if (io->read_cr(io->ctx, 0x38) != 0x48 || io->read_cr(io->ctx, 0x39) != 0xa5)
        return fail(s, "crtc_locked");
    io->write8(io->ctx, 0x83c4, 0x08); io->write8(io->ctx, 0x83c5, 0x06);
    if (supersavage) {
        io->write8(io->ctx, 0x83c4, 0x26); io->write8(io->ctx, 0x83c5, 0x40);
        if (io->read8(io->ctx, 0x83c5) != 0x40) return fail(s, "pipe_select_failed");
    }
    uint8_t misc = io->read8(io->ctx, 0x83cc);
    if (misc == 0xff) return fail(s, "dead_vga");
    io->write8(io->ctx, 0x83c2, misc | 1u);
    if (!(io->read8(io->ctx, 0x83cc) & 1u)) return fail(s, "crtc_select_failed");
    /* CR66.0 may only change during blank/retrace. Preserve the VBE engine
     * enablement; a firmware mode without it retains software presentation. */
    uint8_t cr66 = io->read_cr(io->ctx, 0x66);
    if (!(cr66 & 1u)) { s->cpu_safe=true; s->reason="engine_disabled"; return -ENODEV; }
    io->write_cr(io->ctx, 0x66, cr66 | 2u);
    int delay = io->delay_us(io->ctx, 10000);
    uint8_t asserted = io->read_cr(io->ctx, 0x66);
    /* Always deassert reset even when pacing failed. Never change CR66.0. */
    io->write_cr(io->ctx, 0x66, cr66 & ~2u);
    int settled = io->delay_us(io->ctx, 10000);
    if (delay || settled) return fail(s, "delay_failed");
    if (asserted != (uint8_t)(cr66 | 2u) || io->read_cr(io->ctx, 0x66) != (uint8_t)(cr66 & ~2u))
        return fail(s, "reset_readback_failed");
    if (sync_engine(s)) return -EIO;
    /* VBE linear aperture only: do not reinterpret a tiled primary stream. */
    if (io->read32(io->ctx, 0x81c8) & 0xc0000000u) return fail(s, "tiled_scanout");
    s->saved40 = io->read_cr(io->ctx, 0x40); s->saved31 = io->read_cr(io->ctx, 0x31);
    s->saved50 = io->read_cr(io->ctx, 0x50); s->saved45 = io->read_cr(io->ctx, 0x45);
    s->saved55 = io->read_cr(io->ctx, 0x55); s->saved_bci = io->read32(io->ctx, BCI_CONTROL);
    /* Active firmware command buffers cannot be repurposed. */
    if (s->saved_bci == UINT32_MAX) return fail(s, "dead_bci");
    if (s->saved_bci & 7u) return fail(s, "firmware_command_buffer");
    if (disable_bci(s, s->saved_bci)) return -EIO;
    s->saved_gbd[0] = io->read32(io->ctx, 0x8168); s->saved_gbd[1] = io->read32(io->ctx, 0x816c);
    s->touched = true;
    io->write_cr(io->ctx, 0x40, s->saved40 | 1u);
    io->write_cr(io->ctx, 0x31, (s->saved31 & ~1u) | 0x0cu);
    io->write32(io->ctx, 0x8128, UINT32_MAX); io->write32(io->ctx, 0x812c, UINT32_MAX);
    io->write32(io->ctx, 0x8134, 0x00070027u);
    io->write32(io->ctx, 0x48c0c, 0); io->write32(io->ctx, 0x48c10, 0x00700040u);
    io->write32(io->ctx, 0x8168, 0);
    uint32_t gbd = s->descriptor | (supersavage ? 1u : 9u);
    io->write32(io->ctx, 0x816c, gbd);
    io->write_cr(io->ctx, 0x50, (s->saved50 & ~0x30u) | (d->bpp==16 ? 0x10u : 0x30u) | 0xc1u);
    if (io->read32(io->ctx, 0x816c) != gbd || !(io->read_cr(io->ctx, 0x40) & 1u))
        return fail(s, "descriptor_readback_failed");
    uint32_t enabled = (s->saved_bci & 0x3ff0u) | 8u;
    io->write32(io->ctx, BCI_CONTROL, enabled);
    /* Upper fields include hardware buffer counters/pointers and reserved
     * bits (RE046-B p. 227). Verify only the command-interface controls. */
    uint32_t control = io->read32(io->ctx, BCI_CONTROL);
    if (control == UINT32_MAX || (control & 0x0fu) != 8u) return fail(s, "bci_enable_failed");
    if (sync_engine(s)) return -EIO;
    s->active = true;
    return 0;
}

static struct savage2d engine;
static volatile uint8_t *registers;
static int reg_claim = -1, cursor_claim = -1;
static gen_t reg_generation, cursor_generation;
static uint32_t cursor_phys;
static uint8_t *cursor_mapping;
static void *register_window, *cursor_window;
static uint64_t last_log;
/* Verbose-boot breadcrumbs: COM1 output is synchronous, so the last step
 * line before a silent stop names the register access that stalled the
 * bus (T23, 2026-10-13). Repeated accesses (polling) print only once. */
static bool tracing;
static uint32_t trace_last = UINT32_MAX;
static void trace(const char *op, uint32_t key, uint32_t reg)
{
    if (!tracing || key == trace_last) return;
    trace_last = key;
    klog("[accel] savage step op=%s reg=%05x", op, reg);
}
static uint32_t hw_read(void *ctx, uint32_t offset)
{
    (void)ctx; trace("r32", offset, offset);
    return registers ? *(volatile uint32_t *)(registers + offset) : UINT32_MAX;
}
static uint8_t hw_read8(void *ctx, uint32_t offset)
{
    (void)ctx; trace("r8", 0x10000000u | offset, offset);
    return registers ? registers[offset] : 0xff;
}
static void hw_write8(void *ctx, uint32_t offset, uint8_t value)
{
    (void)ctx; trace("w8", 0x20000000u | offset, offset);
    if (registers) registers[offset] = value;
}
static void hw_write(void *ctx, uint32_t offset, uint32_t value)
{
    (void)ctx; trace("w32", 0x40000000u | offset, offset);
    if (registers) *(volatile uint32_t *)(registers + offset) = value;
}
static unsigned cr_port(void) { return registers[0x83cc] & 1u ? 0x83d4 : 0x83b4; }
static uint8_t cr_read(uint8_t index) { unsigned p=cr_port(); registers[p]=index; return registers[p+1]; }
static void cr_write(uint8_t index, uint8_t value) { unsigned p=cr_port(); registers[p]=index; registers[p+1]=value; }
static uint8_t hw_cr_read(void *ctx, uint8_t index)
{
    (void)ctx; trace("crr", 0x80000000u | index, index);
    if (index == 0x38 || index == 0x39) return cr_read(index);
    uint8_t a = cr_read(0x38), b = cr_read(0x39);
    cr_write(0x38, 0x48); cr_write(0x39, 0xa5);
    uint8_t value = cr_read(index);
    cr_write(0x39,b); cr_write(0x38,a);
    return value;
}
static void hw_cr_write(void *ctx, uint8_t index, uint8_t value)
{
    (void)ctx; trace("crw", 0xc0000000u | index, index);
    if (index == 0x38 || index == 0x39) { cr_write(index, value); return; }
    uint8_t a = cr_read(0x38), b = cr_read(0x39);
    cr_write(0x38, 0x48); cr_write(0x39, 0xa5);
    cr_write(index,value); cr_write(0x39,b); cr_write(0x38,a);
}
static uint64_t hw_now(void *ctx) { (void)ctx; return g_ticks * 1000; }
static bool initializing;
static int hw_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    while (us) {
        uint32_t part = us > 500 ? 500 : us;
        int err = udelay(part);
        if (err) return err;
        us -= part;
        if (initializing) kwork_yield();
    }
    return 0;
}
static void hw_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static void record(void)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    klog("[accel] driver=savage ops=%llu idle_waits=%llu fallbacks=%llu quarantined=%u",
        engine.operations, engine.idle_waits, engine.fallbacks, engine.quarantined);
}
void savage2d_record(void)
{ if (reg_claim >= 0 && g_ticks - last_log >= 2000) { last_log = g_ticks; record(); } }
static void hw_fault(void *ctx)
{
    (void)ctx;
    if (reg_claim >= 0) registry_quarantine(reg_claim, reg_generation);
    if (cursor_claim >= 0) registry_quarantine(cursor_claim, cursor_generation);
    record();
}
static bool safe_extent(uint32_t base, uint32_t size)
{
    if (base < 0x100000 || size > UINT32_MAX - base) return false;
    uint64_t end = (uint64_t)base + size;
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type == CBI_E820_RAM && base < e->base + e->length && end > e->base) return false;
    }
    return !(base < g_boot.kernel_end && end > g_boot.kernel_start);
}
static bool cursor_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    if (!engine.cpu_safe) return false;
    vmm_revoke_mmio(cursor_mapping, PAGE_SIZE); cursor_mapping = 0;
    return true;
}
static bool register_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    if (!engine.cpu_safe) return false;
    vmm_revoke_mmio((void *)registers,SAVAGE_MMIO_BYTES); registers=0;
    return true;
}
static int runtime_stop(void *ctx)
{
    int err = stop_engine(ctx);
    if (err || engine.quarantined) return err;
    if (cursor_claim >= 0 && cursor_mapping) {
        err = registry_quiesce(cursor_claim, cursor_generation, cursor_idle);
        if (!err) err = registry_release(cursor_claim, cursor_generation);
        if (err) { hw_fault(0); engine.quarantined = true; return -EIO; }
        cursor_claim = -1;
    }
    if (reg_claim >= 0) {
        err=registry_quiesce(reg_claim,reg_generation,register_idle);
        if (!err) err=registry_release(reg_claim,reg_generation);
        if (err) { hw_fault(0); engine.quarantined=true; return -EIO; }
        reg_claim=-1;
    }
    return 0;
}
/* One bounded file, on the same settings volume as desktop.cfg. A nonempty
 * or unreadable record never authorizes engine access. Commit includes the
 * directory/FAT/data barrier; close failures also prevent an attempt. */
#define ATTEMPT_PATH "C:/system/settings/accel.cfg"
static struct vfs_table attempt_table;
static bool attempt_blocked;
static int attempt_marker(struct vfs *vfs, bool clear, bool *previous)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int err = vfs_table_init(vfs, &attempt_table, 2);
    if (err) return err;
    if (!clear) {
        struct fat_entry entry;
        err = vfs_stat(&attempt_table, ATTEMPT_PATH, &entry);
        if (!err && entry.size) { *previous = true; goto out; }
        if (err != -FS_ENOENT && err) goto out;
    }
    if (vfs->volumes[2]->readonly) { err = -FS_EROFS; goto out; }
    int fd = vfs_open(&attempt_table, ATTEMPT_PATH, VFS_WRITE | VFS_CREATE, VFS_DENY_ALL, 0);
    if (fd < 0) { err = fd; goto out; }
    if (clear) err = vfs_truncate(&attempt_table, fd, 0);
    else {
        static const char marker[] = "accel=attempt\n";
        size_t done = 0;
        err = vfs_write(&attempt_table, fd, marker, sizeof(marker)-1, &done);
        if (!err && done != sizeof(marker)-1) err = -FS_EIO;
    }
    if (!err) err = vfs_commit(&attempt_table, fd);
    int closed = vfs_close(&attempt_table, fd);
    if (!err) err = closed;
out:
    vfs_table_destroy(&attempt_table);
    return err;
}
static const struct accel_ops *decline(const char *reason)
{
    klog("[accel] savage quarantined reason=%s", reason);
    return 0;
}
static const struct accel_ops *init_with_io(const struct fb_device *d, struct vfs *vfs,
                                          const struct savage_io *io)
{
    if (engine.quarantined || attempt_blocked)
        return decline(engine.reason ? engine.reason : "attempt_blocked");
    const struct pci_func *gpu = 0; unsigned displays = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p && p->class_code == 3) { displays++; gpu = p; }
    }
    if (displays != 1 || !savage2d_supported(gpu)) return decline("unsupported_device");
    if (!d || !d->present) return decline("framebuffer_absent");
    if (!(gpu->command & 2u)) return decline("memory_decode_disabled");
    if ((gpu->bar[0] & 7u) || (gpu->bar[1] & 7u) || !gpu->bar[0] ||
        ((gpu->bar[0]&~15u)&(SAVAGE_MMIO_BYTES-1u)) ||
        (gpu->bar[1] & ~15u) != d->phys || !safe_extent(gpu->bar[0] & ~15u, SAVAGE_MMIO_BYTES))
        return decline("invalid_aperture");
    /* No side effects for unsupported geometry. */
    if (!mode_supported(d)) return decline("unsupported_mode");
    bool previous = false;
    int marker = attempt_marker(vfs, false, &previous);
    if (previous) {
        attempt_blocked = true;
        klog("[accel] savage skipped after a failed attempt");
        return decline("previous_attempt");
    }
    if (marker) { attempt_blocked = true; return decline("marker_unavailable"); }
    klog("[accel] savage attempt persisted");
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage probe pci=%04x:%04x cmd=%04x bar0=%08x bar1=%08x lfb=%08x vram=%u",
            gpu->vendor, gpu->device, gpu->command, gpu->bar[0], gpu->bar[1], d->phys,
            g_boot.display.tag == CBI_DISPLAY_TAG ? g_boot.display.vram_bytes : 0);
    if (reg_claim < 0) {
        uint32_t base = gpu->bar[0] & ~15u;
        reg_claim = registry_claim(RES_MMIO, base, base + SAVAGE_MMIO_BYTES, "savage2d", false);
        if (reg_claim < 0) { attempt_blocked = true; return decline("register_claim_failed"); }
        reg_generation = registry_get(reg_claim)->generation;
        if (!register_window) register_window=vmm_reserve_mmio(SAVAGE_MMIO_BYTES);
        registers = register_window ? vmm_replace_mmio(register_window,SAVAGE_MMIO_BYTES,base,SAVAGE_MMIO_BYTES) : 0;
        if (!registers || registry_activate(reg_claim, reg_generation)) {
            hw_fault(0); engine.quarantined = true; return decline("register_map_failed");
        }
    }
    uint32_t vram = g_boot.display.tag == CBI_DISPLAY_TAG ? g_boot.display.vram_bytes : 0;
    uint32_t off = vram >= PAGE_SIZE && vram <= 32 * 1024 * 1024u ? (vram - PAGE_SIZE) & ~(PAGE_SIZE - 1u) : 0;
    if (off >= d->size && off && d->phys <= UINT32_MAX - off - PAGE_SIZE) {
        uint32_t phys = d->phys + off;
        if (cursor_claim < 0 && safe_extent(phys, PAGE_SIZE)) {
            cursor_claim = registry_claim(RES_MMIO, phys, phys + PAGE_SIZE, "savage-cursor", false);
            if (cursor_claim >= 0) {
                cursor_generation = registry_get(cursor_claim)->generation; cursor_phys = phys;
                if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
                cursor_mapping=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,phys,PAGE_SIZE) : 0;
                if (!cursor_mapping || registry_activate(cursor_claim, cursor_generation)) {
                    registry_quarantine(cursor_claim, cursor_generation); cursor_mapping = 0;
                }
            }
        }
    }
    uint8_t *cursor = cursor_mapping && cursor_phys >= d->phys + d->size ? cursor_mapping : 0;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage mapped mmio=%p cursor=%p", (void *)registers, (void *)cursor);
    tracing = !!(g_boot.flags & CBI_F_VERBOSE); trace_last = UINT32_MAX;
    initializing = true;
    int err = savage2d_start(&engine, io, d, gpu->device != 0x8a22, cursor, cursor_phys - d->phys);
    initializing = false;
    tracing = false;
    engine.ops.stop = runtime_stop;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage pci=%04x:%04x mmio=%08x lfb=%08x mode=%u bpp=%u pitch=%u caps=%u init_result=%d",
            gpu->vendor,gpu->device,gpu->bar[0]&~15u,d->phys,d->mode_id,d->bpp,d->pitch,capabilities(&engine),err);
    record();
    if (err) {
        /* No command was submitted by init. Keep the failed engine private;
         * software fbdev must not poll it again just to draw the desktop. */
        if (!engine.quarantined) { engine.quarantined = true; hw_fault(0); }
        return decline(engine.reason ? engine.reason : "init_failed");
    }
    marker = attempt_marker(vfs, true, &previous);
    if (marker) {
        /* Init submitted no commands. Do not introduce another status-read
         * exposure after the clear may have reached disk despite an error. */
        engine.quarantined = true; hw_fault(0);
        return decline("marker_clear_failed");
    }
    klog("[accel] savage ready");
    return &engine.ops;
}
const struct accel_ops *savage2d_init(const struct fb_device *d, struct vfs *vfs)
{
    const struct savage_io io = {0, hw_read, hw_write, hw_cr_read, hw_cr_write, hw_now, hw_delay,
        hw_drain, hw_fault, hw_read8, hw_write8};
    return init_with_io(d, vfs, &io);
}
