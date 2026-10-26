/* Private DOS lifecycle, software virtual IF and checked guest memory.
 * Research/decisions: docs/design/f3-acceptance.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/probe.h>
#include <ciuki/input.h>
#include <ciuki/abi.h>
#include <ciuki/timing.h>
#include <ciuki/cpustat.h>
#include <ciuki/work.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include "dosvm_internal.h"
#pragma weak dosvm_game_ports_apply
#pragma weak dosvm_game_ports_close
#pragma weak dosvm_lpt_stop
#pragma weak dosvm_print_service
#pragma weak dosvm_print_flush
/* fx-4 refusal record: zone figures and the inventory step. Weak only so the
 * guest-CPU harness, which links neither, keeps its fixture seams. */
#pragma weak pmm_zone_free
#pragma weak dosvm_game_program_checked
#include "dosvm_network.h"

extern void v86_enter(const struct v86_frame *, uint32_t *);
extern void v86_leave(uint32_t) __attribute__((noreturn));
/* f4-05 DOS VM audio output (audio/vm_source.c): one mixer source per VM
 * while a backend is ready. Weak: host harnesses link the VM without F4. */
extern int audio_vm_attach(struct dosvm_key) __attribute__((weak));
extern void audio_vm_detach(struct dosvm_key) __attribute__((weak));
extern void audio_vm_kick(void) __attribute__((weak));
static struct dosvm slots[DOSVM_LIMIT];
/* Admission remains visible while an unpublished slot is cleared a page
 * at a time. Other creators cannot reuse it or spend its commitment. */
static uint32_t initializing[DOSVM_LIMIT];
#define POOL_PINS 7u
static struct mm_pin pool_pins[DOSVM_LIMIT][POOL_PINS];
static void pool_unpin(struct dosvm *v)
{
    unsigned slot=v->info.key.id-1;
    if (slot>=DOSVM_LIMIT) return;
    if (mm_unpin) for (unsigned i=0;i<POOL_PINS;i++) if (pool_pins[slot][i].ticket)
        (void)mm_unpin(&pool_pins[slot][i],v->info.key.generation);
    memset(pool_pins[slot],0,sizeof(pool_pins[slot]));
}
static int pool_pin(struct dosvm *v)
{
    /* f3-46: only the locked (eager, resident) pool is pinned as a whole;
     * a paged pool pins exactly its device/model/DMA/DPMI-locked pages. */
    if (!mm_pin || !v->memory.quota || !v->memory.pool.locked) return 0;
    unsigned slot=v->info.key.id-1, index=0;
    const uint32_t *pd=P2V(v->as.pd_phys);
    uint32_t end=VM_POOL_BASE+v->memory.quota;
    for (uint32_t va=VM_POOL_BASE;va<end;) {
        if (index>=POOL_PINS) return -ENOMEM;
        uint32_t entries=1024u-((va/PAGE_SIZE)&1023u), pages=(end-va)/PAGE_SIZE;
        if (entries>pages) entries=pages;
        struct mm_pin *pin=&pool_pins[slot][index++];
        *pin=(struct mm_pin){.generation=v->info.key.generation,
            .first_pfn=as_lookup(&v->as,va,0)/PAGE_SIZE,.pages=entries,
            .address_mask=UINT32_MAX,.alignment=PAGE_SIZE,.direction=MM_PIN_CPU,
            .pfn_table=pd[va>>22]&~4095u,.first_entry=(va/PAGE_SIZE)&1023u};
        int e=mm_pin(pin); if (e) return e;
        va+=entries*PAGE_SIZE;
    }
    return 0;
}
static struct dosvm_key input_focus;

/* A 200-byte polled UART record at 38400 baud costs about one BIOS tick.
 * Printing inside a guest IRQ that sends early EOI then STI can manufacture
 * recursive IRQ0s. DMX exhausts its private IRQ stacks after four such
 * entries and returns without EOI (f3-12 validation). Snapshot now, print
 * only outside the whole IRQ chain, including its real calls/callbacks.
 * Never change delivery, virtual IF, device time or the guest's EOI. */
static bool dosvm_in_irq(const struct dosvm *v)
{
    for (unsigned i = 0; i < v->dpmi.depth; i++)
        if ((v->dpmi.rec[i].flags & DPMI_RF_HWINT) && !v->dpmi.rec[i].isr_expired) return true;
    return false;
}

#include "dosvm_perf.inc"
#include "dosvm_io_burst.inc"
#include "dosvm_clipboard.inc"

/* ---- fx-27 lean port exits ----
 * A trapped IN/OUT to a hot emulated port (the VGA register file, PIT,
 * PIC, AdLib/OPL and the DMA controllers) is a VM's most frequent
 * exit: DOOM's Mode Y selects the plane of every column with an OUT to
 * 3C5h (about 950 per frame), Quake loads 768 DAC bytes per palette change
 * and reads the PIT and DMA counters several thousand times a second. The
 * T23 trials measured 14-22 us for each such exit (vm_perf_cost vga_io,
 * pit_pic, io) against 11-12 us for a V86 CLI; nearly all of it is the
 * monitor's per-exit housekeeping rather than the device emulation.
 * An exit is lean when, before it, the VM has no boundary obligation that
 * this exit must serve (no STI/MOV SS shadow, single-step, IF-image or
 * return tracking, CLI-seed demand, return to an interrupted context, idle
 * poll, mouse/IPX event, full-screen hold) and, after the emulated
 * instruction (with the same
 * handler and bursts as the ordinary path), no virtual IRQ is pending, no
 * reschedule is due and no EXEC/launch frame was reached. Then everything
 * else the ordinary path does at an exit is periodic (UART/IPX service,
 * present sampling, heartbeats, reports, audio log) and runs at the next
 * ordinary exit, at most one host tick later: the host timer interrupt
 * itself always takes the ordinary path. When an obligation appears
 * after the instruction, the exit continues through the ordinary tail
 * without emulating again, so delivery and scheduling are unchanged. */
static bool lean_port(const struct dosvm *v, uint16_t port, unsigned width)
{
    if ((port >= 0x3C0 && port <= 0x3CF) || port == 0x3D4 || port == 0x3D5 || port == 0x3DA ||
        port == 0x3B4 || port == 0x3B5 || port == 0x3BA)
        return width <= 2 && dosvm_vga_ports_modeled(v);
    if (width != 1) return false;
    /* t23-dosperf: the Sound Blaster's own OPL3 ports (base+0..3, base+8/9)
     * reach the same dosvm_opl_io as 388h-38Bh (dosvm_sb16.c sb_io).
     * Warcraft II's music writes them through 220h/222h: 4,400 exits per
     * second at 12.8 us in trial c42f556e (bank n=3), never lean. */
    uint16_t sb = v->audio.sb.base;
    if (sb && ((port >= sb && port <= sb + 3) || port == sb + 8 || port == sb + 9)) return true;
    return (port >= 0x40 && port <= 0x43) || port == 0x20 || port == 0x21 || port == 0xA0 ||
        port == 0xA1 || (port >= 0x388 && port <= 0x38B) || port <= 0x0F ||
        (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF);
}
static bool lean_fetch(struct dosvm *v, const struct v86_frame *f, const struct dpmi_seg *cs,
                       unsigned at, uint8_t *byte)
{
    uint32_t linear;
    if (cs) {
        uint32_t off = f->tf.eip + at;
        if (!cs->big) off &= 0xFFFF;
        if (off > cs->limit) return false;
        linear = cs->base + off;
    } else {
        if (f->tf.cs > 0xFFFF || f->tf.eip + at > 0xFFFF) return false;
        linear = (f->tf.cs << 4) + f->tf.eip + at;
    }
    if (linear >= 0xA0000 && linear < 0xC0000) return false;
    uint32_t pte = 0, phys = as_lookup(&v->as, linear, &pte);
    /* Not resident: the ordinary path. Nor a borrowed page: a VBE full
     * screen's direct LFB lease (dosvm_vbe_direct_enter) puts the provider's
     * physical framebuffer in the VM's PTEs, which P2V does not map (T23
     * BAR E8000000h); the ordinary monitor reads it through the provider
     * alias (monitor_memory, transfer). Codex review of 1bb063bd0, P1. */
    if (!phys || !(pte & PTE_U) || (pte & PTE_BORROWED)) return false;
    *byte = *(const uint8_t *)P2V(phys);
    return true;
}
static bool lean_before(const struct dosvm *v, const struct v86_frame *f)
{
    /* fx-doomfs2: only VGA ports depend on display ownership. OPL, PIC,
     * PIT, DMA and PM flags stay virtualized in a VGA full screen too;
     * gating them on modeled VGA cost DOOM 0.8-1.3 s per 5 s under load
     * on the T23. Keep all transition and interrupt obligations below. */
    if (v->info.state >= DOSVM_STOPPING || !v->info.dos_ready ||
        v->fullscreen_hold || v->fullscreen_suspended || v->idle.depth || v->network_attached ||
        v->audio.sb.irq_entry_ready || v->monitor.state == V86_HALTED || g_need_resched) return false;
    if (v->dpmi.active && !dpmi_lean_eligible(v, f)) return false;
    if (v->frame_pm) return true;
    if (v->monitor.shadow || (f->tf.eflags & V86_TF)) return false;
    const struct dosvm_mouse *m = &v->mouse;
    return !(m->pending_mask && m->mask && !m->active);
}
/* IF clear: nothing may become pending between this check and IRET. */
static bool lean_after(struct dosvm *v, const struct v86_frame *f)
{
    /* A DMA channel just programmed (or completed) is pinned at the
     * ordinary settle point (f3-46 dosvm_pool_settle), never later. */
    if (v->info.state >= DOSVM_STOPPING || g_need_resched || v->monitor.state == V86_HALTED ||
        v->audio.sb.irq_entry_ready || (v->memory.pool.dma_dirty && v->memory.pool.pages)) return false;
    dosvm_pic_cascade(&v->monitor);
    if (dosvm_pic_pending(&v->monitor) >= 0) return false;
    if (v->dpmi.active && !dpmi_lean_quiet(v, f)) return false;
    if (v->frame_pm) return true;
    if (v->launch_exec && f->tf.cs == v->launch_cs && f->tf.eip == v->launch_ip &&
        f->tf.user_ss == v->launch_ss && (uint16_t)f->tf.user_esp == v->launch_sp) return false;
    if (v->exec_pending) {
        unsigned top = v->exec_pending - 1u;
        if (f->tf.cs == v->exec_frame[top].cs && f->tf.eip == v->exec_frame[top].ip &&
            f->tf.user_ss == v->exec_frame[top].ss && (uint16_t)f->tf.user_esp == v->exec_frame[top].sp) return false;
    }
    return true;
}
/* Returns 0 when the exit is not lean (nothing was done), 1 when the
 * instruction retired and the exit can return now (IF clear on return),
 * 2 when it retired and the ordinary tail must run (*error set). */
static int lean_io(struct dosvm *v, struct v86_frame *f, int *error)
{
    *error = 0;
    /* t23-dosperf: a PM STI shadow covers one instruction; pm_trap ends it
     * at every later trap that is not a physical IRQ (or an owned execute
     * fault) at the shadowed address, so at every #GP(0). End it here the
     * same way, so the exit after a lean STI can be lean too (Warcraft II:
     * STI, then the next CLI). A pending IRQ is still delivered on the
     * ordinary tail (lean_after). */
    if (v->frame_pm) v->dpmi.shadow = false;
    if (!lean_before(v, f)) return 0;
    struct dpmi_seg seg, *cs = 0;
    if (v->frame_pm) {
        if (!dpmi_seg(v, (uint16_t)f->tf.cs, &seg) || !seg.code || !seg.present) return 0;
        cs = &seg;
    }
    uint8_t op, imm;
    unsigned len = 1, width = cs && cs->big ? 4 : 2;
    if (!lean_fetch(v, f, cs, 0, &op)) return 0;
    if (op == 0x66) {
        width = width == 4 ? 2 : 4;
        if (!lean_fetch(v, f, cs, len++, &op)) return 0;
    }
    /* t23-dosperf: an unprefixed PM CLI/STI (DOS/4GW and Watcom clients:
     * Warcraft II, 10,143 per second in trial c42f556e bank n=4) takes the
     * same lean return as a port exit. V86 CLI/STI keep v86_emulate. */
    if ((op == 0xFA || op == 0xFB) && len == 1 && cs) {
        bool quiet = dpmi_lean_flags(v, f, cs, op);
        cli();
        if (quiet && lean_after(v, f)) { v->perf.lean_exits++; return 1; }
        sti();
        return 2;
    }
    if (op < 0xE4 || (op > 0xE7 && op < 0xEC) || op > 0xEF) return 0;
    uint16_t port = (uint16_t)f->tf.edx;
    if (op < 0xE8) {
        if (!lean_fetch(v, f, cs, len++, &imm)) return 0;
        port = imm;
    }
    if (!(op & 1)) width = 1;
    bool write = op & 2;
    if (!lean_port(v, port, width)) return 0;
    uint32_t value = f->tf.eax;
    int e = 0;
    /* The same dispatch as dosvm_io_fast: the cached PIT/OPL handlers,
     * then the port registry (dosvm_io). */
    if (!dosvm_io_hot(v, port, width, write, &value, &e)) e = v86_io(&v->monitor, port, width, write, &value);
    if (e) {
        *error = v->frame_pm ? (e < 0 ? e : -EFAULT) : v86_abort(&v->monitor, f, e);
        return 2;
    }
    if (!write) {
        uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
        f->tf.eax = (f->tf.eax & ~mask) | (value & mask);
    }
    f->tf.eip += len;
    if (!cs || !cs->big) f->tf.eip &= 0xFFFF;
    if (!v->frame_pm) v->monitor.stats.insn[write ? V86_OUT : V86_IN]++;
    uint8_t next;
    if ((v->perf.cause == DOSVM_COST_VGA_IO || (v->perf.cause == DOSVM_COST_PIT_PIC &&
        v->perf.io_port >= 0x40 && v->perf.io_port <= 0x43)) &&
        (v->video_dac_burst || !lean_fetch(v, f, cs, 0, &next) || io_burst_candidate(next))) {
        e = dosvm_port_bursts(v, f);
        if (e) { *error = e; return 2; }
    }
    v->video_dac_burst = false;
    if (v->frame_pm) dpmi_lean_retire(v, f);
    else if (v->dpmi.active) v->dpmi.if_steps = 0; /* dpmi_deliver's if_recovery_end */
    cli();
    if (lean_after(v, f)) { v->perf.lean_exits++; return 1; }
    sti();
    return 2;
}

void dosvm_launch_video(struct dosvm *v)
{
    if (v->launch_armed && v->launch_exec && !v->launch_failed && !v->launch_ready) {
        v->launch_ready = true;
        dosvm_present_notify(v);
    }
}
void dosvm_console_byte(struct dosvm *v, uint8_t byte)
{
    if (!v->info.dos_ready && v->boot_log_count < sizeof(v->boot_log))
        v->boot_log[v->boot_log_count++] = (char)byte;
    if (byte == '\r' || byte == '\n') {
        v->console_length = 0; v->console_sequence++;
        dosvm_present_notify(v); return;
    }
    if (byte >= 32 && byte < 127 && v->console_length < sizeof(v->console_line) - 1) {
        v->console_line[v->console_length++] = (char)byte;
        v->console_line[v->console_length] = 0;
    }
}

