/* Per-VM VGA bus, synthetic INT10 and bounded private-aperture faults.
 * Sources/decisions: docs/design/f3-acceptance.md#f3-02.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include "dosvm_internal.h"

extern const uint8_t font_cfn_regular[];
static int video_read(void *arg, uint32_t address, uint8_t *value)
{
    struct dosvm *v = arg;
    int e = dosvm_read(v, address, value, 1);
    if (e) v->video_bus_fault = true;
    return e;
}
static int video_write(void *arg, uint32_t address, uint8_t value)
{
    struct dosvm *v = arg;
    int e = dosvm_write(v, address, &value, 1);
    if (e) v->video_bus_fault = true;
    return e;
}
static int video_fetch(void *arg, uint32_t address, uint8_t *value)
{
    if (address >= 0xA0000 && address < 0xC0000) return -EFAULT;
    return video_read(arg, address, value);
}
static int video_port(struct dosvm *v, uint16_t port, unsigned width,
                       bool write, uint32_t *value, void *arg)
{
    (void)arg;
    if (port + width > 0x3E0) return -EINVAL;
    uint32_t result = 0;
    /* VGA indexed OUTW is index then value at consecutive ports. */
    uint8_t status = (g_ticks % 33 >= 30) ? 9 : 0;
    for (unsigned i = 0; i < width; i++) {
        if (write) cvga_write_port(&v->vga, port + i, *value >> (8 * i));
        else result |= (uint32_t)cvga_read_port(&v->vga, port + i, status) << (8 * i);
    }
    if (!write) *value = result;
    return 0;
}
int dosvm_vga_bios(struct dosvm *v, struct v86_frame *f)
{
    if ((f->tf.eax & 0xFF00) == 0x4F00) {
        f->tf.eax = (f->tf.eax & 0xFFFF0000u) | 0x014F;
        return 0;
    }
    unsigned ax = f->tf.eax & 0xFFFF, bytes = 0, offset = 0;
    bool output = false;
    if (ax == 0x1012 || ax == 0x1017) {
        bytes = (uint16_t)f->tf.ecx * 3u; offset = (uint16_t)f->tf.edx;
        output = ax == 0x1017;
    } else if (ax == 0x1002 || ax == 0x1009) {
        bytes = 17; offset = (uint16_t)f->tf.edx; output = ax == 0x1009;
    } else if ((ax & 0xFFEF) == 0x1100) {
        bytes = (uint16_t)f->tf.ecx * ((f->tf.ebx >> 8) & 255); offset = (uint16_t)f->tf.ebp;
    } else if ((ax >> 8) == 0x13) {
        bytes = (uint16_t)f->tf.ecx * ((ax & 2) ? 2u : 1u); offset = (uint16_t)f->tf.ebp;
    }
    if (bytes && (bytes > 65536 || offset > 65536 - bytes ||
        !dosvm_range(v, (f->es << 4) + offset, bytes, output))) return -EFAULT;
    cvbios_regs r = {f->tf.eax, f->tf.ebx, f->tf.ecx, f->tf.edx,
                     f->tf.esi, f->tf.edi, f->tf.ebp, f->es, f->ds};
    v->video_bus_fault = false;
    cvbios_int10(&v->video_bios, &r);
    if (v->video_bus_fault) return -EFAULT;
    f->tf.eax = r.eax; f->tf.ebx = r.ebx; f->tf.ecx = r.ecx; f->tf.edx = r.edx;
    f->tf.esi = r.esi; f->tf.edi = r.edi; f->tf.ebp = r.ebp; f->es = r.es; f->ds = r.ds;
    if ((ax >> 8) == 0 && (ax & 0x7F) <= 3) {
        /* Existing 8x16 console glyphs: eight dots, no ninth-dot stretch. */
        cvga_write_port(&v->vga, 0x3C4, 1);
        cvga_write_port(&v->vga, 0x3C5, v->vga.seq[1] | 1);
    }
    uint16_t pos;
    if (!dosvm_read(v, 0x450, &pos, 2)) v->cursor = (pos >> 8) * 80 + (pos & 255);
    return 0;
}
int dosvm_vga_init(struct dosvm *v)
{
    cvga_init(&v->vga);
    v->video_bus = (cvx_bus){video_read, video_write, video_fetch, v, 0};
    v->video_bios.vga = &v->vga; v->video_bios.bus = &v->video_bus;
    static const unsigned heights[] = {8, 14, 16};
    static const unsigned addresses[] = {0xF3000, 0xF4000, 0xF2000};
    for (unsigned which = 0; which < 3; which++) {
        unsigned height = heights[which], address = addresses[which];
        for (unsigned c = 0; c < 256; c++) for (unsigned y = 0; y < height; y++) {
            unsigned glyph = c >= 32 && c <= 126 ? c - 32 : '?' - 32;
            uint8_t bits = c == 0 ? 0 : font_cfn_regular[95 + (glyph * 16 + y * 16 / height) * 2 + 1];
            unsigned at = address + c * height + y;
            ((uint8_t *)P2V(v->frames[at / PAGE_SIZE]))[at % PAGE_SIZE] = bits;
        }
    }
    v->video_bios.font[CVBIOS_FONT_8X16] = 0xF0002000;
    v->video_bios.font[CVBIOS_FONT_8X8] = 0xF0003000;
    v->video_bios.font[CVBIOS_FONT_8X8_HIGH] = 0xF0003400;
    v->video_bios.font[CVBIOS_FONT_8X14] = 0xF0004000;
    struct v86_frame f = {0}; f.tf.eax = 3;
    int e = dosvm_vga_bios(v, &f);
    return e ? e : dosvm_port_register(v->info.key, 0x3B0, 0x3DF, video_port, 0);
}
int dosvm_vga_fault(struct dosvm *v, struct v86_frame *f)
{
    cvx_cpu c = {0};
    c.gpr[0] = f->tf.eax; c.gpr[1] = f->tf.ecx; c.gpr[2] = f->tf.edx; c.gpr[3] = f->tf.ebx;
    c.gpr[4] = f->tf.user_esp; c.gpr[5] = f->tf.ebp; c.gpr[6] = f->tf.esi; c.gpr[7] = f->tf.edi;
    c.eip = f->tf.eip; c.eflags = f->tf.eflags;
    uint32_t segments[] = {f->es, f->tf.cs, f->tf.user_ss, f->ds, f->fs, f->gs};
    for (unsigned i = 0; i < 6; i++) {
        if (segments[i] > 65535) return -EFAULT;
        c.sreg[i] = segments[i]; c.seg_base[i] = segments[i] << 4;
    }
    cvx_result result;
    int e = cvx_execute(&c, &v->video_bus, 256, &result);
    if (e != CVX_DONE && e != CVX_PARTIAL) return -EFAULT;
    f->tf.eax = c.gpr[0]; f->tf.ecx = c.gpr[1]; f->tf.edx = c.gpr[2]; f->tf.ebx = c.gpr[3];
    f->tf.user_esp = c.gpr[4]; f->tf.ebp = c.gpr[5]; f->tf.esi = c.gpr[6]; f->tf.edi = c.gpr[7];
    f->tf.eip = c.eip; f->tf.eflags = c.eflags;
    if (e == CVX_DONE && v->monitor.shadow) return v86_debug_step(&v->monitor, f);
    return 0;
}

