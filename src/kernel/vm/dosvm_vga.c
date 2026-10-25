/* Per-VM VGA bus, synthetic INT 10h, guarded A0000-BFFFF faults and the
 * window presenter. Windowed operation never uses physical VGA or firmware;
 * the full-screen adapter synchronizes hardware around synthetic INT 10h.
 * Sources/decisions: docs/design/f3-acceptance.md#f3-02.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include "dosvm_internal.h"
#include "vga_presenter.h"

#define VIDEO_LINEAR 5u
static void video_direct(struct dosvm *, bool port_timed);
static void video_collect(struct dosvm *, bool planes);
extern const uint8_t font_cfn_regular[]; /* 95 widths, then 95 x 16 u16 rows */

static bool vram(uint32_t address) { return address >= 0xA0000 && address < 0xC0000; }

/* Guest-originated cycles: aperture reads load the VGA latches, exactly as a
 * CPU read does on hardware. Kernel-side dosvm_read() peeks instead. */
static int video_read(void *arg, uint32_t address, uint8_t *value)
{
    struct dosvm *v = arg;
    if (vram(address)) {
        dosvm_vga_model(v);
        *value = cvga_read_vram(&v->vga, address); return 0;
    }
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
        const struct dos_video_mode *m=dos_video_mode(v->vbe_mode);
        uint64_t period = m ? dos_video_frame_us(m) : vga_frame_us(&v->vga);
        uint64_t next = v->video_vblank_us + period;
        v->video_vblank_us = next > now ? next : now + period - (now - next) % period;
    }
    return (v->video_hsync++ & 1) ? 1 : 0;
}

static unsigned video_register(const cvga_state *v, unsigned p)
{
    if (p == 0x3C4) return DOSVM_VGA_SR_INDEX;
    if (p == 0x3C5) return v->seq_index == 2 ? DOSVM_VGA_SR_MASK : DOSVM_VGA_SR_OTHER;
    if (p == 0x3CE) return DOSVM_VGA_GC_INDEX;
    if (p == 0x3CF) return v->gc_index == 4 ? DOSVM_VGA_GC_READ :
                           v->gc_index == 5 ? DOSVM_VGA_GC_MODE : DOSVM_VGA_GC_OTHER;
    if (p == 0x3D5 || p == 0x3B5) return DOSVM_VGA_CRTC;
    if (p >= 0x3C6 && p <= 0x3C9) return DOSVM_VGA_DAC;
    return DOSVM_VGA_OTHER;
}
int dosvm_vga_port(struct dosvm *v, uint16_t port, unsigned width,
                      bool write, uint32_t *value, void *arg)
{
    (void)arg;
    uint32_t result = 0;
    if (v->fullscreen && !v->vbe_mode) return dosvm_fullscreen_port(port,width,write,value);
    /* Indexed OUTW/OUTD address consecutive ports, low byte first. Bytes past
     * the registered 3B0h-3DFh range belong to no device: all ones/ignored. */
    bool window = false;
    for (unsigned i = 0; i < width; i++) {
        unsigned p = port + i;
        uint8_t byte = 0xFF;
        if (p <= 0x3DF) {
            if (write) {
                unsigned reg = video_register(&v->vga, p);
                if (p == 0x3C8 || p == 0x3C9) v->video_dac_burst = true; /* fx-27 */
                uint64_t begin = dosvm_perf_task_clock();
                v->perf.vga_reg = reg; v->perf.port_count[reg]++;
                /* f3-19: the common data-port path. A single-plane change
                 * runs just the register update and one PTE batch. Indexed
                 * OUTW arrives here low byte first through the same loop. */
                bool fast = (p == 0x3C5 && v->vga.seq_index == 2) ||
                    (p == 0x3CF && (v->vga.gc_index == 4 || v->vga.gc_index == 5));
                if (fast && (!v->launch_exec || v->launch_ready)) {
                    uint8_t data = (uint8_t)(*value >> (8 * i)) &
                        (p == 0x3C5 ? 15 : v->vga.gc_index == 4 ? 3 : 0x7B);
                    uint8_t old = p == 0x3C5 ? v->vga.seq[2] : v->vga.gc[v->vga.gc_index];
                    bool was_read = v->video_read_window;
                    if (p == 0x3C5) v->video_read_window = false;
                    else if (v->vga.gc_index == 4 && cvga_direct_read_plane(&v->vga) >= 0)
                        v->video_read_window = true;
                    unsigned remaps = v->perf.port_remaps;
                    if (data != old || was_read != v->video_read_window) {
                        dosvm_vga_model(v); /* linear backing uses the OLD memory state */
                        cvga_write_port(&v->vga, (uint16_t)p, data);
                        video_direct(v, true);
                    }
                    uint64_t elapsed = dosvm_perf_task_clock() - begin;
                    v->perf.port_cycles[reg] += elapsed;
                    /* Share the enclosing clock pair: remap attribution
                     * includes register update/selection on this path. */
                    if (v->perf.port_remaps != remaps) v->perf.remap_cycles += elapsed;
                    continue;
                }
                /* f3-07: harvest using the OLD memory state. DAC and CRTC
                 * start-address writes remain ordinary trapped port cycles. */
                uint8_t data = (uint8_t)(*value >> (8 * i));
                bool crtc = p == 0x3D5 || p == 0x3B5;
                bool flip = crtc && (v->vga.crtc_index == 0x0C || v->vga.crtc_index == 0x0D);
                unsigned old_start = (unsigned)v->vga.crtc[0x0C] * 256u + v->vga.crtc[0x0D];
                bool mode = crtc && v->vga.crtc_index < ARRAY_SIZE(v->vga.crtc) &&
                    v->vga.crtc_index != 0x0C && v->vga.crtc_index != 0x0D &&
                    data != v->vga.crtc[v->vga.crtc_index];
                bool access = (p == 0x3C2 && data != v->vga.misc) ||
                    (p == 0x3C3 && (data & 1) != v->vga.enable) ||
                    (p == 0x3C5 && v->vga.seq_index < ARRAY_SIZE(v->vga.seq) && data != v->vga.seq[v->vga.seq_index]) ||
                    (p == 0x3CF && v->vga.gc_index < ARRAY_SIZE(v->vga.gc) && data != v->vga.gc[v->vga.gc_index]);
                if (mode || access) dosvm_vga_model(v);
                /* Completion must publish the last plane's D pages even
                 * before the native presenter next polls. Keep the window
                 * writable, including a redundant start-address write. */
                if (flip && v->video_direct == VIDEO_LINEAR && v->as.pd_phys) video_collect(v, false);
                /* Mode X/Y flips select the source without collecting D. */
                /* f3-06: a CRTC start-address-high store is a page flip
                 * (DOOM's I_FinishUpdate, Wolf3D's VW_UpdateScreen). */
                if ((p == 0x3D5 || p == 0x3B5) && v->vga.crtc_index == 0x0C) v->video_starts++;
                window |= access || mode;
                cvga_write_port(&v->vga, (uint16_t)p, (uint8_t)(*value >> (8 * i)));
                if (flip && old_start != (unsigned)v->vga.crtc[0x0C] * 256u + v->vga.crtc[0x0D]) {
                    uint64_t now = g_ticks * 1000u, period = vga_frame_us(&v->vga);
                    /* Low/high writes within half a refresh form one change.
                     * Two refresh-spaced transitions establish fast flipping;
                     * repeated writes of the same address never do. */
                    if (!v->video_flip_seen || now - v->video_flip_us >= period / 2) {
                        uint64_t gap = now - v->video_flip_us;
                        v->video_flip_run = v->video_flip_seen && gap >= period / 2 && gap <= period * 3 / 2 ?
                            v->video_flip_run + (v->video_flip_run < 2) : 0;
                        v->video_flip_us = now; v->video_flip_seen = true;
                    }
                }
                v->perf.port_cycles[reg] += dosvm_perf_task_clock() - begin;
            }
            /* Only input status 1 reads advance the status sequence (not
             * DAC, CRTC or other reads in the same range). */
            else {
                v->perf.vga_reg = video_register(&v->vga, p);
                byte = cvga_read_port(&v->vga, (uint16_t)p,
                                      p == 0x3DA || p == 0x3BA ? dosvm_vga_status(v, g_ticks) : 0);
            }
        }
        result |= (uint32_t)byte << (8 * i);
    }
    if (!write) *value = result;
    /* Misc, sequencer and graphics-controller stores decide whether CPU
     * writes are a plain store into one plane (cvga_direct_plane). */
    if (window) dosvm_vga_direct(v);
    if (write && v->launch_exec && !v->launch_ready) {
        cvga_geometry geometry;
        if (cvga_get_geometry(&v->vga, &geometry) && !geometry.text) dosvm_launch_video(v);
    }
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
 * pending page bitmap when the window moves and into presenter granules
 * once per frame. Reads
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
    return V2P(&v->vga.plane[plane][page * PAGE_SIZE]) & ~0xFFFu;
}
static uint32_t video_frame(struct dosvm *v, unsigned want, unsigned page)
{
    return want == VIDEO_LINEAR ? v->frames[VIDEO_WINDOW_PAGE + page] : plane_frame(v, want - 1, page);
}
/* Reuse the VM's own aperture pages, which normally stay supervisor-only.
 * The model uses physical offsets: chain-4 clears A0/A1 without shifting. */
