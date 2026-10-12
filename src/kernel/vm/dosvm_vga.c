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

/* Input status 1 (3BAh/3DAh). Bit 0 is the inverted display enable (set in
 * horizontal and vertical blanking), bit 3 the vertical sync (FreeVGA, Input
 * Status #1). Each frame of the programmed timing ends with the vertical
 * non-display region: display disabled before the sync, the sync, display
 * disabled after it. id's VL_SetScreen (Wolf3D ID_VL_A.ASM) leaves its loop
 * only on five successive reads with bit 0 set and bit 3 clear (the bottom
 * of the screen) when its TimeCount escape cannot fire, as in SignonScreen
 * before SD_Startup; the former model (sync in the last 3 of every 33 host
 * ticks, bit 0 alternating otherwise) never gave two such reads in a row and
 * Wolf3D spun at VW_SetScreen(0x8000,0) (wolf3d-qemu-t23, 2026-10-12).
 * The frame schedule follows the host tick (1 kHz, independent of the
 * guest's IF): one frame per programmed vertical total (CR06, CR07 bits 0
 * and 5, plus 2) at the 31.469 kHz line rate of every standard VGA mode,
 * i.e. 70.09 Hz for 449-line modes (text, 13h, Mode Y) and 59.94 Hz for
 * 525-line modes (12h, Mode X). The model's 8-dot text clocks are a
 * presenter choice (dosvm_vga_bios), so the dot clock is not used. Each
 * region of the vertical blank lasts a minimum number of reads, so any
 * poller sees it whatever the trap cost. In the active period bit 0
 * alternates on successive reads, standing in for horizontal blanking. */
#define VGA_BLANK_TOP 16u
#define VGA_BLANK_SYNC 4u
#define VGA_BLANK_BOTTOM 16u
#define VGA_LINE_HZ 31469u
static uint64_t vga_frame_us(const cvga_state *g)
{
    uint64_t lines = (uint64_t)g->crtc[6] + ((g->crtc[7] & 1u) << 8) + ((g->crtc[7] & 0x20u) << 4) + 2;
    if (lines < 262) lines = 449;                 /* unprogrammed or nonsensical: 70 Hz */
    return lines * 1000000u / VGA_LINE_HZ;
}
uint8_t dosvm_vga_status(struct dosvm *v, uint64_t tick)
{
    uint64_t now = tick * 1000u;
    if (v->video_blank || now >= v->video_vblank_us) {
        unsigned step = v->video_blank++;
        if (step < VGA_BLANK_TOP) return 1;
        if (step < VGA_BLANK_TOP + VGA_BLANK_SYNC) return 9;
        if (step < VGA_BLANK_TOP + VGA_BLANK_SYNC + VGA_BLANK_BOTTOM) return 1;
        /* Next frame's blank: one period after this one was due, or the
         * next due time after now when polling stopped for longer. */
        v->video_blank = 0;
        uint64_t period = vga_frame_us(&v->vga), next = v->video_vblank_us + period;
        v->video_vblank_us = next > now ? next : now + period - (now - next) % period;
    }
    return (v->video_hsync++ & 1) ? 1 : 0;
}

static int video_port(struct dosvm *v, uint16_t port, unsigned width,
                      bool write, uint32_t *value, void *arg)
{
    (void)arg;
    uint32_t result = 0;
    /* Indexed OUTW/OUTD address consecutive ports, low byte first. Bytes past
     * the registered 3B0h-3DFh range belong to no device: all ones/ignored. */
    bool window = false;
    for (unsigned i = 0; i < width; i++) {
        unsigned p = port + i;
        uint8_t byte = 0xFF;
        if (p <= 0x3DF) {
            if (write) {
                /* f3-06: a CRTC start-address-high store is a page flip
                 * (DOOM's I_FinishUpdate, Wolf3D's VW_UpdateScreen). */
                if ((p == 0x3D5 || p == 0x3B5) && v->vga.crtc_index == 0x0C) v->video_starts++;
                window |= p >= 0x3C2 && p <= 0x3CF;
                cvga_write_port(&v->vga, (uint16_t)p, (uint8_t)(*value >> (8 * i)));
            }
            /* Only input status 1 reads advance the status sequence (not
             * DAC, CRTC or other reads in the same range). */
            else byte = cvga_read_port(&v->vga, (uint16_t)p,
                                       p == 0x3DA || p == 0x3BA ? dosvm_vga_status(v, g_ticks) : 0);
        }
        result |= (uint32_t)byte << (8 * i);
    }
    if (!write) *value = result;
    /* Misc, sequencer and graphics-controller stores decide whether CPU
     * writes are a plain store into one plane (cvga_direct_plane). */
    if (window) dosvm_vga_direct(v);
    return 0;
}

