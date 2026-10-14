/* Independently implemented from register references in docs/design/savage-2d.md.
 * Firmware retains scanout; no PCI activation, DMA, mode or clock programming.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/registry.h>
#include <ciuki/savage2d.h>
#include <ciuki/work.h>
#include <ciuki/timing.h>
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
#define INITIAL_POLL_US 1000u
#define INITIAL_POLLS (WAIT_US / INITIAL_POLL_US + 1u)
#define STATUS_SAMPLES 8u
#define BCI_CONTROL 0x48c18u
#define GBD_READ_MASK 0x13ff1ffdu
static bool status_polling;
static bool initializing;
static bool trace_read_begin(const char *, uint32_t, uint32_t);
static void trace_value(const char *, uint32_t, uint32_t);
static void init_record(const char *, uint32_t, unsigned, uint32_t, uint32_t);
static uint8_t port_cr_read(struct savage2d *s, uint8_t index)
{
    s->io.write_port(s->io.ctx, 0x3d4, index);
    return s->io.read_port(s->io.ctx, 0x3d5);
}
static void port_cr_write(struct savage2d *s, uint8_t index, uint8_t value)
{
    s->io.write_port(s->io.ctx, 0x3d4, index);
    s->io.write_port(s->io.ctx, 0x3d5, value);
}
static uint8_t init_cr_write(struct savage2d *s, uint8_t index, uint8_t value)
{
    port_cr_write(s, index, value);
    uint8_t check = port_cr_read(s, index);
    init_record("cr", index, 8, value, check);
    return check;
}
static uint8_t init_port_write(struct savage2d *s, uint16_t port, uint16_t read_port, uint8_t value)
{
    s->io.write_port(s->io.ctx, port, value);
    uint8_t check = s->io.read_port(s->io.ctx, read_port);
    init_record("port", port, 8, value, check);
    return check;
}
static uint8_t init_seq_write(struct savage2d *s, uint8_t index, uint8_t value)
{
    s->io.write_port(s->io.ctx, 0x3c4, index);
    s->io.write_port(s->io.ctx, 0x3c5, value);
    uint8_t check = s->io.read_port(s->io.ctx, 0x3c5);
    init_record("sr", index, 8, value, check);
    return check;
}
static uint32_t read_2d(struct savage2d *s, uint32_t reg)
{
    uint32_t lo = s->io.read16(s->io.ctx, reg);
    return lo | ((uint32_t)s->io.read16(s->io.ctx, reg + 2) << 16);
}
static uint32_t init_write(struct savage2d *s, uint32_t reg, uint32_t value, bool bank_2d)
{
    s->io.write32(s->io.ctx, reg, value);
    uint32_t check = bank_2d ? read_2d(s, reg) : s->io.read32(s->io.ctx, reg);
    init_record("mmio", reg, 32, value, check);
    return check;
}
static void init_mix(struct savage2d *s, uint32_t reg, uint16_t value)
{
    s->io.write16(s->io.ctx, reg, value);
    init_record("mmio", reg, 16, value, s->io.read16(s->io.ctx, reg));
}
static void restore_mmio(struct savage2d *s)
{
    if (!s->mmio_saved) return;
    /* This is the final VGA access, including when the MMIO alias is dead.
     * Restore only our bit, never overwrite another CR40 field. */
    uint8_t value = port_cr_read(s, 0x40);
    uint8_t restored = (value & ~1u) | (s->saved40 & 1u);
    if (initializing) init_cr_write(s, 0x40, restored);
    else port_cr_write(s, 0x40, restored);
    s->mmio_saved = false;
}
static uint32_t xy(uint32_t x, uint32_t y) { return (y << 16) | x; }
static bool idle_status(struct savage2d *s, uint32_t value)
{
    /* X.Org uses this same empty/idle mask for SuperSavage and Savage4.
     * RE046-B assigns PF/idle differently (p. 231); accept only a complete
     * signature. No reference supports discarding SuperSavage FIFO bits. */
    bool xorg = (value & 0x00e1ffffu) == 0x00e00000u;
    bool manual = (value & 0x00cfffffu) == 0x004e0000u;
    if (value == UINT32_MAX) { s->reason = "dead_mmio"; return false; }
    if (!s->idle_mask) {
        if (xorg) { s->idle_mask = 0x00e1ffffu; s->idle_value = 0x00e00000u; }
        else if (manual) { s->idle_mask = 0x00cfffffu; s->idle_value = 0x004e0000u; }
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
    bool first = !s->status_logged;
    struct { uint64_t us; uint32_t value; } samples[STATUS_SAMPLES];
    unsigned count = 0;
    uint32_t old_mask = s->idle_mask;
    unsigned polls = first ? INITIAL_POLLS : WAIT_POLLS;
    uint32_t interval = first ? INITIAL_POLL_US : POLL_US;
    /* Keep the pre-read breadcrumb outside the budget, then buffer status
     * diagnostics. At 38400 baud a single log can exceed the entire wait. */
    bool traced = trace_read_begin("r32", SAVAGE_STATUS, SAVAGE_STATUS);
    uint32_t first_value = 0;
    uint64_t start = s->io.now_us(s->io.ctx);
    int result = -EIO;
    /* BCI self-test completion has the same serial deadline hazard as the
     * first wait. Defer transport logs for every bounded status poll. */
    status_polling = true;
    for (unsigned i = 0; i < polls; i++) {
        /* No pacing before the first read of a new idle check. Reset has
         * already settled; only consecutive busy reads need the interval. */
        if (i) {
            uint64_t elapsed = s->io.now_us(s->io.ctx) - start;
            if (elapsed >= WAIT_US) break;
            uint32_t delay = interval;
            if (delay > WAIT_US - elapsed) delay = (uint32_t)(WAIT_US - elapsed);
            if (s->io.delay_us(s->io.ctx, delay)) { s->reason = "delay_failed"; break; }
        }
        uint32_t status = s->io.read32(s->io.ctx, SAVAGE_STATUS);
        if (!i) first_value = status;
        uint64_t elapsed = s->io.now_us(s->io.ctx) - start;
        if (first && count < STATUS_SAMPLES) {
            samples[count].us = elapsed; samples[count].value = status; count++;
        }
        /* Retain the original fault value if later idle proof permits CPU
         * fallback on an already quarantined engine. */
        if (!s->quarantined) s->status = status;
        s->status_logged = true;
        if (idle_status(s, status)) {
            result = 0; break;
        }
        if (status == UINT32_MAX || (s->reason && !strncmp(s->reason, "status_contradiction", sizeof("status_contradiction"))) ||
            elapsed >= WAIT_US) break;
    }
    status_polling = false;
    s->poll_us += s->io.now_us(s->io.ctx) - start;
    if (traced) trace_value("r32", SAVAGE_STATUS, first_value);
    if (first && count) {
        klog("[accel] savage status reg=48c60 raw=%08x", samples[0].value);
        for (unsigned i = 0; i < count; i++)
            klog("[accel] savage status sample=%u us=%llu raw=%08x", i, samples[i].us, samples[i].value);
    }
    if (!old_mask && s->idle_mask)
        klog("[accel] savage idle mask=%08x value=%08x", s->idle_mask, s->idle_value);
    if (result && !s->reason) s->reason = "idle_timeout";
    s->cpu_safe = !result;
    return result;
}
static int fault(struct savage2d *s)
{
    if (!s->quarantined) {
        s->quarantined = true; s->active = false; s->timeouts++;
        /* Cursor is not an engine writer; hide it without touching scanout.
         * Never reset/re-enable or release an engine with unproven idle. */
        if (s->touched) {
            if (initializing) init_cr_write(s, 0x45, s->saved45 & ~1u);
            else port_cr_write(s, 0x45, s->saved45 & ~1u);
        }
        restore_mmio(s);
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
    if (!s->status_ready) return s->cpu_safe ? 0 : -EIO;
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
    s->submitted = true;
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
    port_cr_write(s, 0x45, (s->saved45 & ~0x11u) | (show ? 1u : 0u));
    if ((port_cr_read(s, 0x45) & 1u) != (show ? 1u : 0u)) return fault(s);
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
    port_cr_write(s, 0x55, s->saved55 & ~0x10u);
    unsigned bytes = ((s->descriptor >> 16) & 255u) == 16 ? 2 : 3;
    (void)port_cr_read(s, 0x45);
    for (unsigned i = 0; i < bytes; i++) port_cr_write(s, 0x4a, c->foreground >> (i * 8));
    (void)port_cr_read(s, 0x45);
    for (unsigned i = 0; i < bytes; i++) port_cr_write(s, 0x4b, c->background >> (i * 8));
    port_cr_write(s, 0x4c, (s->cursor_offset >> 18) & 0x7f);
    port_cr_write(s, 0x4d, (s->cursor_offset >> 10) & 0xfc);
    if (port_cr_read(s,0x4c) != ((s->cursor_offset >> 18)&0x7f) ||
        port_cr_read(s,0x4d) != ((s->cursor_offset >> 10)&0xfc)) return fault(s);
    s->cursor_loaded = true;
    return 0;
}
static int cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct savage2d *s = ctx;
    if (!s->active || s->quarantined || !s->cursor) return -ENODEV;
    if (x < -63 || y < -63 || x > 2047 || y > 2047) return -EINVAL;
    uint32_t px = x < 0 ? 0u : (uint32_t)x, py = y < 0 ? 0u : (uint32_t)y;
    port_cr_write(s, 0x4e, x < 0 ? (uint32_t)-x : 0);
    port_cr_write(s, 0x4f, y < 0 ? (uint32_t)-y : 0);
    port_cr_write(s, 0x46, px >> 8); port_cr_write(s, 0x47, px);
    port_cr_write(s, 0x49, py); port_cr_write(s, 0x48, py >> 8);
    s->operations++;
    return 0;
}
static int stop_engine(void *ctx)
{
    struct savage2d *s = ctx;
    if (!s->touched) {
        int err = s->cpu_safe ? 0 : sync_engine(s);
        restore_mmio(s);
        return err;
    }
    if (sync_engine(s)) return -EIO;
    /* Never touch quarantined resources beyond idle observation. */
    if (s->quarantined) return 0;
    port_cr_write(s, 0x45, s->saved45 & ~1u);
    if (disable_bci(s, s->saved_bci)) return -EIO;
    s->io.write32(s->io.ctx, 0x8168, s->saved_gbd[0]);
    s->io.write32(s->io.ctx, 0x816c, s->saved_gbd[1]);
    port_cr_write(s, 0x31, s->saved31);
    port_cr_write(s, 0x50, s->saved50);
    port_cr_write(s, 0x55, s->saved55);
    restore_mmio(s);
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
    if (!io || !io->read32 || !io->write32 || !io->read16 || !io->write16 ||
        !io->now_us || !io->delay_us || !io->drain ||
        !io->read_port || !io->write_port || !mode_supported(d) ||
        (cursor && (cursor_offset & 4095u))) return -EINVAL;
    s->io = *io; s->width = d->width; s->height = d->height; s->offset = 0;
    s->supersavage = supersavage; s->cursor = cursor; s->cursor_offset = cursor_offset;
    s->descriptor = 0x10000000u | ((uint32_t)d->bpp << 16) | (d->pitch / (d->bpp / 8));
    s->ops = (struct accel_ops){s, sync_engine, copy_rect, fill_rect, pattern_rect,
        cursor_set, cursor_move, cursor_show, stop_engine, capabilities, cpu_ready};
    s->reason = 0; s->idle_mask = s->idle_value = s->status = 0; s->status_logged = false;
    s->status_ready = false; s->submitted = false;
    s->reset_us = s->poll_us = 0;
    /* All operational VGA access stays on the exclusively owned ports.
     * RE046-B CR39: A0 unlocks extensions without the extra A5 permissions. */
    uint8_t vga = io->read_port(io->ctx, 0x3c3);
    init_port_write(s, 0x3c3, 0x3c3, vga | 1u);
    uint8_t port_misc = io->read_port(io->ctx, 0x3cc);
    init_port_write(s, 0x3c2, 0x3cc, port_misc | 1u);
    /* RE046-B pp. 75–76: unspecified key bits need not read back as
     * written (T23: 68/A8). Keep both reads in the diagnostic transport. */
    uint8_t cr38 = init_cr_write(s, 0x38, 0x48);
    init_cr_write(s, 0x39, 0xa0);
    s->saved40 = port_cr_read(s, 0x40); s->mmio_saved = true;
    if (!(init_cr_write(s, 0x40, s->saved40 | 1u) & 1u))
        return fail(s, "crtc_locked");
    if (io->read8 && io->write8) {
        io->write8(io->ctx, 0x83d4, 0x38);
        uint8_t alias = io->read8(io->ctx, 0x83d5);
        klog("[accel] savage vga_alias reg=083d5 port=%02x alias=%02x match=%u",
            cr38, alias, alias == cr38);
    }
    /* Nothing in the engine domain, especially MM48C60, before CR66 reset. */
    init_seq_write(s, 0x08, 0x06);
    if (supersavage) {
        if (init_seq_write(s, 0x26, 0x40) != 0x40) return fail(s, "pipe_select_failed");
    }
    uint8_t misc = io->read_port(io->ctx, 0x3cc);
    if (misc == 0xff) return fail(s, "dead_vga");
    if (!(init_port_write(s, 0x3c2, 0x3cc, misc | 1u) & 1u)) return fail(s, "crtc_select_failed");
    /* CR66.0 may only change during blank/retrace. Preserve the VBE engine
     * enablement; a firmware mode without it retains software presentation. */
    uint8_t cr66 = port_cr_read(s, 0x66) & 0xbbu;
    if (!(cr66 & 1u)) {
        s->cpu_safe=true; s->reason="engine_disabled"; restore_mmio(s); return -ENODEV;
    }
    uint64_t reset_start = io->now_us(io->ctx);
    port_cr_write(s, 0x66, cr66 | 2u);
    int delay = io->delay_us(io->ctx, 10000);
    uint8_t asserted = port_cr_read(s, 0x66);
    init_record("cr", 0x66, 8, cr66 | 2u, asserted);
    /* Always deassert reset even when pacing failed. Never change CR66.0. */
    port_cr_write(s, 0x66, cr66 & ~2u);
    int settled = io->delay_us(io->ctx, 10000);
    s->reset_us = io->now_us(io->ctx) - reset_start;
    uint8_t cleared = port_cr_read(s, 0x66);
    init_record("cr", 0x66, 8, cr66 & ~2u, cleared);
    if (delay || settled) return fail(s, "delay_failed");
    /* Reserved CR66.2/.6 have undefined reads. All defined controls,
     * especially enable and both reset edges, must still agree. */
    if ((asserted & 0xbbu) != ((cr66 | 2u) & 0xbbu) ||
        (cleared & 0xbbu) != ((cr66 & ~2u) & 0xbbu))
        return fail(s, "reset_readback_failed");
    /* Independent engine-window probe after reset. RE046-B gives MM48C18
     * a zero power-on default, but not a guaranteed CR66 reset value. Test
     * the defined controls, preserving firmware counters/reserved fields;
     * never guess GBD1's explicitly undefined default. */
    s->saved_bci = io->read32(io->ctx, BCI_CONTROL);
    klog("[accel] savage engine_probe reg=48c18 raw=%08x", s->saved_bci);
    if (s->saved_bci == UINT32_MAX) return fail(s, "dead_bci");
    if (s->saved_bci & 7u) return fail(s, "firmware_command_buffer");
    s->status_ready = true;
    if (sync_engine(s)) return -EIO;
    /* VBE linear aperture only: do not reinterpret a tiled primary stream. */
    if (io->read32(io->ctx, 0x81c8) & 0xc0000000u) return fail(s, "tiled_scanout");
    s->saved31 = port_cr_read(s, 0x31);
    s->saved50 = port_cr_read(s, 0x50); s->saved45 = port_cr_read(s, 0x45);
    s->saved55 = port_cr_read(s, 0x55);
    /* Repeat the upstream engine access sequence after the reset. Reserved
     * CR40 fields are zero in both acceleration initializers. */
    if (!(init_cr_write(s, 0x40, 1) & 1u)) return fail(s, "crtc_locked");
    init_cr_write(s, 0x31, 0x0c);
    uint32_t disabled = init_write(s, BCI_CONTROL, s->saved_bci & 0x3ff0u, false);
    if (disabled == UINT32_MAX || (disabled & 0x0fu)) return fail(s, "bci_disable_failed");
    s->saved_gbd[0] = read_2d(s, 0x8168); s->saved_gbd[1] = read_2d(s, 0x816c) & GBD_READ_MASK;
    s->touched = true;
    init_write(s, 0x8128, UINT32_MAX, true); init_write(s, 0x812c, UINT32_MAX, true);
    init_mix(s, 0x8134, 0x27); init_mix(s, 0x8136, 0x07);
    init_write(s, 0x48c10, 0x00700040u, false); init_write(s, 0x48c0c, 0, false);
    uint32_t enabled = (s->saved_bci & 0x3ff0u) | 8u;
    uint32_t control = init_write(s, BCI_CONTROL, enabled, false);
    if (control == UINT32_MAX || (control & 0x0fu) != 8u) return fail(s, "bci_enable_failed");
    /* PM/Twister GBD preamble, retaining firmware primary stream registers. */
    if (supersavage) init_write(s, 0x816c, 0x10000001u, true);
    init_cr_write(s, 0x50, s->saved50 | 0xc1u);
    init_write(s, 0x8168, 0, true);
    uint32_t gbd = s->descriptor | (supersavage ? 1u : 9u);
    uint32_t check = init_write(s, 0x816c, gbd, true);
    uint8_t cr40 = port_cr_read(s, 0x40);
    klog("[accel] savage descriptor wrote=%08x read=%08x mask=%08x cr40=%02x", gbd, check, GBD_READ_MASK, cr40);
    if (check == UINT32_MAX || (check & GBD_READ_MASK) != gbd || !(cr40 & 1u))
        return fail(s, "descriptor_readback_failed");
    if (sync_engine(s)) return -EIO;
    s->active = true;
    return 0;
}

static struct savage2d engine;
static volatile uint8_t *registers;
static int reg_claim = -1, cursor_claim = -1, port_claim = -1;
static gen_t reg_generation, cursor_generation, port_generation;
static uint32_t cursor_phys;
static uint8_t *cursor_mapping;
static void *register_window, *cursor_window;
static uint64_t last_log;
/* Verbose-boot breadcrumbs: COM1 output is synchronous, so the last step
 * line before a silent stop names the register access that stalled the
 * bus (T23, 2026-10-13). Repeated accesses (polling) print only once. */
static bool tracing;
static uint32_t trace_last = UINT32_MAX;
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks * 1000;
    uint64_t cycles = ktime_cycles();
    return cycles / g_tsc_per_ms * 1000 + cycles % g_tsc_per_ms * 1000 / g_tsc_per_ms;
}
static uint64_t (*activation_now)(void *) = hw_now;
static void *activation_ctx;
static uint64_t activation_start, trace_us;
static bool attempt_in_progress;
static uint64_t activation_elapsed(void)
{ return activation_now(activation_ctx) - activation_start; }
static bool trace_read_begin(const char *op, uint32_t key, uint32_t reg)
{
    if (status_polling && key == SAVAGE_STATUS) return false;
    if (!tracing || key == trace_last) return false;
    trace_last = key;
    uint64_t start = activation_now(activation_ctx);
    klog("[accel] savage step op=%s reg=%05x value=pending us=%llu", op, reg, start - activation_start);
    trace_us += activation_now(activation_ctx) - start;
    return true;
}
static void trace_value(const char *op, uint32_t reg, uint32_t value)
{
    uint64_t start = activation_now(activation_ctx);
    klog("[accel] savage step op=%s reg=%05x value=%08x us=%llu", op, reg, value, start - activation_start);
    trace_us += activation_now(activation_ctx) - start;
}
static void trace_write(const char *op, uint32_t key, uint32_t reg, uint32_t value)
{
    if (!tracing) return;
    trace_last = key;
    trace_value(op, reg, value);
}
static void init_record(const char *bank, uint32_t reg, unsigned width, uint32_t wrote, uint32_t read)
{
    uint64_t start = activation_now(activation_ctx);
    klog("[accel] savage init reg=%05x wrote=%08x read=%08x bank=%s width=%u", reg, wrote, read, bank, width);
    trace_us += activation_now(activation_ctx) - start;
}
static uint32_t hw_read(void *ctx, uint32_t offset)
{
    (void)ctx; bool logged = trace_read_begin("r32", offset, offset);
    uint32_t value = registers ? *(volatile uint32_t *)(registers + offset) : UINT32_MAX;
    if (logged) trace_value("r32", offset, value);
    return value;
}
static uint8_t hw_read8(void *ctx, uint32_t offset)
{
    (void)ctx; bool logged = trace_read_begin("r8", 0x10000000u | offset, offset);
    uint8_t value = registers ? registers[offset] : 0xff;
    if (logged) trace_value("r8", offset, value);
    return value;
}
static void hw_write8(void *ctx, uint32_t offset, uint8_t value)
{
    (void)ctx; trace_write("w8", 0x20000000u | offset, offset, value);
    if (registers) registers[offset] = value;
}
static void hw_write(void *ctx, uint32_t offset, uint32_t value)
{
    (void)ctx; trace_write("w32", 0x40000000u | offset, offset, value);
    if (registers) *(volatile uint32_t *)(registers + offset) = value;
}
static uint16_t hw_read16(void *ctx, uint32_t offset)
{
    (void)ctx; bool logged = trace_read_begin("r16", 0x60000000u | offset, offset);
    uint16_t value = registers ? *(volatile uint16_t *)(registers + offset) : UINT16_MAX;
    if (logged) trace_value("r16", offset, value);
    return value;
}
static void hw_write16(void *ctx, uint32_t offset, uint16_t value)
{
    (void)ctx; trace_write("w16", 0x70000000u | offset, offset, value);
    if (registers) *(volatile uint16_t *)(registers + offset) = value;
}
static uint8_t hw_port_read(void *ctx, uint16_t port)
{
    (void)ctx; bool logged = trace_read_begin("pr8", 0x30000000u | port, port);
    uint8_t value = port >= 0x3b0 && port < 0x3e0 ? inb(port) : 0xff;
    if (logged) trace_value("pr8", port, value);
    return value;
}
static void hw_port_write(void *ctx, uint16_t port, uint8_t value)
{
    (void)ctx; trace_write("pw8", 0x50000000u | port, port, value);
    if (port >= 0x3b0 && port < 0x3e0) outb(port, value);
}
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
    klog("[accel] driver=savage ops=%llu idle_waits=%llu fallbacks=%llu quarantined=%u status=%08x status_valid=%u",
        engine.operations, engine.idle_waits, engine.fallbacks, engine.quarantined, engine.status, engine.status_logged);
}
const struct accel_ops *savage2d_fallback_ops(void)
{
    return engine.quarantined && engine.submitted ? &engine.ops : 0;
}
static int bci_test(struct savage2d *s, const struct fb_device *d)
{
    if (!d->mapped) return fail(s, "bci_test_unmapped");
    /* The caller owns fbdev's presenter and nonwaiting-console exclusion.
     * Save one pixel, use a distinct packed marker, restore only after idle. */
    unsigned bytes = d->bpp / 8;
    volatile uint8_t *p = d->mapped + (d->height - 1) * d->pitch + (d->width - 1) * bytes;
    s->io.drain(s->io.ctx);
    uint32_t before = 0;
    for (unsigned i = 0; i < bytes; i++) before |= (uint32_t)p[i] << (8 * i);
    uint32_t mask = bytes == 2 ? 0xffffu : 0xffffffu;
    uint32_t marker = (before ^ 0x005aa55au) & mask;
    struct fb_rect r = {(int32_t)d->width - 1, (int32_t)d->height - 1, 1, 1};
    klog("[accel] savage bci_test phase=fill x=%u y=%u wrote=%08x before=%08x", r.x, r.y, marker, before);
    int err = fill_rect(s, &r, marker);
    uint32_t read = 0;
    bool valid = !err && s->cpu_safe;
    if (valid) {
        for (unsigned i = 0; i < bytes; i++) read |= (uint32_t)p[i] << (8 * i);
        for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(before >> (8 * i));
        s->io.drain(s->io.ctx);
        if ((read & mask) != marker) err = fail(s, "bci_test_readback_failed");
    }
    klog("[accel] savage bci_test result=%s error=%d wrote=%08x read=%08x read_valid=%u restored=%u status=%08x status_valid=%u",
        err ? "failed" : "passed", err, marker, read, valid, valid, s->status, s->status_logged);
    return err;
}
void savage2d_record(void)
{ if (reg_claim >= 0 && g_ticks - last_log >= 2000) { last_log = g_ticks; record(); } }
/* Caller holds the fbdev presenter mutex; this never reads hardware. */
void savage2d_snapshot(struct ciuki_accel_info *out)
{
    if (reg_claim < 0 && !engine.touched && !engine.quarantined) return;
    out->driver = CIUKI_ACCEL_DRIVER_SAVAGE;
    out->quarantined = engine.quarantined;
    out->operations = engine.operations;
    out->idle_waits = engine.idle_waits;
    out->fallbacks = engine.fallbacks;
}
static void hw_fault(void *ctx)
{
    (void)ctx;
    engine.active = false;
    restore_mmio(&engine);
    if (reg_claim >= 0) registry_quarantine(reg_claim, reg_generation);
    if (cursor_claim >= 0) registry_quarantine(cursor_claim, cursor_generation);
    if (port_claim >= 0) registry_quarantine(port_claim, port_generation);
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
static bool port_idle(int handle, gen_t generation)
{
    (void)handle; (void)generation;
    return !engine.active && !engine.mmio_saved && engine.cpu_safe;
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
    if (port_claim >= 0) {
        err = registry_quiesce(port_claim, port_generation, port_idle);
        if (!err) err = registry_release(port_claim, port_generation);
        if (err) { hw_fault(0); engine.quarantined = true; return -EIO; }
        port_claim = -1;
    }
    return 0;
}
/* One bounded file, on the same settings volume as desktop.cfg. A nonempty
 * or unreadable record never authorizes engine access. Commit includes the
 * directory/FAT/data barrier; close failures also prevent an attempt. */
#define ATTEMPT_PATH "C:/system/settings/accel.cfg"
static struct vfs_table attempt_table;
static bool attempt_blocked;
static int attempt_marker_io(struct vfs *vfs, bool clear, bool *previous, const char *diagnostic)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int err = vfs_table_init(vfs, &attempt_table, 2);
    if (err) return err;
    if (!clear && !diagnostic) {
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
        const char *marker = diagnostic ? diagnostic : "accel=attempt\n";
        size_t bytes = strlen(marker);
        size_t done = 0;
        err = vfs_write(&attempt_table, fd, marker, bytes, &done);
        if (!err && done != bytes) err = -FS_EIO;
    }
    if (!err) err = vfs_commit(&attempt_table, fd);
    int closed = vfs_close(&attempt_table, fd);
    if (!err) err = closed;
out:
    vfs_table_destroy(&attempt_table);
    return err;
}
static int attempt_marker(struct vfs *vfs, bool clear, bool *previous)
{
    uint64_t start = hw_now(0);
    int err = attempt_marker_io(vfs, clear, previous, 0);
    klog("[accel] savage phase=marker_%s us=%llu error=%d", clear ? "clear" : "write", hw_now(0) - start, err);
    return err;
}
static const struct accel_ops *decline(const char *reason)
{
    klog("[accel] savage quarantined reason=%s us=%llu reset_us=%llu poll_us=%llu trace_us=%llu status=%08x status_valid=%u",
        reason, activation_elapsed(), engine.reset_us, engine.poll_us, trace_us, engine.status, engine.status_logged);
    attempt_in_progress = false;
    return 0;
}
static const char *eligibility(const struct fb_device *d, const struct pci_func **found)
{
    if (engine.quarantined || attempt_blocked)
        return engine.reason ? engine.reason : "attempt_blocked";
    const struct pci_func *gpu = 0; unsigned displays = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p && p->class_code == 3) { displays++; gpu = p; }
    }
    if (displays != 1 || !savage2d_supported(gpu)) return "unsupported_device";
    if (!d || !d->present) return "framebuffer_absent";
    if (!(gpu->command & 1u)) return "io_decode_disabled";
    if (!(gpu->command & 2u)) return "memory_decode_disabled";
    if ((gpu->bar[0] & 7u) || (gpu->bar[1] & 7u) || !gpu->bar[0] ||
        ((gpu->bar[0]&~15u)&(SAVAGE_MMIO_BYTES-1u)) ||
        (gpu->bar[1] & ~15u) != d->phys || !safe_extent(gpu->bar[0] & ~15u, SAVAGE_MMIO_BYTES))
        return "invalid_aperture";
    /* No side effects for unsupported geometry. */
    if (!mode_supported(d)) return "unsupported_mode";
    *found = gpu;
    return 0;
}
bool savage2d_eligible(const struct fb_device *d)
{
    const struct pci_func *gpu;
    const char *reason = eligibility(d, &gpu);
    if (reason) decline(reason);
    return !reason;
}
static const struct accel_ops *init_with_io(const struct fb_device *d, struct vfs *vfs,
                                          const struct savage_io *io)
{
    if (vfs || !attempt_in_progress) {
        activation_now = io->now_us; activation_ctx = io->ctx;
        activation_start = activation_now(activation_ctx);
    }
    trace_us = 0;
    const struct pci_func *gpu;
    const char *reason = eligibility(d, &gpu);
    if (reason) return decline(reason);
    /* With a VFS the marker is handled here (host harness); the kernel caller
     * passes NULL and persists/clears it outside the presenter mutex
     * (savage2d_attempt_begin/end): file I/O takes the ATA channel mutex and
     * the T23 panicked on lock order on 2026-10-13. */
    bool previous = false;
    if (vfs) {
        int marker = attempt_marker(vfs, false, &previous);
        if (previous) {
            attempt_blocked = true;
            klog("[accel] savage skipped after a failed attempt");
            return decline("previous_attempt");
        }
        if (marker) { attempt_blocked = true; return decline("marker_unavailable"); }
        klog("[accel] savage attempt persisted");
    }
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage probe pci=%04x:%04x cmd=%04x bar0=%08x bar1=%08x lfb=%08x vram=%u",
            gpu->vendor, gpu->device, gpu->command, gpu->bar[0], gpu->bar[1], d->phys,
            g_boot.display.tag == CBI_DISPLAY_TAG ? g_boot.display.vram_bytes : 0);
    if (port_claim < 0) {
        port_claim = registry_claim(RES_PORT, 0x3b0, 0x3e0, "savage-vga", false);
        if (port_claim < 0) { attempt_blocked = true; return decline("port_claim_failed"); }
        port_generation = registry_get(port_claim)->generation;
        if (registry_activate(port_claim, port_generation)) {
            hw_fault(0); engine.quarantined = true; return decline("port_activate_failed");
        }
    }
    if (reg_claim < 0) {
        uint32_t base = gpu->bar[0] & ~15u;
        reg_claim = registry_claim(RES_MMIO, base, base + SAVAGE_MMIO_BYTES, "savage2d", false);
        if (reg_claim < 0) {
            hw_fault(0); engine.quarantined = true; return decline("register_claim_failed");
        }
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
    if (!err) err = bci_test(&engine, d);
    else klog("[accel] savage bci_test result=skipped reason=%s", engine.reason ? engine.reason : "init_failed");
    initializing = false;
    tracing = false;
    engine.ops.stop = runtime_stop;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage pci=%04x:%04x mmio=%08x lfb=%08x mode=%u bpp=%u pitch=%u caps=%u init_result=%d",
            gpu->vendor,gpu->device,gpu->bar[0]&~15u,d->phys,d->mode_id,d->bpp,d->pitch,capabilities(&engine),err);
    record();
    if (err) {
        /* fbdev retains an idle guard only if the marker test submitted a
         * command. Earlier failures keep the existing software path. */
        if (!engine.quarantined) { engine.quarantined = true; hw_fault(0); }
        if (vfs) savage2d_attempt_failed(vfs);
        return decline(engine.reason ? engine.reason : "init_failed");
    }
    if (vfs) {
        int marker = attempt_marker(vfs, true, &previous);
        if (marker) {
            /* The marker command completed; no writer remains. */
            engine.quarantined = true; engine.active = false; hw_fault(0);
            return decline("marker_clear_failed");
        }
    }
    klog("[accel] savage ready us=%llu reset_us=%llu poll_us=%llu trace_us=%llu status=%08x status_valid=%u",
        activation_elapsed(), engine.reset_us, engine.poll_us, trace_us, engine.status, engine.status_logged);
    attempt_in_progress = false;
    return &engine.ops;
}