/* F0-44 indivisible transaction envelope: the aperture is 16 * 4096
 * bytes regardless of VM quota. Scalar chain-4 conversion is charged at
 * 4x ordinary RAM copy (32 us/page), <=512 us per seed/harvest. A combined
 * harvest + seed + 64 mask updates + 32 PTE checks/updates is <=1280 us.
 * This is a host cost model, not physical qualification. Never yield while
 * plane state, dirty bits and window PTEs describe different generations. */
_Static_assert(VIDEO_WINDOW_PAGES == 16 && PAGE_SIZE == 4096, "VGA transaction bound");
static void video_linear_seed(struct dosvm *v)
{
    for (unsigned page = 0; page < VIDEO_WINDOW_PAGES; page++) {
        uint8_t *bytes = P2V(v->frames[VIDEO_WINDOW_PAGE + page]);
        for (unsigned i = 0; i < PAGE_SIZE; i++)
            bytes[i] = v->vga.plane[i & 3][(page * PAGE_SIZE + i) & ~3u];
    }
}
/* Dirty window pages -> model/presenter granules; clears D. Plane mappings
 * still store directly into the model; linear mappings deinterleave first. */
/* A plane switch records only page bits; it never copies or walks damage
 * granules. The next presenter/sync merges these with the currently open
 * plane. Stores already landed in the exact plane shadow. */
static uint32_t video_exchange(struct dosvm *v, unsigned want)
{
    uint32_t frames[VIDEO_WINDOW_PAGES], dirty = 0, accessed = 0;
    for (unsigned i = 0; i < VIDEO_WINDOW_PAGES; i++)
        frames[i] = want ? video_frame(v, want, i) : v->frames[VIDEO_WINDOW_PAGE + i];
    int e = as_replace_pages(&v->as, VIDEO_WINDOW_PAGE * PAGE_SIZE, frames,
                              VIDEO_WINDOW_PAGES, want ? PTE_U | (v->video_writable ? PTE_W : 0) : dosvm_low_flags(VIDEO_WINDOW_PAGE),
                              &dirty, &accessed);
    if (e) panic("dosvm: missing video window table");
    if (v->video_direct && v->video_direct < VIDEO_LINEAR)
        v->video_pending[v->video_direct - 1] |= (uint16_t)dirty;
    if (v->video_direct && !want && (accessed & ~dirty)) v->perf.latch_read_windows++;
    v->video_direct = want;
    return dirty;
}
/* fx-27: chain-4 stores stay in the linear backing until a model consumer
 * needs the planes. A collection only records the D pages: as "stale" (the
 * planes lack them) and as presenter damage. The presenter converts chained
 * rows straight from the linear backing, as DOSBox renders mode 13h from its
 * linear fastmem copy (vga_memory.cpp VGA_ChainedVGA_Handler writeCache).
 * Every consumer of the planes (dosvm_vga_model/sync, a window switch, the
 * presenter's planar path, snapshots) deinterleaves the stale pages first. */
static void video_linear_flush(struct dosvm *v)
{
    uint16_t stale = v->video_linear_stale;
    v->video_linear_stale = 0;
    for (unsigned i = 0; i < VIDEO_WINDOW_PAGES; i++) if (stale & (1u << i)) {
        cvga_store_chain4_span(&v->vga, i * PAGE_SIZE,
                               P2V(v->frames[VIDEO_WINDOW_PAGE + i]), PAGE_SIZE);
        v->perf.direct_pages++; v->video_linear_harvests++;
    }
}
static void video_collect(struct dosvm *v, bool planes)
{
    uint64_t begin = dosvm_perf_task_clock();
    uint32_t dirty = 0;
    if (v->video_direct) {
        if (as_replace_pages(&v->as, VIDEO_WINDOW_PAGE * PAGE_SIZE, 0,
                             VIDEO_WINDOW_PAGES, 0, &dirty, 0)) panic("dosvm: missing video window table");
        if (v->video_direct < VIDEO_LINEAR)
            v->video_pending[v->video_direct - 1] |= (uint16_t)dirty;
        else if (dirty) {
            v->video_linear_stale |= (uint16_t)dirty;
            v->video_linear_damage |= (uint16_t)dirty;
            v->vga.changes++;
        }
    }
    if (planes) video_linear_flush(v);
    bool harvested = dirty != 0;
    for (unsigned plane = 0; plane < 4; plane++) {
        uint32_t pending = v->video_pending[plane];
        harvested |= pending != 0;
        for (unsigned i = 0; i < VIDEO_WINDOW_PAGES; i++) if (pending & (1u << i)) {
            memset(&v->vga.dirty[plane][i * (PAGE_SIZE / CVGA_DIRTY_GRANULE / 8)], 0xFF,
                   PAGE_SIZE / CVGA_DIRTY_GRANULE / 8);
            v->video_flushes++; v->perf.direct_pages++; v->vga.changes++;
        }
        v->video_pending[plane] = 0;
    }
    if (harvested) { v->perf.harvests++; dosvm_launch_video(v); }
    dosvm_present_notify(v);
    v->perf.harvest_cycles += dosvm_perf_task_clock() - begin;
}
/* Every page is published to the planes (the pre-fx-27 harvest). */
static void video_harvest(struct dosvm *v) { video_collect(v, true); }
void dosvm_vga_sync(struct dosvm *v, bool unmap)
{
    if (v->fullscreen) return;
    if (!v->as.pd_phys) return;
    /* VGA state/PTEs are thread-owned on this UP kernel. No scheduling
     * point in harvest/seed/exchange: the guest cannot run, while timer
     * and device IRQs can. In particular the 64 KiB chain-4 conversion
     * must never be enclosed in an IRQ lock. */
    video_harvest(v);
    if (unmap && v->video_direct) video_exchange(v, 0);
}
static void video_direct(struct dosvm *v, bool port_timed)
{
    if (v->fullscreen || v->vbe_mode) return;
    if (!v->as.pd_phys || v->info.state >= DOSVM_STOPPING) return;
    int write_plane = cvga_direct_plane(&v->vga);
    int plane = v->video_read_window ? cvga_direct_read_plane(&v->vga) : write_plane;
    unsigned want = plane < 0 ? (cvga_direct_linear(&v->vga) ? VIDEO_LINEAR : 0) : (unsigned)plane + 1;
    bool writable = want && (!v->video_read_window || plane == write_plane || want == VIDEO_LINEAR);
    if (want == v->video_direct && writable == v->video_writable) return;
    uint64_t begin = port_timed ? 0 : dosvm_perf_task_clock();
    /* Chained backing needs deinterleaving before memory state changes.
     * Single planes need only their D bitmap carried across the swap. */
    if (v->video_direct == VIDEO_LINEAR) video_harvest(v);
    if (want == VIDEO_LINEAR) video_linear_seed(v);
    v->video_writable = writable;
    video_exchange(v, want);
    v->perf.port_remaps++;
    if (want == VIDEO_LINEAR) v->video_linear_maps++;
    else if (want) v->video_maps++;
    if (!port_timed) v->perf.remap_cycles += dosvm_perf_task_clock() - begin;
}
void dosvm_vga_direct(struct dosvm *v) { video_direct(v, false); }
void dosvm_vga_model(struct dosvm *v)
{
    if (v->video_direct == VIDEO_LINEAR) dosvm_vga_sync(v, true);
}
bool dosvm_vga_retry(struct dosvm *v, uint32_t address, uint32_t instruction, bool pm)
{
    if (v->video_direct || address < 0xA0000 || address >= 0xB0000 || vram(instruction) ||
        !cvga_direct_linear(&v->vga) || !v->as.pd_phys || v->info.state >= DOSVM_STOPPING) return false;
    dosvm_vga_direct(v);
    if (v->video_direct != VIDEO_LINEAR) return false;
    v->video_faults++;
    if (pm) v->video_pm_faults++;
    return true;
}

