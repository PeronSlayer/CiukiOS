/* Per-VM VGA bus, synthetic INT 10h, guarded A0000-BFFFF faults and the
 * window presenter. No physical VGA port, mode, font or firmware is used.
 * Sources/decisions: docs/design/f3-acceptance.md#f3-02.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include "dosvm_internal.h"

extern const uint8_t font_cfn_regular[]; /* 95 widths, then 95 x 16 u16 rows */

static bool vram(uint32_t address) { return address >= 0xA0000 && address < 0xC0000; }

/* Guest-originated cycles: aperture reads load the VGA latches, exactly as a
 * CPU read does on hardware. Kernel-side dosvm_read() peeks instead. */
static int video_read(void *arg, uint32_t address, uint8_t *value)
{
    struct dosvm *v = arg;
    if (vram(address)) { *value = cvga_read_vram(&v->vga, address); return 0; }
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
    if (vram(address)) return -EFAULT; /* no instruction fetch from the aperture */
    return video_read(arg, address, value);
}

/* Input status 1 (3BAh/3DAh). Bit 3: vertical retrace during the last
 * DOSVM_RETRACE_TICKS of each presenter frame. Bit 0 (display disabled) is
 * set throughout vertical retrace and alternates on successive reads in the
 * active period, standing in for horizontal blanking. Polling never advances
 * the frame clock. */
uint8_t dosvm_vga_status(struct dosvm *v, uint64_t tick)
{
    if (tick % DOSVM_FRAME_TICKS >= DOSVM_FRAME_TICKS - DOSVM_RETRACE_TICKS) return 9;
    return (v->video_hsync++ & 1) ? 1 : 0;
}

static int video_port(struct dosvm *v, uint16_t port, unsigned width,
                      bool write, uint32_t *value, void *arg)
{
    (void)arg;
    uint32_t result = 0;
    /* Indexed OUTW/OUTD address consecutive ports, low byte first. Bytes past
     * the registered 3B0h-3DFh range belong to no device: all ones/ignored. */
    for (unsigned i = 0; i < width; i++) {
        unsigned p = port + i;
        uint8_t byte = 0xFF;
        if (p <= 0x3DF) {
            if (write) cvga_write_port(&v->vga, (uint16_t)p, (uint8_t)(*value >> (8 * i)));
            else byte = cvga_read_port(&v->vga, (uint16_t)p, dosvm_vga_status(v, g_ticks));
        }
        result |= (uint32_t)byte << (8 * i);
    }
    if (!write) *value = result;
    return 0;
}

int dosvm_vga_bios(struct dosvm *v, struct v86_frame *f)
{
    /* VBE 3.0 status: AL=4Fh function recognised, AH=01h call failed. Guest
     * VBE is unavailable through F4 (dos-dpmi-contract.md). */
    if ((f->tf.eax & 0xFF00) == 0x4F00) {
        f->tf.eax = (f->tf.eax & 0xFFFF0000u) | 0x014F;
        return 0;
    }
    unsigned ax = f->tf.eax & 0xFFFF, bytes = 0, offset = 0;
    bool output = false;
    if (ax == 0x1012 || ax == 0x1017) {           /* DAC block ES:DX, CX entries */
        bytes = (uint16_t)f->tf.ecx * 3u; offset = (uint16_t)f->tf.edx;
        output = ax == 0x1017;
    } else if (ax == 0x1002 || ax == 0x1009) {    /* 17 palette bytes at ES:DX */
        bytes = 17; offset = (uint16_t)f->tf.edx; output = ax == 0x1009;
    } else if ((ax & 0xFFEF) == 0x1100) {         /* user font ES:BP, CX x BH */
        bytes = (uint16_t)f->tf.ecx * ((f->tf.ebx >> 8) & 255); offset = (uint16_t)f->tf.ebp;
    } else if ((ax >> 8) == 0x13) {               /* string ES:BP, CX cells */
        bytes = (uint16_t)f->tf.ecx * ((ax & 2) ? 2u : 1u); offset = (uint16_t)f->tf.ebp;
    }
    /* The complete guest buffer is checked before any register or VRAM change. */
    if (bytes && (bytes > 65536 || offset > 65536 - bytes || f->es > 0xFFFF ||
        !dosvm_range(v, (f->es << 4) + offset, bytes, output))) return -EFAULT;
    cvbios_regs r = {f->tf.eax, f->tf.ebx, f->tf.ecx, f->tf.edx,
                     f->tf.esi, f->tf.edi, f->tf.ebp, (uint16_t)f->es, (uint16_t)f->ds};
    v->video_bus_fault = false;
    cvbios_int10(&v->video_bios, &r);
    if (v->video_bus_fault) return -EFAULT;
    f->tf.eax = r.eax; f->tf.ebx = r.ebx; f->tf.ecx = r.ecx; f->tf.edx = r.edx;
    f->tf.esi = r.esi; f->tf.edi = r.edi; f->tf.ebp = r.ebp; f->es = r.es; f->ds = r.ds;
    if ((ax >> 8) == 0 && (ax & 0x7F) <= 3) {
        /* The ROM font has eight-dot glyphs: select 8-dot character clocks
         * (SR01 bit 0), giving 640-pixel text rows without a ninth column. */
        cvga_write_port(&v->vga, 0x3C4, 1);
        cvga_write_port(&v->vga, 0x3C5, v->vga.seq[1] | 1);
    }
    uint16_t pos;
    if (!dosvm_read(v, 0x450, &pos, 2)) v->cursor = (pos >> 8) * 80 + (pos & 255);
    return 0;
}

