/* Bounded VGA output coalescing at an actual trapped instruction boundary.
 * No speculative memory writes, branch execution or physical I/O. Ordinary
 * MOV-immediate setup is committed only when followed by another VGA OUT.
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
    return !(linear >= 0xA0000 && linear < 0xC0000) && !dosvm_read(v, linear, byte, 1);
}
static bool burst_blocked(const struct dosvm *v, const struct v86_frame *f)
{
    return v->info.state >= DOSVM_STOPPING || g_need_resched || (f->tf.eflags & V86_TF) ||
        v->dpmi.dr7 || v->dpmi.if_armed || v->dpmi.return_steps || v->monitor.shadow || v->dpmi.shadow ||
        (v->monitor.vif && v86_irq_pending(&v->monitor) >= 0);
}
int dosvm_io_burst(struct dosvm *v, struct v86_frame *f)
{
    for (unsigned budget = 1; budget < DOSVM_IO_BUDGET && !burst_blocked(v, f); budget++) {
        struct v86_frame n = *f;
        struct dpmi_seg cs;
        bool big = v->frame_pm && dpmi_seg(v, (uint16_t)n.tf.cs, &cs) && cs.big;
        unsigned len = 0, width = big ? 4 : 2, moves = 0;
        bool rep = false;
        uint8_t op;
        for (;;) {
            if (len >= 15 || !io_burst_fetch(v, &n, len++, &op)) return 0;
            if (op == 0x66) { width = big ? 2 : 4; continue; }
            if (op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
            if (op == 0xF3 || op == 0xF2) { rep = true; continue; }
            break;
        }
        if (rep && (op == 0x6E || op == 0x6F) && (uint16_t)n.tf.edx >= 0x3B0 && (uint16_t)n.tf.edx <= 0x3DF) {
            /* The first element already completed in the scalar emulator.
             * Reuse its complete per-element segment, DF and fault checks. */
            int e = v->frame_pm ? dpmi_io_repeat(v, f) : v86_emulate(&v->monitor, f);
            if (e) return e;
            if (f->tf.ecx == n.tf.ecx) return 0; /* exception reflection or empty REP */
            v->perf.batched++;
            if (f->tf.cs != n.tf.cs || f->tf.eip != n.tf.eip) return 0;
            continue;
        }
        if (rep) return 0;
        while (op == 0xB0 || op == 0xB4 || op == 0xB8 || op == 0xBA) {
            if (++moves > 2) return 0;
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
        if (op == 0xE6 || op == 0xE7) {
            uint8_t byte;
            if (!io_burst_fetch(v, &n, len++, &byte)) return 0;
            port = byte;
        } else if (op != 0xEE && op != 0xEF) return 0;
        if (port < 0x3B0 || port > 0x3DF) return 0;
        if (!(op & 1)) width = 1;
        uint32_t value = n.tf.eax;
        *f = n; /* setup instructions retired even if the OUT fails */
        int e = v86_io(&v->monitor, port, width, true, &value);
        if (e) return e;
        f->tf.eip += len;
        if (!big) f->tf.eip &= 0xFFFF;
        if (!v->frame_pm) v->monitor.stats.insn[V86_OUT]++;
        v->perf.batched++;
    }
    return 0;
}
