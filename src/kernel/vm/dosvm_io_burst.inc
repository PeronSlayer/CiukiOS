/* Bounded virtual VGA/PIT/PIC I/O at an actual trapped instruction boundary.
 * No speculative memory writes, branch execution or physical I/O. Ordinary
 * MOV/INC setup is committed only when followed by another supported I/O cycle.
 * TF, watchpoints, IF restoration, shadows, enabled IRQs and rescheduling
 * all stop the burst. SPDX-License-Identifier: GPL-2.0-only */
#define DOSVM_IO_BUDGET 64u
static bool io_burst_fetch(struct dosvm *v, const struct v86_frame *f, unsigned at, uint8_t *byte)
{
    uint32_t ip = f->tf.eip + at, linear;
    if (ip < f->tf.eip) return false;
    if (v->frame_pm) {
        if (!dpmi_ptr(v, (uint16_t)f->tf.cs, ip, 1, false, &linear)) return false;
    } else {
        if (f->tf.cs > 0xFFFF || ip > 0xFFFF) return false;
        linear = (f->tf.cs << 4) + ip;
    }
    if (linear >= 0xA0000 && linear < 0xC0000) return false;
    if (!v->frame_pm) {
        const uint8_t *p = v->monitor.ops.memory(v->monitor.ops.arg, linear, 1, false);
        if (!p) return false;
        *byte = *p;
        return true;
    }
    return !dosvm_read(v, linear, byte, 1);
}
/* t23-dosperf: an armed flags image does not stop the burst (the same
 * argument as video_run_blocked in dosvm_vga.c: this decoder retires port
 * cycles and MOV/INC register setup, never POPF/IRET; its only stores are
 * REP INS elements, emulated as the ordinary path emulates them). */
static bool burst_blocked(const struct dosvm *v, const struct v86_frame *f)
{
    return v->info.state >= DOSVM_STOPPING || g_need_resched || (f->tf.eflags & V86_TF) ||
        v->dpmi.dr7 || v->dpmi.return_steps || v->monitor.shadow || v->dpmi.shadow ||
        (v->monitor.vif && v86_irq_pending(&v->monitor) >= 0);
}
static bool io_hot_port(uint16_t port)
{
    return (port >= 0x40 && port <= 0x43) || port == 0x388 || port == 0x389 ||
        port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1;
}
/* Common byte cycles and VGA word OUTs need no generic decoder. Other
 * prefixes, dword and string operations retain the scalar checks. */