/* Row-based form of the retained vga_presenter logic: same dirty dependency
 * queries and six-bit DAC expansion, without a contiguous framebuffer copy. */
int dosvm_vga_present(struct dosvm *v, int (*row)(void *, unsigned, const void *, unsigned),
                      void *arg, uint64_t tick, struct ciuki_rect *damage)
{
    memset(damage, 0, sizeof(*damage));
    if (v->video_presented && tick - v->video_tick < 33) return 0;
    cvga_geometry g = {0};
    if (!cvga_get_geometry(&v->vga, &g) || !g.width || !g.height || g.width > 640 || g.height > 480)
        return -EOPNOTSUPP;
    unsigned scale = 640 / g.width;
    if (480 / g.height < scale) scale = 480 / g.height;
    unsigned width = g.width * scale, height = g.height * scale;
    unsigned left = (640 - width) / 2, top = (480 - height) / 2;
    unsigned phase = (tick / 500) & 1 ? 0 : CVGA_BLINK_VISIBLE | CVGA_CURSOR_VISIBLE;
    bool full = !v->video_presented || v->video_display != v->vga.display_changes ||
                memcmp(&g, &v->video_geometry, sizeof(g));
    if (g.text) for (unsigned i = 0; i < CVGA_DIRTY_BYTES; i++) if (v->vga.dirty[2][i]) { full = true; break; }
    unsigned first = 480, last = 0;
    for (unsigned y = 0; y < 480; y++) {
        bool inside = y >= top && y < top + height;
        unsigned source = inside ? (y - top) / scale : 0;
        bool dirty = full || (inside && (cvga_row_reads_dirty(&v->vga, source, v->vga.dirty) ||
                       (phase != v->video_phase && cvga_row_phase_sensitive(&v->vga, source))));
        if (!dirty) continue;
        memset(v->video_pixels, 0, sizeof(v->video_pixels));
        if (inside) {
            if (cvga_render_row8(&v->vga, source, v->video_row, sizeof(v->video_row), phase) != g.width)
                return -EOPNOTSUPP;
            for (unsigned x = 0; x < width; x++) {
                const uint8_t *p = v->vga.dac[v->video_row[x / scale]];
                unsigned r = (p[0] << 2) | (p[0] >> 4), gr = (p[1] << 2) | (p[1] >> 4), b = (p[2] << 2) | (p[2] >> 4);
                v->video_pixels[left + x] = g.blank ? 0 : (r << 16) | (gr << 8) | b;
            }
        }
        int e = row(arg, y, v->video_pixels, sizeof(v->video_pixels));
        if (e) { v->video_presented = false; return e; } /* retry full after a partial write */
        if (y < first) first = y;
        last = y + 1;
    }
    memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
    v->video_tick = tick; v->video_phase = phase; v->video_display = v->vga.display_changes;
    v->video_geometry = g; v->video_presented = true;
    if (last) *damage = (struct ciuki_rect){0, (int32_t)first, 0, (int32_t)first, 640, last - first};
    return last ? 1 : 0;
}