static void dosvm_trace_flush(struct dosvm *v)
{
    uint32_t flags = irq_save(); irq_restore(flags);
    if (!(flags & V86_IF) || (dosvm_in_irq(v) && v->info.state != DOSVM_STOPPING)) return;
    uint64_t begin = v->deferred_trace.count || v->deferred_trace.dropped ? dosvm_perf_clock() : 0;
    for (unsigned i = 0; i < v->deferred_trace.count; i++)
        rec_diag(dosvm_probe(v), "%s", v->deferred_trace.line[i]);
    v->deferred_trace.count = 0;
    if (v->deferred_trace.dropped) {
        rec_diag(dosvm_probe(v), "group=vm_trace_drop vm=%u gen=%u count=%u",
                 v->info.key.id, v->info.key.generation, v->deferred_trace.dropped);
        v->deferred_trace.dropped = 0;
    }
    if (begin) v->perf.log_cycles += dosvm_perf_clock() - begin;
}

void dosvm_diag(struct dosvm *v, const char *fmt, ...)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    uint32_t flags = irq_save(); irq_restore(flags);
    bool defer = !(flags & V86_IF) || (dosvm_in_irq(v) && v->info.state != DOSVM_STOPPING);
    if (defer && v->deferred_trace.count == ARRAY_SIZE(v->deferred_trace.line)) {
        if (v->deferred_trace.dropped != UINT32_MAX) v->deferred_trace.dropped++;
        return;
    }
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (defer) {
        memcpy(v->deferred_trace.line[v->deferred_trace.count++], line, strlen(line) + 1);
    } else {
        dosvm_trace_flush(v);
        uint64_t begin = dosvm_perf_clock();
        rec_diag(dosvm_probe(v), "%s", line);
        v->perf.log_cycles += dosvm_perf_clock() - begin;
    }
}

/* Called only with physical IF enabled in the worker, never from the timer
 * hook. Ages come from INTA, including PIC poll acknowledgements. Sampling
 * ISR alone cannot distinguish an EOI and reinjection in the same trap. */
static void dosvm_pic_stall(struct dosvm *v, const struct v86_frame *f)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || v->info.state >= DOSVM_STOPPING ||
        v->trace.pic_stalls >= DOSVM_PIC_STALL_LIMIT) return;
    struct {
        uint64_t age;
        uint8_t chip, irq, isr, irr, imr, ae, sm;
    } sample[16];
    unsigned count = 0;
    uint32_t flags = irq_save();
    uint64_t tick = g_ticks;
    for (unsigned chip = 0; chip < 2; chip++) {
        struct v86_pic *p = &v->monitor.pic[chip];
        uint8_t pending = p->isr & ~p->stall_reported;
        if (!pending) continue;
        for (unsigned irq = 0; irq < 8; irq++) {
            uint8_t bit = 1u << irq;
            if (!(pending & bit) || v->trace.pic_stalls >= DOSVM_PIC_STALL_LIMIT) continue;
            uint64_t age = tick - p->service_tick[irq];
            if (age <= DOSVM_PIC_STALL_TICKS) continue;
            p->stall_reported |= bit;
            sample[count].age = age; sample[count].chip = chip; sample[count].irq = irq + chip * 8;
            sample[count].isr = p->isr; sample[count].irr = p->irr; sample[count].imr = p->imr;
            sample[count].ae = p->auto_eoi; sample[count].sm = p->special_mask;
            count++;
            v->trace.pic_stalls++;
        }
    }
    irq_restore(flags);
    for (unsigned i = 0; i < count; i++) {
        klog("[dosvm] event=pic_stall vm=%u gen=%u pic=%u irq=%u ms=%llu cs=%04x eip=%08x isr=%02x irr=%02x imr=%02x vec=%02x to=%04x:%08x boundary=%s at=%04x:%08x pm=%u depth=%u ae=%u sm=%u",
             v->info.key.id, v->info.key.generation, sample[i].chip, sample[i].irq, sample[i].age,
             f->tf.cs & 0xFFFF, f->tf.eip, sample[i].isr, sample[i].irr, sample[i].imr,
             v->monitor.stats.last_vector & 255, v->trace.injected_cs, v->trace.injected_eip,
             v->trace.boundary ? v->trace.boundary : "none", v->trace.boundary_cs, v->trace.boundary_eip,
             !(f->tf.eflags & V86_VM), v->dpmi.depth, sample[i].ae, sample[i].sm);
    }
}

/* f3-10: sample the interrupted guest, before emulation or IRQ injection can
 * replace CS:EIP with a handler's entry. Device time keeps advancing while
 * virtual IF is clear. Only this preemptible path prints, never the IRQ hook.
 * Start with graphics, so a slow WAD load cannot use the menu's budget. */
static void dosvm_heartbeat(struct dosvm *v, const struct v86_frame *f)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || v->info.state >= DOSVM_STOPPING ||
        v->trace.heartbeats >= DOSVM_HEARTBEAT_LIMIT) return;
    uint32_t flags = irq_save();
    uint64_t tick = g_ticks; /* coherent on i686, including the low-word wrap */
    irq_restore(flags);
    if (!v->trace.heartbeat_started) {
        if (!(v->vga.gc[6] & 1)) return;
        v->trace.heartbeat_started = true;
        v->trace.heartbeat_tick = tick;
        return;
    }
    if (tick - v->trace.heartbeat_tick < DOSVM_HEARTBEAT_TICKS) return;
    /* The timer IRQ mutates PIT, PIC and DSP counters. Copy a single coherent
     * sample before the first UART write, without copying the large queues. */
    struct {
        uint64_t tick, edges, lost;
        struct v86_pic pic[2];
        struct dosvm_pit_channel pit;
        struct dosvm_dma_channel dma;
        struct dosvm_sb_transfer x;
        uint32_t irq0, irq1, sb_irqs, kq, kr, units, sb_edges, services;
        uint32_t frames, starts, linear, planes, faults;
        uint32_t harvests, pages, direct, chain;
        uint64_t harvest_cycles, remap_cycles, present_cycles;
        uint32_t port_remaps, burst_insn;
        uint8_t kcmd, sb_irq, causes, error;
    } r;
    flags = irq_save();
    r.tick = g_ticks; r.edges = v->pit.edges; r.lost = v->pit.dropped_ticks;
    memcpy(r.pic, v->monitor.pic, sizeof(r.pic)); r.pit = v->pit.channel[0];
    r.irq0 = v->monitor.stats.delivered[0]; r.irq1 = v->monitor.stats.delivered[1];
    r.kq = v->kbd.count; r.kr = v->kbd.reads; r.kcmd = v->kbd.command;
    const struct dosvm_sb16 *sb = &v->audio.sb;
    r.x = sb->x; r.dma = v->audio.dma.ch[r.x.channel & 7];
    r.units = sb->units; r.sb_edges = sb->irq_edges; r.services = sb->services;
    r.sb_irq = sb->irq; r.sb_irqs = v->monitor.stats.delivered[sb->irq & 15];
    r.causes = sb->causes; r.error = sb->last_error;
    r.frames = v->video_frames; r.starts = v->video_starts;
    r.linear = v->video_linear_harvests; r.planes = v->video_flushes; r.faults = v->video_faults;
    r.harvests = v->perf.harvests; r.pages = v->perf.direct_pages;
    r.harvest_cycles = v->perf.harvest_cycles;
    r.port_remaps = v->perf.port_remaps; r.remap_cycles = v->perf.remap_cycles;
    r.present_cycles = v->perf.present_cycles; r.burst_insn = v->perf.burst_insn;
    r.direct = v->video_direct; r.chain = !!(v->vga.seq[4] & 8);
    v->trace.heartbeat_tick = r.tick;
    unsigned n = ++v->trace.heartbeats;
    irq_restore(flags);
    unsigned id = v->info.key.id, gen = v->info.key.generation;
    dosvm_diag(v, "group=vm_heartbeat vm=%u gen=%u n=%u tick=%llu pm=%u cs=%04x eip=%08x if=%u vif=%u arm=%u sh=%u depth=%u",
             id, gen, n, r.tick, !(f->tf.eflags & V86_VM), f->tf.cs & 0xFFFF, f->tf.eip,
             !!(f->tf.eflags & V86_IF), v->monitor.vif, v->dpmi.if_armed,
             v->monitor.shadow || v->dpmi.shadow, v->dpmi.depth);
    dosvm_diag(v, "group=vm_beat_irq vm=%u gen=%u n=%u irr=%02x%02x isr=%02x%02x imr=%02x%02x irq0=%u irq1=%u kq=%u kr=%u kcmd=%02x",
             id, gen, n, r.pic[1].irr, r.pic[0].irr, r.pic[1].isr, r.pic[0].isr,
             r.pic[1].imr, r.pic[0].imr, r.irq0, r.irq1, r.kq, r.kr, r.kcmd);
    dosvm_diag(v, "group=vm_beat_pit vm=%u gen=%u n=%u edges=%llu lost=%llu reload=%u pending=%u mode=%u run=%u gate=%u",
             id, gen, n, r.edges, r.lost, r.pit.reload, r.pit.pending, r.pit.mode, r.pit.running, r.pit.gate);
    dosvm_diag(v, "group=vm_beat_sb vm=%u gen=%u n=%u ch=%u addr=%04x count=%04x units=%u edges=%u irq=%u hits=%u ack=%u a=%u p=%u w=%u m=%u cause=%02x err=%u",
             id, gen, n, r.x.channel, r.dma.address, r.dma.count, r.units, r.sb_edges, r.sb_irq, r.sb_irqs,
             r.services, r.x.active, r.x.paused, r.x.waiting, r.dma.masked, r.causes, r.error);
    dosvm_diag(v, "group=vm_beat_vga vm=%u gen=%u n=%u frames=%u flips=%u linear=%u planes=%u faults=%u direct=%u chain=%u",
             id, gen, n, r.frames, r.starts, r.linear, r.planes, r.faults, r.direct, r.chain);
    /* Split wide counters to respect the 240-byte record envelope. */
    dosvm_diag(v, "group=vm_beat_vga vm=%u gen=%u n=%u part=fast faults=%u port_remaps=%u burst_insn=%u",
             id, gen, n, r.faults, r.port_remaps, r.burst_insn);
    dosvm_diag(v, "group=vm_beat_vga vm=%u gen=%u n=%u part=time remap_us=%llu present_us=%llu",
             id, gen, n, perf_us(r.remap_cycles), perf_us(r.present_cycles));
    dosvm_diag(v, "group=vm_beat_vga_mem vm=%u gen=%u n=%u harvests=%u dirty_pages=%u harvest_us=%llu",
             id, gen, n, r.harvests, r.pages, perf_us(r.harvest_cycles));
}

/* Runtime diagnostics live here to keep record ownership in the lifecycle
 * service. Call only from the preemptible monitor, never dosvm_tick or an
 * IF-disabled device transaction (polled UART writes can lose PIT ticks). */
static const char *dosvm_step_owner(const struct dosvm *v, const struct v86_frame *f, uint32_t dr6)
{
    const struct dpmi *d = &v->dpmi;
    if (!(f->tf.eflags & V86_VM) && d->return_steps && d->return_slot < DPMI_WATCH &&
        !d->watch[d->return_slot].used && (dr6 & (1u << d->return_slot))) return "pm_checkpoint";
    if (!(dr6 & (1u << 14))) return "client";
    if (f->tf.eflags & V86_VM) {
        if (v->monitor.shadow) return "rm_ss";
        return v->monitor.vif && v86_irq_pending(&v->monitor) >= 0 ? "rm_irq" : "rm_tf";
    }
    if (d->if_step) return "pm_if";
    if (d->if_armed) return "pm_flags";
    if (d->return_step && (uint16_t)f->tf.cs == d->return_step_target_cs && (uint16_t)f->tf.user_ss == d->return_ss &&
        d->depth == d->return_depth && d->clients == d->return_client) return "pm_return";
    return "client";
}

void dosvm_trace(struct dosvm *v, enum dosvm_trace_kind kind, const struct v86_frame *f,
                 uint32_t a, uint32_t b, uint32_t c)
{
    static const uint8_t limits[DOSVM_TRACE_COUNT] = {32, 24, 10, 32, 8, 64, 10, 1};
    if (kind != DOSVM_TRACE_FAULT && !(g_boot.flags & CBI_F_VERBOSE)) return;
    if (kind == DOSVM_TRACE_FLAGS) dosvm_boundary(v, f, a ? "if_restore" : b ? "if_arm" : "cli");
    if (kind == DOSVM_TRACE_FLAGS && !v->trace.timer_programmed && !a && !v->dpmi.if_armed) {
        struct dpmi_seg cs;
        /* Extender initialization contains many 16-bit CLI/STI pairs.
         * Keep the sample budget for the application's flat code. */
        if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || !cs.big) return;
    }
    if (kind == DOSVM_TRACE_VECTOR && (a >> 8) == 0x0205 && (uint8_t)a == v->monitor.pic[0].base &&
        v->trace.irq_epochs < 4) {
        v->trace.irq_epochs++;
        v->trace.used[DOSVM_TRACE_IRQ0] = 0;
    }
    /* DOS/4GW can spend seconds loading the WAD before the application's
     * PIT setup. Reserve clock samples for the programmed guest timer. */
    if (kind == DOSVM_TRACE_PIT && a == 0x40 && !v->pit.channel[0].write_phase &&
        v->pit.channel[0].pending && !v->trace.timer_programmed) {
        v->trace.timer_programmed = true;
        v->trace.used[DOSVM_TRACE_DB_PM] = v->trace.used[DOSVM_TRACE_FLAGS] = 0;
        if (v->trace.irq_epochs < 4) {
            v->trace.irq_epochs++;
            v->trace.used[DOSVM_TRACE_IRQ0] = 0;
        }
    }
    if (v->trace.used[kind] >= limits[kind]) return;
    if (kind == DOSVM_TRACE_TIMER) {
        if (!v->trace.timer_programmed) return;
        if (v->trace.used[kind] && g_ticks - v->trace.timer_tick < 250) return;
        v->trace.timer_tick = g_ticks;
    }
    unsigned n = ++v->trace.used[kind];
    bool pm = !(f->tf.eflags & V86_VM);
    switch (kind) {
    case DOSVM_TRACE_VECTOR:
        dosvm_diag(v, "group=vm_vec vm=%u gen=%u n=%u api=%04x vec=%02x sel=%04x off=%08x pm=%u cs=%04x eip=%08x",
                 v->info.key.id, v->info.key.generation, n, a >> 8, a & 255, b, c, pm,
                 f->tf.cs & 0xFFFF, f->tf.eip);
        break;
    case DOSVM_TRACE_PIT: {
        unsigned ch = a == 0x43 ? b >> 6 : a - 0x40;
        const struct dosvm_pit_channel *p = &v->pit.channel[ch];
        dosvm_diag(v, "group=vm_pit vm=%u gen=%u n=%u port=%02x byte=%02x ch=%u mode=%u access=%u reload=%u pending=%u half=%u cs=%04x eip=%08x vif=%u armed=%u",
                 v->info.key.id, v->info.key.generation, n, a, b, ch, p->mode, p->access,
                 p->reload, p->pending, p->write_phase, f->tf.cs & 0xFFFF, f->tf.eip, v->monitor.vif, v->dpmi.if_armed);
        break;
    }
    case DOSVM_TRACE_IRQ0:
        dosvm_diag(v, "group=vm_irq0 vm=%u gen=%u epoch=%u n=%u pm=%u cs=%04x eip=%08x if=%u vif=1 tf=%u vec=%02x to_pm=%u sel=%04x off=%08x",
                 v->info.key.id, v->info.key.generation, v->trace.irq_epochs, n, pm, f->tf.cs & 0xFFFF,
                 f->tf.eip, !!(f->tf.eflags & V86_IF), !!(f->tf.eflags & V86_TF), a, v->frame_pm, b, c);
        break;
    case DOSVM_TRACE_DB_PM: case DOSVM_TRACE_DB_RM:
        dosvm_diag(v, "group=vm_db vm=%u gen=%u n=%u pm=%u cs=%04x eip=%08x dr6=%08x cause=%08x tf=%u if=%u vif=%u armed=%u step=%s hits=%u",
                 v->info.key.id, v->info.key.generation, n, pm, f->tf.cs & 0xFFFF, f->tf.eip, a, b,
                 !!(f->tf.eflags & V86_TF), !!(f->tf.eflags & V86_IF), v->monitor.vif, v->dpmi.if_armed,
                 dosvm_step_owner(v, f, b), v->perf.return_hits);
        break;
    case DOSVM_TRACE_FLAGS:
        dosvm_diag(v, "group=vm_if vm=%u gen=%u n=%u restore=%u matched=%u cs=%04x eip=%08x vif=%u armed=%u seed=%u seed_cs=%04x seed_ip=%08x",
                 v->info.key.id, v->info.key.generation, n, a, b, f->tf.cs & 0xFFFF, f->tf.eip,
                 v->monitor.vif, v->dpmi.if_armed, v->dpmi.decode_valid, v->dpmi.decode_cs, v->dpmi.decode_eip);
        break;
    case DOSVM_TRACE_TIMER: {
        uint32_t flags = irq_save();
        uint64_t tick = g_ticks, clocks = v->pit.clocks, edges = v->pit.edges;
        uint32_t delivered = v->monitor.stats.delivered[0];
        struct v86_pic pic = v->monitor.pic[0];
        irq_restore(flags);
        dosvm_diag(v, "group=vm_clock vm=%u gen=%u n=%u tick=%llu clocks=%llu edges=%llu delivered=%u reload=%u",
                 v->info.key.id, v->info.key.generation, n, tick, clocks, edges, delivered, v->pit.channel[0].reload);
        dosvm_diag(v, "group=vm_gate vm=%u gen=%u n=%u pm=%u cs=%04x eip=%08x if=%u vif=%u armed=%u shadow=%u irr=%02x isr=%02x imr=%02x",
                 v->info.key.id, v->info.key.generation, n, pm, f->tf.cs & 0xFFFF, f->tf.eip,
                 !!(f->tf.eflags & V86_IF), v->monitor.vif, v->dpmi.if_armed,
                 v->monitor.shadow || v->dpmi.shadow, pic.irr, pic.isr, pic.imr);
        break;
    }
    case DOSVM_TRACE_FAULT: {
        const struct dpmi_record *r = v->dpmi.depth ? &v->dpmi.rec[v->dpmi.depth - 1] : 0;
        rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_fault_pc vm=%u gen=%u pm=%u vec=%u cs=%04x eip=%08x linear=%08x address=%08x error=%x depth=%u kind=%u flags=%x",
                 v->info.key.id, v->info.key.generation, pm, v->info.fault_vector,
                 f->tf.cs & 0xFFFF, f->tf.eip, v->info.fault_eip, v->info.fault_address, v->info.fault_error,
                 v->dpmi.depth, r ? r->kind : 0, r ? r->flags : 0);
        break;
    }
    default: break;
    }
}