static bool io_fast_fetch(struct dosvm *v, const struct v86_frame *f,
                          const struct dpmi_seg *cs, unsigned at, uint8_t *byte)
{
    if (!cs) return io_burst_fetch(v, f, at, byte);
    uint32_t off = f->tf.eip + at;
    if (!cs->big) off &= 0xFFFF;
    return off <= cs->limit && !dosvm_read(v, cs->base + off, byte, 1);
}
bool dosvm_io_fast(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs, int *error)
{
    uint8_t op, imm;
    unsigned len = 1, width = cs && cs->big ? 4 : 2;
    if (!io_fast_fetch(v, f, cs, 0, &op)) return false;
    if (op == 0x66) {
        width = width == 4 ? 2 : 4;
        if (!io_fast_fetch(v, f, cs, len++, &op) || op != 0xEF) return false;
    }
    if (op != 0xE4 && op != 0xE6 && op != 0xEC && op != 0xEE && op != 0xEF) return false;
    uint16_t port = (uint16_t)f->tf.edx;
    if (op < 0xE8) {
        if (!io_fast_fetch(v, f, cs, len++, &imm)) return false;
        port = imm;
    }
    if (!(op & 1)) width = 1;
    bool write = op & 2;
    bool vga = port == 0x3C4 || port == 0x3C5 || port == 0x3CE || port == 0x3CF;
    if (!((width == 1 && io_hot_port(port)) || (write && vga && width <= 2))) return false;
    uint32_t value = f->tf.eax;
    if (!dosvm_io_hot(v,port,width,write,&value,error))
        *error = v86_io(&v->monitor, port, width, write, &value);
    if (*error) {
        if (!v->frame_pm) *error = v86_abort(&v->monitor, f, *error);
        return true;
    }
    if (!write) f->tf.eax = (f->tf.eax & ~255u) | (uint8_t)value;
    f->tf.eip += len;
    if (!cs || !cs->big) f->tf.eip &= 0xFFFF;
    if (!v->frame_pm) {
        v->monitor.stats.insn[write ? V86_OUT : V86_IN]++;
        if (v->monitor.shadow) {
            v->monitor.shadow = false;
            f->tf.eflags &= ~V86_TF;
        }
    }
    return true;
}
static void io_burst_inc(struct v86_frame *f, unsigned width)
{
    uint32_t mask = width == 4 ? UINT32_MAX : 0xFFFFu;
    uint32_t old = f->tf.edx & mask, value = (old + 1) & mask;
    uint32_t sign = width == 4 ? 0x80000000u : 0x8000u;
    unsigned parity = value & 255u;
    parity ^= parity >> 4; parity ^= parity >> 2; parity ^= parity >> 1;
    uint32_t flags = (value & sign ? 0x80u : 0) | (!value ? 0x40u : 0) |
        (!(parity & 1) ? 4u : 0) | ((old ^ value) & 0x10u) | (value == sign ? 0x800u : 0);
    f->tf.edx = (f->tf.edx & ~mask) | value;
    f->tf.eflags = (f->tf.eflags & ~0x8D4u) | flags; /* CF is unchanged. */
}
/* The opcodes dosvm_io_burst can continue with (its first-instruction
 * filter below); fx-27's lean exit peeks with a resident-page lookup and
 * skips the burst's checked fetch when the next instruction is none of them. */