int dosvm_vga_bios(struct dosvm *v, struct v86_frame *f)
{
    /* Only full-screen BIOS operations synchronize the physical device;
     * the f3-19 windowed register/direct-window paths are unchanged. */
    bool hardware_mode=v->fullscreen;
    unsigned function = (f->tf.eax >> 8) & 255;
    if (!function) v->video_read_window = false;
    if (function == 0x0E) dosvm_console_byte(v, (uint8_t)f->tf.eax);
    dosvm_vga_model(v);                          /* before any BIOS mutation, including VBE */
    if ((f->tf.eax & 0xFF00) == 0x4F00) return dosvm_vbe_bios(v,f);
    bool linear_mode=v->vbe_mode!=0;
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
    if (hardware_mode && !linear_mode) dosvm_fullscreen_model_begin(v);
    cvbios_regs r = {f->tf.eax, f->tf.ebx, f->tf.ecx, f->tf.edx,
                     f->tf.esi, f->tf.edi, f->tf.ebp, (uint16_t)f->es, (uint16_t)f->ds};
    v->video_bus_fault = false;
    int handled = cvbios_int10(&v->video_bios, &r);
    if (!function && handled) {
        v->vbe_mode=0; v->video_presented=false;
        if (hardware_mode && linear_mode) dosvm_fullscreen_mode_changed(v);
    }
    if (handled && (!function || function == 0x0E || function == 9 || function == 0x0A || function == 0x13))
        dosvm_launch_video(v);
    dosvm_vga_direct(v);                          /* f3-06: the mode set may close the window */
    if (v->video_bus_fault) {
        if (hardware_mode && !linear_mode) dosvm_fullscreen_model_end(v);
        return -EFAULT;
    }
    f->tf.eax = r.eax; f->tf.ebx = r.ebx; f->tf.ecx = r.ecx; f->tf.edx = r.edx;
    if ((ax >> 8) == 0) { v->video_flip_seen = false; v->video_flip_run = 0; }
    f->tf.esi = r.esi; f->tf.edi = r.edi; f->tf.ebp = r.ebp; f->es = r.es; f->ds = r.ds;
    if ((ax >> 8) == 0 && (ax & 0x7F) <= 3) {
        /* The ROM font has eight-dot glyphs: select 8-dot character clocks
         * (SR01 bit 0), giving 640-pixel text rows without a ninth column. */
        cvga_write_port(&v->vga, 0x3C4, 1);
        cvga_write_port(&v->vga, 0x3C5, v->vga.seq[1] | 1);
    }
    uint16_t pos;
    if (!dosvm_read(v, 0x450, &pos, 2)) v->cursor = (pos >> 8) * 80 + (pos & 255);
    if (hardware_mode && !linear_mode) dosvm_fullscreen_model_end(v);
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

void cvbios_init_point(const cvbios *b)
{
    if (!b->bus || b->bus->write != video_write) return;
    const struct dosvm *v = b->bus->context;
    if (v->info.state == DOSVM_CREATING && !v->started) kpreempt_point();
}
int dosvm_vga_init(struct dosvm *v)
{
    /* The unpublished slot was already cleared in page-sized chunks by
     * dosvm_create. Avoid a second uninterrupted 267 KiB zero here. */
    v->vga.dac_mask = 255;
    cvga_set_bios_mode(&v->vga, 3, 1);
    v->video_bus = (cvx_bus){.read=video_read, .write=video_write, .fetch=video_fetch, .context=v};
    v->video_bios.vga = &v->vga; v->video_bios.bus = &v->video_bus;
    static const unsigned heights[] = {8, 14, 16};
    static const unsigned addresses[] = {0xF3000, 0xF4000, 0xF2000};
    for (unsigned which = 0; which < 3; which++) {
        unsigned height = heights[which], address = addresses[which];
        for (unsigned c = 0; c < 256; c++) {
            for (unsigned y = 0; y < height; y++) {
                unsigned at = address + c * height + y;
                ((uint8_t *)P2V(v->frames[at / PAGE_SIZE]))[at % PAGE_SIZE] = glyph_row(c, y, height);
            }
            if (!(c & 15)) kpreempt_point();
        }
    }
    v->video_bios.font[CVBIOS_FONT_8X16] = 0xF0002000;
    v->video_bios.font[CVBIOS_FONT_8X8] = 0xF0003000;
    v->video_bios.font[CVBIOS_FONT_8X8_HIGH] = 0xF0003400;
    v->video_bios.font[CVBIOS_FONT_8X14] = 0xF0004000;
    dosvm_vbe_init(v);
    struct v86_frame f = {0}; f.tf.eax = 3;
    int e = dosvm_vga_bios(v, &f);
    /* 3BCh..3BEh are the monochrome adapter's printer port, not VGA.
     * Keep them available to the separately registered virtual LPT3. */
    if (!e) e=dosvm_port_register(v->info.key,0x3B0,0x3BB,dosvm_vga_port,0);
    return e ? e : dosvm_port_register(v->info.key,0x3BF,0x3DF,dosvm_vga_port,0);
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
 * therefore keeps emulating following instructions in V86 or PM with the
 * same decoder while they keep touching the aperture/VGA ports (at most
 * VIDEO_BURST_IDLE in a row without), up to VIDEO_BURST instructions.
 * Register ALU/setup and short branches can connect planar runs. It stops
 * before any instruction it does not decode (stack/far transfers, FPU),
 * any store outside the aperture (those stay native), a guest or monitor
 * single-step (TF), an open STI/MOV SS shadow and a deliverable interrupt,
 * so the guest observes exactly the sequential result, only sooner. */
#define VIDEO_BURST 1024u
#define VIDEO_BURST_IDLE 8u /* INC/INC/LOOP plus the next RAM load must reach its VGA store. */
static bool video_run_blocked(const struct dosvm *v, const struct v86_frame *f)
{
    return (f->tf.eflags & V86_TF) || g_need_resched || v->monitor.shadow || v->dpmi.shadow ||
        v->dpmi.dr7 || v->dpmi.if_armed || v->dpmi.return_steps || v->dpmi.if_step ||
        v->dpmi.if_restore_step || v->info.state >= DOSVM_STOPPING ||
        (v->monitor.vif && v86_irq_pending(&v->monitor) >= 0);
}
static int pm_validate(void *arg, uint16_t sel, uint32_t offset, unsigned size, int write)
{
    uint32_t linear;
    return dpmi_ptr(arg, sel, offset, size, write != 0, &linear) ? 0 : -EFAULT;
}
/* A full bounded REP is one exit. Validate both spans before plane changes;
 * RAM may be physically discontiguous, so feed runs from each backing page. */
static int video_string_run(void *arg, uint32_t source, uint32_t dest, unsigned size,
                         uint32_t count, int back, int move, uint32_t value, bool preflight)
{
    struct dosvm *v = arg;
    /* The fast plane copier accepts RAM source pointers only. A VBE owner
     * can still address the virtual VGA aperture; use the ordinary monitor
     * transfer so a borrowed LFB source never becomes a RAM direct-map read. */
    if (v->vbe_direct.pages) return 0;
    unsigned bytes = count * size, tail = (count - 1) * size;
    uint32_t low = back ? dest - tail : dest, src_low = back ? source - tail : source;
    if (low < 0xA0000 || low > 0xB0000 || bytes > 0xB0000 - low ||
        (move && (!dosvm_range(v, src_low, bytes, false) ||
                   (src_low < 0xC0000 && src_low + bytes > 0xA0000 &&
                    (src_low < 0xA0000 || src_low + bytes > 0xB0000))))) return 0;
    unsigned mode = v->vga.gc[5] & 3;
    if ((v->vga.seq[4] & 0x0e) != 6 || !v->vga.enable || !(v->vga.misc & 2) ||
        (v->vga.gc[6] & 0x0e) != 4 || (v->vga.gc[5] & 0x18) || mode > 1 ||
        (!mode && (v->vga.gc[1] || v->vga.gc[3] || v->vga.gc[8] != 255)) ||
        (mode && (!move || !vram(source) || size != 1))) return 0;
    /* f3-46: a RAM source is made resident before any plane changes. A
     * burst preflight never blocks: a source not resident ends the burst,
     * and the CPU repeats the instruction natively (its fault resolves). */
    if (move && !vram(source) &&
        (preflight ? dosvm_pool_resident(v, src_low, bytes) : dosvm_pool_prefault(v, src_low, bytes, false)))
        return 0;
    if (preflight) return 1;
    if (!move || vram(source)) {
        if (!cvga_planar_run(&v->vga, dest - 0xA0000, 0, source - 0xA0000,
                             size, count, back, move, value)) return 0;
    } else {
        unsigned remaining = count;
        while (remaining) {
            unsigned off = source & (PAGE_SIZE - 1);
            unsigned run = back ? off / size + 1 : (PAGE_SIZE - off) / size;
            if (run > remaining) run = remaining;
            uint8_t element[4];
            const uint8_t *p;
            uint32_t phys = 0;
            int frame = off + size > PAGE_SIZE ? -EFAULT : dosvm_pool_frame(v, source, false, false, &phys);
            if (frame == -ENOENT) phys = as_lookup(&v->as, source, 0);
            else if (frame) phys = 0;
            if (!phys) {
                /* Crossing a page, or (never, after the prefault above and
                 * under the transaction hold) a pool page not resident. */
                if (dosvm_read(v, source, element, size)) panic("dosvm: validated REP source");
                p = element; run = 1;
            } else p = P2V(phys);
            if (!cvga_planar_run(&v->vga, dest - 0xA0000, p, 0, size, run, back, true, value))
                panic("dosvm: validated planar REP");
            remaining -= run;
            if (remaining) { unsigned n = run * size; dest += back ? 0u - n : n; source += back ? 0u - n : n; }
        }
    }
    v->perf.trapped_bytes += bytes; dosvm_launch_video(v);
    return 1;
}
static int video_string(void *arg, uint32_t src, uint32_t dst, unsigned size,
                         uint32_t count, int back, int move, uint32_t value)
{ return video_string_run(arg, src, dst, size, count, back, move, value, false); }
static uint32_t pm_segment_base(void *arg, uint16_t selector);
struct video_burst_bus { struct dosvm *v; bool vram, preflight; };
static int burst_read(void *arg, uint32_t address, uint8_t *value)
{
    struct video_burst_bus *b = arg;
    if (b->preflight) {
        if (vram(address)) { *value = cvga_peek_vram(&b->v->vga, address); return 0; }
        /* f3-46: never page in between burst instructions; a pool page that
         * is not resident ends the burst before this instruction commits. */
        return dosvm_read_resident(b->v, address, value, 1);
    }
    if (vram(address)) b->vram = true;
    return video_read(b->v, address, value);
}
static int burst_write(void *arg, uint32_t address, uint8_t value)
{
    struct video_burst_bus *b = arg;
    if (!vram(address)) return -EFAULT;             /* refused before any byte moves */
    if (b->preflight) return 0;
    b->vram = true;
    return video_write(b->v, address, value);
}
static int burst_fetch(void *arg, uint32_t address, uint8_t *value)
{
    struct video_burst_bus *b = arg;
    return video_fetch(b->v, address, value);
}
static uint32_t burst_base(void *arg, uint16_t sel)
{ return pm_segment_base(((struct video_burst_bus *)arg)->v, sel); }
static int burst_validate(void *arg, uint16_t sel, uint32_t off, unsigned size, int write)
{ return pm_validate(((struct video_burst_bus *)arg)->v, sel, off, size, write); }
static int burst_string(void *arg, uint32_t src, uint32_t dst, unsigned size,
                         uint32_t count, int back, int move, uint32_t value)
{
    struct video_burst_bus *b = arg;
    /* The same full-span checks run twice; only the committed call writes
     * planes. Unsupported string modes use the bytewise preflight. */
    int done = video_string_run(b->v, src, dst, size, count, back, move, value, b->preflight);
    if (done && !b->preflight) b->vram = true;
    return done;
}
static int burst_port(void *arg, uint16_t port, unsigned size, int write, uint32_t *value)
{
    struct video_burst_bus *b = arg;
    /* Restrict to the VGA register file. Unknown/other devices stay native.
     * Preflight reads peek a copied register file (DAC/status side effects
     * cannot escape). The status clock is advanced only when committed. */
    if (port < 0x3B0 || port + size - 1 > 0x3DF ||
        (port<=0x3BE && port+size>0x3BC)) return -EFAULT;
    if (b->preflight) {
        if (!write) *value = 0; /* IN has no access-dependent side effects in this instruction. */
        return 0;
    }
    b->vram = true;
    return dosvm_vga_port(b->v, port, size, write != 0, value, 0);
}
static void video_burst(struct dosvm *v, struct v86_frame *f, const cvx_cpu *first)
{
    struct video_burst_bus ctx = {v, false, false};
    const cvx_bus bus = {.read=burst_read, .write=burst_write, .fetch=burst_fetch, .context=&ctx,
        .segment_base=v->frame_pm ? burst_base : 0, .validate=v->frame_pm ? burst_validate : 0,
        .string=burst_string, .port=burst_port};
    cvx_cpu c = *first;
    c.gpr[0] = f->tf.eax; c.gpr[1] = f->tf.ecx; c.gpr[2] = f->tf.edx; c.gpr[3] = f->tf.ebx;
    c.gpr[4] = f->tf.user_esp; c.gpr[5] = f->tf.ebp; c.gpr[6] = f->tf.esi; c.gpr[7] = f->tf.edi;
    c.eip = f->tf.eip; c.eflags = f->tf.eflags;
    unsigned idle = 0;
    for (unsigned n = 0; n < VIDEO_BURST && idle < VIDEO_BURST_IDLE; n++) {
        if (video_run_blocked(v, f)) break;
        cvx_cpu next = c;
        cvx_result result;
        /* Same decoder and bounded REP budget, but reads only peek and
         * writes only validate. Validate every access before any latch or
         * plane changes, including all bytes of an element and both DF
         * directions. Memory callbacks cannot yield; committed VGA ports may
         * swap mappings, just as a scalar port instruction does. */
        ctx.preflight = true;
        int e = cvx_execute(&next, &bus, VIDEO_STRING_BUDGET, &result);
        ctx.preflight = false;
        if (e != CVX_DONE && e != CVX_PARTIAL) break;
        next = c;
        ctx.vram = false;
        v->video_bus_fault = false;
        e = cvx_execute(&next, &bus, VIDEO_STRING_BUDGET, &result);
        if ((e != CVX_DONE && e != CVX_PARTIAL) || v->video_bus_fault) {
            v->video_bus_fault = false;              /* nothing moved: the CPU repeats it natively */
            break;
        }
        /* PARTIAL: a REP string moved its budget; its count/index registers
         * are committed and EIP stays on it, as after an interrupted REP. */
        c = next;
        idle = ctx.vram ? 0 : idle + 1;
        v->video_burst++; v->perf.burst_insn++;
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
    cvx_bus bus = v->video_bus;
    bus.string = video_run_blocked(v, f) ? 0 : video_string;
    return video_emulate(v, f, &c, &bus, false);
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
/* ---- fx-27 DAC palette burst ----
 * A DAC load is 768 OUTs to 3C9h (DOOM I_SetPalette, Quake VGA_SetPalette:
 * movzx/sar/mov edx,3C9h/out/inc/cmp/jng per byte), each a monitor exit:
 * 17 ms per palette change on the T23 (trial f441c5bd vga_io 21.8 us per
 * 3C9h exit). After a trapped DAC write the monitor decodes ahead, on a
 * copy, at most DAC_LOOKAHEAD instructions for the next VGA DAC or status
 * port access. Only instructions with no memory store and no VGA memory
 * read (latch loads) qualify: register setup/ALU, shifts, short branches
 * and loads from resident RAM. If such an access is found, the copy's state
 * is exactly what the CPU would have produced for those instructions, so it
 * is committed and the port instruction is executed through the VGA port
 * callback; otherwise nothing is committed and the CPU continues natively.
 * The burst stops on TF, watchpoints, IF-image/return tracking, STI/MOV SS
 * shadows, a deliverable IRQ, a reschedule, a host tick, any instruction
 * the decoder does not support and after DAC_BURST_INSN instructions, so
 * the guest observes the sequential result, sooner. One exit per palette
 * load; DOSBox (render.cpp RENDER_SetPal/Check_Palette) and QEMU (vga.c
 * update_palette256) likewise apply a whole palette change at the next
 * frame rather than per entry. */
#define DAC_LOOKAHEAD 8u
#define DAC_BURST_INSN 8192u
struct dac_bus {
    struct dosvm *v;
    bool detect, found;
    uint32_t page[2];               /* cached resident linear pages (code, data) */
    const uint8_t *host[2];
    /* Descriptors decoded once per burst: the decoder loads no selector. */
    struct { uint16_t sel; bool valid; struct dpmi_seg seg; } seg[3];
    unsigned next_seg;
};
static bool dac_port_ok(uint16_t port, unsigned size, int write)
{
    if (size > 2) return false;
    if (port >= 0x3C6 && port <= 0x3C9) return true;   /* DAC mask, indexes, data */
    return !write && size == 1 && (port == 0x3DA || port == 0x3BA);
}
static int dac_byte(struct dac_bus *b, unsigned slot, uint32_t address, uint8_t *value)
{
    if (vram(address)) return -EFAULT;  /* a VGA memory read loads the latches */
    uint32_t page = address & ~0xFFFu;
    if (!b->host[slot] || b->page[slot] != page) {
        uint32_t pte = 0, phys = as_lookup(&b->v->as, address, &pte);
        if (!phys || !(pte & PTE_U)) return -EFAULT;     /* never pages in */
        b->page[slot] = page; b->host[slot] = P2V(phys & ~0xFFFu);
    }
    *value = b->host[slot][address & 0xFFFu];
    return 0;
}
static int dac_read(void *arg, uint32_t address, uint8_t *value) { return dac_byte(arg, 1, address, value); }
static int dac_fetch(void *arg, uint32_t address, uint8_t *value) { return dac_byte(arg, 0, address, value); }
static int dac_write(void *arg, uint32_t address, uint8_t value)
{ (void)arg; (void)address; (void)value; return -EFAULT; }
static uint32_t dac_base(void *arg, uint16_t sel) { return pm_segment_base(((struct dac_bus *)arg)->v, sel); }
/* dpmi_ptr's segment checks on a per-burst descriptor cache; the page
 * check is dac_byte's (resident user pages only, stricter than
 * dosvm_range), so no per-byte page-table walk. */
static int dac_validate(void *arg, uint16_t sel, uint32_t off, unsigned size, int write)
{
    struct dac_bus *b = arg;
    const struct dpmi_seg *s = 0;
    for (unsigned i = 0; i < 3 && !s; i++) if (b->seg[i].valid && b->seg[i].sel == sel) s = &b->seg[i].seg;
    if (!s) {
        unsigned i = b->next_seg++ % 3;
        b->seg[i].valid = dpmi_seg(b->v, sel, &b->seg[i].seg);
        if (!b->seg[i].valid) return -EFAULT;
        b->seg[i].sel = sel; s = &b->seg[i].seg;
    }
    if (!size || !s->present || (write ? !s->writable : !s->readable)) return -EFAULT;
    uint32_t last = off + size - 1;
    if (last < off) return -EFAULT;
    if (s->down) {
        if (off <= s->limit || last > (s->big ? 0xFFFFFFFFu : 0xFFFFu)) return -EFAULT;
    } else if (last > s->limit) return -EFAULT;
    return s->base + off + size - 1 < s->base + off ? -EFAULT : 0;
}
static int dac_port(void *arg, uint16_t port, unsigned size, int write, uint32_t *value)
{
    struct dac_bus *b = arg;
    if (!dac_port_ok(port, size, write)) return -EFAULT;
    if (b->detect) { b->found = true; return -EAGAIN; }  /* unsupported: no effect */
    return dosvm_vga_port(b->v, port, size, write != 0, value, 0);
}
int dosvm_vga_dac_burst(struct dosvm *v, struct v86_frame *f)
{
    if (v->fullscreen || video_run_blocked(v, f)) return 0;
    cvx_cpu c = {0};
    struct dac_bus ctx = {.v = v};
    cvx_bus bus = {.read = dac_read, .write = dac_write, .fetch = dac_fetch, .context = &ctx, .port = dac_port};
    if (v->frame_pm) {
        struct dpmi_seg cs;
        if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || !cs.code || !cs.present) return 0;
        uint32_t selectors[] = {f->tf.es, f->tf.cs, f->tf.user_ss, f->tf.ds, f->tf.fs, f->tf.gs};
        for (unsigned i = 0; i < 6; i++) c.sreg[i] = (uint16_t)selectors[i];
        c.code32 = cs.big; c.seg_base[CVX_CS] = cs.base; c.base_valid = 1u << CVX_CS;
        bus.segment_base = dac_base; bus.validate = dac_validate;
    } else {
        uint32_t segments[] = {f->es, f->tf.cs, f->tf.user_ss, f->ds, f->fs, f->gs};
        for (unsigned i = 0; i < 6; i++) {
            if (segments[i] > 65535) return 0;
            c.sreg[i] = (uint16_t)segments[i]; c.seg_base[i] = segments[i] << 4;
        }
    }
    c.gpr[0] = f->tf.eax; c.gpr[1] = f->tf.ecx; c.gpr[2] = f->tf.edx; c.gpr[3] = f->tf.ebx;
    c.gpr[4] = f->tf.user_esp; c.gpr[5] = f->tf.ebp; c.gpr[6] = f->tf.esi; c.gpr[7] = f->tf.edi;
    c.eip = f->tf.eip; c.eflags = f->tf.eflags;
    uint64_t tick = g_ticks;
    unsigned executed = 0, ports = 0;
    while (executed < DAC_BURST_INSN && g_ticks == tick && !video_run_blocked(v, f)) {
        cvx_cpu ahead = c;
        unsigned pure = 0;
        ctx.detect = true; ctx.found = false; v->video_bus_fault = false;
        while (pure < DAC_LOOKAHEAD) {
            cvx_cpu next = ahead;
            cvx_result result;
            if (cvx_execute(&next, &bus, 1, &result) != CVX_DONE || v->video_bus_fault) break;
            ahead = next; pure++;
        }
        if (!ctx.found || v->video_bus_fault) break;
        cvx_cpu next = ahead;
        cvx_result result;
        ctx.detect = false;
        if (cvx_execute(&next, &bus, 1, &result) != CVX_DONE || v->video_bus_fault) {
            v->video_bus_fault = false;
            c = ahead;                  /* the CPU executes the port instruction itself */
            executed += pure;
            break;
        }
        c = next; executed += pure + 1; ports++;
        f->tf.eax = c.gpr[0]; f->tf.ecx = c.gpr[1]; f->tf.edx = c.gpr[2]; f->tf.ebx = c.gpr[3];
        f->tf.user_esp = c.gpr[4]; f->tf.ebp = c.gpr[5]; f->tf.esi = c.gpr[6]; f->tf.edi = c.gpr[7];
        f->tf.eip = c.eip; f->tf.eflags = c.eflags;
    }
    f->tf.eax = c.gpr[0]; f->tf.ecx = c.gpr[1]; f->tf.edx = c.gpr[2]; f->tf.ebx = c.gpr[3];
    f->tf.user_esp = c.gpr[4]; f->tf.ebp = c.gpr[5]; f->tf.esi = c.gpr[6]; f->tf.edi = c.gpr[7];
    f->tf.eip = c.eip; f->tf.eflags = c.eflags;
    v->video_bus_fault = false;
    v->perf.dac_batched += ports; v->perf.burst_insn += executed; v->video_burst += executed;
    return 0;
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
    bus.segment_base = pm_segment_base; bus.validate = pm_validate;
    bus.string = video_run_blocked(v, f) ? 0 : video_string;
    uint32_t fault_ip = f->tf.eip;
    int e = video_emulate(v, f, &c, &bus, true);
    if (!e && f->tf.eip != fault_ip && v->frame_pm && !(f->tf.eflags & V86_VM) &&
        f->tf.cs == c.sreg[CVX_CS]) video_burst(v, f, &c);
    return e;
}

bool dosvm_video_info(struct dosvm_key key, struct dosvm_video_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !v->info.key.generation || v->info.state == DOSVM_DEAD || !v->as.pd_phys) return false;
    memset(out, 0, sizeof(*out));
    uint8_t mode = 0;
    dosvm_read(v, 0x449, &mode, 1);
    out->mode = v->vbe_mode ? v->vbe_mode : mode;
    if (v->vbe_mode) {
        const struct dos_video_mode *m=dos_video_mode(v->vbe_mode);
        out->width=m->width; out->height=m->height; out->frames=v->video_frames;
        return true;
    }
    out->unchained = (v->vga.seq[4] & 0x0C) == 0x04;
    cvga_geometry g;
    if (cvga_get_geometry(&v->vga, &g)) { out->width = g.width; out->height = g.height; }
    out->faults = v->video_faults; out->pm_faults = v->video_pm_faults;
    out->starts = v->video_starts; out->maps = v->video_maps; out->flushes = v->video_flushes;
    out->frames = v->video_frames; out->burst = v->video_burst;
    out->linear_maps = v->video_linear_maps; out->linear_harvests = v->video_linear_harvests;
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
/* Retain the actual displayed indexed bytes, grouped by plane. Mode X/Y
 * spans are contiguous; chain-4 uses every fourth byte in each plane.
 * The total of the four spans is exactly width, including partial dots
 * from pan. Do not confuse a dirty PTE with changed pixels, or a CRTC flip
 * with changed palette/geometry. fx-27: 0 unchanged, 1 redrawn because
 * forced (palette, geometry, pan, layout), 2 displayed bytes differ. */
static unsigned video_planar_changed(struct dosvm *v, const cvga_state *scan, unsigned y, unsigned width, bool force)
{
    unsigned base = v->video_row_base[y], pan = v->video_row_pan[y];
    uint8_t *previous = v->video_indexes[y];
    bool forced = force || v->video_index_pan[y] != (pan & 3), differs = false;
    v->video_index_pan[y] = (uint8_t)(pan & 3);
    for (unsigned p = 0; p < 4; p++) {
        unsigned first = (p + 4 - (pan & 3)) & 3;
        if (first >= width) continue;
        unsigned count = (width - first + 3) / 4;
        if (v->video_chain4) {
            unsigned at = (base + ((pan + first) / 4) * 4) & 0xffffu;
            for (unsigned x = 0; x < count; x++, at = (at + 4) & 0xffffu) {
                uint8_t index = scan->plane[p][at];
                differs |= previous[x] != index;
                previous[x] = index;
            }
            previous += count;
            continue;
        }
        unsigned at = (base + (pan + first) / 4) & 0xffffu;
        unsigned n = CVGA_PLANE_SIZE - at;
        if (n > count) n = count;
        if (!forced && !differs && (memcmp(previous, &scan->plane[p][at], n) ||
            (n < count && memcmp(previous + n, scan->plane[p], count - n)))) differs = true;
        memcpy(previous, &scan->plane[p][at], n);
        if (n < count) memcpy(previous + n, scan->plane[p], count - n);
        previous += count;
    }
    return differs ? 2 : forced ? 1 : 0;
}
/* fx-27: chain-4 scanout read from the linear backing while the linear
 * window is open. With a dword-aligned row base (video_linear_rows) dot x
 * of row y is linear byte (base + pan + x) & FFFFh: plane (dot & 3) at
 * plane offset base + (dot / 4) * 4, which is where the window stores CPU
 * byte o (plane o & 3, offset o & FFFCh). The 16 backing pages are not
 * contiguous; rows are taken page by page. video_indexes keeps x order. */
static const uint8_t *video_linear_span(const struct dosvm *v, unsigned at, unsigned *n)
{
    unsigned off = at & (PAGE_SIZE - 1);
    if (*n > PAGE_SIZE - off) *n = PAGE_SIZE - off;
    return (const uint8_t *)P2V(v->frames[VIDEO_WINDOW_PAGE + (at >> 12)]) + off;
}
static unsigned video_linear_changed(struct dosvm *v, unsigned y, unsigned width, bool force)
{
    unsigned pan = v->video_row_pan[y], start = (v->video_row_base[y] + pan) & 0xffffu;
    uint8_t *previous = v->video_indexes[y];
    bool forced = force || v->video_index_pan[y] != (pan & 3), differs = false;
    v->video_index_pan[y] = (uint8_t)(pan & 3);
    for (unsigned x = 0; x < width;) {
        unsigned n = width - x;
        const uint8_t *src = video_linear_span(v, (start + x) & 0xffffu, &n);
        if (!differs && memcmp(previous + x, src, n)) differs = true;
        memcpy(previous + x, src, n);
        x += n;
    }
    return differs ? 2 : forced ? 1 : 0;
}
static void video_linear_row32(const struct dosvm *v, unsigned y, unsigned width, unsigned scale,
                               const uint32_t lut[256], uint32_t *dest)
{
    unsigned start = (v->video_row_base[y] + v->video_row_pan[y]) & 0xffffu;
    uint8_t mask = v->vga.dac_mask;
    for (unsigned x = 0; x < width;) {
        unsigned n = width - x;
        const uint8_t *src = video_linear_span(v, (start + x) & 0xffffu, &n);
        if (scale == 2) for (unsigned i = 0; i < n; i++) {
            uint32_t c = lut[src[i] & mask];
            dest[0] = c; dest[1] = c; dest += 2;
        } else for (unsigned i = 0; i < n; i++) {
            uint32_t c = lut[src[i] & mask];
            for (unsigned k = 0; k < scale; k++) *dest++ = c;
        }
        x += n;
    }
}
/* Rows of [start, start + width) that lie in a damaged backing page. */
static bool video_linear_damaged(unsigned start, unsigned width, uint16_t damage)
{
    if (!damage || !width) return false;
    unsigned first = start >> 12, last = ((start + width - 1) & 0xffffu) >> 12;
    for (unsigned page = first;; page = (page + 1) & 15) {
        if (damage & (1u << page)) return true;
        if (page == last) return false;
    }
}
/* One cadence policy for the wait, dirty sampler and presenter. Keep the
 * microsecond phase internally; only the public timeout is rounded up. */
uint64_t dosvm_vga_period_us(struct dosvm *v, uint64_t tick)
{
    uint64_t period = vga_frame_us(&v->vga);
    const struct dos_video_mode *mode = dos_video_mode(v->vbe_mode);
    cvga_geometry g = {0};
    if (mode) period = dos_video_frame_us(mode);
    else if (cvga_get_geometry(&v->vga, &g) && !g.text && (v->vga.gc[5] & 0x40) &&
             !(v->video_flip_run >= 2 && tick * 1000u - v->video_flip_us <= period * 2)) period *= 2;
    return period;
}
unsigned dosvm_vga_period_ms(struct dosvm *v)
{
    uint64_t period = dosvm_vga_period_us(v, g_ticks);
    unsigned ms = (unsigned)((period + 999) / 1000);
    return ms > CIUKI_DOSVM_WAIT_MAX_MS ? CIUKI_DOSVM_WAIT_MAX_MS : ms ? ms : 1;
}

uint64_t dosvm_vga_next_due(uint64_t due_us, uint64_t period, uint64_t tick)
{
    uint64_t next = due_us + period;
    return next / 1000u > tick ? next : tick * 1000u + period;
}

void dosvm_present_notify(struct dosvm *v)
{
    if (v->present_wait.head) kwait_wake_ready(&v->present_wait);
}

void dosvm_present_checkpoint(struct dosvm *v)
{
    if (!v->present_wait.head) return;
    /* Deferred/VM thread context only. The predicate never walks PTEs or
     * converts pixels. Observe direct stores even without a trapped OUT. */
    if (deadline_passed(v->present_sample_due_us / 1000u) && v->info.state < DOSVM_STOPPING &&
        !v->fullscreen && !v->fullscreen_hold) {
        v->present_sample_due_us = dosvm_vga_next_due(v->present_sample_due_us, dosvm_vga_period_us(v, g_ticks), g_ticks);
        /* fx-27: observe D pages only; the presenter reads chained pages
         * from the linear backing, so no deinterleave per sample. */
        if (!v->fullscreen && v->as.pd_phys) video_collect(v, false);
        if (v->vbe_mode && dosvm_vbe_dirty(v, false)) v->present_vbe_changes++;
    }
    dosvm_present_notify(v);
}

int dosvm_vga_present(struct dosvm *v, int (*row)(void *, unsigned, const void *, unsigned),
                      void *arg, uint64_t tick, struct ciuki_rect *damage)
{
    memset(damage, 0, sizeof(*damage));
    v->video_bands = 0;
    memset(v->video_rows,0,sizeof(v->video_rows));
    if (v->fullscreen || v->fullscreen_hold) return 0;
    if (v->launch_armed && !v->launch_ready) return 0;
    if (v->vbe_mode) return dosvm_vbe_present(v,row,arg,tick,damage);
    uint64_t period = dosvm_vga_period_us(v, tick);
    cvga_geometry current = {0};
    cvga_get_geometry(&v->vga, &current);
    v->video_present_rate = (unsigned)((1000000u + period / 2) / period);
    if (!v->video_presented || v->video_period_us != period) {
        v->video_due_us = v->video_presented ? v->video_tick * 1000u + period : tick * 1000u;
        v->video_period_us = period;
    }
    /* Check before harvest/dirty consumption, including palette and CRTC
     * changes. A completion write cannot bypass the per-VM cap. */
    if (v->video_presented && !current.text && tick < v->video_due_us / 1000u) return 0;
    v->present_samples++;
    /* Quantize the deadline to the host tick, retaining its fractional
     * phase: ceil(14.268 ms) on every frame would turn 70 Hz into 66 Hz.
     * Drop missed deadlines instead of accumulating a catch-up burst. */
    v->video_due_us = dosvm_vga_next_due(v->video_due_us, period, tick);
    uint64_t begin = dosvm_perf_task_clock();
    /* fx-27: collect D pages (including dirt carried by earlier plane
     * switches) without deinterleaving the chain-4 backing; the granules
     * are taken below, once the chained source is known. */
    video_collect(v, false);
    uint32_t display = v->vga.display_changes, changes = v->vga.changes;
    uint8_t dac_mask = v->vga.dac_mask;
    uint32_t starts = v->video_starts;
    unsigned phase = ((tick / DOSVM_BLINK_TICKS) & 1 ? 0 : CVGA_BLINK_VISIBLE) |
                     ((tick / DOSVM_CURSOR_TICKS) & 1 ? 0 : CVGA_CURSOR_VISIBLE);
    if (v->video_presented && display == v->video_display && changes == v->video_changes &&
        !memcmp(&current, &v->video_geometry, sizeof(current)) && (!current.text || phase == v->video_phase)) {
        v->video_tick = tick; v->video_phase = phase; v->video_seen_start = starts;
        v->perf.present_cycles += dosvm_perf_task_clock() - begin;
        return 0; /* unchanged models do not scan rows */
    }
    /* Scan live planes across row-group yields. Window tearing is allowed:
     * stores can appear in this sample or the next. New dirt/change counters
     * remain pending; exact comparison re-samples the previous scanout bytes
     * next time, without copying all four planes for every sample. */
    const cvga_state *scan = &v->vga;
    cvga_geometry g = {0};
    bool supported = cvga_get_geometry(scan, &g) && g.width && g.height &&
                     g.width <= DOSVM_VIDEO_WIDTH && g.height <= DOSVM_VIDEO_HEIGHT;
    unsigned scale = 1, width = 0, height = 0, left = 0, top = 0;
    if (supported) {
        scale = DOSVM_VIDEO_WIDTH / g.width;
        if (DOSVM_VIDEO_HEIGHT / g.height < scale) scale = DOSVM_VIDEO_HEIGHT / g.height;
        width = g.width * scale; height = g.height * scale;
        left = (DOSVM_VIDEO_WIDTH - width) / 2; top = (DOSVM_VIDEO_HEIGHT - height) / 2;
    } else memset(&g, 0, sizeof(g)); /* custom scanout: a black frame, VM keeps running */
    bool full = !v->video_presented || v->video_display != display ||
                memcmp(&g, &v->video_geometry, sizeof(g));
    bool borders = !v->video_presented || memcmp(&g, &v->video_geometry, sizeof(g));
    bool planar_cached = (v->video_modex || v->video_chain4) && v->video_presented;
    if (full) {
        unsigned base, pan;
        v->video_chain4 = supported && cvga_chain4_row(scan, 0, &base, &pan);
        v->video_modex = supported && !v->video_chain4;
        for (unsigned y = 0; y < g.height && (v->video_modex || v->video_chain4); y++) {
            bool valid = v->video_chain4 ? cvga_chain4_row(scan, y, &base, &pan) :
                                          cvga_modex_row(scan, y, &base, &pan);
            if (!valid) { v->video_modex = v->video_chain4 = false; break; }
            v->video_row_base[y] = base; v->video_row_pan[y] = (uint8_t)pan;
            if (!(y & 3)) kpreempt_point();
        }
        v->video_linear_rows = v->video_chain4;
        for (unsigned y = 0; y < g.height && v->video_linear_rows; y++)
            v->video_linear_rows = !(v->video_row_base[y] & 3u);
    }
    /* fx-27: while the chain-4 window is open, its linear backing is the
     * complete chained image; any other scanout reads planes made current
     * first. Take and clear the dirty granules atomically with respect to
     * the VM worker: a store after this point stays marked for the next
     * frame (granules, or a D page for the next collection). */
    bool linear = supported && v->video_chain4 && v->video_linear_rows && v->video_direct == VIDEO_LINEAR;
    if (!linear) video_linear_flush(v);
    memcpy(v->video_taken, v->vga.dirty, sizeof(v->video_taken));
    memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
    uint16_t linear_damage = v->video_linear_damage;
    v->video_linear_damage = 0;
    /* video_indexes keeps the layout of the source that filled it; a
     * change of source compares every row again (recorded below, once rows
     * are actually compared). */
    bool layout = v->video_index_linear != linear;
    bool indexed = v->video_modex || v->video_chain4;
    if (g.text) for (unsigned i = 0; i < CVGA_DIRTY_BYTES && !full; i++) full = v->video_taken[2][i] != 0;
    if (!full && changes == v->video_changes && (!g.text || phase == v->video_phase)) {
        v->video_tick = tick; v->video_phase = phase;
        v->video_seen_start = starts;
        v->perf.present_cycles += dosvm_perf_task_clock() - begin;
        return 0; /* an idle prompt/game never scans every row at the poll rate */
    }
    v->video_index_linear = linear;
    /* 101-b: one palette delta per sampled frame. Retained source indexes
     * let an unused DAC entry produce no conversion or surface traffic. */
    bool palette_changed = false;
    uint32_t palette_delta[8] = {0};
    if (!v->video_lut_valid || v->video_lut_display != display) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t color = rgb(scan, (uint8_t)i);
            if (!v->video_lut_valid || color != v->video_lut[i]) {
                palette_delta[i/32] |= 1u << (i%32);
                palette_changed = true;
            }
            v->video_lut[i] = color;
        }
        v->video_lut_display = display; v->video_lut_valid = true;
    }
    uint8_t bands[DOSVM_VIDEO_HEIGHT / DOSVM_VIDEO_BAND] = {0};
    uint8_t dirty_rows[DOSVM_VIDEO_HEIGHT] = {0};
    if (supported) for (unsigned y = 0; y < g.height; y++) {
        if (!(y & 3)) kpreempt_point();
        unsigned band = y / DOSVM_VIDEO_BAND;
        if ((indexed || !bands[band]) && (full || layout || (v->video_chain4 ?
            cvga_chain4_dirty(v->video_row_base[y], v->video_row_pan[y], g.width, v->video_taken) ||
            (linear && video_linear_damaged((v->video_row_base[y] + v->video_row_pan[y]) & 0xffffu,
                                            g.width, linear_damage)) : v->video_modex ?
            cvga_modex_dirty(v->video_row_base[y], v->video_row_pan[y], g.width, v->video_taken) :
            cvga_row_reads_dirty(scan, y, v->video_taken)) ||
            (g.text && (phase ^ v->video_phase) && cvga_row_phase_sensitive(scan, y)))) {
            bool force = !planar_cached || borders || g.blank || layout ||
                v->video_geometry.blank != g.blank || v->video_dac_mask != scan->dac_mask;
            unsigned changed = !indexed ? 1 : linear ? video_linear_changed(v, y, g.width, force) :
                video_planar_changed(v, scan, y, g.width, force);
            if (!changed && indexed && palette_changed) {
                for (unsigned x=0;x<g.width;x++) {
                    unsigned index=v->video_indexes[y][x] & dac_mask;
                    if (palette_delta[index/32] & (1u << (index%32))) {
                        changed=2; /* equal indexes, provably different RGB */
                        break;
                    }
                }
            }
            if (changed) {
                bands[band] = 1; dirty_rows[y] = (uint8_t)changed;
            }
        }
    }
    unsigned first = DOSVM_VIDEO_HEIGHT, last = 0, rendered = UINT32_MAX;
    bool restart = false;
    for (unsigned y = 0; y < DOSVM_VIDEO_HEIGHT; y++) {
        if (!(y & 3)) kpreempt_point();
        if (v->vbe_mode || v->fullscreen || v->fullscreen_hold ||
            v->info.state >= DOSVM_STOPPING) { restart = true; break; }
        bool inside = supported && y >= top && y < top + height;
        unsigned source = inside ? (y - top) / scale : 0;
        bool dirty = inside ? (indexed ? dirty_rows[source] : full || bands[source / DOSVM_VIDEO_BAND]) : borders;
        if (!dirty) continue;
        if (!inside) memset(v->video_pixels, 0, sizeof(v->video_pixels));
        else if (source != rendered) {
            /* Conversion overwrites the complete image span. Clear only
             * its horizontal borders, avoiding a second 640-pixel store
             * pass for the usual full-width Mode X/Y output. */
            if (left) memset(v->video_pixels, 0, left * sizeof(uint32_t));
            if (left + width < DOSVM_VIDEO_WIDTH)
                memset(v->video_pixels + left + width, 0,
                       (DOSVM_VIDEO_WIDTH - left - width) * sizeof(uint32_t));
            if (linear && !g.blank) {
                video_linear_row32(v, source, g.width, scale, v->video_lut, v->video_pixels + left);
                v->perf.linear_direct++;
            } else if (v->video_chain4 && !g.blank) {
                cvp_chain4_row32(scan, v->video_row_base[source], v->video_row_pan[source],
                                 g.width, scale, v->video_lut, v->video_pixels + left);
            } else if (v->video_modex && !g.blank) {
                cvp_modex_row32(scan, v->video_row_base[source], v->video_row_pan[source],
                                g.width, scale, v->video_lut, v->video_pixels + left);
            } else if (cvga_render_row8(scan, source, v->video_row, sizeof(v->video_row), phase) != g.width) {
                /* The guest changed scanout during this frame: redraw it whole. */
                restart = true;
                break;
            }
            if (!indexed || g.blank) for (unsigned x = 0, dest = left; x < g.width; x++) {
                uint32_t pixel = g.blank ? 0 : v->video_lut[v->video_row[x]];
                for (unsigned copy = 0; copy < scale; copy++) v->video_pixels[dest++] = pixel;
            }
            v->perf.converted_bytes += g.width;
            rendered = source;
        }
        /* fx-27: an exact index difference proves the row changed; the
         * surface writer then needs no comparison with its old contents. */
        v->video_row_proven = inside && indexed && !g.blank && dirty_rows[source] == 2;
        int e = row(arg, y, v->video_pixels, sizeof(v->video_pixels));
        v->video_row_proven = false;
        if (e<0) {
            v->perf.present_cycles += dosvm_perf_task_clock() - begin;
            v->video_presented = false; return e;
        } /* retry whole after a partial write */
        if (e>0) continue; /* destination already contains this scanline */
        v->perf.rows++;
        v->video_bands |= 1ull << (y / DOSVM_VIDEO_BAND);
        v->video_rows[y/32] |= 1u<<(y%32);
        if (y < first) first = y;
        last = y + 1;
    }
    v->video_tick = tick; v->video_phase = phase; v->video_display = display;
    v->video_changes = changes;
    v->video_seen_start = starts;
    v->video_geometry = g;
    /* A mask change across a yield must stay pending with its display
     * generation, even if some live rows already used the new mask. */
    v->video_dac_mask = dac_mask;
    v->video_presented = !restart;
    v->perf.present_cycles += dosvm_perf_task_clock() - begin;
    if (!last) return 0;
    v->video_frames++;
    *damage = (struct ciuki_rect){0, (int32_t)first, 0, (int32_t)first, DOSVM_VIDEO_WIDTH, last - first};
    return 1;
}

/* Probe/host digest sink: the complete frame, every row in order. */
struct frame_sink {
    struct sha256_ctx sha; unsigned next;
    int (*row)(void *,unsigned,const void *,unsigned); void *arg;
};
static int hash_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{
    struct frame_sink *sink = arg;
    if (y != sink->next || bytes != DOSVM_VIDEO_WIDTH * 4) return -EINVAL;
    if (sink->row) {
        int error=sink->row(sink->arg,y,pixels,bytes);
        if (error) return error;
    }
    sha256_update(&sink->sha, pixels, bytes);
    sink->next++;
    return 0;
}
int dosvm_frame_capture(struct dosvm *v, uint64_t tick, struct dosvm_frame *out,
                       int (*row)(void *,unsigned,const void *,unsigned),void *arg)
{
    /* This scanout consumer only reads the model. Publish pending pages but
     * retain the writable aperture; closing it strands a planar stream until
     * a changed map mask, since the old fault retry only admits chain-4. */
    dosvm_vga_sync(v, false);
    struct frame_sink sink = {.next = 0,.row=row,.arg=arg};
    struct ciuki_rect damage;
    sha256_init(&sink.sha);
    v->video_presented = false;                   /* every row, borders included */
    int e = v->vbe_mode ? dosvm_vbe_capture(v,hash_row,&sink,tick,&damage) :
        dosvm_vga_present(v, hash_row, &sink, tick, &damage);
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
    out->mode = v->vbe_mode ? v->vbe_mode : mode;
    return 0;
}
int dosvm_frame_digest(struct dosvm *v,uint64_t tick,struct dosvm_frame *out)
{ return dosvm_frame_capture(v,tick,out,0,0); }