bool dosvm_admit(uint64_t free_bytes, uint64_t reserved, uint64_t resident,
                 uint32_t request, struct dosvm_admission *out)
{
    uint64_t outstanding = reserved >= resident ? reserved - resident : UINT64_MAX;
    bool ok = request >= DOSVM_LOW_CAP && request <= DOSVM_RESIDENT_CAP &&
              !(request & (PAGE_SIZE - 1)) && outstanding <= free_bytes &&
              (uint64_t)request + DOSVM_HEADROOM <= free_bytes - outstanding;
    if (out) *out = (struct dosvm_admission){free_bytes, reserved, resident,
        outstanding, request, DOSVM_HEADROOM, ok};
    return ok;
}

struct dosvm *dosvm_lookup(struct dosvm_key key)
{
    if (!key.id || key.id > DOSVM_LIMIT || !key.generation) return 0;
    struct dosvm *v = &slots[key.id - 1];
    return gen_matches(v->info.key.generation, key.generation) ? v : 0;
}
struct dosvm_key dosvm_key(const struct dosvm *v) { return v->info.key; }
bool dosvm_valid(struct dosvm_key key)
{
    struct dosvm *v = dosvm_lookup(key);
    return v && v->info.state < DOSVM_STOPPING;
}
void dosvm_info(const struct dosvm *v, struct dosvm_info *out)
{
    *out = v->info;
    out->pool_resident = v->memory.pool.resident * PAGE_SIZE;
    out->pool_swapped = v->memory.pool.swapped * PAGE_SIZE;
    out->pool_pinned = v->memory.pool.pinned * PAGE_SIZE;
}
void dosvm_snapshot(struct dosvm_ledger *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct dosvm *v = &slots[i];
        if (initializing[i]) {
            out->vms++; out->reserved += initializing[i];
            continue;
        }
        if (!v->info.key.generation || v->info.state == DOSVM_DEAD) continue;
        out->vms++;
        out->tasks += v->task != 0;
        /* Count owned backing once, including unmapped EMS-window backing.
         * f3-46: pool frames only while resident, plus the object records. */
        const struct vm_pool *pool = &v->memory.pool;
        if (v->as.pd_phys) out->pages += DOSVM_PAGES + pool->resident + pool->dir_pages + v->as.tables + 1;
        out->pages += v->audio.q.page_count; /* f3-05 PCM/event queues */
        out->pages += v->read_pages + v->save.index_pages;
        out->committed += pool->commit; out->swapped += pool->swapped;
        out->pinned += pool->pinned; out->cache += v->read_pages;
        out->overlay_rows += v->sector_count;
        out->handles += v->base_open + v->overlay_open;
        out->devices += 4; /* authoritative PIC, PIT, text/input service and VGA adapter */
        out->reserved += v->info.commitment;
        out->resident += v->info.resident;
    }
}

/* The VM worker owns monitor/VGA transactions. Only independent callers
 * (creation, controller copies/text snapshots) may schedule inside a copy.
 * Disk gate holders must finish seek/transfer/cache publication first. */
static bool copy_preemptible(const struct dosvm *v)
{ return g_current && g_current != v->task && !v->disk_busy; }

bool dosvm_range(const struct dosvm *v, uint32_t linear, unsigned bytes, bool write)
{
    uint32_t end=VM_POOL_BASE+v->memory.quota;
    bool low=linear<DOSVM_LINEAR_END && bytes<=DOSVM_LINEAR_END-linear;
    bool pool=linear>=VM_POOL_BASE && linear<=end && bytes<=end-linear;
    if ((!low && !pool) || !v->as.pd_phys)
        return false;
    unsigned pages = 0;
    while (bytes) {
        unsigned n = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (n > bytes) n = bytes;
        /* f3-46: a committed pool page or a mapped EMS-window alias is
         * valid whether or not it is resident now; a present translation
         * keeps its own permission check. */
        if (!(linear >= 0xA0000 && linear < 0xC0000) && !as_range_ok(&v->as, linear, n, write) &&
            !(dosvm_pool_candidate(&v->memory.pool, linear) && dosvm_pool_range(v, linear, 0) &&
              !as_lookup(&v->as, linear, 0))) return false;
        linear += n; bytes -= n;
        if (!(++pages & 15u) && copy_preemptible(v)) kpreempt_point();
    }
    return true;
}
/* f3-46: a pool or EMS-window page without a present translation, and
 * every pool page a kernel write stores into, resolves through the pool
 * object, one page at a time, immediately before that page's bytes move (no
 * yield in between). wait=false (virtual DMA in the clock work) never
 * materializes: a page that is not resident returns -EAGAIN with nothing
 * copied from it. */
static int64_t guest_frame(struct dosvm *v, uint32_t linear, bool write, bool wait)
{
    /* Returns the frame address, or -EAGAIN / -EFAULT (no address-taken
     * local on the callers' present-translation fast path). */
    uint32_t phys = 0;
    int e = dosvm_pool_candidate(&v->memory.pool, linear) ? dosvm_pool_frame(v, linear, write, wait, &phys) : -ENOENT;
    if (e == -ENOENT) { phys = as_lookup(&v->as, linear, 0); return phys ? (int64_t)phys : -EFAULT; }
    return e == -EAGAIN ? e : e ? -EFAULT : (int64_t)phys;
}
static int transfer(struct dosvm *v, uint32_t linear, void *buffer, unsigned bytes, bool write, bool wait)
{
    if (!dosvm_range(v, linear, bytes, write)) return -EFAULT;
    uint8_t *b = buffer;
    while (bytes) {
        unsigned n = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (n > bytes) n = bytes;
        uint32_t window;
        /* Range test inline: DMA and other low-memory copies skip the call
         * (IRQ budget of the per-VM audio clock). */
        if (linear >= 0xA0000 && linear < 0xB0000 && dosvm_vbe_window(v, linear, &window)) {
            /* war2vbe: a windowed VBE mode's A0000h-AFFFFh is 64 KiB of the
             * VBE buffer (4F05h bank). Bank and page boundaries coincide, so
             * this page-bounded span is one buffer page; a kernel store sets
             * no PTE D bit, so mark the page for the presenter. */
            int e = transfer(v, window, b, n, write, wait);
            if (e) return e;
            if (write) dosvm_vbe_mark(v, (window - v->vbe_buffer.base) / PAGE_SIZE, 1);
            b += n; linear += n; bytes -= n; continue;
        }
        /* The VBE buffer is a pool allocation. Conventional-memory DMA
         * never names it, even while a direct framebuffer is active. */
        if (linear >= VM_POOL_BASE && v->vbe_direct.pages) {
            int direct=dosvm_vbe_direct_transfer(v,linear,b,n,write);
            if (direct<0) return direct;
            if (direct) {
                b+=n; linear+=n; bytes-=n;
                if (bytes && copy_preemptible(v)) kpreempt_point();
                continue;
            }
        }
        if (linear >= 0xA0000 && linear < 0xC0000) {
            /* fx-22: only the adapter's owner continuation or the switch
             * service touches the physical aperture; its writes are dirty. */
            if (v->fullscreen && !v->vbe_mode && !v->fullscreen_model &&
                dosvm_fullscreen_aperture(v, linear, n, write)) {
                volatile uint8_t *aperture = P2V(linear);
                for (unsigned i=0;i<n;i++) {
                    if (write) aperture[i]=b[i]; else b[i]=aperture[i];
                    if (!((i + 1) & 255u) && copy_preemptible(v)) kpreempt_point();
                }
                b+=n; linear+=n; bytes-=n; continue;
            }
            if (write) { v->perf.trapped_bytes += n; dosvm_launch_video(v); }
            dosvm_vga_model(v); /* f3-07: harvest before model peeks/stores */
            /* Kernel reads peek (no latch load); guest cycles use the VGA bus. */
            for (unsigned i = 0; i < n; i++) {
                if (write) cvga_write_vram(&v->vga, linear + i, b[i]);
                else b[i] = cvga_peek_vram(&v->vga, linear + i);
                if (!((i + 1) & 255u) && copy_preemptible(v)) kpreempt_point();
            }
            b += n; linear += n; bytes -= n; continue;
        }
        /* A present translation names the current frame (no yield before
         * the copy). A pool write still goes through the object: the dirty
         * version must advance (a kernel store sets no PTE D bit) before a
         * clean copy could be reused. */
        uint32_t phys = as_lookup(&v->as, linear, 0);
        if (write ? dosvm_pool_candidate(&v->memory.pool, linear)
                  : !phys && dosvm_pool_candidate(&v->memory.pool, linear)) {
            int64_t r = guest_frame(v, linear, write, wait);
            if (r < 0) return (int)r;
            phys = (uint32_t)r;
            /* Pool resolution can yield to the display worker. Recheck
             * before borrowing RAM; entry may now have mapped the LFB.
             * The resident path above cannot yield, so its first check
             * remains authoritative (notably for clock-task DMA reads). */
            int direct=v->vbe_direct.pages ? dosvm_vbe_direct_transfer(v,linear,b,n,write) : 0;
            if (direct<0) return direct;
            if (direct) {
                b+=n; linear+=n; bytes-=n;
                if (bytes && copy_preemptible(v)) kpreempt_point();
                continue;
            }
        }
        if (!phys) return -EFAULT;
        if (write) memcpy(P2V(phys), b, n);
        else memcpy(b, P2V(phys), n);
        b += n; linear += n; bytes -= n;
        if ((bytes || n == PAGE_SIZE) && copy_preemptible(v)) kpreempt_point();
    }
    return 0;
}
int dosvm_read(struct dosvm *v, uint32_t linear, void *b, unsigned n) { return transfer(v, linear, b, n, false, true); }
int dosvm_write(struct dosvm *v, uint32_t linear, const void *b, unsigned n) { return transfer(v, linear, (void *)b, n, true, true); }
int dosvm_read_resident(struct dosvm *v, uint32_t linear, void *b, unsigned n)
{ return transfer(v, linear, b, n, false, false); }
static int monitor_copy(void *arg, uint32_t linear, void *b, unsigned n, bool write)
{ return transfer(arg, linear, b, n, write, true); }
static void *monitor_memory(void *arg, uint32_t linear, unsigned n, bool write)
{
    struct dosvm *v = arg;
    /* The monitor only dereferences one-byte fetches itself. Larger ranges
     * use monitor_copy; this pointer is solely its validation token. The
     * fetch precedes the instruction's effects: a pool alias may resolve. */
    if (!dosvm_range(v, linear, n, write)) return 0;
    if (v->vbe_direct.pages && linear-v->vbe_direct.base<v->vbe_direct.pages*PAGE_SIZE) {
        /* Never lend an MMIO pointer across a possible ownership change.
         * Only byte fetches dereference this token; actual data transfers
         * use monitor_copy's per-page, IRQ-excluded provider access. */
        return dosvm_vbe_direct_transfer(v,linear,&v->vbe_direct.fetch,1,false)==1 ? &v->vbe_direct.fetch : 0;
    }
    if (linear >= 0xA0000 && linear < 0xC0000) return P2V(as_lookup(&v->as, linear, 0));
    int64_t phys = guest_frame(v, linear, write, true);
    return phys < 0 ? 0 : P2V((uint32_t)phys);
}
static uint8_t absent_in(void *arg, uint16_t port) { (void)arg; (void)port; return 0xFF; }
static void absent_out(void *arg, uint16_t port, uint8_t value) { (void)arg; (void)port; (void)value; }

int dosvm_set_a20(struct dosvm *v, bool enabled)
{
    if (!v->as.pd_phys) return -EFAULT;
    for (unsigned i = 0; i < 16; i++) {
        int e = as_map(&v->as, 0x100000 + i * PAGE_SIZE,
                       v->frames[enabled ? 256 + i : i], PTE_U | PTE_W);
        if (e) return e; /* table already exists: no allocation here */
    }
    v->a20 = enabled;
    return 0;
}