/* ---- f3-06 direct plane window ----
 * A protected-mode game (DOOM's I_UpdateBox: map mask 1/2/4/8, write mode
 * 0, 16-bit stores; Wolf3D's wall posts: one map-mask write per column)
 * stores each Mode X/Y plane tens of thousands of times per frame; one #PF
 * per store is far too slow. When cvga_direct_plane() reports that a CPU
 * write stores its byte unchanged into exactly one plane, the window pages
 * A0000h-AFFFFh map that plane of the VM's own model (cvga_state.plane,
 * page-aligned in struct dosvm) user-writable: stores land in the model's
 * memory, so the presenter, INT 10h and the emulator need no copy. A plane
 * switch only rewrites the 16 window PTEs. The CPU sets the dirty bit of
 * each stored page (Intel SDM Vol. 3A, 4.8); it is harvested into the
 * presenter's granules when the window moves and before each frame. Reads
 * through an open window return the mapped plane without loading the
 * latches: exact while read map select equals the mapped plane; a latch
 * copy (write mode 1), set/reset, logical operation, bit mask, read mode or
 * memory-map change closes the window first (all are port writes). */
#define DOSVM_PTE_DIRTY 0x040u
#define VIDEO_WINDOW_PAGE 0xA0u
#define VIDEO_WINDOW_PAGES 16u
_Static_assert(CVGA_PLANE_SIZE == VIDEO_WINDOW_PAGES * PAGE_SIZE, "one plane per window");
static uint32_t plane_frame(struct dosvm *v, unsigned plane, unsigned page)
{
    return V2P(&v->vga.plane[plane][page * PAGE_SIZE]);
}
/* Dirty pages of the mapped plane -> presenter granules; clears D. */
static void video_harvest(struct dosvm *v)
{
    unsigned plane = v->video_direct - 1;
    for (unsigned i = 0; i < VIDEO_WINDOW_PAGES; i++) {
        uint32_t va = (VIDEO_WINDOW_PAGE + i) * PAGE_SIZE, pte = 0;
        if (!as_lookup(&v->as, va, &pte) || !(pte & DOSVM_PTE_DIRTY)) continue;
        as_map(&v->as, va, plane_frame(v, plane, i), PTE_U | PTE_W);
        memset(&v->vga.dirty[plane][i * (PAGE_SIZE / CVGA_DIRTY_GRANULE / 8)], 0xFF,
               PAGE_SIZE / CVGA_DIRTY_GRANULE / 8);
        v->video_flushes++;
        v->vga.changes++;
    }
}
static void video_map(struct dosvm *v, unsigned want)
{
    for (unsigned i = 0; i < VIDEO_WINDOW_PAGES; i++) {
        unsigned page = VIDEO_WINDOW_PAGE + i;
        if (want) as_map(&v->as, page * PAGE_SIZE, plane_frame(v, want - 1, i), PTE_U | PTE_W);
        else as_map(&v->as, page * PAGE_SIZE, v->frames[page], dosvm_low_flags(page));
    }
    v->video_direct = want;
}
void dosvm_vga_sync(struct dosvm *v, bool unmap)
{
    if (!v->video_direct || !v->as.pd_phys) return;
    uint32_t flags = irq_save();
    if (v->video_direct) {
        video_harvest(v);
        if (unmap) video_map(v, 0);
    }
    irq_restore(flags);
}
void dosvm_vga_direct(struct dosvm *v)
{
    if (!v->as.pd_phys) return;
    int plane = cvga_direct_plane(&v->vga);
    unsigned want = plane < 0 ? 0 : (unsigned)plane + 1;
    if (want == v->video_direct) return;
    uint32_t flags = irq_save();
    if (v->video_direct) video_harvest(v);
    video_map(v, want);
    if (want) v->video_maps++;
    irq_restore(flags);
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
    dosvm_vga_direct(v);                          /* f3-06: the mode set may close the window */
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
 * interrupted REP does on hardware; unsupported forms fault the VM locally.
 * V86 frames use segment:offset bases; a DPMI client frame (f3-06) uses its
 * descriptors' bases and the code segment's D bit, looked up on first use. */
#define VIDEO_STRING_BUDGET 4096u
/* f3-06 (wolf3d-qemu-t23, 2026-10-12): a Wolf3D wall strip wider than one
 * pixel sets a multi-plane map mask (WL_SCALE.C mapmasks1-3: 3, 7, 15, 6,
 * 14, 12), which no single-plane window can carry, and its compiled scaler
 * (WL_SCALE.C BuildCompScale) is a straight run of "mov al,[si+d8]" and
 * "mov es:[di+d16],al" ending in RETF: one #PF per stored byte, about 1.1
 * million in the 7 s play stage. After a V86 aperture fault the monitor
 * therefore keeps emulating the following instructions with the same
 * decoder while they keep touching the aperture (at most VIDEO_BURST_IDLE
 * in a row without), up to VIDEO_BURST instructions. It stops before any
 * instruction it does not decode (register forms, I/O, control transfers),
 * any store outside the aperture (those stay native), a guest or monitor
 * single-step (TF), an open STI/MOV SS shadow and a deliverable interrupt,
 * so the guest observes exactly the sequential result, only sooner. */
#define VIDEO_BURST 1024u
#define VIDEO_BURST_IDLE 4u
struct video_burst_bus { struct dosvm *v; bool vram; };
static int burst_read(void *arg, uint32_t address, uint8_t *value)
{
    struct video_burst_bus *b = arg;
    if (vram(address)) b->vram = true;
    return video_read(b->v, address, value);
}
static int burst_write(void *arg, uint32_t address, uint8_t value)
{
    struct video_burst_bus *b = arg;
    if (!vram(address)) return -EFAULT;             /* refused before any byte moves */
    b->vram = true;
    return video_write(b->v, address, value);
}
static int burst_fetch(void *arg, uint32_t address, uint8_t *value)
{
    struct video_burst_bus *b = arg;
    return video_fetch(b->v, address, value);
}
static void video_burst(struct dosvm *v, struct v86_frame *f, const cvx_cpu *first)
{
    struct video_burst_bus ctx = {v, false};
    const cvx_bus bus = {burst_read, burst_write, burst_fetch, &ctx, 0};
    cvx_cpu c = *first;
    c.gpr[0] = f->tf.eax; c.gpr[1] = f->tf.ecx; c.gpr[2] = f->tf.edx; c.gpr[3] = f->tf.ebx;
    c.gpr[4] = f->tf.user_esp; c.gpr[5] = f->tf.ebp; c.gpr[6] = f->tf.esi; c.gpr[7] = f->tf.edi;
    c.eip = f->tf.eip; c.eflags = f->tf.eflags;
    unsigned idle = 0;
    for (unsigned n = 0; n < VIDEO_BURST && idle < VIDEO_BURST_IDLE; n++) {
        if ((f->tf.eflags & V86_TF) || v->monitor.shadow || v->info.state >= DOSVM_STOPPING ||
            (v->monitor.vif && v86_irq_pending(&v->monitor) >= 0)) break;
        cvx_cpu next = c;
        cvx_result result;
        ctx.vram = false;
        v->video_bus_fault = false;
        int e = cvx_execute(&next, &bus, VIDEO_STRING_BUDGET, &result);
        if ((e != CVX_DONE && e != CVX_PARTIAL) || v->video_bus_fault) {
            v->video_bus_fault = false;              /* nothing moved: the CPU repeats it natively */
            break;
        }
        /* PARTIAL: a REP string moved its budget; its count/index registers
         * are committed and EIP stays on it, as after an interrupted REP. */
        c = next;
        idle = ctx.vram ? 0 : idle + 1;
        v->video_burst++;
        f->tf.eax = c.gpr[0]; f->tf.ecx = c.gpr[1]; f->tf.edx = c.gpr[2]; f->tf.ebx = c.gpr[3];
        f->tf.user_esp = c.gpr[4]; f->tf.ebp = c.gpr[5]; f->tf.esi = c.gpr[6]; f->tf.edi = c.gpr[7];
        f->tf.eip = c.eip; f->tf.eflags = c.eflags;
        if (e == CVX_PARTIAL) break;
    }
}
static int video_emulate(struct dosvm *v, struct v86_frame *f, cvx_cpu *c, const cvx_bus *bus, bool pm)
{
    c->gpr[0] = f->tf.eax; c->gpr[1] = f->tf.ecx; c->gpr[2] = f->tf.edx; c->gpr[3] = f->tf.ebx;
    c->gpr[4] = f->tf.user_esp; c->gpr[5] = f->tf.ebp; c->gpr[6] = f->tf.esi; c->gpr[7] = f->tf.edi;
    c->eip = f->tf.eip; c->eflags = f->tf.eflags;
    cvx_result result;
    v->video_bus_fault = false;
    int e = cvx_execute(c, bus, VIDEO_STRING_BUDGET, &result);
    /* DIV/IDIV by a VRAM operand: the architectural #DE, through the guest's
     * INT 0 vector (V86) or the client's exception 0 (DPMI), at the
     * unchanged faulting instruction. */
    if (e == CVX_DIVIDE_ERROR && !v->video_bus_fault)
        return pm ? dpmi_exception(v, f, 0, 0) : v86_reflect(&v->monitor, f, 0);
    if (e != CVX_DONE && e != CVX_PARTIAL) return -EFAULT;
    if (v->video_bus_fault) return -EFAULT;
    v->video_faults++;
    if (pm) v->video_pm_faults++;
    f->tf.eax = c->gpr[0]; f->tf.ecx = c->gpr[1]; f->tf.edx = c->gpr[2]; f->tf.ebx = c->gpr[3];
    f->tf.user_esp = c->gpr[4]; f->tf.ebp = c->gpr[5]; f->tf.esi = c->gpr[6]; f->tf.edi = c->gpr[7];
    f->tf.eip = c->eip; f->tf.eflags = c->eflags;
    if (!pm && e == CVX_DONE && v->monitor.shadow) return v86_debug_step(&v->monitor, f);
    return 0;
}
int dosvm_vga_fault(struct dosvm *v, struct v86_frame *f)
{
    cvx_cpu c = {0};
    uint32_t segments[] = {f->es, f->tf.cs, f->tf.user_ss, f->ds, f->fs, f->gs};
    for (unsigned i = 0; i < 6; i++) {
        if (segments[i] > 65535) return -EFAULT;
        c.sreg[i] = (uint16_t)segments[i]; c.seg_base[i] = segments[i] << 4;
    }
    return video_emulate(v, f, &c, &v->video_bus, false);
}
/* The trap path's continuation of a V86 aperture fault (dosvm_trap); the
 * instruction-level entry above stays one instruction. */
void dosvm_vga_burst(struct dosvm *v, struct v86_frame *f)
{
    if (v->frame_pm) return;
    cvx_cpu c = {0};
    uint32_t segments[] = {f->es, f->tf.cs, f->tf.user_ss, f->ds, f->fs, f->gs};
    for (unsigned i = 0; i < 6; i++) {
        if (segments[i] > 65535) return;
        c.sreg[i] = (uint16_t)segments[i]; c.seg_base[i] = segments[i] << 4;
    }
    video_burst(v, f, &c);
}
/* Selector base for the decoder. A selector the client could not have
 * loaded (null, absent) makes the access a bus fault: the VM stops with
 * the diagnostic instead of the emulator using base 0. */
static uint32_t pm_segment_base(void *arg, uint16_t selector)
{
    struct dosvm *v = arg;
    struct dpmi_seg s;
    if ((selector & ~3u) && dpmi_seg(v, selector, &s) && s.present) return s.base;
    v->video_bus_fault = true;
    return 0;
}
int dosvm_vga_fault_pm(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi_seg cs;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || !cs.code || !cs.present) return -EFAULT;
    cvx_cpu c = {0};
    uint32_t selectors[] = {f->tf.es, f->tf.cs, f->tf.user_ss, f->tf.ds, f->tf.fs, f->tf.gs};
    for (unsigned i = 0; i < 6; i++) c.sreg[i] = (uint16_t)selectors[i];
    c.code32 = cs.big;
    c.seg_base[CVX_CS] = cs.base;
    c.base_valid = 1u << CVX_CS;
    cvx_bus bus = v->video_bus;
    bus.segment_base = pm_segment_base;
    return video_emulate(v, f, &c, &bus, true);
}

bool dosvm_video_info(struct dosvm_key key, struct dosvm_video_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !v->info.key.generation || v->info.state == DOSVM_DEAD || !v->as.pd_phys) return false;
    memset(out, 0, sizeof(*out));
    uint8_t mode = 0;
    dosvm_read(v, 0x449, &mode, 1);
    out->mode = mode;
    out->unchained = (v->vga.seq[4] & 0x0C) == 0x04;
    cvga_geometry g;
    if (cvga_get_geometry(&v->vga, &g)) { out->width = g.width; out->height = g.height; }
    out->faults = v->video_faults; out->pm_faults = v->video_pm_faults;
    out->starts = v->video_starts; out->maps = v->video_maps; out->flushes = v->video_flushes;
    out->frames = v->video_frames; out->burst = v->video_burst;
    return true;
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
    if (v->video_direct) video_harvest(v);        /* f3-06: pages stored through the window */
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