/* Kernel callers: marker file I/O outside any presenter lock. */
int savage2d_attempt_begin(struct vfs *vfs)
{
    activation_now = hw_now; activation_ctx = 0; activation_start = hw_now(0);
    trace_us = 0; attempt_in_progress = true;
    if (engine.quarantined || attempt_blocked) { attempt_in_progress = false; return -1; }
    bool previous = false;
    int marker = attempt_marker(vfs, false, &previous);
    if (previous) {
        attempt_blocked = true;
        klog("[accel] savage skipped after a failed attempt");
        decline("previous_attempt");
        return -1;
    }
    if (marker) { attempt_blocked = true; decline("marker_unavailable"); return -1; }
    klog("[accel] savage attempt persisted");
    return 0;
}

int savage2d_attempt_end(struct vfs *vfs)
{
    bool previous = false;
    return attempt_marker(vfs, true, &previous);
}

int savage2d_attempt_failed(struct vfs *vfs)
{
    if (!engine.quarantined || !engine.status_logged) return 0;
    /* Best effort, outside presenter/device-work locks. Never truncate the
     * durable attempt: even a failed annotation must continue to block boot. */
    char marker[] = "accel=attempt\nstatus=00000000\n";
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; i++)
        marker[sizeof("accel=attempt\nstatus=")-1+i] = hex[(engine.status >> (28-i*4)) & 15u];
    uint64_t start = hw_now(0);
    int err = attempt_marker_io(vfs, false, 0, marker);
    klog("[accel] savage phase=marker_status us=%llu error=%d status=%08x",
        hw_now(0) - start, err, engine.status);
    return err;
}

void savage2d_quarantine(const char *reason)
{
    if (!engine.quarantined && engine.touched)
        port_cr_write(&engine, 0x45, engine.saved45 & ~1u);
    engine.quarantined = true; engine.active = false; engine.reason = reason;
    hw_fault(0);
    decline(reason);
}
const struct accel_ops *savage2d_init(const struct fb_device *d, struct vfs *vfs)
{
    const struct savage_io io = {0, hw_read, hw_write, hw_now, hw_delay,
        hw_drain, hw_fault, hw_read8, hw_write8, hw_port_read, hw_port_write, hw_read16, hw_write16};
    return init_with_io(d, vfs, &io);
}