static struct dosvm *owner(const struct task *t)
{
    for (unsigned i = 0; i < DOSVM_LIMIT; i++)
        if (slots[i].task == t && t) return &slots[i];
    return 0;
}
/* Policy and source notes: validation/2026-10-14-f3-17-host.md. An idle
 * indication removes the focus reservation, including its event handling
 * tail, but expires promptly if the guest starts doing useful work again. */
static bool idle_recent(const struct dosvm *v)
{
    return v->idle.sleeping || (v->idle.seen && g_ticks - v->idle.last_tick < DOSVM_IDLE_WINDOW);
}
/* f3-04 scheduler hook: the incoming worker's LDT and watchpoints, or none. */
void dosvm_task_switch(const struct task *t)
{
    dosvm_fullscreen_switch(t);
    struct dosvm *v = owner(t);
    perf_switch(v);
    if (v && v->dpmi.active) {
        /* Native #DB signal handling need not inspect DR6. Its sticky BS
         * must not become this VM's next breakpoint cause (SDM 18.2.3).
         * A VM trap captures its DR6 before STI/scheduling, so clearing at
         * the incoming context boundary cannot discard that trap's cause. */
        arch_debug_take();
        uint32_t address[4] = {0};
        for (unsigned i = 0; i < DPMI_WATCH; i++) address[i] = v->dpmi.watch[i].addr;
        arch_ldt_load(v->dpmi.ldt, DPMI_LDT_ENTRIES);
        arch_debug_load(address, v->dpmi.dr7);
    } else {
        arch_ldt_load(0, 0);
        arch_debug_load((const uint32_t[4]){0}, 0);
    }
}
uint32_t dosvm_task_quantum(const struct task *t, uint32_t normal)
{
    struct dosvm *v = owner(t);
    if (!v || !v->owner_pid || !v->window_input) return normal;
    if (idle_recent(v)) return normal < 1u ? normal : 1u;
    /* Native VGA removes most I/O/page-fault exits. Keep a busy audio guest's
     * dispatch bounded while the P_DEVICE refill worker and IRQ delivery
     * share the CPU; the physical timer still owns virtual DMA progress. */
    if (v->fullscreen && (v->audio.sb.x.active || v->audio.q.event_count))
        return normal < 2u ? normal : 2u;
    bool focused = input_focus.id == v->info.key.id && input_focus.generation == v->info.key.generation;
    if (focused) return normal < 8u ? normal : 8u;
    return normal < 4u ? normal : 4u;
}
const struct task *dosvm_task_focus(void)
{
    struct dosvm *v = dosvm_lookup(input_focus);
    return v && v->owner_pid && v->window_input && v->info.state < DOSVM_STOPPING &&
           v->info.state != DOSVM_BLOCKED && !v->window_marking && !v->fullscreen_suspended && !idle_recent(v) ? v->task : 0;
}
const struct task *dosvm_task_irq(void)
{
    struct dosvm *v = dosvm_lookup(input_focus);
    if (!v || !v->owner_pid || !v->window_input || v->info.state >= DOSVM_STOPPING ||
        v->info.state == DOSVM_BLOCKED)
        return 0;
    /* Dispatch also helps the guest finish CLI/STI-shadow/IRQ-handler work.
     * This is only a scheduling hint: virtual IF and acknowledgement rules
     * still belong entirely to the monitor's existing delivery path. */
    int irq = dosvm_pic_pending(&v->monitor);
    return irq == 0 || irq == DOSVM_SB_IRQ ? v->task : 0;
}
bool dosvm_client(const struct task *t)
{
    struct dosvm *v = owner(t);
    return v && v->continuation && v->dpmi.active;
}
bool dosvm_dpmi_info(struct dosvm_key key, struct dpmi_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !v->info.key.generation || v->info.state == DOSVM_DEAD) return false;
    dpmi_info_fill(v, out);
    return true;
}
uint32_t dosvm_task_cr3(const struct task *t, uint32_t normal)
{
    struct dosvm *v = owner(t);
    /* Startup diagnostics can yield before the first v86_enter sets the
     * continuation. The worker owns this directory until it is reaped,
     * before as_destroy; only ESP0 depends on a live entry continuation. */
    return v ? v->as.pd_phys : normal;
}
uint32_t dosvm_task_esp0(const struct task *t, uint32_t normal)
{
    struct dosvm *v = owner(t);
    return v && v->continuation ? v->continuation : normal;
}
/* f3-61: Windows 9x closes an MS-DOS Prompt whose COMMAND.COM waits at its
 * prompt without the "still active" warning (the close-aware shell, RBIL
 * INT 2Fh/168Fh); only a running program is terminated with possible data
 * loss. Here: DOS ready, no DPMI client, no V86 EXEC frame outstanding and
 * the worker blocked in (or just back from) its idle wait. */
bool dosvm_shell_idle(const struct dosvm *v)
{
    return v->info.state < DOSVM_STOPPING && v->info.dos_ready && !v->dpmi.active && !v->dpmi.clients &&
        !v->exec_pending && !v->exec_overflow &&
        (v->idle.sleeping || (v->idle.seen && g_ticks - v->idle.last_tick < 250));
}
int dosvm_stop(struct dosvm_key key, enum dosvm_reason reason)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v) return -EINVAL;
    if (v->info.state >= DOSVM_STOPPING) return 0;
    v->info.reason = reason;
    v->info.state = DOSVM_STOPPING;
    if (dosvm_lpt_stop) dosvm_lpt_stop(v);
    dosvm_present_notify(v);
    dosvm_fullscreen_stopping(v);
    dosvm_vga_sync(v, true);
    v->mouse.mask = v->mouse.pending_mask = 0;
    v->mouse.active = false;
    if (input_focus.id == key.id && input_focus.generation == key.generation) input_focus = (struct dosvm_key){0};
    /* No new callback/port/input work can now validate this key. */
    kwait_wake_all(&v->wake);
    kwait_wake_all(&v->clipboard.wait);
    if (v->task && !v->started) task_kill(v->task, reason);
    return 0;
}

static bool wake_condition(void *arg)
{
    struct dosvm *v = arg;
    return v->info.state == DOSVM_STOPPING ||
           (v->monitor.vif && !v->monitor.shadow && !v->dpmi.shadow &&
            (v86_irq_pending(&v->monitor) >= 0 || (!v->frame_pm && v->mouse.pending_mask)));
}
void dosvm_idle_wake(struct dosvm *v)
{
    uint32_t flags = irq_save();
    v->idle.events++;
    kwait_wake_all(&v->wake);
    irq_restore(flags);
}
struct idle_wait {
    struct dosvm *v;
    uint32_t events;
    bool halt;
};
static bool idle_event(void *arg)
{
    struct idle_wait *w = arg;
    struct dosvm *v = w->v;
    return v->info.state >= DOSVM_STOPPING || v->idle.events != w->events ||
           (w->halt ? wake_condition(v) :
            (v86_irq_pending(&v->monitor) >= 0 || (!v->frame_pm && v->mouse.pending_mask) ||
             v->key_count || dosvm_kbd_irq_ready(v)));
}
static uint64_t idle_deadline(const struct dosvm *v)
{
    const struct dosvm_pit_channel *c = &v->pit.channel[0];
    uint32_t clocks = 65536;
    if (c->running && c->gate) {
        if (c->reload && c->phase < c->reload) clocks = c->reload - c->phase;
        if (c->pending && c->pending < clocks) clocks = c->pending;
        if (c->load) clocks = c->pending + 1; /* count transfer consumes a CLK */
    }
    uint32_t ms = (clocks + 1192u) / 1193u;
    if (!ms) ms = 1;
    if (ms > 55) ms = 55;
    uint64_t until = g_ticks + ms;
    if (!v->info.dos_ready && v->boot_deadline - g_ticks < ms) until = v->boot_deadline;
    return until;
}
static void idle_wait(struct dosvm *v, enum dosvm_idle_reason reason)
{
    if (dosvm_print_service && (read_eflags()&0x200)) dosvm_print_service(v);
    /* COMMAND now announces an empty prompt with INT 28h rather than a
     * BIOS read. Preserve the launch gate's detection of returning to input
     * without ever producing the requested program's first frame. */
    if (reason == DOSVM_IDLE_28 && v->launch_enter_consumed && !v->launch_ready)
        v->launch_failed = true;
    dosvm_input_poll(v);
    uint32_t flags = irq_save();
    struct idle_wait w = {v, v->idle.events, reason == DOSVM_IDLE_HLT};
    uint64_t until = idle_deadline(v);
    bool block = !idle_event(&w);
    if (block) {
        v->idle.seen = v->idle.sleeping = true;
        v->idle.last_tick = g_ticks;
        v->idle.blocks++; v->idle.reasons |= 1u << reason; v->idle.count[reason]++;
        v->info.state = DOSVM_BLOCKED;
    }
    irq_restore(flags);
    if (!block) return;
    /* kwait atomically rechecks both pending state and the event sequence
     * while registering: no input/IRQ is lost between this test and sleep.
     * f3-46: a blocked guest is a published continuation; its pages are
     * pageable while it sleeps (memory accessed later resolves again). */
    uint32_t hold = v->memory.pool.hold;
    v->memory.pool.hold = 0;
    kwait_wait_until(&v->wake, idle_event, &w, until);
    v->memory.pool.hold = hold;
    flags = irq_save();
    v->idle.sleeping = false;
    v->idle.last_tick = g_ticks;
    if (v->info.state == DOSVM_BLOCKED)
        v->info.state = v->info.dos_ready ? DOSVM_RUNNABLE : DOSVM_BOOTING;
    irq_restore(flags);
    dosvm_clipboard_checkpoint(v);
}
static bool idle_safe(const struct dosvm *v)
{
    return v->monitor.vif && !v->monitor.shadow && !v->dpmi.shadow &&
           !dosvm_in_irq(v) && !v->monitor.pic[0].isr && !v->monitor.pic[1].isr;
}
static void idle_track_poll(struct dosvm *v, const struct v86_frame *f, bool pm)
{
    unsigned fn = (uint8_t)(f->tf.eax >> 8);
    if ((fn != 1 && fn != 0x11) || v->idle.depth == ARRAY_SIZE(v->idle.poll)) return;
    unsigned i = v->idle.depth++;
    v->idle.poll[i].cs = f->tf.cs; v->idle.poll[i].ip = f->tf.eip;
    v->idle.poll[i].ss = f->tf.user_ss;
    v->idle.poll[i].sp = pm ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
    v->idle.poll[i].pm = pm;
}
static void idle_poll_return(struct dosvm *v, const struct v86_frame *f)
{
    for (unsigned n = v->idle.depth; n; n--) {
        unsigned i = n - 1;
        if (v->frame_pm != v->idle.poll[i].pm || f->tf.cs != v->idle.poll[i].cs || f->tf.eip != v->idle.poll[i].ip ||
            f->tf.user_ss != v->idle.poll[i].ss ||
            (v->frame_pm ? f->tf.user_esp : (uint16_t)f->tf.user_esp) != v->idle.poll[i].sp) continue;
        v->idle.depth = i;
        if (!(f->tf.eflags & V86_ZF)) { v->idle.polls = 0; return; }
        if (!idle_safe(v)) return;
        if (v->idle.poll_tick != g_ticks) { v->idle.poll_tick = g_ticks; v->idle.polls = 0; }
        unsigned threshold=v->window_idle ? 1u+(101u-v->window_idle)*15u/100u : DOSVM_IDLE_POLLS;
        if (++v->idle.polls >= threshold) {
            v->idle.polls = 0;
            idle_wait(v, DOSVM_IDLE_16);
        }
        return;
    }
}
void dosvm_input_poll(struct dosvm *v)
{
    if (v->owner_pid || v->window_input || !dosvm_valid(v->info.key) || input_focus.id != v->info.key.id ||
        !gen_matches(input_focus.generation, v->info.key.generation)) return;
    struct input_event event;
    unsigned budget = 32;
    while (budget-- && v->key_count < ARRAY_SIZE(v->keyboard) && input_read(&event)) {
        if (event.tick - v->input_since >= (1ull << 63)) continue;
        if (event.type == INPUT_TEXT && event.code && event.code < 128)
            dosvm_keyboard(v->info.key, event.code);
    }
}
int dosvm_wait(struct dosvm *v, bool keyboard, uint64_t deadline)
{
    if (keyboard && v->launch_enter_consumed && !v->launch_ready) v->launch_failed = true;
    if (keyboard) {
        if (dosvm_keyboard_refresh(v)) return -EFAULT;
        idle_wait(v, DOSVM_IDLE_16);
        dosvm_fullscreen_checkpoint(v);
        return v->info.state == DOSVM_STOPPING ? -V86_EIO : 0;
    }
    v->info.state = DOSVM_BLOCKED;
    while (v->info.state != DOSVM_STOPPING) {
        if (deadline_passed(deadline)) break;
        if (!v->info.dos_ready && deadline_passed(v->boot_deadline)) {
            dosvm_stop(v->info.key, DOSVM_TIMEOUT);
            break;
        }
        /* INT 15h timed wait; keyboard waits use the event queue above. */
        dosvm_fullscreen_checkpoint(v);
        uint32_t hold = v->memory.pool.hold;
        v->memory.pool.hold = 0;
        task_sleep_ms(1);
        v->memory.pool.hold = hold;
    }
    if (v->info.state == DOSVM_STOPPING) return -V86_EIO;
    v->info.state = v->info.dos_ready ? DOSVM_RUNNABLE : DOSVM_BOOTING;
    return 0;
}

/* Every trap of the worker's guest, V86 or (f3-04) a ring-3 DPMI client.
 * The hardware frame is copied into v->frame, the single working copy all
 * emulation, dispatch and mode switches use; it goes back to the stack when
 * the mode is unchanged, otherwise the worker re-enters with the new frame
 * (a PM trap_frame and a V86 frame differ in size on the same TSS stack). */
/* The physical IRQ handler runs before dosvm_trap. End guest time at its
 * entry so DMA/PIT/device work is not billed as native guest execution. */
void dosvm_perf_enter(void)
{
    struct dosvm *v = owner(g_current);
    if (v && v->continuation && v->perf.phase == 1) perf_phase(v, 2);
}

/* f3-46: IRQ arbitration and frame construction run with physical IF clear
 * (one bounded transaction, dpmi_deliver/v86_irq_deliver) and may push on
 * the guest's current stack: a nested DPMI handler's own stack (f3-23
 * flat-stack ISRs) or a V86 stack inside an EMS window. Resolve that page
 * first, while blocking is still allowed; no yield separates the two. */
static void stack_prefault(struct dosvm *v, const struct v86_frame *f)
{
    if (!v->memory.pool.pages) return;
    uint32_t top;
    if (v->frame_pm) {
        struct dpmi_seg seg;
        if (!dpmi_seg(v, (uint16_t)f->tf.user_ss, &seg)) return;
        top = seg.base + (seg.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp);
    } else top = ((f->tf.user_ss & 0xFFFFu) << 4) + (uint16_t)f->tf.user_esp;
    uint32_t low = top >= 64 ? top - 64 : 0;
    if (top > low) (void)dosvm_pool_prefault(v, low, top - low, false);
}
/* Commit at eligible monitor boundaries, including the final IF-clear
 * return after scheduling/reporting. Physical PIC programming is unchanged. */