/* ---- Synthetic ROM fonts ----
 * Deterministic conversion of the kernel's Inconsolata 8x16 console bitmap
 * (OFL-1.1, assets/fonts/LICENSES.TXT) for printable ASCII. CP437 accented
 * letters use their unaccented base letter; box-drawing, shade and block
 * characters are drawn geometrically below. Other codes show '?'. 8x14 and
 * 8x8 tables sample the 16 source rows. No firmware font is copied. */
static const char cp437_base[] = /* 80h-A5h */
    "CueaaaaceeeiiiAAE??ooouuyOU?????aiounN";
/* CP437 B3h-DAh: up<<6 | down<<4 | left<<2 | right; 1 single, 2 double. */
static const uint8_t box_lines[] = {
    0x50, 0x54, 0x58, 0xA4, 0x24, 0x18, 0xA8, 0xA0, 0x28, 0x88, 0x84, 0x48, 0x14, /* B3-BF */
    0x41, 0x45, 0x15, 0x51, 0x05, 0x55, 0x52, 0xA1, 0x82, 0x22, 0x8A, 0x2A, 0xA2, /* C0-CC */
    0x0A, 0xAA, 0x4A, 0x85, 0x1A, 0x25, 0x81, 0x42, 0x12, 0x21, 0xA5, 0x5A, 0x44, /* CD-D9 */
    0x11,                                                                         /* DA */
};
static uint8_t box_row(unsigned lines, unsigned y, unsigned height)
{
    unsigned up = lines >> 6, down = (lines >> 4) & 3, left = (lines >> 2) & 3, right = lines & 3;
    unsigned mid = height / 2 - 1;
    bool hdouble = left == 2 || right == 2, vdouble = up == 2 || down == 2;
    uint8_t bits = 0;
    if (hdouble && vdouble) {
        /* Two tracks per arm: columns 1-2/5-6, rows mid-2/mid. Each track
         * stops at the perpendicular track that closes its corner. */
        unsigned upper = mid - 2, lower = mid;
        if (up && y <= (left ? upper : lower)) bits |= 0x60;
        if (up && y <= (right ? upper : lower)) bits |= 0x06;
        if (down && y >= (left ? lower : upper)) bits |= 0x60;
        if (down && y >= (right ? lower : upper)) bits |= 0x06;
        if (y == upper || y == lower) {
            bool blocked = y == upper ? up : down;
            if (left) bits |= blocked ? 0xE0 : 0xFE;
            if (right) bits |= blocked ? 0x07 : 0x7F;
        }
        return bits;
    }
    /* Vertical strokes reach the cell edges and cover the horizontal band. */
    unsigned top = hdouble ? mid - 2 : mid;
    if ((up && y <= mid) || (down && y >= top) || (up && down))
        bits |= (up == 2 || down == 2) ? 0x66 : 0x18;
    bool row = (hdouble && (y == mid - 2 || y == mid)) || (!hdouble && y == mid);
    if (row && left) bits |= vdouble ? 0xFE : 0xF8;
    if (row && right) bits |= vdouble ? 0x7F : 0x1F;
    return bits;
}
static uint8_t glyph_row(unsigned c, unsigned y, unsigned height)
{
    if (c >= 0xB3 && c <= 0xDA) return box_row(box_lines[c - 0xB3], y, height);
    switch (c) {
    case 0x00: case 0x20: case 0xFF: return 0;
    case 0xB0: return (y & 1) ? 0x22 : 0x88;
    case 0xB1: return (y & 1) ? 0x55 : 0xAA;
    case 0xB2: return (y & 1) ? 0xDD : 0x77;
    case 0xDB: return 0xFF;
    case 0xDC: return y >= height / 2 ? 0xFF : 0;
    case 0xDD: return 0xF0;
    case 0xDE: return 0x0F;
    case 0xDF: return y < height / 2 ? 0xFF : 0;
    case 0xFE: return y >= height * 5 / 16 && y < height * 11 / 16 ? 0x3C : 0;
    case 0xF9: case 0xFA: return y >= height / 2 - 1 && y <= height / 2 ? 0x18 : 0;
    default: break;
    }
    unsigned ascii = c >= 0x80 && c <= 0xA5 ? (unsigned char)cp437_base[c - 0x80] : c;
    unsigned glyph = ascii >= 32 && ascii <= 126 ? ascii - 32 : '?' - 32;
    /* High byte of the little-endian row word: pixels 0-7, MSB leftmost.
     * Shorter cells OR the source rows they cover, keeping one-pixel strokes. */
    unsigned first = y * 16 / height, end = (y + 1) * 16 / height;
    uint8_t bits = 0;
    for (unsigned source = first; source < end || source == first; source++)
        bits |= font_cfn_regular[95 + (glyph * 16 + source) * 2 + 1];
    return bits;
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
            unsigned at = address + c * height + y;
            ((uint8_t *)P2V(v->frames[at / PAGE_SIZE]))[at % PAGE_SIZE] = glyph_row(c, y, height);
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

/* Guest #PF on a supervisor-only A0000-BFFFF page: emulate the faulting
 * instruction through the VGA bus. REP strings resume after the budget, as an
 * interrupted REP does on hardware; unsupported forms fault the VM locally. */
#define VIDEO_STRING_BUDGET 4096u
int dosvm_vga_fault(struct dosvm *v, struct v86_frame *f)
{
    cvx_cpu c = {0};
    c.gpr[0] = f->tf.eax; c.gpr[1] = f->tf.ecx; c.gpr[2] = f->tf.edx; c.gpr[3] = f->tf.ebx;
    c.gpr[4] = f->tf.user_esp; c.gpr[5] = f->tf.ebp; c.gpr[6] = f->tf.esi; c.gpr[7] = f->tf.edi;
    c.eip = f->tf.eip; c.eflags = f->tf.eflags;
    uint32_t segments[] = {f->es, f->tf.cs, f->tf.user_ss, f->ds, f->fs, f->gs};
    for (unsigned i = 0; i < 6; i++) {
        if (segments[i] > 65535) return -EFAULT;
        c.sreg[i] = (uint16_t)segments[i]; c.seg_base[i] = segments[i] << 4;
    }
    cvx_result result;
    v->video_bus_fault = false;
    int e = cvx_execute(&c, &v->video_bus, VIDEO_STRING_BUDGET, &result);
    /* DIV/IDIV by a VRAM operand: the architectural #DE, through the guest's
     * INT 0 vector, at the unchanged faulting instruction. */
    if (e == CVX_DIVIDE_ERROR && !v->video_bus_fault) return v86_reflect(&v->monitor, f, 0);
    if (e != CVX_DONE && e != CVX_PARTIAL) return -EFAULT;
    v->video_faults++;
    f->tf.eax = c.gpr[0]; f->tf.ecx = c.gpr[1]; f->tf.edx = c.gpr[2]; f->tf.ebx = c.gpr[3];
    f->tf.user_esp = c.gpr[4]; f->tf.ebp = c.gpr[5]; f->tf.esi = c.gpr[6]; f->tf.edi = c.gpr[7];
    f->tf.eip = c.eip; f->tf.eflags = c.eflags;
    if (e == CVX_DONE && v->monitor.shadow) return v86_debug_step(&v->monitor, f);
    return 0;
}

/* Row-based form of the retained vga_presenter logic: the same dirty
 * dependency queries and six-bit DAC expansion, written into the
 * noncontiguous surface one 640-pixel row at a time. */
static uint32_t rgb(const cvga_state *g, uint8_t index)
{
    const uint8_t *p = g->dac[index];
    uint32_t r = (p[0] << 2) | (p[0] >> 4), gr = (p[1] << 2) | (p[1] >> 4), b = (p[2] << 2) | (p[2] >> 4);
    return (r << 16) | (gr << 8) | b;
}
int dosvm_vga_present(struct dosvm *v, int (*row)(void *, unsigned, const void *, unsigned),
                      void *arg, uint64_t tick, struct ciuki_rect *damage)
{
    memset(damage, 0, sizeof(*damage));
    if (v->video_presented && tick - v->video_tick < DOSVM_FRAME_TICKS) return 0;
    /* Take and clear the dirty granules atomically with respect to the VM
     * worker: a store after this point stays marked for the next frame. */
    uint32_t flags = irq_save();
    memcpy(v->video_taken, v->vga.dirty, sizeof(v->video_taken));
    memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
    uint32_t display = v->vga.display_changes;
    irq_restore(flags);
    cvga_geometry g = {0};
    bool supported = cvga_get_geometry(&v->vga, &g) && g.width && g.height &&
                     g.width <= DOSVM_VIDEO_WIDTH && g.height <= DOSVM_VIDEO_HEIGHT;
    unsigned scale = 1, width = 0, height = 0, left = 0, top = 0;
    if (supported) {
        scale = DOSVM_VIDEO_WIDTH / g.width;
        if (DOSVM_VIDEO_HEIGHT / g.height < scale) scale = DOSVM_VIDEO_HEIGHT / g.height;
        width = g.width * scale; height = g.height * scale;
        left = (DOSVM_VIDEO_WIDTH - width) / 2; top = (DOSVM_VIDEO_HEIGHT - height) / 2;
    } else memset(&g, 0, sizeof(g)); /* custom scanout: a black frame, VM keeps running */
    unsigned phase = ((tick / DOSVM_BLINK_TICKS) & 1 ? 0 : CVGA_BLINK_VISIBLE) |
                     ((tick / DOSVM_CURSOR_TICKS) & 1 ? 0 : CVGA_CURSOR_VISIBLE);
    bool full = !v->video_presented || v->video_display != display ||
                memcmp(&g, &v->video_geometry, sizeof(g));
    if (g.text) for (unsigned i = 0; i < CVGA_DIRTY_BYTES && !full; i++) full = v->video_taken[2][i] != 0;
    unsigned first = DOSVM_VIDEO_HEIGHT, last = 0, rendered = UINT32_MAX;
    bool restart = false;
    for (unsigned y = 0; y < DOSVM_VIDEO_HEIGHT; y++) {
        bool inside = supported && y >= top && y < top + height;
        unsigned source = inside ? (y - top) / scale : 0;
        bool dirty = full || (inside && (cvga_row_reads_dirty(&v->vga, source, v->video_taken) ||
                       ((phase ^ v->video_phase) && cvga_row_phase_sensitive(&v->vga, source))));
        if (!dirty) continue;
        if (!inside) memset(v->video_pixels, 0, sizeof(v->video_pixels));
        else if (source != rendered) {
            memset(v->video_pixels, 0, sizeof(v->video_pixels));
            if (cvga_render_row8(&v->vga, source, v->video_row, sizeof(v->video_row), phase) != g.width) {
                /* The guest changed scanout during this frame: redraw it whole. */
                restart = true;
                break;
            }
            for (unsigned x = 0; x < width; x++)
                v->video_pixels[left + x] = g.blank ? 0 : rgb(&v->vga, v->video_row[x / scale]);
            rendered = source;
        }
        int e = row(arg, y, v->video_pixels, sizeof(v->video_pixels));
        if (e) { v->video_presented = false; return e; } /* retry whole after a partial write */
        if (y < first) first = y;
        last = y + 1;
    }
    v->video_tick = tick; v->video_phase = phase; v->video_display = display;
    v->video_geometry = g;
    v->video_presented = !restart;
    if (!last) return 0;
    v->video_frames++;
    *damage = (struct ciuki_rect){0, (int32_t)first, 0, (int32_t)first, DOSVM_VIDEO_WIDTH, last - first};
    return 1;
}

/* Probe/host digest sink: the complete frame, every row in order. */
struct frame_sink { struct sha256_ctx sha; unsigned next; };
static int hash_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    struct frame_sink *sink = arg;
    if (y != sink->next || bytes != DOSVM_VIDEO_WIDTH * 4) return -EINVAL;
    sha256_update(&sink->sha, pixels, bytes);
    sink->next++;
    return 0;
}
int dosvm_frame_digest(struct dosvm *v, uint64_t tick, struct dosvm_frame *out)
{
    struct frame_sink sink = {.next = 0};
    struct ciuki_rect damage;
    sha256_init(&sink.sha);
    v->video_presented = false;                   /* every row, borders included */
    int e = dosvm_vga_present(v, hash_row, &sink, tick, &damage);
    v->video_presented = false;                   /* a window redraws its surface whole */
    if (e < 0) return e;
    if (sink.next != DOSVM_VIDEO_HEIGHT) return -EAGAIN; /* scanout changed mid-frame */
    sha256_final(&sink.sha, out->digest);
    const cvga_geometry *g = &v->video_geometry;
    out->width = g->width; out->height = g->height; out->text = g->text;
    out->scale = g->width && g->height ? (DOSVM_VIDEO_WIDTH / g->width < DOSVM_VIDEO_HEIGHT / g->height ?
                 DOSVM_VIDEO_WIDTH / g->width : DOSVM_VIDEO_HEIGHT / g->height) : 0;
    uint8_t mode = 0;
    dosvm_read(v, 0x449, &mode, 1);
    out->mode = mode;
    return 0;
}