static bool io_burst_candidate(uint8_t op)
{
    return op == 0xB0 || op == 0xB4 || op == 0xB8 || op == 0xBA || op == 0x88 || op == 0x42 ||
        (op >= 0xE4 && op <= 0xE7) || (op >= 0xEC && op <= 0xEF) || op == 0x6E || op == 0x6F ||
        op == 0x66 || op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
        op == 0x64 || op == 0x65 || op == 0xF2 || op == 0xF3;
}
int dosvm_io_burst(struct dosvm *v, struct v86_frame *f)
{
    unsigned cause = v->perf.cause;
    for (unsigned budget = 1; budget < DOSVM_IO_BUDGET && !burst_blocked(v, f); budget++) {
        uint8_t op;
        if (!io_burst_fetch(v, f, 0, &op)) return 0;
        /* Most scalar EOIs end in POP/IRET. Avoid a frame copy and full
         * prefix decoder when no supported burst instruction can follow. */
        if (!io_burst_candidate(op)) return 0;
        struct v86_frame n = *f;
        struct dpmi_seg cs;
        bool big = v->frame_pm && dpmi_seg(v, (uint16_t)n.tf.cs, &cs) && cs.big;
        unsigned len = 0, width = big ? 4 : 2, moves = 0;
        bool rep = false;
        bool first = true;
        for (;;) {
            if (!first && (len >= 15 || !io_burst_fetch(v, &n, len, &op))) return 0;
            first = false; len++;
            if (op == 0x66) { width = big ? 2 : 4; continue; }
            if (op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
            if (op == 0xF3 || op == 0xF2) { rep = true; continue; }
            break;
        }
        if (rep && (op == 0x6E || op == 0x6F) && (uint16_t)n.tf.edx >= 0x3B0 && (uint16_t)n.tf.edx <= 0x3DF &&
            ((uint16_t)n.tf.edx<0x3BC || (uint16_t)n.tf.edx>0x3BE)) {
            /* The first element already completed in the scalar emulator.
             * Reuse its complete per-element segment, DF and fault checks. */
            int e = v->frame_pm ? dpmi_io_repeat(v, f) : v86_emulate(&v->monitor, f);
            v->perf.cause = cause;
            if (e) return e;
            if (f->tf.ecx == n.tf.ecx) return 0; /* exception reflection or empty REP */
            v->perf.batched++;
            if (f->tf.cs != n.tf.cs || f->tf.eip != n.tf.eip) return 0;
            continue;
        }
        if (rep) return 0;
        while (op == 0xB0 || op == 0xB4 || op == 0xB8 || op == 0xBA || op == 0x88 || op == 0x42) {
            if (++moves > 2) return 0;
            if (op == 0x42) {
                io_burst_inc(&n, width);
            } else if (op == 0x88) {
                uint8_t modrm;
                if (len >= 15 || !io_burst_fetch(v, &n, len++, &modrm) || modrm != 0xC4) return 0;
                n.tf.eax = (n.tf.eax & ~0xFF00u) | ((n.tf.eax & 255) << 8); /* MOV AH,AL */
            } else {
                unsigned bytes = op == 0xB0 || op == 0xB4 ? 1 : width;
                uint32_t value = 0;
                for (unsigned i = 0; i < bytes; i++) {
                    uint8_t byte;
                    if (len >= 15 || !io_burst_fetch(v, &n, len++, &byte)) return 0;
                    value |= (uint32_t)byte << (8 * i);
                }
                uint32_t *reg = op == 0xBA ? &n.tf.edx : &n.tf.eax;
                unsigned shift = op == 0xB4 ? 8 : 0;
                uint32_t mask = bytes == 4 ? UINT32_MAX : ((1u << (bytes * 8)) - 1) << shift;
                *reg = (*reg & ~mask) | (value << shift);
            }
            if (!big && (n.tf.eip > 0xFFFF || len > 0x10000u - n.tf.eip)) return 0;
            n.tf.eip += len;
            if (!big) n.tf.eip &= 0xFFFF;
            len = 0; width = big ? 4 : 2;
            if (!io_burst_fetch(v, &n, len++, &op)) return 0;
            if (op == 0x66) {
                width = big ? 2 : 4;
                if (!io_burst_fetch(v, &n, len++, &op)) return 0;
            }
        }
        uint16_t port = (uint16_t)n.tf.edx;
        if (op >= 0xE4 && op <= 0xE7) {
            uint8_t byte;
            if (len >= 15 || !io_burst_fetch(v, &n, len++, &byte)) return 0;
            port = byte;
        } else if (op < 0xEC || op > 0xEF) return 0;
        if (!(op & 1)) width = 1;
        bool write = op & 2;
        if (!((write && port >= 0x3B0 && port <= 0x3DF && (port<0x3BC || port>0x3BE)) ||
              (width == 1 && io_hot_port(port)))) return 0;
        if (!big && (n.tf.eip > 0xFFFF || len > 0x10000u - n.tf.eip)) return 0;
        uint32_t value = n.tf.eax;
        *f = n; /* setup instructions retired even if the I/O fails */
        int e = v86_io(&v->monitor, port, width, write, &value);
        v->perf.cause = cause; /* mixed bursts retain their first port's cost */
        if (e) return e;
        if (!write) {
            uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
            f->tf.eax = (f->tf.eax & ~mask) | (value & mask);
        }
        f->tf.eip += len;
        if (!big) f->tf.eip &= 0xFFFF;
        if (!v->frame_pm) v->monitor.stats.insn[write ? V86_OUT : V86_IN]++;
        v->perf.batched++;
    }
    return 0;
}
/* fx-27: the monitor's port bursts after a trapped IN/OUT: the straight-line
 * VGA/PIT/OPL sequence above, then, after a DAC write, the decoded palette
 * loop of dosvm_vga_dac_burst (DAC loads are one OUT per byte). */
int dosvm_port_bursts(struct dosvm *v, struct v86_frame *f)
{
    int e = dosvm_io_burst(v, f);
    if (!e && v->video_dac_burst && v->info.state < DOSVM_STOPPING) e = dosvm_vga_dac_burst(v, f);
    v->video_dac_burst = false;
    return e;
}