static int dosvm_deliver_irq(struct dosvm *v, struct v86_frame *f)
{
    dosvm_uart_service(v,g_ticks);
    if (dosvm_print_service && (read_eflags()&0x200)) dosvm_print_service(v);
    if (read_eflags() & 0x200) dosvm_network_service(v, g_ticks);
    stack_prefault(v, f);
    uint64_t delivered = v->monitor.stats.reflected_irqs;
    uint32_t sb_delivered = v->monitor.stats.delivered[v->audio.sb.irq & 15];
    int e = 0;
    uint32_t flags = irq_save();
    e = dosvm_network_dispatch(v, f);
    if (!v->frame_pm && !e) e = dosvm_mouse_dispatch(v, f);
    if (!v->dpmi.active && !e) e = v86_irq_deliver(&v->monitor, f);
    irq_restore(flags);
    if (v->dpmi.active && !e) e = dpmi_deliver(v, f);
    if (v->monitor.stats.delivered[v->audio.sb.irq & 15] != sb_delivered)
        v->audio.sb.irq_entry_ready = v->audio.sb.irq_entry_pending;
    if (v->monitor.stats.reflected_irqs != delivered) {
        v->trace.injected_cs = (uint16_t)f->tf.cs;
        v->trace.injected_eip = f->tf.eip;
    }
    return e;
}

bool dosvm_trap(struct trap_frame *tf)
{
    struct dosvm *v = owner(g_current);
    if (!v || !v->continuation) return false;
    crit_begin();
    if (crit_label) crit_label("dosvm_entry");
    bool pm = !(tf->eflags & V86_VM);
    if (v->perf.phase != 2) perf_phase(v, 2);
    v->perf.trap_active = true; v->perf.trap_cycles = 0; v->perf.io_valid = false;
    v->perf.cause = tf->vector == 1 ? DOSVM_COST_DB : tf->vector == 0x20 ? DOSVM_COST_TIMER :
        tf->vector > 0x20 && tf->vector < 0x30 ? DOSVM_COST_HOST_IRQ : DOSVM_COST_OTHER;
    unsigned cause = tf->vector == 13 ? 0 : tf->vector == 14 ? 1 : tf->vector == 1 ? 2 : tf->vector == 0x20 ? 3 : 4;
    v->perf.exits[cause]++;
    if (pm) v->perf.exits[5]++;
    /* CR2/DR6 belong to this trap only while interrupts stay off. */
    uint32_t cr2 = tf->vector == 14 ? read_cr2() : 0;
    uint32_t dr6 = tf->vector == 1 ? arch_debug_take() : 0;
    if (tf->vector == 14 && cr2 >= 0xA0000 && cr2 < 0xC0000) v->perf.cause = DOSVM_COST_VGA_PF;
    unsigned size = pm ? sizeof(struct trap_frame) : sizeof(struct v86_frame);
    struct v86_frame *f = &v->frame;
    memcpy(f, tf, size);
    v->frame_pm = pm;
    v->video_dac_burst = false;
    crit_end();
    sti();
    int lean_error = 0;
    int lean = tf->vector == 13 && !tf->err ? lean_io(v, f, &lean_error) : 0;
    if (lean == 1) {
        /* fx-27: the ordinary return sequence below, IF already clear. */
        if (!v->frame_pm) v86_prepare_return(&v->monitor, f);
        perf_exit_end(v);
        /* Guest time starts at the exit's closing stamp: one serialized
         * TSC read (two CPUIDs) fewer than perf_phase(v, 1). */
        v->perf.phase = 1;
        crit_begin();
        if (crit_label) crit_label("dosvm_return");
        memcpy(tf, f, size);
        return true;
    }
    /* f3-46: a not-present fault on a pool page or EMS-window alias is
     * resolved here, at the published continuation (the faulting
     * instruction restarts; nothing was emulated), before the transaction
     * hold: the wait for a frame may reclaim this VM's own cold pages. */
    int pool_fault = tf->vector == 14 && v->info.state != DOSVM_STOPPING ?
        dosvm_pool_fault(v, cr2, tf->err) : -ENOENT;
    dosvm_pool_hold(v, true);
    if (dosvm_file_deadline) dosvm_file_deadline(v);
    dpmi_irq_resync(v, f);
    dosvm_fullscreen_checkpoint(v);
    dosvm_fullscreen_progress(v,f->tf.cs,f->tf.eip);
    dosvm_pic_stall(v, f);
    dosvm_heartbeat(v, f);
    if (tf->vector == 1)
        dosvm_trace(v, pm ? DOSVM_TRACE_DB_PM : DOSVM_TRACE_DB_RM, f, dr6, dpmi_debug_status(v, dr6), 0);
    int e = pool_fault == -ENOENT ? 0 : pool_fault ? -EFAULT : 0;
    if (lean) e = lean_error;           /* retired above: only the tail runs */
    if (!lean && pool_fault == -ENOENT && v->info.state != DOSVM_STOPPING) {
        if (pm) {
            bool eligible = idle_safe(v);
            unsigned depth = v->dpmi.depth;
            bool soft = tf->vector == 0x80 || (tf->vector == 13 && (tf->err & 3) == 2);
            uint8_t vector = tf->vector == 0x80 ? 0x80 : (uint8_t)(tf->err >> 3);
            if (tf->vector == 0x80) v->perf.cause = DOSVM_COST_PM_INT;
            uint16_t ax = f->tf.eax;
            uint64_t begin = soft ? perf_monitor_stamp(v) : 0;
            e = dpmi_pm_trap(v, f, cr2, dr6);
            if (soft) {
                perf_vector_end(v, 2, vector, ax, begin);
                if (vector == 0x31 && v->perf.cause == DOSVM_COST_PM_INT)
                    v->perf.cause = DOSVM_COST_DPMI;
            }
            /* Default PM INTs reflect straight to the IVT, without passing
             * interrupt_service. Observe the committed reflection record;
             * keep the f3-15 decoder/dispatch and its frames unchanged. */
            if (e >= 0 && eligible && !v->frame_pm && v->dpmi.depth == depth + 1) {
                const struct dpmi_record *r = &v->dpmi.rec[depth];
                if (r->kind == DPMI_REC_REFLECT && !(r->flags & DPMI_RF_HWINT)) {
                    if (r->vector == 0x28 || (r->vector == 0x2F && (uint16_t)r->saved.tf.eax == 0x1680))
                        idle_wait(v, r->vector == 0x28 ? DOSVM_IDLE_28 : DOSVM_IDLE_2F);
                    if (r->vector == 0x16) idle_track_poll(v, &r->saved, true);
                }
            }
        }
        else if (tf->vector == 13) {
            bool was_vif = v->monitor.vif;
            uint64_t iret_count = v->monitor.stats.insn[V86_IRET];
            uint64_t popf_count = v->monitor.stats.insn[V86_POPF];
            uint64_t sti_count = v->monitor.stats.insn[V86_STI];
            v86_retire_shadow(&v->monitor, f);
            if (!dosvm_io_fast(v, f, 0, &e)) {
                uint64_t ints = v->monitor.stats.insn[V86_INT], irets = v->monitor.stats.insn[V86_IRET];
                uint64_t flags_insn[4] = {0}, hlts = 0;
                {
                    for (unsigned i = 0; i < 4; i++) flags_insn[i] = v->monitor.stats.insn[V86_PUSHF + i];
                    hlts = v->monitor.stats.insn[V86_HLT];
                }
                v->perf.cause = DOSVM_COST_RM_FLAGS;
                e = v86_emulate(&v->monitor, f);
                if (v->monitor.stats.insn[V86_INT] != ints) v->perf.cause = DOSVM_COST_RM_INT;
                if (v->monitor.stats.insn[V86_IRET] != irets) v->perf.cause = DOSVM_COST_RM_IRET;
                {
                    for (unsigned i = 0; i < 4; i++) if (v->monitor.stats.insn[V86_PUSHF + i] != flags_insn[i])
                        v->perf.cause = DOSVM_COST_RM_PUSHF + i;
                    if (v->monitor.stats.insn[V86_HLT] != hlts) v->perf.cause = DOSVM_COST_RM_HLT;
                }
            }
            if (!was_vif && v->monitor.vif) {
                if (v->monitor.stats.insn[V86_IRET] != iret_count) v->perf.if_enable[DOSVM_IF_IRET]++;
                else if (v->monitor.stats.insn[V86_POPF] != popf_count) v->perf.if_enable[DOSVM_IF_POPF]++;
                else if (v->monitor.stats.insn[V86_STI] != sti_count) v->perf.if_enable[DOSVM_IF_STI]++;
            }
            if (v->monitor.stats.insn[V86_IRET]!=iret_count && dosvm_file_return &&
                !(v->dpmi.depth && f->tf.cs==0xf000 && f->tf.eip==DPMI_RM_RETURN))
                dosvm_file_return(v,f);
            if (!e && !v->frame_pm && (v->perf.cause == DOSVM_COST_VGA_IO ||
                (v->perf.cause == DOSVM_COST_PIT_PIC && v->perf.io_port >= 0x40 &&
                 v->perf.io_port <= 0x43))) e = dosvm_port_bursts(v, f);
        }
        else if (tf->vector == 1 && (v->monitor.shadow || v->monitor.track_boundaries)) {
            dr6 = dpmi_debug_status(v, dr6);
            if (dr6 & 15) {
                dpmi_watch_hits(v, dr6);
                for (unsigned i = 0; i < DPMI_WATCH; i++)
                    if ((dr6 & (1u << i)) && v->dpmi.watch[i].type == 0) f->tf.eflags |= 1u << 16;
            }
            e = v86_debug_step(&v->monitor, f);
            dosvm_boundary(v, f, v->monitor.shadow ? "rm_ss" : "rm_step");
        }
        else if (tf->vector == 14 && cr2 >= 0xA0000 && cr2 < 0xC0000) {
            if (!dosvm_vga_retry(v, cr2, (f->tf.cs << 4) + f->tf.eip, false)) {
                e = dosvm_vga_fault(v, f);
                if (!e) dosvm_vga_burst(v, f);   /* f3-06: the rest of a store run */
            }
        }
        else if (tf->vector == 7) fpu_handle_nm();
        else if (tf->vector == 3 || tf->vector == 4) e = v86_reflect(&v->monitor, f, tf->vector);
        else if (tf->vector < 0x20 || tf->vector >= 0x30) e = -EFAULT;
    }
    if (!v->frame_pm) v86_retire_shadow(&v->monitor, f);
    /* Observe the completed DOS IRET before IRQ construction replaces this
     * return frame. No stepping or guest patching. */
    if (!v->frame_pm && v->launch_exec && f->tf.cs == v->launch_cs && f->tf.eip == v->launch_ip &&
        f->tf.user_ss == v->launch_ss && (uint16_t)f->tf.user_esp == v->launch_sp && (f->tf.eflags & V86_CF)) {
        v->launch_failed = true; v->launch_error = f->tf.eax & 0xFFFF;
    }
    if (!v->frame_pm && v->exec_pending) {
        const uint8_t top = v->exec_pending - 1;
        if (f->tf.cs == v->exec_frame[top].cs && f->tf.eip == v->exec_frame[top].ip &&
            f->tf.user_ss == v->exec_frame[top].ss && (uint16_t)f->tf.user_esp == v->exec_frame[top].sp) {
            v->exec_pending = top;
            if (v->launch_exec && !(f->tf.eflags & V86_CF) && f->tf.cs == v->launch_cs && f->tf.eip == v->launch_ip &&
                f->tf.user_ss == v->launch_ss && (uint16_t)f->tf.user_esp == v->launch_sp) {
                v->launch_returned = true; v->launch_returned_tick = g_ticks;
                dosvm_present_notify(v);
            }
        }
    }
    /* Restored IF can expose an audio edge now. Commit it before any idle
     * wait, preemption point or UART/report work. */
    if (!e && v->info.state < DOSVM_STOPPING && v->monitor.state != V86_HALTED)
        e = dosvm_deliver_irq(v, f);
    if (e >= 0 && v->info.state < DOSVM_STOPPING) idle_poll_return(v, f);
    /* f3-46: the instruction is complete. Virtual DMA pins follow the
     * device state just programmed; then the VM's pages are pageable again. */
    if (v->info.state < DOSVM_STOPPING) dosvm_pool_settle(v);
    dosvm_pool_hold(v, false);
    dosvm_present_checkpoint(v);
    dosvm_clipboard_checkpoint(v);
    kpreempt_point(); /* complete emulation transaction, before diagnostics */
    dosvm_trace_flush(v);
    if (!dosvm_in_irq(v)) dosvm_audio_log(v);
    if (e < 0) {
        v->info.fault_vector = tf->vector;
        v->info.fault_eip = pm ? dpmi_fault_linear(v, (const struct v86_frame *)tf) : (tf->cs << 4) + tf->eip;
        v->info.fault_address = cr2;
        v->info.fault_error = tf->err;
        dosvm_trace(v, DOSVM_TRACE_FAULT, (const struct v86_frame *)tf, 0, 0, 0);
        dosvm_stop(v->info.key, DOSVM_FAULT);
    }
    if (!v->info.dos_ready && deadline_passed(v->boot_deadline))
        dosvm_stop(v->info.key, DOSVM_TIMEOUT);
    while (v->info.state != DOSVM_STOPPING) {
        if (!v->info.dos_ready && deadline_passed(v->boot_deadline)) {
            dosvm_stop(v->info.key, DOSVM_TIMEOUT);
            break;
        }
        e = dosvm_deliver_irq(v, f);
        if (e < 0) {
            v->info.fault_vector = tf->vector;
            v->info.fault_eip = dpmi_fault_linear(v, f);
            dosvm_trace(v, DOSVM_TRACE_FAULT, f, 0, 0, 0);
            dosvm_stop(v->info.key, DOSVM_FAULT);
            break;
        }
        if (v->monitor.state != V86_HALTED) break;
        v->info.state = DOSVM_BLOCKED;
        if (v->monitor.vif) idle_wait(v, DOSVM_IDLE_HLT);
        else kwait_wait_until(&v->wake, wake_condition, v, deadline_after_ms(10));
        dosvm_pic_stall(v, f);
        dosvm_heartbeat(v, f); /* a guest HLT with IF clear also needs samples */
        dosvm_startup_report(v, false);
        if (!dosvm_in_irq(v)) dosvm_audio_log(v);
    }
    dosvm_fullscreen_checkpoint(v);
    /* A protected-mode frame must let the kernel IRET succeed. */
    if (v->info.state != DOSVM_STOPPING && v->frame_pm && !dpmi_frame_check(v, f)) {
        v->info.fault_vector = 13;
        v->info.fault_eip = dpmi_fault_linear(v, f);
        v->dpmi.stats.faults++;
        dosvm_trace(v, DOSVM_TRACE_FAULT, f, 0, 0, 0);
        dosvm_stop(v->info.key, DOSVM_FAULT);
    }
stop_return:
    if (v->info.state == DOSVM_STOPPING) {
        dosvm_trace_flush(v);
        if (v->frame_pm != pm) v->perf.cause = DOSVM_COST_SWITCH;
        perf_exit_end(v);
        cli();
        crit_begin();
        if (crit_label) crit_label("dosvm_leave");
        v86_leave(v->continuation);
    }
    if (v->info.state == DOSVM_BLOCKED)
        v->info.state = v->info.dos_ready ? DOSVM_RUNNABLE : DOSVM_BOOTING;
    if (tf->vector == 0x20 && v->dpmi.active)
        dosvm_trace(v, DOSVM_TRACE_TIMER, f, 0, 0, 0);
    if (g_need_resched) schedule();
    dosvm_startup_report(v, false);
    dosvm_perf_report(v);
    if (v->monitor.vif) stack_prefault(v, f);
    cli();
    /* A tick may have arrived in a checkpoint, scheduler or report above.
     * With physical IF clear there is no new gap between this and IRET. */
    e = v->monitor.vif && dosvm_pic_pending(&v->monitor) >= 0 ? dosvm_deliver_irq(v, f) : 0;
    if (e < 0) {
        sti();
        v->info.fault_vector = tf->vector;
        v->info.fault_eip = dpmi_fault_linear(v, f);
        dosvm_trace(v, DOSVM_TRACE_FAULT, f, 0, 0, 0);
        dosvm_stop(v->info.key, DOSVM_FAULT);
        goto stop_return;
    }
    if (!v->frame_pm) v86_prepare_return(&v->monitor, f);
    if (v->frame_pm != pm) v->perf.cause = DOSVM_COST_SWITCH;
    perf_exit_end(v);
    perf_phase(v, 1);
    /* A mode-changing frame re-enters through worker after v86_leave's STI.
     * Keep its timestamp pending across any scheduling in that interval. */
    if (v->frame_pm == pm && v->audio.sb.irq_entry_ready)
        dosvm_audio_irq_enter(v, v->audio.sb.irq);
    crit_begin();
    if (crit_label) crit_label("dosvm_return");
    if (v->frame_pm != pm) v86_leave(v->continuation);
    memcpy(tf, f, size);
    return true;
}

