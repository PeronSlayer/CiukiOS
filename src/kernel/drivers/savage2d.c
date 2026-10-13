/* Independently implemented from register references in docs/design/savage-2d.md.
 * Firmware retains scanout; no PCI activation, DMA, mode or clock programming.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/registry.h>
#include <ciuki/savage2d.h>
#define RECT 0x48000000u
#define XP 0x01000000u
#define YP 0x02000000u
#define DEST_NEW 0x00000c00u
#define SRC_NEW 0x00000140u
#define COLOR 0x00008000u
#define WAIT_US 20000u
#define WAIT_POLLS 100000u
static uint32_t xy(uint32_t x, uint32_t y) { return (y << 16) | x; }
static bool idle_status(uint32_t s)
{ return s != UINT32_MAX && (s & 0x00e1ffffu) == 0x00e00000u; }
static int wait_idle(struct savage2d *s)
{
    s->idle_waits++;
    uint64_t start = s->io.now_us(s->io.ctx);
    for (unsigned i = 0; i < WAIT_POLLS; i++) {
        uint32_t status = s->io.read32(s->io.ctx, SAVAGE_STATUS);
        if (idle_status(status)) { s->cpu_safe = true; return 0; }
        if (status == UINT32_MAX || s->io.now_us(s->io.ctx) - start >= WAIT_US) break;
        s->io.relax(s->io.ctx);
    }
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
    s->io.write32(s->io.ctx, 0x48c18, s->saved_bci & ~0x0fu);
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
        !io->now_us || !io->relax || !io->drain || !mode_supported(d) ||
        (cursor && (cursor_offset & 4095u))) return -EINVAL;
    s->io = *io; s->width = d->width; s->height = d->height; s->offset = 0;
    s->supersavage = supersavage; s->cursor = cursor; s->cursor_offset = cursor_offset;
    s->descriptor = 0x10000000u | ((uint32_t)d->bpp << 16) | (d->pitch / (d->bpp / 8));
    s->ops = (struct accel_ops){s, sync_engine, copy_rect, fill_rect, pattern_rect,
        cursor_set, cursor_move, cursor_show, stop_engine, capabilities, cpu_ready};
    /* CR66.0 may only change during blank/retrace. Preserve the VBE engine
     * enablement; a firmware mode without it retains software presentation. */
    if (!(io->read_cr(io->ctx,0x66)&1u)) { s->cpu_safe=true; return -ENODEV; }
    if (sync_engine(s)) return -EIO;
    /* VBE linear aperture only: do not reinterpret a tiled primary stream. */
    if (io->read32(io->ctx, 0x81c8) & 0xc0000000u) return -ENODEV;
    s->saved40 = io->read_cr(io->ctx, 0x40); s->saved31 = io->read_cr(io->ctx, 0x31);
    s->saved50 = io->read_cr(io->ctx, 0x50); s->saved45 = io->read_cr(io->ctx, 0x45);
    s->saved55 = io->read_cr(io->ctx, 0x55); s->saved_bci = io->read32(io->ctx, 0x48c18);
    s->saved_gbd[0] = io->read32(io->ctx, 0x8168); s->saved_gbd[1] = io->read32(io->ctx, 0x816c);
    /* Active firmware command buffers cannot be repurposed. */
    if (s->saved_bci & 7u) return -ENODEV;
    s->touched = true;
    io->write_cr(io->ctx, 0x40, s->saved40 | 1u);
    io->write_cr(io->ctx, 0x31, (s->saved31 & ~1u) | 0x0cu);
    io->write32(io->ctx, 0x8128, UINT32_MAX); io->write32(io->ctx, 0x812c, UINT32_MAX);
    io->write32(io->ctx, 0x8134, 0x00070027u);
    io->write32(io->ctx, 0x48c18, s->saved_bci & 0x3ff0u);
    io->write32(io->ctx, 0x48c0c, 0); io->write32(io->ctx, 0x48c10, 0x00700040u);
    io->write32(io->ctx, 0x8168, 0);
    uint32_t gbd = s->descriptor | (supersavage ? 1u : 9u);
    io->write32(io->ctx, 0x816c, gbd);
    io->write_cr(io->ctx, 0x50, (s->saved50 & ~0x30u) | (d->bpp==16 ? 0x10u : 0x30u) | 0xc1u);
    io->write32(io->ctx, 0x48c18, (s->saved_bci & 0x3ff0u) | 8u);
    if (io->read32(io->ctx, 0x816c) != gbd || !(io->read_cr(io->ctx, 0x40) & 1u) || sync_engine(s)) return fault(s);
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
static void hw_write(void *ctx, uint32_t offset, uint32_t value)
{
    (void)ctx; trace("w32", 0x40000000u | offset, offset);
    if (registers) *(volatile uint32_t *)(registers + offset) = value;
}
static uint8_t cr_read(uint8_t index) { registers[0x83d4] = index; return registers[0x83d5]; }
static void cr_write(uint8_t index, uint8_t value) { registers[0x83d4] = index; registers[0x83d5] = value; }
static uint8_t hw_cr_read(void *ctx, uint8_t index)
{
    (void)ctx; trace("crr", 0x80000000u | index, index);
    uint8_t a = cr_read(0x38), b = cr_read(0x39);
    cr_write(0x38, 0x48); cr_write(0x39, 0xa5);
    uint8_t value = cr_read(index);
    cr_write(0x39,b); cr_write(0x38,a);
    return value;
}
static void hw_cr_write(void *ctx, uint8_t index, uint8_t value)
{
    (void)ctx; trace("crw", 0xc0000000u | index, index);
    uint8_t a = cr_read(0x38), b = cr_read(0x39);
    cr_write(0x38, 0x48); cr_write(0x39, 0xa5);
    cr_write(index,value); cr_write(0x39,b); cr_write(0x38,a);
}
static uint64_t hw_now(void *ctx) { (void)ctx; return g_ticks * 1000; }
static void hw_relax(void *ctx) { (void)ctx; __asm__ volatile("pause" ::: "memory"); }
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
const struct accel_ops *savage2d_init(const struct fb_device *d)
{
    if (engine.quarantined) return engine.ops.sync ? &engine.ops : 0;
    const struct pci_func *gpu = 0; unsigned displays = 0;
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        if (p && p->class_code == 3) { displays++; gpu = p; }
    }
    if (displays != 1 || !savage2d_supported(gpu) || !d->present || !(gpu->command & 2u) ||
        (gpu->bar[0] & 7u) || (gpu->bar[1] & 7u) || !gpu->bar[0] ||
        ((gpu->bar[0]&~15u)&(SAVAGE_MMIO_BYTES-1u)) ||
        (gpu->bar[1] & ~15u) != d->phys || !safe_extent(gpu->bar[0] & ~15u, SAVAGE_MMIO_BYTES)) return 0;
    /* No side effects for unsupported geometry. */
    if (!mode_supported(d)) return 0;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage probe pci=%04x:%04x cmd=%04x bar0=%08x bar1=%08x lfb=%08x vram=%u",
            gpu->vendor, gpu->device, gpu->command, gpu->bar[0], gpu->bar[1], d->phys,
            g_boot.display.tag == CBI_DISPLAY_TAG ? g_boot.display.vram_bytes : 0);
    if (reg_claim < 0) {
        uint32_t base = gpu->bar[0] & ~15u;
        reg_claim = registry_claim(RES_MMIO, base, base + SAVAGE_MMIO_BYTES, "savage2d", false);
        if (reg_claim < 0) return 0;
        reg_generation = registry_get(reg_claim)->generation;
        if (!register_window) register_window=vmm_reserve_mmio(SAVAGE_MMIO_BYTES);
        registers = register_window ? vmm_replace_mmio(register_window,SAVAGE_MMIO_BYTES,base,SAVAGE_MMIO_BYTES) : 0;
        if (!registers || registry_activate(reg_claim, reg_generation)) { hw_fault(0); engine.quarantined = true; return 0; }
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
    struct savage_io io = {0, hw_read, hw_write, hw_cr_read, hw_cr_write, hw_now, hw_relax, hw_drain, hw_fault};
    uint8_t *cursor = cursor_mapping && cursor_phys >= d->phys + d->size ? cursor_mapping : 0;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage mapped mmio=%p cursor=%p", (void *)registers, (void *)cursor);
    tracing = !!(g_boot.flags & CBI_F_VERBOSE); trace_last = UINT32_MAX;
    int err = savage2d_start(&engine, &io, d, gpu->device != 0x8a22, cursor, cursor_phys - d->phys);
    tracing = false;
    engine.ops.stop = runtime_stop;
    if (g_boot.flags & CBI_F_VERBOSE)
        klog("[accel] savage pci=%04x:%04x mmio=%08x lfb=%08x mode=%u bpp=%u pitch=%u caps=%u init_result=%d",
            gpu->vendor,gpu->device,gpu->bar[0]&~15u,d->phys,d->mode_id,d->bpp,d->pitch,capabilities(&engine),err);
    record();
    if (err && !engine.touched && !engine.quarantined) {
        (void)runtime_stop(&engine); return 0;
    }
    return !err || engine.touched || engine.quarantined ? &engine.ops : 0;
}