static int interrupt_dispatch(void *arg, struct v86_frame *f, uint8_t vector)
{
    struct dosvm *v = arg;
    /* fx-10: MSCDEX (AH=15h) before the guest's INT 2Fh chain, whose
     * FreeDOS handler leaves BX=0 ("MSCDEX not loaded"). */
    if (vector==0x2f) {
        int clip=dosvm_clipboard_interrupt(v,f);
        if (clip!=-ENOSYS) return clip;
    }
    if (vector == 0x2f && dosvm_cdaudio_interrupt) {
        int cd = dosvm_cdaudio_interrupt(v, f);
        if (cd != -ENOSYS) return cd;
    }
    if (vector == 0x2f || vector == 0x7a || vector == 0xfc) {
        int network = dosvm_network_interrupt(v, f, vector);
        if (network != -ENOSYS) return network;
    }
    if (dosvm_file_interrupt && (vector==0x21 || vector==0x2f || vector==0xfe)) {
        int file=dosvm_file_interrupt(v,f,vector,0); /* DOS_FILE_V86 */
        if (file!=-ENOSYS) return file;
    }
    if ((vector == 0x28 || vector == 0x2F || vector == 0x16) && idle_safe(v)) {
        if (vector == 0x28 || (vector == 0x2F && (uint16_t)f->tf.eax == 0x1680))
            idle_wait(v, vector == 0x28 ? DOSVM_IDLE_28 : DOSVM_IDLE_2F);
        if (vector == 0x16) idle_track_poll(v, f, false);
    }
    if (vector == 0x21 && (f->tf.eax & 0xFFFF) == 0x4B00 && v->launch_armed && !v->launch_exec) {
        v->launch_exec = true;
        v->launch_cs = (uint16_t)f->tf.cs; v->launch_ip = f->tf.eip;
        v->launch_ss = (uint16_t)f->tf.user_ss; v->launch_sp = (uint16_t)f->tf.user_esp;
    }
    if (vector == 0x21 && (f->tf.eax & 0xFFFF) == 0x4B00 && v->info.dos_ready) {
        /* f3-61: a program now runs above its parent until the kernel's IRET
         * completes this frame; a later EXEC means the armed program's return
         * no longer leaves only the shell running. Boot-time EXECs (the
         * kernel's own load of its shell, which never returns, and AUTOEXEC
         * programs before DOS ready) are not user programs. */
        if (v->exec_pending < ARRAY_SIZE(v->exec_frame)) {
            v->exec_frame[v->exec_pending].cs = (uint16_t)f->tf.cs; v->exec_frame[v->exec_pending].ip = f->tf.eip;
            v->exec_frame[v->exec_pending].ss = (uint16_t)f->tf.user_ss;
            v->exec_frame[v->exec_pending].sp = (uint16_t)f->tf.user_esp;
            v->exec_pending++;
        } else v->exec_overflow = true;
        v->launch_returned = false;
    }
    /* XMS 3.0 installation check and control-function address (2Fh/4300h,
     * 4310h) in front of the guest's INT 2Fh chain, as HIMEM.SYS hooks INT 2Fh
     * after the DOS kernel and Windows 98's V86MMGR serves XMS in every VM.
     * The FreeDOS kernel's handler returns AH=43h unchained (int2f.asm
     * Int2f?iret), so the ROM default answer was unreachable: no program saw
     * XMS and the XMS-swap FreeCOM stayed resident (tests/host/dos/dos_memory.py). */
    if (vector == 0x2F) {
        int xms = dosvm_xms_multiplex(v, f);
        if (xms != -ENOSYS) return xms;
    }
    if (vector == 0xFD) return dosvm_mouse_return(v, f);
    if (vector != 0xFE) return dpmi_v86_int(v, f, vector);
    int stub = dpmi_v86_stub(v, f);
    if (stub != -ENOSYS) return stub;
    if (f->tf.cs == VM_XMS_SEG && f->tf.eip == 4) return dosvm_xms(v, f);
    if (f->tf.cs == VM_XMS_SEG && f->tf.eip == 18) return dosvm_ems_return(v, f);
    /* Only synthetic ROM stubs may enter the private firmware service.
     * Original INT already pushed its own FLAGS/CS/IP; no second INT frame. */
    uint8_t service;
    if (f->tf.cs == 0xF100 && f->tf.eip == 2) service = 0x67; /* EMS stub page */
    else if (f->tf.cs != 0xF000 || (f->tf.eip & 15) != 2 || f->tf.eip >= 0x1000) return -V86_EPERM;
    else service = (uint8_t)(f->tf.eip >> 4);
    /* Simulated interrupts and PM reflection arrive through the IVT's
     * synthetic INT FEh; leave their original IRET frame intact. */
    if (service == 0x2f || service == 0x7a) {
        int network = dosvm_network_interrupt(v, f, service);
        if (network != -ENOSYS) return network;
    }
    uint32_t off = (uint16_t)f->tf.user_esp;
    if (off > 0xFFFA || f->tf.user_ss > 0xFFFF) return -EFAULT;
    uint32_t linear = (f->tf.user_ss << 4) + off + 4;
    uint16_t saved;
    if (dosvm_read(v, linear, &saved, 2)) return -EFAULT;
    f->tf.eflags = (f->tf.eflags & ~(V86_CF | V86_ZF)) | (saved & (V86_CF | V86_ZF));
    uint16_t ax = f->tf.eax;
    uint64_t begin = perf_monitor_stamp(v);
    int e = dosvm_bios(v, f, service);
    perf_vector_end(v, 1, service, ax, begin);
    if (e) return e;
    saved = (saved & ~(V86_CF | V86_ZF)) | (f->tf.eflags & (V86_CF | V86_ZF));
    return dosvm_write(v, linear, &saved, 2);
}

static int interrupt_service(void *arg, struct v86_frame *f, uint8_t vector)
{
    struct dosvm *v = arg;
    uint16_t ax = f->tf.eax;
    uint16_t cs=f->tf.cs,ip=f->tf.eip,ss=f->tf.user_ss,sp=f->tf.user_esp;
    uint64_t begin = perf_monitor_stamp(v);
    int e = interrupt_dispatch(arg, f, vector);
    /* Native multiplex replies (notably DPMI 1687h) return directly from
     * the monitor, without an IRET. Close only a completed original frame;
     * reflected/handler/mode-changing calls keep their existing return edge. */
    if (!e && vector==0x2f && dosvm_file_return &&
        f->tf.cs==cs && f->tf.eip==ip && (uint16_t)f->tf.user_ss==ss && (uint16_t)f->tf.user_esp==sp)
        dosvm_file_return(v,f);
    perf_vector_end(v, 0, vector, ax, begin);
    return e;
}

static void worker(void *arg)
{
    struct dosvm *v = arg;
    write_cr3(v->as.pd_phys);
    dosvm_startup_report(v, true);
    /* f3-04: a mode switch leaves the trap stack and re-enters here with
     * the new frame; only a stopping VM ends the loop. */
    for (;;) {
        perf_phase(v, 1);
        stack_prefault(v, &v->frame);
        cli();
        /* v86_leave enables IF on a mode switch. A newly raised audio
         * request in that worker interval must also get this boundary. */
        if (v->audio.sb.irq_entry_pending && !v->audio.sb.irq_entry_ready && v->monitor.vif &&
            dosvm_deliver_irq(v, &v->frame) < 0) {
            sti();
            v->info.fault_vector = v->frame.tf.vector;
            v->info.fault_eip = dpmi_fault_linear(v, &v->frame);
            dosvm_stop(v->info.key, DOSVM_FAULT);
            break;
        }
        if (v->audio.sb.irq_entry_ready) dosvm_audio_irq_enter(v, v->audio.sb.irq);
        v86_enter(&v->frame, &v->continuation);
        perf_phase(v, 2);
        if (v->info.state == DOSVM_STOPPING) break;
    }
    v->continuation = 0;
    dosvm_startup_report(v, true); /* retain a short-lived startup failure */
    if (v->dpmi.active) {
        arch_ldt_load(0, 0);
        arch_debug_load((const uint32_t[4]){0}, 0);
    }
    write_cr3(vmm_kernel_pd());
    tss_set_kernel_stack((uint32_t)v->task->kstack + KSTACK_SIZE);
    klog("[dosvm] lifecycle stage=stop vm=%u generation=%u reason=%u vector=%u eip=%08x",
         v->info.key.id,v->info.key.generation,v->info.reason,v->info.fault_vector,v->info.fault_eip);
    uint8_t code[8] = {0};
    if (v->info.reason == DOSVM_FAULT) {
        /* Also cover worker-side delivery failures without a trap trace. */
        dosvm_trace(v, DOSVM_TRACE_FAULT, &v->frame, 0, 0, 0);
        dosvm_read(v, v->info.fault_eip, code, sizeof(code));
    }
    if (rec_active()) rec_emit(dosvm_probe(v), "DATA", "group=vm_stop vm=%u generation=%u reason=%u vector=%u eip=%08x address=%08x error=%x code=%02x%02x%02x%02x%02x%02x%02x%02x",
             v->info.key.id, v->info.key.generation, v->info.reason,
             v->info.fault_vector, v->info.fault_eip, v->info.fault_address, v->info.fault_error,
             code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
    if (v->info.reason != DOSVM_EXIT) {
        /* Guest registers and stack top of the last frame: the INT frame
         * and return addresses name the DOS path without a build-time trace. */
        const struct v86_frame *s = &v->frame;
        char row[240];
        /* A DPMI frame shows selectors and the protected-mode CS:EIP. */
        ksnprintf(row, sizeof(row), "group=vm_fault pm=%u ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x ds=%04x es=%04x ss=%04x sp=%04x cs=%04x ip=%08x",
                 v->frame_pm, s->tf.eax & 0xFFFF, s->tf.ebx & 0xFFFF, s->tf.ecx & 0xFFFF, s->tf.edx & 0xFFFF,
                 s->tf.esi & 0xFFFF, s->tf.edi & 0xFFFF, s->tf.ebp & 0xFFFF,
                 (v->frame_pm ? s->tf.ds : s->ds) & 0xFFFF, (v->frame_pm ? s->tf.es : s->es) & 0xFFFF,
                 s->tf.user_ss & 0xFFFF, s->tf.user_esp & 0xFFFF, s->tf.cs & 0xFFFF, s->tf.eip);
        if (v->info.reason == DOSVM_FAULT) rec_diag_disk_bootlog(dosvm_probe(v), "%s", row);
        else if (rec_active()) rec_emit(dosvm_probe(v), "DATA", "%s", row);
        uint32_t top = ((s->tf.user_ss & 0xFFFF) << 4) + (s->tf.user_esp & 0xFFFF);
        struct dpmi_seg stack;
        if (v->frame_pm && dpmi_seg(v, (uint16_t)s->tf.user_ss, &stack))
            top = stack.base + (stack.big ? s->tf.user_esp : (s->tf.user_esp & 0xFFFF));
        for (unsigned part = 0; part < 3; part++) {
            uint16_t w[16] = {0};
            dosvm_read(v, top + part * sizeof(w), w, sizeof(w));
            ksnprintf(row, sizeof(row), "group=vm_stack part=%u words=%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x",
                     part, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
            if (!part && v->info.reason == DOSVM_FAULT) rec_diag_disk_bootlog(dosvm_probe(v), "%s", row);
            else if (rec_active()) rec_emit(dosvm_probe(v), "DATA", "%s", row);
        }
    }
    task_exit(v->info.reason);
}

static bool destroying[DOSVM_LIMIT];

/* The transaction's own task cannot wait for itself. */
static int fullscreen_pin_wait(const struct dosvm *v)
{
    if (dosvm_fullscreen_service()) return -EBUSY;
    uint64_t until = deadline_after_ms(DOSVM_FULLSCREEN_PIN_WAIT_MS);
    while (dosvm_fullscreen_pinned(v) && !deadline_passed(until)) task_sleep_ms(1);
    return dosvm_fullscreen_pinned(v) ? -EBUSY : 0;
}
static int fullscreen_release(const struct dosvm *v, struct dosvm_key key)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        int e = dosvm_fullscreen_leave(key, false);
        if (e != -EBUSY) return e;
        int pinned = fullscreen_pin_wait(v);
        if (pinned) return pinned;
    }
    return -EBUSY;
}
int dosvm_destroy(struct dosvm_key key, bool forced)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v) return -EINVAL;
    if (v->info.state == DOSVM_DEAD) return v->info.cleanup_error;
    /* fx-22: a switch pins the VM and its address space. The return is
     * bounded (supersavage-fullscreen.md, fx-22): wait for it before any
     * side effect, rather than reap and close devices and then fail at the
     * display step (T23 f441c5bd: Quake closing during Alt+Enter). */
    if (v->task == g_current) return -V86_EPERM;
    if (dosvm_fullscreen_pinned(v)) {
        int pinned = fullscreen_pin_wait(v);
        if (pinned) return pinned;
        if (v->info.state == DOSVM_DEAD) return v->info.cleanup_error;
    }
    unsigned slot=key.id-1;
    if (destroying[slot]) return -EBUSY;
    destroying[slot]=true;
    dosvm_stop(key, forced ? DOSVM_TERMINATE : DOSVM_EXIT);
    uint64_t until = deadline_after_ms(6000);
    while (v->task && task_alive(v->task)) {
        if (deadline_passed(until)) {
            klog("[dosvm] failure stage=destroy-wait vm=%u generation=%u result=%d fallback=retain",
                 key.id,key.generation,-EBUSY);
            destroying[slot]=false;
            return -16; /* retain resources until quiescent */
        }
        task_sleep_ms(1);
    }
    /* The stop above requests the return (dosvm_fullscreen_stopping); the
     * display worker may already run it while the task ends. Finish or wait
     * for it (bounded) before reaping and closing anything, never close
     * devices underneath it and then fail at the display step. */
    int video_error = fullscreen_release(v, key);
    if (video_error) {
        klog("[dosvm] failure stage=destroy-display vm=%u generation=%u result=%d fallback=retain",
             key.id,key.generation,video_error);
        destroying[slot]=false;
        return video_error;
    }
    if (v->task) { task_reap(v->task); v->task = 0; }
    dosvm_uart_close(v);
    /* Keep the host VFS table and buffered output for an explicit retry. */
    int capture_error=dosvm_print_flush ? dosvm_print_flush(v,true) : 0;
    if (capture_error) {
        v->info.cleanup_error=capture_error;
        destroying[slot]=false;
        return capture_error;
    }
    int printer_cleanup=dosvm_game_ports_close ? dosvm_game_ports_close(v) : 0;
    int network_error = dosvm_network_detach(v);
    if (network_error) { destroying[slot] = false; return network_error; }
    if (dosvm_cdaudio_close) dosvm_cdaudio_close(v); /* fx-10: stop its CD stream */
    int file_cleanup=0;
    v->save_discarding=forced || v->info.reason!=DOSVM_EXIT;
    if (dosvm_file_destroy) {
        int file_error=dosvm_file_destroy(v);
        if (file_error) {
            v->info.cleanup_error=file_error; v->info.data_loss=v->info.dirty;
            destroying[slot]=false; return file_error;
        }
        file_cleanup=v->info.cleanup_error;
    }
    dosvm_perf_dump(v);
    v->info.data_loss = printer_cleanup || (v->info.dirty && (forced || v->info.reason != DOSVM_EXIT));
    int floppy_error=dosvm_floppy_close(v);
    if (floppy_error) { destroying[slot]=false; return floppy_error; }
    int e = dosvm_disk_close(v, forced || v->info.reason != DOSVM_EXIT || (v->persistent && file_cleanup));
    if (!e) e=file_cleanup;
    if (!e) e=printer_cleanup;
    if (e && v->info.dirty) v->info.data_loss = true;
    if (audio_vm_detach) audio_vm_detach(key); /* f4-05: revoke the mixer source first */
    dosvm_audio_close(v);
    if (v->as.pd_phys) {
        /* f3-06: window PTEs may name the model's plane pages (kernel memory);
         * restore the VM's own frames before the address space frees them. */
        dosvm_vga_sync(v, true);
        dpmi_close(v);
        pool_unpin(v);
        dosvm_memory_close(v);
        as_destroy(&v->as);
    }
    memset(v->frames, 0, sizeof(v->frames));
    memset(v->ports, 0, sizeof(v->ports));
    v->key_count = v->line_length = 0;
    if (v->info.commitment && ledger_promise) ledger_promise(MM_VM_PRIVATE,
        -(int32_t)(v->info.commitment/PAGE_SIZE),-(int32_t)(v->info.commitment/PAGE_SIZE));
    v->info.commitment = v->info.resident = 0;
    v->info.cleanup_error = e;
    /* A failed unlink retains the generation/table for retry, never reuses
     * a slot while its private scratch file still exists. */
    v->info.state = v->overlay_created ? DOSVM_STOPPING : DOSVM_DEAD;
    klog("[dosvm] lifecycle stage=cleanup vm=%u generation=%u result=%d data_loss=%u retained=%u",
         key.id,key.generation,e,v->info.data_loss,v->overlay_created);
    if (rec_active()) rec_emit(dosvm_probe(v), "DATA", "group=vm_cleanup vm=%u generation=%u data_loss=%u error=%d",
             key.id, key.generation, v->info.data_loss, e);
    if (ledger_record) ledger_record("vm-teardown");
    destroying[slot]=false;
    return e;
}

/* A game creation keeps its reservation across every sleeping admission/open.
 * Removal holds the opposite reservation through FAT and COW cleanup. Include
 * stopped/retained VMs: their overlay can remain open until destruction ends. */
static unsigned game_creating[CIUKI_DOSVM_GAME_COUNT+1];
static bool game_removing[CIUKI_DOSVM_GAME_COUNT+1];
int dosvm_game_remove_reserve(unsigned game)
{
    if(!game || game>CIUKI_DOSVM_GAME_COUNT)return -EINVAL;
    uint32_t flags=irq_save();
    bool busy=game_removing[game] || game_creating[game];
    for(unsigned i=0;i<DOSVM_LIMIT;i++)
        if(slots[i].info.key.generation && slots[i].info.state!=DOSVM_DEAD && slots[i].game==game)busy=true;
    if(!busy)game_removing[game]=true;
    irq_restore(flags);return busy ? -EBUSY : 0;
}
void dosvm_game_remove_release(unsigned game)
{
    uint32_t flags=irq_save();game_removing[game]=false;irq_restore(flags);
}
/* fx-4 (T23 trial 9a60215f): the window ABI folds most creation errors into
 * EIO (dosvm_client.c closed_error), and DOOM/Quake failed with error=-5 and
 * no other record. Every refused creation now leaves one record naming the
 * stage, the internal error and step, and for a memory refusal the shortage:
 * admission arithmetic and free pages per zone. */
struct create_trace {
    const char *stage, *step, *detail;
    struct dosvm_admission admission;
    bool admission_known;
};
static const char *refusal_reason(int e)
{
    switch (e) {
    case -ENOMEM: return "memory";
    case -ENOSPC: return "vm-slots";
    case -EINTR: return "owner-gone";
    case -EBUSY: return "busy";
    case -ENOENT: return "not-installed";
    case -EINVAL: return "invalid";
    case -EIO: return "io";
    case -EROFS: return "read-only";
    default: return "error";
    }
}
static void create_refused(const struct dosvm_config *config, int e, const struct create_trace *t)
{
    unsigned game = config ? config->game : 0;
    uint32_t dma = pmm_zone_free ? pmm_zone_free(ZONE_DMA) : 0;
    uint32_t normal = pmm_zone_free ? pmm_zone_free(ZONE_NORMAL) : pmm_free_count();
    if (t->admission_known) {
        const struct dosvm_admission *a = &t->admission;
        klog("[dosvm] create refused stage=%s error=%d game=%u reason=%s free_kib=%llu outstanding_kib=%llu request_kib=%u headroom_kib=%u dma_free_pages=%u normal_free_pages=%u",
             t->stage, e, game, refusal_reason(e), (unsigned long long)(a->free_bytes >> 10),
             (unsigned long long)(a->outstanding_bytes >> 10), a->requested_bytes >> 10,
             a->headroom_bytes >> 10, dma, normal);
    } else {
        klog("[dosvm] create refused stage=%s error=%d game=%u reason=%s step=%s detail=%s dma_free_pages=%u normal_free_pages=%u",
             t->stage, e, game, refusal_reason(e), t->step, t->detail, dma, normal);
    }
}
static int game_create(const struct dosvm_config *,struct dosvm **,struct create_trace *);
static int create_checked(const struct dosvm_config *config,struct dosvm **out,struct create_trace *trace)
{
    if(out)*out=0;
    trace->stage="config";
    if (config && config->kernel && config->kernel!=CIUKI_DOSVM_UNIFIED) return -EINVAL;
    if (config) for (unsigned i=0;i<4;i++) if (config->serial[i]>8) return -EINVAL;
    unsigned game=config ? config->game : 0;
    if(game>CIUKI_DOSVM_GAME_COUNT)return -EINVAL;
    trace->stage="game-reservation";
    uint32_t flags=irq_save();
    if(game && game_removing[game]){irq_restore(flags);return -EBUSY;}
    if(game)game_creating[game]++;
    irq_restore(flags);
    uint32_t bytes=0;
    trace->stage="game-inventory";
    int e=!game ? 0 : dosvm_game_program_checked ?
        dosvm_game_program_checked(game,false,&bytes,&trace->step,&trace->detail) :
        dosvm_game_program(game,false,&bytes);
    if(e>0){e=-ENOENT;trace->step="pending-removal";} /* a pending removal cannot be relaunched */
    if(!e)e=game_create(config,out,trace);
    flags=irq_save();if(game)game_creating[game]--;irq_restore(flags);
    return e;
}
int dosvm_create(const struct dosvm_config *config,struct dosvm **out)
{
    struct create_trace trace={.stage="request",.step="none",.detail="none"};
    int e=create_checked(config,out,&trace);
    if (e) create_refused(config,e,&trace);
    return e;
}

extern void joystick_default_route(struct joystick_route *) __attribute__((weak));
static int game_create(const struct dosvm_config *config, struct dosvm **out, struct create_trace *trace)
{
    trace->stage = "request"; trace->step = trace->detail = "none";
    if (!out) return -EINVAL;
    *out = 0;
    trace->stage = "admission";
    uint32_t commitment = config && config->commitment ? config->commitment : DOSVM_RESIDENT_CAP;
    struct dosvm_ledger ledger;
    struct dosvm_admission admission;
    dosvm_snapshot(&ledger);
    bool admit = dosvm_admit((uint64_t)pmm_free_count() * PAGE_SIZE,
                             ledger.reserved, ledger.resident, commitment, &admission);
    if (rec_active()) rec_emit(config && config->probe ? config->probe : "vm-boot", "DATA", "group=admission free=%llu reserved=%llu resident=%llu outstanding=%llu request=%u headroom=%u admitted=%u",
             admission.free_bytes, admission.reserved_bytes, admission.resident_bytes,
             admission.outstanding_bytes, commitment, DOSVM_HEADROOM, admit);
    if (!admit) { trace->admission = admission; trace->admission_known = true; return -ENOMEM; }
    trace->stage = "slot";
    struct dosvm *v = 0;
    unsigned slot;
    for (slot = 0; slot < DOSVM_LIMIT; slot++)
        if (!destroying[slot] && !initializing[slot] && (!slots[slot].info.key.generation || slots[slot].info.state == DOSVM_DEAD)) {
            v = &slots[slot]; break;
        }
    if (!v) return -ENOSPC;
    uint32_t generation = gen_alloc();
    if (!generation) return -ENOSPC;
    if (ledger_promise) ledger_promise(MM_VM_PRIVATE,commitment/PAGE_SIZE,commitment/PAGE_SIZE);
    initializing[slot] = commitment;
    for (size_t at = 0; at < sizeof(*v); at += PAGE_SIZE) {
        size_t bytes = sizeof(*v) - at;
        if (bytes > PAGE_SIZE) bytes = PAGE_SIZE;
        memset((uint8_t *)v + at, 0, bytes);
        kpreempt_point();
    }
    v->info.key = (struct dosvm_key){slot + 1, generation};
    v->info.state = DOSVM_CREATING;
    if (config) {
        v->disable_file_bridge=config->disable_file_bridge || config->kernel!=0;
        v->game=config->game; v->kernel=config->kernel;
        v->record_observer=config->record_observer; v->record_arg=config->record_arg;
    }
    /* Internal probes without a fixture use the same sole image as windows.
     * Explicit fixture bases retain the host bridge testing seam. */
    if (!config || !config->base || !strncmp(config->base,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",sizeof("C:\\SYSTEM\\DOS\\UNIFIED.IMG"))) { v->kernel=CIUKI_DOSVM_UNIFIED; v->disable_file_bridge=true; }
    v->info.commitment = commitment;
    v->info.resident = sizeof(*v);
    initializing[slot] = 0;
    v->base = v->overlay = -1;
    v->a20 = true;
    v->probe = config ? config->probe : 0;
    v->perf.report_tick = v->perf.serial.tick = g_ticks;
    v->dpmi_enabled = config && config->dpmi;
    v->return_tracker_tf = (g_boot.flags & CBI_F_SMBIOS_QEMU) != 0;
    kwait_init(&v->wake);
    kwait_init(&v->present_wait);
    kwait_init(&v->clipboard.wait);
    v->present_wait.owner_preemptible = true;
    v->present_due_us = v->present_sample_due_us = g_ticks * 1000u;
    kwait_init(&v->disk_wait); v->disk_busy = false;
    trace->stage = "address-space";
    int e = as_create(&v->as);
    if (e) goto fail;
    v->as.owner=MM_VM_PRIVATE; v->as.pool_base=VM_POOL_BASE;
    trace->stage = "low-memory";
    for (unsigned i = 0; i < DOSVM_PAGES; i++) {
        uint32_t phys = pmm_alloc_zone(ZONE_NORMAL);
        if (!phys) { e = -ENOMEM; goto fail; }
        mm_claim(phys,1,MM_VM_PRIVATE);
        memset(P2V(phys), 0, PAGE_SIZE);
        e = as_map(&v->as, i * PAGE_SIZE, phys, dosvm_low_flags(i));
        if (e) { mm_free(phys,MM_VM_PRIVATE); goto fail; }
        v->frames[i] = phys;
        v->info.resident += PAGE_SIZE;
        kpreempt_point(); /* page initialized and mapping/accounting published */
    }
    v->info.resident += (v->as.tables + 1) * PAGE_SIZE;
    const struct v86_ops ops = {monitor_memory, absent_in, absent_out, v};
    v86_init(&v->monitor, &ops);
    v->monitor.copy = monitor_copy;
    v->monitor.io = dosvm_io;
    v->monitor.interrupt = interrupt_service;
    v->monitor.track_boundaries = true;
    v->monitor.pic[0].imr = 0xF8; /* timer and cascade, no physical input IRQ */
    v->monitor.pic[1].imr = 0xFF;
    dosvm_pit_init(v); dosvm_kbd_init(v); dosvm_mouse_init(v);
    if (joystick_default_route) joystick_default_route(&v->joystick.route);
    kpreempt_point();
    trace->stage = "devices";
    if ((e = dosvm_port_register(v->info.key, 0x40, 0x43, dosvm_pit_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x61, 0x61, dosvm_pit_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x60, 0x60, dosvm_kbd_port, 0)) ||
        (e = dosvm_port_register(v->info.key, 0x64, 0x64, dosvm_kbd_port, 0)) ||
        (dosvm_joystick_port && (e = dosvm_port_register(v->info.key,0x201,0x201,dosvm_joystick_port,0))) ||
        (e = dosvm_audio_init(v)) || (e = dosvm_floppy_init(v))) goto fail; /* virtual devices only */
    kpreempt_point();
    trace->stage = "bios";
    e = dosvm_bios_init(v);
    if (e) goto fail;
    trace->stage = "serial";
    if ((e=dosvm_uart_init(v))) goto fail;
    if (config) for (unsigned i=0;i<4;i++) if (config->serial[i]) {
        const char *reason=0;
        e=dosvm_uart_bind(v->info.key,i,config->serial[i]-1,&reason);
        if (e) { klog("[dosvm] COM%u lease refused: %s (%d)",i+1,reason,e); goto fail; }
    }
    kpreempt_point();
    trace->stage = "memory";
    e = dosvm_memory_init(v, config);
    if (e) goto fail;
    kpreempt_point();
    trace->stage = "pool-pin";
    e = pool_pin(v);
    if (e) goto fail;
    trace->stage = "vga";
    e = dosvm_vga_init(v);
    if (e) goto fail;
    trace->stage = "dpmi-rom";
    if ((e = dpmi_rom_init(v))) goto fail;
    kpreempt_point();
    /* The cold base identity hash can take seconds on a 1 GHz machine
     * (about 20 s for the 27.7 MB unified base on the T23 trial); an owner
     * that stops meanwhile ends it with EINTR instead of waiting it out. */
    trace->stage = "disk";
    if (config) { v->create_cancelled = config->cancelled; v->create_cancel_arg = config->cancel_arg; }
    e = dosvm_disk_open_step(v, config ? config->base : 0, &trace->step);
    v->create_cancelled = 0; v->create_cancel_arg = 0;
    if (e) goto fail;
    trace->stage = "game-ports";
    if (v->game && dosvm_game_ports_apply && (e=dosvm_game_ports_apply(v))) goto fail;
    if (v->game || v->kernel)
        rec_diag_disk_bootlog(dosvm_probe(v),"group=game_launch game=%u kernel=%s base_sha256=%s generation=%u bridge=%u cached=%u source=%s",
                      v->game,v->kernel==2 ? "unified" : "fixture",v->base_sha256,
                      v->info.key.generation,!v->disable_file_bridge,v->base_cached,
                      v->base_source ? v->base_source : "none");
    kpreempt_point();
    trace->stage = "clock";
    /* fx-9: the shared vm-clock task, created once with the first VM. */
    if ((e = dosvm_clock_start())) goto fail;
    trace->stage = "worker";
    v->task = task_create_kernel("dosvm", worker, v, P_NORMAL);
    if (!v->task) { e = -ENOMEM; goto fail; }
    trace->stage = "resident-bound";
    v->info.resident += KSTACK_SIZE + PAGE_SIZE + sizeof(struct task);
    /* The image cache has its own charged backing inside the commitment;
     * retain the low/device/table bound independently of that cache. */
    if ((uint64_t)v->info.resident > (uint64_t)v->memory.quota +
            v->read_pages * PAGE_SIZE + DOSVM_LOW_CAP ||
        v->info.resident > commitment) { e = -ENOMEM; goto fail; }
    /* f4-05: the VM's SB16/OPL output joins the mixer when a backend is
     * ready; without one f3-05 keeps discarding with its counter. */
    v->audio.diag.attach_error = audio_vm_attach ? audio_vm_attach(v->info.key) : -ENODEV;
    rec_diag_disk_bootlog(dosvm_probe(v), "group=vm_start vm=%u gen=%u tracker=%s",
                         v->info.key.id, v->info.key.generation, v->return_tracker_tf ? "tf" : "breakpoint");
    *out = v;
    if (ledger_record) ledger_record("maximum-vm-created");
    return 0;
fail:
    dosvm_destroy(v->info.key, true);
    return e;
}

int dosvm_boot(struct dosvm *v)
{
    if (v->info.state != DOSVM_CREATING || !v->task) return -EINVAL;
    int e = dosvm_disk_read(v, 0, v->sector);
    if (e || v->sector[510] != 0x55 || v->sector[511] != 0xAA) return -V86_EIO;
    if ((e = dosvm_write(v, 0x7C00, v->sector, 512))) return e;
    kpreempt_point(); /* boot sector copied; no monitor transaction started */
    memset(&v->frame, 0, sizeof(v->frame));
    v->frame.tf.eip = 0x7C00;
    v->frame.tf.edx = 0x80;
    v->frame.tf.user_esp = 0x7C00;
    v->frame.tf.eflags = V86_VM | V86_IF | 2;
    v->monitor.state = V86_RUNNING;
    v->monitor.vif = true;
    v->boot_deadline = deadline_after_ms(30000);
    v->ticks = g_ticks;
    v->input_since = g_ticks;
    v->info.state = DOSVM_BOOTING;
    v->started = true;
    if (!dosvm_valid(input_focus)) input_focus = v->info.key;
    v86_prepare_return(&v->monitor, &v->frame);
    task_start(v->task);
    return 0;
}

bool dosvm_irq_raise(struct dosvm_key key, unsigned irq)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || irq >= 16) return false;
    uint32_t flags = irq_save();
    v86_irq_raise(&v->monitor, irq);
    irq_restore(flags);
    dosvm_idle_wake(v);
    return true;
}
int dosvm_keyboard_refresh(struct dosvm *v)
{
    uint16_t pos[2];
    if (dosvm_read(v, 0x41A, pos, sizeof(pos))) return -EFAULT;
    if ((pos[0] & 1) || (pos[1] & 1) || pos[0] < 0x1E || pos[0] >= 0x3E || pos[1] < 0x1E || pos[1] >= 0x3E) return -EFAULT;
    v->key_head = (pos[0] - 0x1E) / 2;
    v->key_count = ((pos[1] - pos[0] + 32) % 32) / 2;
    return dosvm_read(v, 0x41E, v->keyboard, 32);
}
bool dosvm_keyboard(struct dosvm_key key, uint16_t bios_key)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save();
    if (dosvm_keyboard_refresh(v) || v->key_count == 15) { irq_restore(flags); return false; }
    unsigned tail = (v->key_head + v->key_count) % 16;
    uint16_t next = 0x1E + ((tail + 1) % 16) * 2;
    bool ok = !dosvm_write(v, 0x41E + tail * 2, &bios_key, 2) && !dosvm_write(v, 0x41C, &next, 2);
    if (ok) { v->keyboard[tail] = bios_key; v->key_count++; dosvm_idle_wake(v); }
    irq_restore(flags); return ok;
}
/* fx-9: one bounded turn of the VM clock, run by the vm-clock task
 * (dosvm_clock.c), never by the shared device worker. The generation pins
 * the VM across the service's preemption points: gen_alloc() never reuses
 * a value, so a stopped or reused slot ends the service. Only permanent slot
 * headers are examined until the generation and live-state checks pass. */
bool dosvm_clock_turn(unsigned slot, uint32_t *generation)
{
    if (slot >= DOSVM_LIMIT) return false;
    struct dosvm *v = &slots[slot];
    if (*generation && v->info.key.generation != *generation) return false;
    if (!v->started || !dosvm_valid(v->info.key)) return false;
    *generation = v->info.key.generation;
    dosvm_uart_service(v,g_ticks); /* wakes a halted COM client, no hard-IRQ I/O */
    v->monitor.ticks = g_ticks;
    uint64_t elapsed = g_ticks - v->ticks;
    /* fx-16: a VM parked at its full-screen checkpoint (the switch, or a
     * background full-screen VM suspended by its settings) is a suspended
     * VM: VPICD simulates no interrupt into it (Win95 DDK,
     * VPICD_Set_Int_Request) and its devices do not advance. Before, the
     * PIT and the SB16 DSP kept running for the whole switch (T23 trial
     * e831e685: parked=808 ms, merged=102, underruns=94): the unrefilled
     * auto-init buffer replayed as a buzz and the game resumed 0.9 s later
     * with its timer ticks merged away. Parked time is discarded. */
    if (v->fullscreen_parked && elapsed) {
        v->parked_ms += elapsed;
        v->ticks = g_ticks;
        elapsed = 0;
    }
    v->perf.if_samples_ms += elapsed;
    if (!v->monitor.vif) {
        v->perf.if_clear_ms += elapsed;
        if (!dosvm_in_irq(v)) v->perf.if_outside_ms += elapsed;
    }
    dosvm_pool_irq_sample(v, elapsed);
    v->ticks = g_ticks;
    v->clock_ms += elapsed;
    uint64_t begin = dosvm_perf_clock();
    if (elapsed) dosvm_pit_advance(v, elapsed);
    uint64_t audio = dosvm_perf_clock();
    bool pending = dosvm_audio_advance(v, elapsed);
    uint64_t end = dosvm_perf_clock();
    if (elapsed) v->perf.ticks++;
    v->perf.audio_cycles += end - audio;
    v->perf.timer_cycles += end - begin;
    if (audio_vm_kick && (v->audio.q.seg_count || v->audio.q.event_count)) audio_vm_kick();
    dosvm_present_checkpoint(v);
    return pending;
}
/* PIT top half (task.c sched_tick, IF clear). fx-9: no VM state advances
 * here. Per started VM it samples which task the tick interrupted while the
 * SB16 entry is pending (the cadence record's descheduled/if_clear/parked
 * milliseconds; R1/D1: host-ready wait recorded apart from guest IF), then
 * wakes the vm-clock task in O(1). From f0-46 to fx-9 this ran on the
 * shared worker, where g_current was always the worker: every pending
 * millisecond read as descheduled and if_clear could never count. */
void dosvm_tick(void)
{
    bool started = false;
    for (unsigned i = 0; i < DOSVM_LIMIT; i++) {
        struct dosvm *v = &slots[i];
        if (!v->started || !dosvm_valid(v->info.key)) continue;
        dosvm_audio_sample(v, g_current == v->task);
        started = true;
    }
    if (started) dosvm_clock_signal();
}
const uint16_t *dosvm_text(const struct dosvm *v, unsigned *cursor)
{
    dosvm_read((struct dosvm *)v, 0xB8000, (void *)v->text, sizeof(v->text));
    dosvm_mouse_text((struct dosvm *)v);
    uint16_t pos = 0;
    dosvm_read((struct dosvm *)v, 0x450, &pos, 2);
    if (cursor) *cursor = v->mouse.visible >= 0 && v->mouse.text_type == 1 && v->mouse.x < 640 && v->mouse.y < 200 ?
        (unsigned)(v->mouse.y / 8) * 80 + v->mouse.x / 8 : (unsigned)(pos >> 8) * 80 + (pos & 255);
    return v->text;
}
int dosvm_port_register(struct dosvm_key key, uint16_t first, uint16_t last,
                        dosvm_port_handler handler, void *arg)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key) || first > last || !handler) return -EINVAL;
    /* These fixed ranges belong to the core. */
    static const uint16_t reserved[][2] = {{0x20,0x21},
        {0x70,0x71},{0x92,0x92},{0xA0,0xA1},{DOSVM_RECORD_PORT,DOSVM_CONTROL_PORT},
        {DOSVM_FAULT_PORT,DOSVM_FAULT_PORT}};
    for (unsigned i = 0; i < ARRAY_SIZE(reserved); i++)
        if (first <= reserved[i][1] && last >= reserved[i][0]) return -16;
    unsigned slot = DOSVM_PORTS;
    for (unsigned i = 0; i < DOSVM_PORTS; i++) {
        if (!v->ports[i].handler) { slot = i; continue; }
        if (first <= v->ports[i].last && last >= v->ports[i].first) return -16;
    }
    if (slot == DOSVM_PORTS) return -ENOSPC;
    v->ports[slot].first = first; v->ports[slot].last = last;
    v->ports[slot].arg = arg; v->ports[slot].handler = handler;
    if (first==0x40 && last>=0x43 && handler==dosvm_pit_port) v->port_hint[0]=(uint8_t)slot;
    if (first==0x388 && last>=0x389 && handler==dosvm_audio_port) v->port_hint[3]=(uint8_t)slot;
    return 0;
}
int dosvm_port_unregister(struct dosvm_key key, uint16_t first)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return -EINVAL;
    for (unsigned i = 0; i < DOSVM_PORTS; i++) if (v->ports[i].handler && v->ports[i].first == first) {
        memset(&v->ports[i], 0, sizeof(v->ports[i])); return 0;
    }
    return -EINVAL;
}

/* Trusted window transport hook. No native syscall or VM-id authority is
 * granted here; f3-02 supplies the owner-bound adapter. */
bool dosvm_input_focus(struct dosvm_key key, bool focused)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save();
    struct dosvm *previous = dosvm_lookup(input_focus);
    if (focused && previous && previous != v && dosvm_valid(input_focus)) {
        task_input_withdraw(previous->task);
        dosvm_kbd_release(previous); dosvm_mouse_event(previous, 0, 0, 0);
        previous->pointer_known = false;
        if (previous->fullscreen_pause && !previous->fullscreen) previous->fullscreen_suspended=true;
    }
    v->window_input = true;
    bool resumed = focused && v->fullscreen_suspended;
    if (focused) { input_focus = key; v->pointer_known = false; v->fullscreen_suspended=false; }
    else if (input_focus.id == key.id && input_focus.generation == key.generation) {
        task_input_withdraw(v->task);
        dosvm_kbd_release(v); dosvm_mouse_event(v, 0, 0, 0);
        v->pointer_known = false; input_focus = (struct dosvm_key){0};
        if (v->fullscreen_pause && !v->fullscreen) v->fullscreen_suspended=true;
    }
    if (resumed) kwait_wake_all(&v->wake); /* fx-22: a parked VM blocks on it */
    irq_restore(flags);
    return true;
}
struct dosvm_key dosvm_focused_key(void) { return input_focus; }
bool dosvm_input_event(struct dosvm_key key, const struct ciuki_input_event *e)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!e || !v || !dosvm_valid(key) || !v->window_input ||
        input_focus.id != key.id || input_focus.generation != key.generation) return false;
    uint32_t flags = irq_save(); bool ok = true;
    switch (e->type) {
    case CIUKI_INPUT_KEY: ok = dosvm_kbd_event(v, e->code, e->value); break;
    case CIUKI_INPUT_TEXT: break; /* KEY already supplies the guest keyboard */
    case CIUKI_INPUT_WHEEL:
        if (e->value<-127 || e->value>127) { ok=false; break; }
        dosvm_mouse_wheel(v,-e->value); break;
    case CIUKI_INPUT_MOTION:
        if (v->pointer_known) {
            int64_t dx = (int64_t)e->value - v->input_x, dy = (int64_t)e->value2 - v->input_y;
            if (dx < INT32_MIN || dx > INT32_MAX || dy < INT32_MIN || dy > INT32_MAX) { ok = false; break; }
            dosvm_mouse_event(v, dx, dy, v->mouse.buttons);
        }
        v->input_x = e->value; v->input_y = e->value2; v->pointer_known = true; break;
    case CIUKI_INPUT_BUTTON:
        if (e->code < 1 || e->code > 3 || (e->value != 0 && e->value != 1)) { ok = false; break; }
        dosvm_mouse_event(v, 0, 0, e->value ? v->mouse.buttons | (1u << (e->code - 1)) :
                         v->mouse.buttons & ~(1u << (e->code - 1))); break;
    case CIUKI_INPUT_RESYNC:
        dosvm_kbd_release(v); dosvm_mouse_event(v, 0, 0, 0); v->pointer_known = false; break;
    default: ok = false;
    }
    if (ok && e->type != CIUKI_INPUT_TEXT) dosvm_idle_wake(v);
    irq_restore(flags);
    return ok;
}
bool dosvm_device_info(struct dosvm_key key, struct dosvm_device_info *out)
{
    struct dosvm *v = dosvm_lookup(key);
    if (!out || !v || !dosvm_valid(key)) return false;
    uint32_t flags = irq_save(); memset(out, 0, sizeof(*out));
    out->host_ticks = v->clock_ms; out->pit_clocks = v->pit.clocks; out->pit_edges = v->pit.edges;
    out->pit_dropped_ticks = v->pit.dropped_ticks; out->key_overflows = v->kbd.overflows;
    out->queued_bytes = v->kbd.count; out->buttons = v->mouse.buttons;
    for (unsigned i = 0; i < sizeof(v->kbd.held); i++)
        for (unsigned bit = 0; bit < 8; bit++) out->held_keys += !!(v->kbd.held[i] & (1u << bit));
    for (unsigned i = 0; i < 2; i++) {
        out->irr[i] = v->monitor.pic[i].irr; out->isr[i] = v->monitor.pic[i].isr; out->imr[i] = v->monitor.pic[i].imr;
        out->pic_base[i] = v->monitor.pic[i].base;
    }
    out->last_vector = v->monitor.stats.last_vector;
    for (unsigned i = 0; i < 16; i++) { out->delivered[i] = v->monitor.stats.delivered[i];
    }
    irq_restore(flags); return true;
}
