/* DPMI 0.9 dispatch: protected-mode traps of a client at ring 3/IOPL 0,
 * software interrupts through the VM's protected-mode vectors, processor
 * exceptions through the client's handlers (locked-stack frame, far
 * return), virtual IRQs to protected-mode handlers or the default real-mode
 * reflection. DPMI 0.9 sections 2.3, 2.4, 3, 10, 17; Intel SDM Vol. 3A
 * 6.12 (INT n privilege check gives #GP(vector*8+2)), 6.8.1 (IOPL-sensitive
 * CLI/STI), 17.3.1.1 (RF for instruction breakpoints).
 * Research and decisions: docs/design/f3-acceptance.md, f3-04.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

#define DPMI_RF (1u << 16)
static uint32_t pm_image(const struct dosvm *v, uint32_t eflags)
{ return (eflags & 0x0DD5u) | (v->monitor.vif ? V86_IF : 0) | 2u; }
static bool is_default(struct dpmi_vec h, uint32_t base, unsigned count)
{ return h.sel == DPMI_HOST_CS && h.off >= base && h.off < base + count; }

bool dpmi_handler_live(struct dosvm *v, struct dpmi_vec h)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_seg s;
    if (h.sel == DPMI_HOST_CS) return h.off <= DPMI_PM_LIMIT;
    if (!dpmi_code_selector(v, h.sel, h.off) || !dpmi_seg(v, h.sel, &s)) return false;
    if (h.sel & 4) {
        unsigned i = h.sel >> 3;
        if (d->owner[i] > d->clients) return false;
    }
    return dpmi_linear_owned(v, s.base + h.off, 1);
}

/* Worker-only diagnostics, outside the delivery transaction. Every master
 * IRQ vector update survives the generic startup trace's lifetime budget.
 * Descriptor snapshots distinguish a client stub from a host reflection or
 * a freed/non-present selector. No guest/PIC state is changed here. */
void dpmi_trace_irq_target(struct dosvm *v, const struct v86_frame *f, const char *event,
                           uint8_t vector, struct dpmi_vec h)
{
    if (!(g_boot.flags & CBI_F_VERBOSE)) return;
    struct dpmi_seg s = {0};
    bool valid = dpmi_seg(v, h.sel, &s);
    unsigned i = h.sel >> 3;
    bool ldt = (h.sel & 4) && i < DPMI_LDT_ENTRIES;
    klog("[dosvm] event=%s vm=%u gen=%u vec=%02x to=%04x:%08x valid=%u present=%u live=%u base=%08x limit=%08x access=%02x big=%u kind=%u owner=%u cs=%04x eip=%08x",
         event, v->info.key.id, v->info.key.generation, vector, h.sel, h.off,
         valid, s.present, dpmi_handler_live(v, h), s.base, s.limit, s.access, s.big,
         ldt ? v->dpmi.kind[i] : 0, ldt ? v->dpmi.owner[i] : 0,
         f->tf.cs & 0xFFFF, f->tf.eip);
}

uint32_t dpmi_fault_linear(struct dosvm *v, const struct v86_frame *f)
{
    struct dpmi_seg s;
    if (!v->frame_pm) return ((f->tf.cs & 0xFFFFu) << 4) + (uint16_t)f->tf.eip;
    return dpmi_seg(v, (uint16_t)f->tf.cs, &s) ? s.base + f->tf.eip : f->tf.eip;
}

void dpmi_watch_hits(struct dosvm *v, uint32_t dr6)
{
    for (unsigned i = 0; i < DPMI_WATCH; i++)
        if ((dr6 & (1u << i)) && v->dpmi.watch[i].used) v->dpmi.watch[i].hit = true;
}

/* Intel SDM Vol. 3B 18.2.3: B0-B3 can include disabled breakpoint
 * comparisons. Only allocated, locally enabled slots belong to this VM.
 * BS/BD/BT are independent causes and must survive this filtering. */
uint32_t dpmi_debug_status(const struct dosvm *v, uint32_t dr6)
{
    uint32_t enabled = 0;
    for (unsigned i = 0; i < DPMI_WATCH; i++)
        if (v->dpmi.watch[i].used && (v->dpmi.dr7 & (1u << (2 * i)))) enabled |= 1u << i;
    if (v->dpmi.return_steps && v->dpmi.return_slot < DPMI_WATCH &&
        (v->dpmi.dr7 & (1u << (2 * v->dpmi.return_slot))))
        enabled |= 1u << v->dpmi.return_slot;
    return (dr6 & ~15u) | (dr6 & enabled);
}

/* Software interrupt n through the protected-mode vector table (0.9 2.4.2:
 * client stack, IF unchanged except INT 0-7, which run with it clear). */
int dpmi_pm_interrupt(struct dosvm *v, struct v86_frame *f, uint8_t n)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_vec h = d->vec[n];
    if (is_default(h, DPMI_PM_INT, 256)) return dpmi_default_int(v, f, (uint8_t)(h.off - DPMI_PM_INT), false);
    if (!dpmi_handler_live(v, h)) { d->stats.stale++; return -EFAULT; }
    struct v86_frame n2 = *f;
    unsigned w = dpmi_width(v);
    if (!dpmi_push(v, &n2, pm_image(v, f->tf.eflags), w) || !dpmi_push(v, &n2, (uint16_t)f->tf.cs, w) ||
        !dpmi_push(v, &n2, f->tf.eip, w))
        return dpmi_exception(v, f, 12, 0);
    n2.tf.cs = h.sel;
    n2.tf.eip = h.off;
    n2.tf.eflags &= ~V86_TF;
    if (n < 8) v->monitor.vif = false;
    *f = n2;
    return 0;
}

static int default_exception(struct dosvm *v, struct v86_frame *f, unsigned n)
{
    /* 0.9 section 10.4: 0-5 and 7 are reflected as interrupts; 6 and
     * 08h-1Fh are fatal. Fatal here terminates this VM only. */
    if (n <= 5 || n == 7) return dpmi_pm_interrupt(v, f, (uint8_t)n);
    v->dpmi.stats.faults++;
    return -EFAULT;
}

/* DPMI stacks: a handler's private locked stack remains current on nested
 * PM entries. Switching back to hstack would overwrite registers pushed
 * there before the client switched away (DOOM's LSS ESP,[EDX+8]).
 * Research: docs/validation/2026-10-15-f3-23-pm-irq-stack.md. */
static int handler_frame(struct dosvm *v, const struct v86_frame *f,
                         struct v86_frame *n, const uint32_t *frame, unsigned words)
{
    struct dpmi *d = &v->dpmi;
    bool nested = false;
    /* The newest record is this entry, not an enclosing handler. A child's
     * application stack must not inherit the parent's locked-stack scope. */
    for (unsigned i = 0; v->frame_pm && i + 1 < d->depth; i++) {
        const struct dpmi_record *r = &d->rec[i];
        if (r->client == d->clients && (r->kind == DPMI_REC_PMINT ||
            r->kind == DPMI_REC_EXCEPTION || r->kind == DPMI_REC_CALLBACK)) nested = true;
    }
    uint16_t ss = nested ? (uint16_t)f->tf.user_ss : DPMI_HOST_SS;
    struct dpmi_seg seg;
    if (!dpmi_seg(v, ss, &seg) || (ss & 3) != 3 || seg.dpl != 3) return -EFAULT;
    uint32_t top = nested ? (seg.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp) : d->lock_top;
    unsigned w = dpmi_width(v), bytes = words * w;
    if (top < bytes || (ss == DPMI_HOST_SS && top - bytes < DPMI_HSTACK_RESERVE)) return -ENOSPC;
    uint32_t esp = top - bytes, lin;
    if (!dpmi_ptr(v, ss, esp, bytes, true, &lin)) return -EFAULT;
    for (unsigned i = 0; i < words; i++)
        if (dosvm_write(v, lin + i * w, &frame[i], w)) return -EFAULT;
    n->tf.user_ss = ss;
    n->tf.user_esp = nested && !seg.big ? (f->tf.user_esp & 0xFFFF0000u) | esp : esp;
    if (ss == DPMI_HOST_SS) d->lock_top = esp & ~3u;
    return 0;
}

int dpmi_exception(struct dosvm *v, struct v86_frame *f, unsigned n, uint32_t error)
{
    struct dpmi *d = &v->dpmi;
    d->stats.exceptions++;
    if (n >= 32) return -EFAULT;
    struct dpmi_vec h = d->exc[n];
    if (h.sel == DPMI_HOST_CS && h.off == DPMI_PM_EXC + n) return default_exception(v, f, n);
    if (!dpmi_handler_live(v, h)) { d->stats.stale++; return -EFAULT; }
    struct dpmi_record *r;
    if (dpmi_push_record(v, f, DPMI_REC_EXCEPTION, &r)) return -ENOSPC;
    uint32_t frame[8] = {DPMI_PM_EXCRET, DPMI_HOST_CS, error, f->tf.eip, (uint16_t)f->tf.cs,
                         pm_image(v, f->tf.eflags), f->tf.user_esp, (uint16_t)f->tf.user_ss};
    struct v86_frame n2 = *f;
    int e = handler_frame(v, f, &n2, frame, ARRAY_SIZE(frame));
    if (e) { dpmi_unwind(v, d->depth - 1); return e; }
    r->vector = (uint8_t)n;
    r->arg[0] = n2.tf.user_esp;
    n2.tf.cs = h.sel;
    n2.tf.eip = h.off;
    n2.tf.eflags = (f->tf.eflags & 0x0CD5u & ~V86_TF) | V86_IF | 2u;
    *f = n2;
    v->monitor.vif = false;
    return 0;
}

/* Pop the exception frame and restore the faulting context the handler may
 * have edited; the general and data segment registers are the handler's
 * (0.9: it must preserve them). The frame is read at the current SS:ESP
 * (after the far return, or below the return address when the handler
 * chained to the default): DOS/4GW's #GP handler moves the frame before
 * returning. The restored CS:EIP/SS:ESP are validated before any IRET. */
static int exception_unframe(struct dosvm *v, struct v86_frame *f, unsigned skip)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_record *r = d->depth ? &d->rec[d->depth - 1] : 0;
    unsigned w = dpmi_width(v);
    struct dpmi_seg ss;
    if (!r || r->kind != DPMI_REC_EXCEPTION || !dpmi_seg(v, (uint16_t)f->tf.user_ss, &ss)) {
        d->stats.stale++;
        return -EFAULT;
    }
    uint32_t esp = (ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp) + skip - 2 * w, lin;
    if (!dpmi_ptr(v, (uint16_t)f->tf.user_ss, esp, 8 * w, false, &lin)) { d->stats.stale++; return -EFAULT; }
    uint32_t value[6] = {0};
    for (unsigned i = 0; i < 6; i++)
        if (dosvm_read(v, lin + (2 + i) * w, &value[i], w)) return -EFAULT;
    struct v86_frame n = *f;
    n.tf.eip = value[1];
    n.tf.cs = (uint16_t)value[2];
    n.tf.eflags = (value[3] & 0x0DD5u) | V86_IF | 2u;
    n.tf.user_esp = d->bits32 ? value[4] : (r->saved.tf.user_esp & 0xFFFF0000u) | (uint16_t)value[4];
    n.tf.user_ss = (uint16_t)value[5];
    dpmi_unwind(v, d->depth - 1);
    *f = n;
    v->monitor.vif = (value[3] & V86_IF) != 0;
    return 0;
}

/* The host's default protected-mode handler for vector n. Reached directly
 * (iret=false: nothing pushed) or by a client chaining to the address
 * 0204h returned (iret=true: an interrupt frame is on the client stack,
 * popped first so the action runs in the original caller's context). */
int dpmi_default_int(struct dosvm *v, struct v86_frame *f, uint8_t n, bool iret)
{
    if (iret) {
        struct v86_frame n2 = *f;
        uint32_t eip, cs, fl;
        unsigned w = dpmi_width(v);
        if (!dpmi_pop(v, &n2, &eip, w) || !dpmi_pop(v, &n2, &cs, w) || !dpmi_pop(v, &n2, &fl, w))
            return dpmi_exception(v, f, 12, 0);
        n2.tf.eip = eip;
        n2.tf.cs = (uint16_t)cs;
        n2.tf.eflags = (fl & 0x0DD5u) | V86_IF | 2u; /* virtual IF unchanged (0.9 2.3) */
        *f = n2;
    }
    uint16_t ax = (uint16_t)f->tf.eax;
    switch (n) {
    case 0x31:
        return dpmi_int31(v, f) < 0 ? -EFAULT : 0;
    case 0x2F:
        if (ax == 0x1686) { dpmi_low(&f->tf.eax, 0); return 0; }
        break;
    case 0x21:
        if ((ax >> 8) == 0x4C || (ax >> 8) == 0x00 || (ax >> 8) == 0x31) return dpmi_terminate_int(v, f, 0x21, 0);
        break;
    case 0x20: case 0x27:
        return dpmi_terminate_int(v, f, n, 0);
    default: break;
    }
    return dpmi_to_real_int(v, f, DPMI_REC_REFLECT, n, 0) ? -ENOSPC : 0;
}

/* Protected-mode handler of a virtual IRQ (from either mode) or of INT
 * 1Ch/23h/24h executed in real mode, on the locked stack with an IRET
 * frame returning to the host (0.9 sections 2.4.1, 3.1.2). */
int dpmi_enter_pm_handler(struct dosvm *v, struct v86_frame *f, uint8_t vector, uint8_t flags)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_vec h = d->vec[vector];
    if (!dpmi_handler_live(v, h)) { d->stats.stale++; return -EFAULT; }
    struct dpmi_record *r;
    if (dpmi_push_record(v, f, DPMI_REC_PMINT, &r)) return -ENOSPC;
    r->vector = vector;
    r->flags = flags;
    uint32_t frame[3] = {(flags & DPMI_RF_HWINT) ? DPMI_PM_IRQRET : DPMI_PM_SOFTRET, DPMI_HOST_CS,
                         (f->tf.eflags & 0x0CD5u) | (v->monitor.vif ? V86_IF : 0) | 2u};
    struct v86_frame n;
    if (v->frame_pm) n = *f;
    else { memset(&n, 0, sizeof(n)); dpmi_gprs(&n, f); }
    int e = handler_frame(v, f, &n, frame, ARRAY_SIZE(frame));
    if (e) { dpmi_unwind(v, d->depth - 1); return e; }
    r->arg[0] = n.tf.user_esp + 3 * dpmi_width(v);
    n.tf.cs = h.sel;
    n.tf.eip = h.off;
    n.tf.eflags = (f->tf.eflags & 0x0CD5u & ~V86_TF) | V86_IF | 2u;
    dpmi_set_pm(v, true);
    *f = n;
    v->monitor.vif = false;
    return 0;
}

/* The interrupted context comes from the kernel record, never from the
 * guest stack, so the handler may IRET from any stack (handlers that copy
 * their frame to a private stack). IRQRET can skip abandoned nested records,
 * but only to this client's newest host-owned hardware continuation. With
 * no such continuation the return is forged or stale and ends the VM. */
static int pmint_return(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_record *r = d->depth ? &d->rec[d->depth - 1] : 0;
    if (!r || r->client != d->clients || r->kind != DPMI_REC_PMINT ||
        (r->flags & DPMI_RF_HWINT ? f->tf.eip != DPMI_PM_IRQRET : f->tf.eip != DPMI_PM_SOFTRET)) {
        unsigned depth = d->depth;
        r = 0;
        if (f->tf.eip == DPMI_PM_IRQRET) {
            for (unsigned n = depth; n; n--) {
                struct dpmi_record *candidate = &d->rec[n - 1];
                if (candidate->client != d->clients || !(candidate->flags & DPMI_RF_HWINT)) continue;
                r = candidate;
                for (unsigned i = n; i < depth; i++) dosvm_isr_end(v, &d->rec[i]);
                dpmi_unwind(v, n);
                v->perf.isr_late_return++;
                uint64_t tick = g_ticks;
                if (!v->perf.isr_late_logged || tick - v->perf.isr_late_tick >= 1000) {
                    v->perf.isr_late_logged = true; v->perf.isr_late_tick = tick;
                    klog("[dosvm] isr late_return vm=%u gen=%u depth=%u skipped=%u cs=%04x eip=%08x",
                         v->info.key.id, v->info.key.generation, depth, depth - n,
                         f->tf.cs & 0xFFFF, f->tf.eip);
                }
                break;
            }
        }
        if (!r) { d->stats.stale++; return -EFAULT; }
    }
    struct dpmi_record rec = *r;
    struct v86_frame result = *f;
    dosvm_isr_end(v, &rec);
    dpmi_unwind(v, d->depth - 1);
    dpmi_set_pm(v, rec.saved_pm);
    *f = rec.saved;
    if (rec.flags & DPMI_RF_RESULTS) {
        dpmi_gprs(f, &result);
        f->tf.eflags = (f->tf.eflags & ~0x08D5u) | (result.tf.eflags & 0x08D5u);
    }
    dosvm_vif_restore(v, rec.vif, DOSVM_IF_TRAMPOLINE);
    if (rec.flags & DPMI_RF_HWINT) {
        d->decode_valid = rec.decode_valid;
        d->decode_cs = rec.decode_cs;
        d->decode_eip = rec.decode_eip;
        d->decode_stack_valid = rec.decode_stack_valid;
        memcpy(d->decode_stack, rec.decode_stack, sizeof(d->decode_stack));
        d->decode_ds_valid = rec.decode_ds_valid;
        d->decode_stack_linear = rec.decode_stack_linear;
        d->decode_ds_base = rec.decode_ds_base;
    }
    return 0;
}

int dpmi_stub_trap(struct dosvm *v, struct v86_frame *f, uint32_t off)
{
    if (off < DPMI_PM_INT + 256) return dpmi_default_int(v, f, (uint8_t)off, true);
    if (off < DPMI_PM_EXC + 32) {
        unsigned n = off - DPMI_PM_EXC;
        int e = exception_unframe(v, f, 2 * dpmi_width(v));  /* chained: return address still on the stack */
        return e ? e : default_exception(v, f, n);
    }
    switch (off) {
    case DPMI_PM_IRQRET: case DPMI_PM_SOFTRET: return pmint_return(v, f);
    case DPMI_PM_EXCRET: return exception_unframe(v, f, 0);
    case DPMI_PM_CBRET: return dpmi_callback_return(v, f);
    case DPMI_PM_RAW: return dpmi_raw_to_real(v, f);
    case DPMI_PM_SAVE: return dpmi_far_return(v, f);
    default: return dpmi_exception(v, f, 13, 0);
    }
}

static bool fetch(struct dosvm *v, const struct v86_frame *f, const struct dpmi_seg *cs, unsigned i, uint8_t *b)
{
    uint32_t off = f->tf.eip + i;
    if (!cs->big) off &= 0xFFFFu;
    return off <= cs->limit && !dosvm_read(v, cs->base + off, b, 1);
}

/* INT n arrives as #GP(n*8+2) with EIP at the instruction (or INTO). */
static int software_int(struct dosvm *v, struct v86_frame *f, uint8_t n)
{
    struct dpmi_seg cs;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs)) return dpmi_exception(v, f, 13, f->tf.err);
    unsigned len = 0;
    uint8_t op = 0, imm = 0;
    for (;;) {
        if (len >= 14 || !fetch(v, f, &cs, len, &op)) return dpmi_exception(v, f, 13, f->tf.err);
        len++;
        if (op == 0x66 || op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 || op == 0x26 ||
            op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
        break;
    }
    if (op == 0xCD) {
        if (!fetch(v, f, &cs, len, &imm) || imm != n) return dpmi_exception(v, f, 13, f->tf.err);
        len++;
    } else if (!(op == 0xCE && n == 4)) return dpmi_exception(v, f, 13, f->tf.err);
    f->tf.eip += len;
    if (!cs.big) f->tf.eip &= 0xFFFFu;
    return dpmi_pm_interrupt(v, f, n);
}

/* ---- f3-06: POPF compatibility for flags-saving CLI sections ----
 * At CPL 3 and IOPL 0, CLI/STI trap but PUSHF/POPF/IRET do not, and POPF
 * and IRET leave IF unchanged (Intel SDM Vol. 2, POPF/IRET operation; DPMI
 * 0.9 section 2.4: they "may not modify the state of the interrupt flag").
 * A client that saves its flags and disables (`pushf; cli` ... `popf`, or
 * Watcom's `pushfd; pop eax; cli` ... `push eax; popfd`: DMX in DOOM,
 * zz-doom5) then stays virtually disabled, and its timer-tick waits never
 * end (I_StartupTimer, I_ShutdownSound). Windows 95's remedy (R. Chen,
 * "Getting MS-DOS games to run on Windows 95: The interrupt flag", The Old
 * New Thing, 2016-04-11): at the trapped CLI, when the code just before it
 * is that pattern and the saved image matches the current flags, copy the
 * virtual IF into the image's TF. The POPF or IRET that later loads the
 * image sets TF, and the single-step trap one instruction later tells the
 * host that the client re-enabled: the host clears TF and sets the virtual
 * IF. Nothing is stepped in between and IOPL stays 0. The independent
 * F3-44 bounded fallback below covers unobserved restores. An image saved
 * while virtually disabled is left
 * alone: its restore keeps IF clear, as on hardware. Once a client holds an
 * armed image, a single-step trap without a watchpoint hit is the host's
 * (a client debugger single-stepping it is not supported). */
static uint32_t *pop_register(struct v86_frame *f, uint8_t op)
{
    switch (op) {
    case 0x58: return &f->tf.eax;
    case 0x59: return &f->tf.ecx;
    case 0x5A: return &f->tf.edx;
    case 0x5B: return &f->tf.ebx;
    case 0x5D: return &f->tf.ebp;
    case 0x5E: return &f->tf.esi;
    case 0x5F: return &f->tf.edi;
    default: return 0;                      /* 5Ch POP ESP: not an image */
    }
}
/* Conservative instruction lengths for a forward walk, never a backwards
 * byte signature. Unsupported encodings stop the walk. No operand is read or
 * instruction executed here. A transfer ends the straight-line boundary proof. */
static unsigned flags_instruction(const uint8_t *b, unsigned n, bool big, uint8_t *opcode, unsigned *width)
{
    unsigned at = 0, w = big ? 4 : 2, a = w, imm = 0;
    bool modrm = false;
    uint8_t op;
    do {
        if (at >= n) return 0;
        op = b[at++];
        if (op == 0x66) { w = big ? 2 : 4; continue; }
        if (op == 0x67) { a = big ? 2 : 4; continue; }
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
        break;
    } while (true);
    *opcode = op; *width = w;
    if (op < 0x40 && (op & 7) <= 5) {
        modrm = (op & 7) < 4;
        imm = (op & 7) == 4 ? 1 : (op & 7) == 5 ? w : 0;
    } else if ((op >= 0x40 && op <= 0x61) || (op >= 0x90 && op <= 0x9F && op != 0x9A) ||
               op == 0xFA || op == 0xFB || op == 0xFC || op == 0xFD ||
               op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 || op == 0x17 || op == 0x1E || op == 0x1F) {
        /* no operands */
    } else if (op >= 0xB0 && op <= 0xBF) imm = op < 0xB8 ? 1 : w;
    else if (op == 0x68 || op == 0xA9) imm = w;
    else if (op == 0x6A || op == 0xA8 || (op >= 0xE4 && op <= 0xE7) || op == 0xCD) imm = 1;
    else if (op >= 0xA0 && op <= 0xA3) imm = a;
    else if (op == 0x69 || op == 0x6B || (op >= 0x80 && op <= 0x8F) ||
             op == 0xC0 || op == 0xC1 || op == 0xC6 || op == 0xC7 ||
             (op >= 0xD0 && op <= 0xD3) || op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) {
        modrm = true;
        if (op == 0x69 || op == 0x81 || op == 0xC7) imm = w;
        else if (op == 0x6B || op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) imm = 1;
    } else if (op == 0x0F) {
        if (at >= n) return 0;
        uint8_t ext = b[at++];
        if ((ext >= 0x40 && ext <= 0x4F) || (ext >= 0x90 && ext <= 0x9F) ||
            ext == 0xAF || ext == 0xB6 || ext == 0xB7 || ext == 0xBE || ext == 0xBF) modrm = true;
        else return 0;
    } else if (op >= 0xEC && op <= 0xEF) { /* port I/O */ }
    else return 0;
    if (modrm) {
        if (at >= n) return 0;
        unsigned m = b[at++], mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
        if ((op == 0xFF && reg > 1) || (op == 0x8F && reg) ||
            ((op == 0xC6 || op == 0xC7) && reg)) return 0;
        if ((op == 0xF6 || op == 0xF7) && reg <= 1) imm = op == 0xF6 ? 1 : w;
        if (mod != 3) {
            if (a == 4 && rm == 4) {
                if (at >= n) return 0;
                unsigned base = b[at++] & 7;
                if (!mod && base == 5) at += 4;
            } else if (!mod && rm == (a == 4 ? 5u : 6u)) at += a;
            if (mod == 1) at++;
            if (mod == 2) at += a;
        }
    }
    at += imm;
    return at <= n && at <= 15 ? at : 0;
}
/* One proof, one local checked code window. Never reuse bytes across guest
 * execution, descriptor edits or hooks. A speculative refill failure falls
 * back to precisely the range the scalar proof would have read. */
struct flags_code { uint32_t at; unsigned n; uint8_t bytes[128]; };
static const uint8_t *flags_read(struct dosvm *v, const struct dpmi_seg *cs,
                                 struct flags_code *c, uint32_t at, unsigned n)
{
    if (at < c->at || at - c->at > c->n || n > c->n - (at - c->at)) {
        uint32_t linear = cs->base + at;
        unsigned count = PAGE_SIZE - (linear & (PAGE_SIZE - 1));
        if (count > sizeof(c->bytes)) count = sizeof(c->bytes);
        if (cs->limit - at < count - 1) count = cs->limit - at + 1;
        if (count < n || (linear >= 0xA0000 && linear < 0xC0000)) count = n;
        if (dosvm_read(v, linear, c->bytes, count)) {
            count = n;
            if (dosvm_read(v, linear, c->bytes, count)) return 0;
        }
        c->at = at; c->n = count;
    }
    return c->bytes + (at - c->at);
}
/* fx-16: *reached (optional) reports a decode that landed exactly on the
 * CLI. flags_instruction refuses every transfer, so such a decode is a
 * straight line from an observed boundary: the bytes before the CLI are
 * known instructions, a failed proof is a disproof, and the Windows 95
 * pattern rule (flags_pattern) must not arm. */
static bool flags_preceding_reach(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                                  uint8_t *pop, unsigned *width, bool boundary_only, bool *reached);
static bool flags_preceding(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                            uint8_t *pop, unsigned *width, bool boundary_only)
{ return flags_preceding_reach(v, f, cs, pop, width, boundary_only, 0); }
static bool flags_preceding_reach(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                                  uint8_t *pop, unsigned *width, bool boundary_only, bool *reached)
{
    struct dpmi *d = &v->dpmi;
    if (reached) *reached = false;
    uint32_t at = d->decode_eip, end = f->tf.eip;
    if (!d->decode_valid || d->decode_cs != (uint16_t)f->tf.cs || at > end ||
        end > cs->limit || end - at > 4096) return false;
    uint8_t previous = 0, last = 0;
    unsigned previous_w = 0, last_w = 0;
    struct flags_code cache = {0};
    while (at < end) {
        uint8_t op;
        unsigned n = end - at < 15 ? end - at : 15, w;
        const uint8_t *bytes = flags_read(v, cs, &cache, at, n);
        if (!bytes) return false;
        unsigned len = flags_instruction(bytes, n, cs->big, &op, &w);
        if (!len) return false;
        previous = last; previous_w = last_w; last = op; last_w = w;
        at += len;
    }
    if (reached) *reached = at == end;
    if (boundary_only) return true;
    if (last == 0x9C) { *pop = 0; *width = last_w; return true; }
    if (previous != 0x9C || previous_w != last_w || !pop_register(f, last)) return false;
    *pop = last; *width = last_w; return true;
}

/* A save can live in a lower-addressed function called after the last
 * trapped OUT (DMX timer setup). Decode control flow from that known
 * boundary, never backwards from CLI. The snapshot permits root RET only
 * along paths with known stack effects and no writes that could replace
 * its return slots (absolute stores outside the stack are harmless).
 * Calls have their own checked return slots; indirect transfers stop. */
struct flags_path {
    uint32_t at;
    int32_t sp;
    uint8_t previous, last, previous_w, last_w, depth;
    bool stack_valid, ds_valid;
    struct { uint32_t pc; int32_t sp; bool valid; uint8_t width; } call[4];
};
static void flags_stack_store(struct flags_path *p, const struct dpmi *d, bool known, uint32_t address, unsigned bytes)
{
    uint64_t end = (uint64_t)address + bytes;
    if (!known || (address < (uint64_t)d->decode_stack_linear + sizeof(d->decode_stack) && end > d->decode_stack_linear))
        p->stack_valid = false;
    for (unsigned i = 0; i < p->depth; i++) {
        uint32_t slot = d->decode_stack_linear + (uint32_t)p->call[i].sp;
        if (!known || (address < (uint64_t)slot + 4 && end > slot)) p->call[i].valid = false;
    }
}
static unsigned flags_opcode(const uint8_t *b, unsigned n, bool big, unsigned *w)
{
    *w = big ? 4 : 2;
    for (unsigned i = 0; i < n; i++) {
        uint8_t op = b[i];
        if (op == 0x66) { *w = big ? 2 : 4; continue; }
        if (op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) continue;
        return i;
    }
    return n;
}
static bool flags_stack_effect(struct flags_path *p, const struct dpmi *d, const uint8_t *b,
                                unsigned pos, unsigned len, unsigned w, bool big)
{
    uint8_t op = b[pos];
    unsigned a = big ? 4 : 2;
    bool absolute = p->ds_valid;
    for (unsigned i = 0; i < pos; i++) {
        if (b[i] == 0x67) a = big ? 2 : 4;
        if (b[i] != 0x66 && b[i] != 0x67) absolute = false;
    }
    if ((op >= 0x50 && op <= 0x57) || op == 0x9C || op == 0x68 || op == 0x6A ||
        op == 0x06 || op == 0x0E || op == 0x16 || op == 0x1E) {
        p->sp -= w;
        flags_stack_store(p, d, true, d->decode_stack_linear + (uint32_t)p->sp, w);
    } else if ((op >= 0x58 && op <= 0x5F) || op == 0x9D || op == 0x07 || op == 0x1F) {
        if (op == 0x5C) return false; /* POP ESP */
        if (op == 0x1F) p->ds_valid = false;
        p->sp += w;
    } else if (op == 0x60) {
        p->sp -= 8 * w;
        flags_stack_store(p, d, true, d->decode_stack_linear + (uint32_t)p->sp, 8 * w);
    }
    else if (op == 0x61) p->sp += 8 * w;
    else if (op == 0x44 || op == 0x4C || op == 0x94 || op == 0xBC || op == 0x17) return false;
    else if (op == 0xA2 || op == 0xA3) {
        uint32_t off = a == 4 ? vm_u32(b + pos + 1) : vm_u16(b + pos + 1);
        flags_stack_store(p, d, absolute, d->decode_ds_base + off, op == 0xA2 ? 1 : w);
    }
    else {
        bool alu = op < 0x40 && (op & 7) < 4;
        bool group = op >= 0x80 && op <= 0x83;
        bool move = op == 0x88 || op == 0x89 || op == 0x8C || op == 0xC6 || op == 0xC7;
        bool load = op == 0x8A || op == 0x8B || op == 0x8D;
        bool rm_write = move || (alu && !(op & 2) && op < 0x38);
        if (alu || group || move || load || op == 0x86 || op == 0x87 || op == 0x8F ||
            op == 0x8E || op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3) ||
            op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) {
            unsigned m = b[pos + 1], mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
            if (op == 0x8E && reg == 2) return false; /* SS assignment */
            if (op == 0x8E && reg == 3) p->ds_valid = false;
            if ((load || (alu && (op & 2) && op < 0x38)) && reg == 4) return false;
            if (group && reg != 7) rm_write = true;
            if (op == 0x86 || op == 0x87) { rm_write = true; if (reg == 4) return false; }
            if (op == 0x8F) { rm_write = true; p->sp += w; }
            if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3) || op == 0xFE || op == 0xFF ||
                ((op == 0xF6 || op == 0xF7) && (reg == 2 || reg == 3))) rm_write = true;
            if (rm_write && mod != 3) {
                bool known = absolute && mod == 0 && rm == (a == 4 ? 5u : 6u);
                uint32_t off = known ? (a == 4 ? vm_u32(b + pos + 2) : vm_u16(b + pos + 2)) : 0;
                bool byte = op == 0x88 || op == 0x86 || op == 0x80 || op == 0x82 || op == 0xC6 ||
                    op == 0xC0 || op == 0xD0 || op == 0xD2 || op == 0xF6 || op == 0xFE;
                flags_stack_store(p, d, known, d->decode_ds_base + off, byte ? 1 : op == 0x8C ? 2 : w);
            }
            if (rm_write && mod == 3 && rm == 4) {
                if ((op != 0x81 && op != 0x83) || (reg != 0 && reg != 5)) return false;
                int32_t imm = op == 0x83 ? (int8_t)b[len - 1] : w == 4 ? (int32_t)vm_u32(b + len - 4) : (int16_t)vm_u16(b + len - 2);
                if (imm < -4096 || imm > 4096) return false;
                p->sp += reg == 0 ? imm : -imm;
            }
        } else if (op == 0x0F) {
            /* The supported two-byte instructions with a register result
             * cannot assign ESP in a return proof. SETcc writes invalidate
             * the root snapshot if their destination is memory. */
            unsigned ext = b[pos + 1], m = b[pos + 2];
            if (ext >= 0x90 && ext <= 0x9F) {
                if ((m >> 6) != 3) flags_stack_store(p, d, false, 0, 1);
            }
            else if (((m >> 3) & 7) == 4) return false;
        }
    }
    return p->sp >= -4096 && p->sp <= 4096;
}
static bool flags_control_preceding(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                                    uint8_t *pop, unsigned *width)
{
    struct dpmi *d = &v->dpmi;
    if (!d->decode_valid || d->decode_cs != (uint16_t)f->tf.cs || f->tf.eip > cs->limit) return false;
    struct flags_path paths[32] = {{.at=d->decode_eip, .stack_valid=d->decode_stack_valid, .ds_valid=d->decode_ds_valid}};
    struct { uint32_t at; int32_t sp; unsigned depth; } seen[64];
    unsigned head = 0, tail = 1, visited = 0, budget = 4096;
    struct flags_code cache = {0};
    while (head < tail && budget) {
        struct flags_path p = paths[head++];
        while (budget && p.at <= cs->limit) {
            if (p.at == f->tf.eip) {
                if (p.last == 0x9C) { *pop = 0; *width = p.last_w; return true; }
                if (p.previous == 0x9C && p.previous_w == p.last_w && pop_register(f, p.last)) {
                    *pop = p.last; *width = p.last_w; return true;
                }
                break;
            }
            bool duplicate = false;
            for (unsigned i = 0; i < visited; i++)
                if (seen[i].at == p.at && seen[i].sp == p.sp && seen[i].depth == p.depth) { duplicate = true; break; }
            if (duplicate) break;
            if (visited < ARRAY_SIZE(seen)) {
                seen[visited].at = p.at; seen[visited].sp = p.sp; seen[visited++].depth = p.depth;
            }
            uint8_t op;
            unsigned n = cs->limit - p.at >= 14 ? 15 : cs->limit - p.at + 1, w;
            const uint8_t *b = flags_read(v, cs, &cache, p.at, n);
            if (!b) break;
            unsigned pos = flags_opcode(b, n, cs->big, &w);
            if (pos == n) break;
            op = b[pos];
            bool call = op == 0xE8, jump = op == 0xE9 || op == 0xEB;
            bool cond = op >= 0x70 && op <= 0x7F;
            bool near_cond = op == 0x0F && pos + 1 < n && b[pos + 1] >= 0x80 && b[pos + 1] <= 0x8F;
            if (call || jump || cond || near_cond) {
                unsigned imm_at = pos + (near_cond ? 2 : 1), iw = cond || op == 0xEB ? 1 : w;
                unsigned len = imm_at + iw;
                if (len > n || len > budget) break;
                budget -= len;
                int32_t rel = iw == 1 ? (int8_t)b[imm_at] : iw == 2 ? (int16_t)vm_u16(b + imm_at) : (int32_t)vm_u32(b + imm_at);
                uint32_t next = p.at + len, target = next + (uint32_t)rel;
                if (iw == 2 || !cs->big) target &= 0xFFFF;
                p.previous = p.last = 0;
                if (call) {
                    if (p.depth == ARRAY_SIZE(p.call)) break;
                    p.sp -= w;
                    flags_stack_store(&p, d, true, d->decode_stack_linear + (uint32_t)p.sp, w);
                    p.call[p.depth].pc = w == 2 ? next & 0xFFFFu : next;
                    p.call[p.depth].sp = p.sp; p.call[p.depth].width = (uint8_t)w; p.call[p.depth++].valid = true;
                    p.at = target;
                } else if (jump) p.at = target;
                else {
                    if (tail < ARRAY_SIZE(paths)) { paths[tail] = p; paths[tail++].at = target; }
                    p.at = next;
                }
                continue;
            }
            if (op == 0xC3) {
                if (pos + 1 > budget) break;
                budget -= pos + 1;
                uint32_t target = 0;
                if (p.depth) {
                    if (p.sp != p.call[p.depth - 1].sp || !p.call[p.depth - 1].valid || p.call[p.depth - 1].width != w) break;
                    target = p.call[--p.depth].pc;
                } else {
                    if (!p.stack_valid || p.sp < 0 || (unsigned)p.sp > sizeof(d->decode_stack) - w) break;
                    memcpy(&target, (const uint8_t *)d->decode_stack + p.sp, w);
                }
                p.sp += w; p.at = target; p.previous = p.last = 0;
                continue;
            }
            unsigned len = flags_instruction(b, n, cs->big, &op, &w);
            if (!len || len > budget || !flags_stack_effect(&p, d, b, pos, len, w, cs->big)) break;
            budget -= len;
            p.previous = p.last; p.previous_w = p.last_w; p.last = op; p.last_w = (uint8_t)w;
            p.at += len;
            if (!cs->big) p.at &= 0xFFFF;
        }
    }
    return false;
}
/* ---- fx-16: proven CLI sites and seed demand ----
 * T23 trial e831e685 (BOOT.LOG vm_perf banks n=7/n=8, DOOM full screen):
 * the monitor took 1.62-1.63 s of every 5 s, as much as DOOM itself.
 * 6536-6719 return-tracker checkpoints (#DB, 37 us each), 7236-7568 PM
 * CLI/STI traps at 57 us each (the forward proofs below) and, on every PM
 * trap, a boundary seed (up to 16 decoded instructions plus a stack
 * snapshot). All of that only keeps a fresh boundary for the next CLI's
 * proof, although DOOM/DMX execute the same few flags-saving CLI sites
 * thousands of times per second. Windows 95 inspected the code before the
 * CLI on every trap (R. Chen, The Old New Thing, 2016-04-11); here the
 * adjacency is proven once by the unchanged forward decode, the proven
 * bytes are kept, and every later CLI at that address compares them before
 * the live image check that already guards every arm. */
#define DPMI_SEED_IDLE_MS 1000u
static bool dpmi_seed_wanted(const struct dosvm *v)
{
    const struct dpmi *d = &v->dpmi;
    return !d->cli_settled || g_ticks - d->cli_miss_tick < DPMI_SEED_IDLE_MS;
}
static struct dpmi_cli_site *cli_site_find(struct dosvm *v, const struct dpmi_seg *cs, uint32_t linear)
{
    struct dpmi *d = &v->dpmi;
    for (unsigned i = 0; i < DPMI_CLI_SITES; i++) {
        struct dpmi_cli_site *s = &d->cli_site[i];
        if (!s->used || s->linear != linear || s->big != cs->big || s->client != d->clients) continue;
        uint8_t code[DPMI_CLI_BYTES];
        if (dosvm_read(v, linear - s->len, code, s->len) || memcmp(code, s->code, s->len)) {
            s->used = 0;                 /* rewritten code: prove it again */
            return 0;
        }
        s->used = ++d->cli_clock ? d->cli_clock : 1;
        return s;
    }
    return 0;
}
/* distance: bytes before the CLI the proof covered (from its observed
 * boundary, or the inspected pattern). The kept window is always the
 * DPMI_CLI_BYTES before the CLI (a 15-byte instruction plus one), so a
 * rewritten instruction just before a short proof is noticed as well. */
static void cli_site_store(struct dosvm *v, const struct v86_frame *f, const struct dpmi_seg *cs,
                           uint8_t pop, unsigned width, uint32_t distance, bool provisional)
{
    struct dpmi *d = &v->dpmi;
    (void)distance;
    unsigned len = f->tf.eip < DPMI_CLI_BYTES ? f->tf.eip : DPMI_CLI_BYTES;
    uint32_t linear = cs->base + f->tf.eip;
    if (!len || f->tf.eip < len) return;
    struct dpmi_cli_site *slot = &d->cli_site[0];
    for (unsigned i = 0; i < DPMI_CLI_SITES; i++)
        if (!d->cli_site[i].used || d->cli_site[i].used < slot->used) slot = &d->cli_site[i];
    struct dpmi_cli_site s = {.linear = linear, .pop = pop, .width = (uint8_t)width,
        .len = (uint8_t)len, .big = cs->big, .client = (uint8_t)d->clients, .provisional = provisional};
    for (unsigned i = 0; i < DPMI_CLI_SITES; i++)   /* an upgrade replaces its own entry */
        if (d->cli_site[i].used && d->cli_site[i].linear == linear) { slot = &d->cli_site[i]; break; }
    if (dosvm_read(v, linear - len, s.code, len)) return;
    s.used = ++d->cli_clock ? d->cli_clock : 1;
    *slot = s;
    d->cli_settled = true;
}
/* fx-16, the Windows 95 rule (Chen, above): when no forward proof reaches
 * the CLI, the code immediately before it must be PUSHF, or PUSHF; POP r
 * with matching operand sizes (Watcom's `pushfd; pop eax; cli`, DMX). The
 * caller then still requires the saved image (stack top or register) to
 * equal the live flags image before it sets TF, the check Windows 95 also
 * made. T23/host evidence: DMX's timer service at 01df:00244264 is reached
 * through `sti; ... lss esp` from its dispatcher; no forward decode can
 * cross the stack switch, so the old monitor left virtual IF clear after
 * DMX's POPFD for the rest of the timer handler, blocking the SB16 IRQ DMX
 * had re-enabled with STI (1134 misses in the host DOOM replay). */
static bool flags_pattern(struct dosvm *v, const struct v86_frame *f, const struct dpmi_seg *cs,
                          uint8_t *pop, unsigned *width)
{
    uint8_t b[5];
    if (f->tf.eip < sizeof(b) || f->tf.eip - sizeof(b) > cs->limit ||
        dosvm_read(v, cs->base + f->tf.eip - sizeof(b), b, sizeof(b))) return false;
    unsigned normal = cs->big ? 4 : 2, other = cs->big ? 2 : 4;
    if (b[4] == 0x9C) {                                    /* [66] 9C | FA */
        *pop = 0; *width = b[3] == 0x66 ? other : normal;
        return true;
    }
    if (b[4] < 0x58 || b[4] > 0x5F || b[4] == 0x5C) return false;
    unsigned at = 3, pop_w = normal;                       /* [66] 9C [66] 5r | FA */
    if (b[at] == 0x66) { pop_w = other; at--; }
    if (b[at] != 0x9C) return false;
    unsigned push_w = at && b[at - 1] == 0x66 ? other : normal;
    if (push_w != pop_w) return false;
    *pop = b[4]; *width = pop_w;
    return true;
}
void dpmi_cli_sites_reset(struct dosvm *v)
{
    struct dpmi *d = &v->dpmi;
    memset(d->cli_site, 0, sizeof(d->cli_site));
    d->cli_settled = false;
}
static bool arm_flags_image(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                            struct dpmi_armed *planted)
{
    /* Neither forward proof changes the candidate image. If no possible
     * POP register or stack word even matches its low half, the old final
     * equality check cannot succeed. Keep the full proof for every match. */
    uint16_t want16 = (f->tf.eflags & 0x0CD5u) | V86_IF | 2u;
    bool candidate = false;
    for (unsigned op = 0x58; op <= 0x5F; op++) {
        uint32_t *r = pop_register(f, (uint8_t)op);
        if (r && (uint16_t)*r == want16) { candidate = true; break; }
    }
    if (!candidate) {
        struct dpmi_seg ss;
        uint32_t lin;
        uint16_t image;
        uint16_t sel = (uint16_t)f->tf.user_ss;
        if (!dpmi_seg(v, sel, &ss) || !dpmi_ptr(v, sel,
            ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp, 2, false, &lin) ||
            dosvm_read(v, lin, &image, 2) || image != want16) return false;
    }
    uint8_t pop;
    unsigned width;
    struct dpmi *d = &v->dpmi;
    d->cli.candidates++;
    struct dpmi_cli_site *site = cli_site_find(v, cs, cs->base + f->tf.eip);
    bool reached = false;
    if (site && site->provisional && d->decode_valid && d->decode_cs == (uint16_t)f->tf.cs &&
        d->decode_eip < f->tf.eip && f->tf.eip - d->decode_eip <= 256) {
        /* A pattern site meets a nearby boundary before it: decode once
         * more (bounded, so an undecodable gap is not paid on every CLI). */
        if (flags_preceding_reach(v, f, cs, &pop, &width, false, &reached)) {
            cli_site_store(v, f, cs, pop, width, f->tf.eip - d->decode_eip, false);
            site = 0; d->cli.proved++;
            goto proven;
        }
        if (reached) { site->used = 0; d->cli.missed++; return false; }   /* disproved */
    }
    if (site) { pop = site->pop; width = site->width; d->cli.hits++; }
    else {
        /* Every CLI that needed a proof keeps the seeds coming for a
         * while: a new site is learned on its next execution at latest. */
        d->cli_miss_tick = g_ticks;
        if (flags_preceding_reach(v, f, cs, &pop, &width, false, &reached)) {
            d->cli.proved++;
            cli_site_store(v, f, cs, pop, width, f->tf.eip - d->decode_eip, false);
        } else if (flags_control_preceding(v, f, cs, &pop, &width)) d->cli.control++;
        else if (!reached && flags_pattern(v, f, cs, &pop, &width)) {
            d->cli.pattern++;
            cli_site_store(v, f, cs, pop, width, 5, true);
        } else { d->cli.missed++; return false; }
    }
proven:;
    /* PUSHF's image of the real flags: IF set (the host never runs a client
     * disabled), IOPL 0, NT and TF clear, status flags as now (POP and the
     * trapped CLI change none). */
    /* PUSHFD also preserves AC/ID. RF/VM are cleared by PUSHFD. Comparing
     * against a truncated uint16_t rejected legitimate high flags. */
    uint32_t want = (f->tf.eflags & (width == 4 ? 0x00240CD5u : 0x0CD5u)) | V86_IF | 2u;
    if (!pop) {                             /* decoded PUSHF; CLI */
        struct dpmi_seg ss;
        uint16_t sel = (uint16_t)f->tf.user_ss;
        uint32_t lin, image = 0;
        if (!dpmi_seg(v, sel, &ss)) return false;
        uint32_t esp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
        if (!dpmi_ptr(v, sel, esp, width, true, &lin) || dosvm_read(v, lin, &image, width) || image != want) return false;
        image |= V86_TF;
        *planted = (struct dpmi_armed){.offset = esp, .image = image, .ss = sel, .width = (uint8_t)width,
                                       .client = (uint8_t)d->clients, .stack = true};
        return !dosvm_write(v, lin, &image, width);
    }
    uint32_t *r = pop_register(f, pop);
    if (!r || (width == 4 ? *r : (uint16_t)*r) != want) return false;
    *r |= V86_TF;
    *planted = (struct dpmi_armed){.width = (uint8_t)width, .client = (uint8_t)d->clients};
    return true;
}

/* 2026-10-25 (dos-dpmi-contract.md, "Selector 40h and nested saved flags
 * images"): the planted images still owned. Before, one owner flag was
 * replaced by every arm and cleared by every owned trap, so an enclosing
 * region's live image kept its TF without an owner and its POPF's #DB
 * reached the client (Warcraft II demo, 1:000576D0 around 1:00057608).
 * Now an image stays owned beside newer ones while it is provably live: a
 * stack image of this client on the current stack that the stack pointer
 * has not passed (at an arm, the slot the new image reuses counts as
 * passed) and whose bytes are unchanged. Everything else is forgotten at
 * the next arm or owned trap, as the flag was: register images (Watcom's
 * flags-in-EAX restore may never POPF), images popped into a register
 * (DOS/16M's INT 31h 0302h wrapper: POP CX / STI) and images on another
 * stack, whose planted TF is taken out first if they are still intact, so
 * no unowned TF is left behind. */
static bool armed_live(struct dosvm *v, const struct v86_frame *f, const struct dpmi_armed *a, bool arming)
{
    if (a->client != v->dpmi.clients || !a->stack || a->ss != (uint16_t)f->tf.user_ss) return false;
    struct dpmi_seg ss;
    uint32_t lin, image = 0;
    if (!dpmi_seg(v, a->ss, &ss)) return false;
    uint32_t esp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
    if (arming ? a->offset <= esp : a->offset < esp) return false;
    return dpmi_ptr(v, a->ss, a->offset, a->width, false, &lin) && !dosvm_read(v, lin, &image, a->width) &&
           image == a->image;
}
static void armed_unplant(struct dosvm *v, const struct v86_frame *f, const struct dpmi_armed *a)
{
    uint32_t lin, image = 0;
    if (!a->stack || a->client != v->dpmi.clients || a->ss == (uint16_t)f->tf.user_ss) return;
    if (!dpmi_ptr(v, a->ss, a->offset, a->width, true, &lin) || dosvm_read(v, lin, &image, a->width) ||
        image != a->image) return;
    image &= ~V86_TF;
    dosvm_write(v, lin, &image, a->width);
}
static void armed_prune(struct dosvm *v, const struct v86_frame *f, bool arming)
{
    struct dpmi *d = &v->dpmi;
    unsigned kept = 0;
    for (unsigned i = 0; i < d->if_armed && i < DPMI_IF_ARMED_MAX; i++) {
        if (armed_live(v, f, &d->armed[i], arming)) d->armed[kept++] = d->armed[i];
        else armed_unplant(v, f, &d->armed[i]);
    }
    d->if_armed = (uint8_t)kept;
}
static void armed_push(struct dosvm *v, const struct v86_frame *f, const struct dpmi_armed *planted)
{
    struct dpmi *d = &v->dpmi;
    armed_prune(v, f, true);
    if (d->if_armed >= DPMI_IF_ARMED_MAX) {
        /* Deeper nesting than tracked: take the TF out of the outermost
         * image (its POPF then restores nothing; IF recovery covers it). */
        const struct dpmi_armed *old = &d->armed[0];
        uint32_t lin, image = 0;
        if (dpmi_ptr(v, old->ss, old->offset, old->width, true, &lin) &&
            !dosvm_read(v, lin, &image, old->width) && image == old->image) {
            image &= ~V86_TF;
            dosvm_write(v, lin, &image, old->width);
        }
        memmove(&d->armed[0], &d->armed[1], (DPMI_IF_ARMED_MAX - 1) * sizeof(d->armed[0]));
        d->if_armed = DPMI_IF_ARMED_MAX - 1;
    }
    d->armed[d->if_armed++] = *planted;
}
/* An owned POPF/IRET trap loaded one planted image (the newest, by
 * nesting); only enclosing live images on this stack stay owned. */
static void armed_consume(struct dosvm *v, const struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (d->if_armed) d->if_armed--;
    armed_prune(v, f, false);
}

/* BS proves a stepped instruction even if it cleared saved TF. MOV/POP SS
 * defer this trap through the following instruction. Other faulting forms
 * retain TF until the monitor has handled them (CLI/INT/#PF). */
static bool flags_restored(struct dosvm *v, struct v86_frame *f, uint32_t dr6)
{
    struct dpmi *d = &v->dpmi;
    bool step = f->tf.vector == 1 ? (dr6 & (1u << 14)) != 0 : (f->tf.eflags & V86_TF) != 0;
    if (!d->if_armed || d->if_step || !step) return false;
    armed_consume(v, f);                /* enclosing regions' live images stay owned */
    f->tf.eflags &= ~V86_TF;
    dosvm_vif_restore(v, true, DOSVM_IF_POPF);
    d->stats.if_restored++;
    dosvm_trace(v, DOSVM_TRACE_FLAGS, f, 1, 1, 0);
    return true;
}

/* f3-34: acquire actual boundaries through sparse execute breakpoints.
 * The remaining budget counts decoding and exceptional fallback TF steps.
 * The safety predicate also serves the independent f3-15 recovery path. */
#define DPMI_RETURN_STEPS 512u
#define DPMI_RETURN_HOPS 32u
static bool return_context(const struct dpmi *d, const struct v86_frame *f)
{
    return (uint16_t)f->tf.cs == d->return_cs && (uint16_t)f->tf.user_ss == d->return_ss &&
           d->depth == d->return_depth && d->clients == d->return_client;
}
static bool return_step_safe(struct dosvm *v, const struct v86_frame *f)
{
    struct dpmi_seg cs;
    uint8_t b[15];
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || f->tf.eip > cs.limit) return false;
    unsigned n = cs.limit - f->tf.eip >= 14 ? 15 : cs.limit - f->tf.eip + 1;
    if (dosvm_read(v, cs.base + f->tf.eip, b, n)) return false;
    /* This PC is an actual CPU boundary, not a static control-flow guess.
     * Inspect every legacy prefix so even REP/LOCK PUSHF cannot save host
     * TF. Ordinary instructions are executed/validated by the CPU; faults
     * cancel tracking in pm_trap. Only operations that save/load flags,
     * suppress #DB through SS, or change our code/context need exclusion. */
    unsigned pos = 0;
    while (pos < n) {
        uint8_t op = b[pos];
        if (op == 0x66 || op == 0x67 || op == 0xF0 || op == 0xF2 || op == 0xF3 ||
            op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) pos++;
        else break;
    }
    if (pos == n) return false;
    unsigned op = b[pos];
    if (op == 0x9C || op == 0x9D || op == 0xFA || op == 0xFB || op == 0x17 ||
        op == 0xCF || op == 0xCB || op == 0xCA || op == 0xEA || op == 0x9A ||
        op == 0xCD || op == 0xCC || op == 0xCE || op == 0xF1) return false;
    if ((op == 0x8E || op == 0xFF) && pos + 1 == n) return false;
    if (op == 0x8E && ((b[pos + 1] >> 3) & 7) == 2) return false;
    if (op == 0xFF && (((b[pos + 1] >> 3) & 7) == 3 || ((b[pos + 1] >> 3) & 7) == 5)) return false;
    if (op == 0x0F) {
        if (pos + 1 == n) return false;
        unsigned ext = b[pos + 1];
        if (ext == 0xB2 || ext == 0x34 || ext == 0x35 || ext == 0x05 || ext == 0x07 || ext == 0xAA) return false;
    }
    return true;
}

/* Recovery uses actual CPU boundaries, never a backwards opcode guess.
 * See docs/validation/2026-10-14-f3-15-virtual-if.md. The time bound also
 * covers a restore that retired before tracing began, or an unsafe barrier. */
#define DPMI_IF_STEPS 4096u
#define DPMI_IF_WAIT_MS 2u
#define DPMI_ISR_WAIT_MS 50u
#define DPMI_IF_RECORDS 16u
static bool if_step_context(const struct dpmi *d, const struct v86_frame *f, bool retired)
{
    return (uint16_t)f->tf.cs == (retired && d->if_restore_step ? d->if_target_cs : d->if_step_cs) &&
           (uint16_t)f->tf.user_ss == d->if_step_ss && d->depth == d->if_step_depth &&
           d->clients == d->if_step_client;
}
static void if_recovery_record(struct dosvm *v, const struct v86_frame *f, uint64_t age,
                               const char *reason)
{
    struct dpmi *d = &v->dpmi;
    if (!(g_boot.flags & CBI_F_VERBOSE) || d->if_records >= DPMI_IF_RECORDS) return;
    d->if_records++;
    uint8_t code[16] = {0}; uint32_t stack[3] = {0}, lin;
    struct dpmi_seg cs, ss;
    bool code_ok = dpmi_seg(v, (uint16_t)f->tf.cs, &cs) && f->tf.eip <= cs.limit &&
        cs.limit - f->tf.eip >= sizeof(code) - 1 && !dosvm_read(v, cs.base + f->tf.eip, code, sizeof(code));
    bool stack_ok = dpmi_seg(v, (uint16_t)f->tf.user_ss, &ss) &&
        dpmi_ptr(v, (uint16_t)f->tf.user_ss, ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp,
                 sizeof(stack), false, &lin) && !dosvm_read(v, lin, stack, sizeof(stack));
    static const char hex[] = "0123456789abcdef";
    char bytes[33];
    for (unsigned i = 0; i < sizeof(code); i++) { bytes[i * 2] = hex[code[i] >> 4]; bytes[i * 2 + 1] = hex[code[i] & 15]; }
    bytes[32] = 0;
    uint32_t flags = irq_save();
    unsigned irr = v->monitor.pic[0].irr | (unsigned)v->monitor.pic[1].irr << 8;
    unsigned isr = v->monitor.pic[0].isr | (unsigned)v->monitor.pic[1].isr << 8;
    unsigned imr = v->monitor.pic[0].imr | (unsigned)v->monitor.pic[1].imr << 8;
    irq_restore(flags);
    /* Two bounded rows form one watchdog snapshot; dosvm_diag defers both
     * during a guest IRQ, so the UART cannot hold up the handler. */
    dosvm_diag(v, "group=vm_if_stall vm=%u gen=%u pc=%04x:%08x vif=%u arm=%u seed=%u:%04x:%08x irr=%04x isr=%04x imr=%04x ms=%llu steps=%u reason=%s",
        v->info.key.id, v->info.key.generation, f->tf.cs & 0xFFFF, f->tf.eip, v->monitor.vif,
        d->if_armed, d->decode_valid, d->decode_cs, d->decode_eip, irr, isr, imr, age, d->if_steps, reason);
    dosvm_diag(v, "group=vm_if_stall_image vm=%u gen=%u pc=%04x:%08x code=%s ss=%04x esp=%08x ok=%u,%u stack=%08x,%08x,%08x path=trap",
        v->info.key.id, v->info.key.generation, f->tf.cs & 0xFFFF, f->tf.eip, bytes,
        f->tf.user_ss & 0xFFFF, f->tf.user_esp, code_ok, stack_ok, stack[0], stack[1], stack[2]);
}
static void if_recovery_end(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (d->if_step && if_step_context(d, f, false)) f->tf.eflags &= ~V86_TF;
    d->if_waiting = d->if_step = d->if_restore_step = false;
    d->if_steps = 0;
}
static bool if_recovery_trap(struct dosvm *v, struct v86_frame *f, uint32_t dr6)
{
    struct dpmi *d = &v->dpmi;
    if (!d->if_step) return false;
    bool bs = f->tf.vector == 1 && (dr6 & (1u << 14));
    if (bs && if_step_context(d, f, true)) {
        f->tf.eflags &= ~V86_TF;
        d->if_steps++;
        if (d->if_restore_step && d->if_restore_vif &&
            f->tf.eip == d->if_target_eip && f->tf.user_esp == d->if_target_sp) {
            dosvm_vif_restore(v, true, DOSVM_IF_POPF);
            d->if_armed = false;
            d->stats.if_restored++;
            dosvm_boundary(v, f, "if_step_restore");
        }
        d->if_step = d->if_restore_step = false;
        /* B0-B3/BD/BT remain client events even when BS is monitor-owned. */
        return !(dr6 & (15u | (1u << 13) | (1u << 15)));
    }
    if (f->tf.vector >= 0x20 && f->tf.vector < 0x30) return false; /* not retirement */
    if (if_step_context(d, f, false)) f->tf.eflags &= ~V86_TF;
    d->if_step = d->if_restore_step = false; /* fault/debugger: do not claim a restore */
    return false;
}
static void if_recovery_prepare(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    uint32_t flags = irq_save();
    dosvm_pic_cascade(&v->monitor);
    bool pending = dosvm_pic_pending(&v->monitor) >= 0;
    uint64_t tick = g_ticks;
    irq_restore(flags);
    if (!v->frame_pm || v->monitor.vif || !pending) { if_recovery_end(v, f); return; }
    /* Handlers/callbacks are not ordinary client code. F3-44 expires stale
     * hardware obligations by evidence/time; their retained late-return
     * continuations cannot lock virtual IF. Other host handler frames
     * still have an observed return and keep their explicit IF state. */
    for (unsigned i = 0; i < d->depth; i++) {
        const struct dpmi_record *r = &d->rec[i];
        bool handler = r->flags & DPMI_RF_HWINT ? !r->isr_expired :
            r->kind == DPMI_REC_PMINT || r->kind == DPMI_REC_EXCEPTION || r->kind == DPMI_REC_CALLBACK;
        if (!handler) continue;
        if_recovery_end(v, f);
        return;
    }
    if (!d->if_waiting) { d->if_waiting = true; d->if_wait_tick = tick; d->if_steps = 0; }
    uint64_t age = tick - d->if_wait_tick;
    /* Host stubs and halted clients are not ordinary client execution.
     * Do not turn a scheduling delay at such a boundary into IF evidence. */
    bool ordinary = (uint16_t)f->tf.cs != DPMI_HOST_CS && v->monitor.state != V86_HALTED;
    if (ordinary && age >= DPMI_IF_WAIT_MS) {
        if_recovery_record(v, f, age, "time");
        if_recovery_end(v, f);
        /* A previously patched saved image can still load TF later. Keep
         * its owner until flags_restored consumes it; never orphan that TF. */
        dosvm_vif_restore(v, true, DOSVM_IF_FORCED);
        dosvm_boundary(v, f, "if_forced");
        return;
    }
    if (d->if_step || d->if_steps >= DPMI_IF_STEPS) return;
    /* fx-16: an armed saved image reports its own restore one instruction
     * after its POPF/IRET; stepping toward it only adds #DB exits (37 us
     * each on the T23). The time bound above still applies. */
    if (d->if_armed) return;
    /* Return acquisition is inactive when virtual IF is clear. Retire
     * its checkpoint before recovery; never clear a client's TF here. */
    if (d->return_steps && return_context(d, f)) dpmi_return_cancel(v);
    if (f->tf.eflags & (V86_TF | (1u << 14)) || d->dr7) return;
    struct dpmi_seg cs;
    uint8_t b[15]; unsigned w;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || f->tf.eip > cs.limit) return;
    unsigned n = cs.limit - f->tf.eip >= 14 ? 15 : cs.limit - f->tf.eip + 1;
    if (dosvm_read(v, cs.base + f->tf.eip, b, n)) return;
    unsigned pos = flags_opcode(b, n, cs.big, &w);
    if (pos == n) return;
    bool restore = b[pos] == 0x9D || b[pos] == 0xCF;
    if (restore) {
        /* Exact boundary, checked image. TF was already set at instruction
         * entry, so BS occurs even when POPF/IRET clears it (Intel SDM).
         * Restrict IRET to ordinary same-ring frames, leaving CPU validation
         * and execution to the CPU. No speculative stack writes. */
        for (unsigned i = 0; i < pos; i++) if (b[i] != 0x66) return;
        struct v86_frame next = *f;
        uint32_t image;
        if (b[pos] == 0xCF) {
            uint32_t ip, sel;
            if (!dpmi_pop(v, &next, &ip, w) || !dpmi_pop(v, &next, &sel, w) ||
                !dpmi_pop(v, &next, &image, w) || (sel & 3) != 3 ||
                !dpmi_handler_live(v, (struct dpmi_vec){.off=ip, .sel=(uint16_t)sel})) return;
            next.tf.cs = (uint16_t)sel; next.tf.eip = ip;
        } else {
            if (!dpmi_pop(v, &next, &image, w)) return;
            next.tf.eip = cs.big ? f->tf.eip + pos + 1 : (uint16_t)(f->tf.eip + pos + 1);
        }
        if (image & (V86_VM | (1u << 14))) return;
        if ((image & V86_TF) && !d->if_armed) return; /* a client's debugger TF */
        d->if_restore_step = true;
        d->if_restore_vif = (image & V86_IF) != 0;
        d->if_target_cs = (uint16_t)next.tf.cs;
        d->if_target_eip = next.tf.eip; d->if_target_sp = next.tf.user_esp;
    } else if (!return_step_safe(v, f)) return;
    d->if_step_cs = (uint16_t)f->tf.cs; d->if_step_ss = (uint16_t)f->tf.user_ss;
    d->if_step_depth = (uint8_t)d->depth; d->if_step_client = (uint8_t)d->clients;
    d->if_step = true;
    f->tf.eflags |= V86_TF;
}

/* F3-45: only an exact interrupted context proves return outside the host
 * stubs. A flat selector can cover multiple private stacks, so even a move
 * above the original frame is not proof. Keep timed-out records as
 * continuations so a late host-owned trampoline still has its exact state.
 * Research/limits: execution-abi.md and f3-42-guest-isr-latency.md. */
void dpmi_irq_resync(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (!d->active) return;
    /* The normal return paths own these boundaries, even if the physical
     * timer interrupts just before the HLT/INT FE executes. */
    if ((v->frame_pm && (uint16_t)f->tf.cs == DPMI_HOST_CS) ||
        (!v->frame_pm && f->tf.cs == 0xF000 &&
         (f->tf.eip == DPMI_RM_RETURN || f->tf.eip == DPMI_RM_RETURN + 2))) return;
    for (unsigned n = d->depth; n; n--) {
        struct dpmi_record *r = &d->rec[n - 1];
        if (!(r->flags & DPMI_RF_HWINT) || (!r->isr_measured && !r->isr_expired) ||
            r->client != d->clients) continue;
        bool returned = r->saved_pm == v->frame_pm && f->tf.cs == r->saved.tf.cs &&
            f->tf.eip == r->saved.tf.eip && f->tf.user_ss == r->saved.tf.user_ss &&
            f->tf.user_esp == r->saved.tf.user_esp;
        if (returned) {
            bool vif = r->vif;
            if_recovery_end(v, f);
            dpmi_return_cancel(v);
            for (unsigned i = n - 1; i < d->depth; i++) dosvm_isr_end(v, &d->rec[i]);
            dpmi_unwind(v, n - 1);
            /* Preserve the exact returned context's live host-stack bytes
             * when the next IRQ allocates a frame. */
            dpmi_lock_below(v, f);
            d->decode_valid = d->decode_stack_valid = d->decode_ds_valid = false;
            v->perf.isr_retired++;
            dosvm_vif_restore(v, vif, DOSVM_IF_TRAMPOLINE);
            continue;
        }
        if (!r->isr_expired && g_ticks - r->isr_tick >= DPMI_ISR_WAIT_MS) {
            dosvm_isr_end(v, r);
            r->isr_measured = false;
            r->isr_expired = true;
            v->perf.isr_expired++;
        }
    }
}
/* A return value may be computed before the pops/RET. These complete
 * encodings write only registers other than SP; no future return slot can
 * be replaced. Length/operand bounds are checked by flags_instruction. */
static bool return_register_instruction(const uint8_t *b, unsigned pos)
{
    uint8_t op = b[pos];
    if (op == 0x8B || op == 0x8D) return ((b[pos + 1] >> 3) & 7) != 4;
    if (op == 0x89 || op == 0xC1 || op == 0xD1 || op == 0xD3)
        return (b[pos + 1] >> 6) == 3 && (b[pos + 1] & 7) != 4;
    if (op < 0x40 && (op & 7) < 4) {
        if (op >= 0x38) return true; /* CMP never assigns SP or memory */
        if (op & 2) return ((b[pos + 1] >> 3) & 7) != 4;
        return (b[pos + 1] >> 6) == 3 && (b[pos + 1] & 7) != 4;
    }
    return false;
}
#include "dpmi_return.inc"
static void return_boundary_seed(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (d->return_steps) {
        return_resume(v, f);
        if (d->return_steps || d->return_step) return;
    }
    if (!v->monitor.vif || d->if_armed || (f->tf.eflags & (V86_TF | (1u << 14)))) return;
    /* fx-16: no CLI needs a fresh boundary while every flags-saving CLI is
     * a proven site; an in-flight acquisition above still completes. */
    if (!dpmi_seed_wanted(v)) { d->cli.seeds_skipped++; return; }
    d->cli.seeds++;
    struct dpmi_seg cs, ss;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs) || !dpmi_seg(v, (uint16_t)f->tf.user_ss, &ss)) return;
    uint32_t at = f->tf.eip, consumed = 0;
    for (unsigned instructions = 0; instructions < 16; instructions++) {
        uint8_t b[15], decoded; unsigned w;
        if (at > cs.limit) return;
        unsigned n = cs.limit - at >= 14 ? 15 : cs.limit - at + 1;
        if (dosvm_read(v, cs.base + at, b, n)) return;
        unsigned pos = flags_opcode(b, n, cs.big, &w);
        if (pos == n) return;
        unsigned op = b[pos], len;
        /* Non-writing epilogue: segment pops, POPAD, ADD SP/ESP, IRET. */
        for (unsigned i = 0; i < pos; i++) if (b[i] != 0x66) return;
        if (op == 0xC3 || op == 0xC2) {
            if (op == 0xC2 && pos + 3 > n) return;
            uint32_t sp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
            uint32_t linear, target = 0;
            if (sp > UINT32_MAX - consumed ||
                !dpmi_ptr(v, (uint16_t)f->tf.user_ss, sp + consumed, w, false, &linear) ||
                dosvm_read(v, linear, &target, w) ||
                !dpmi_handler_live(v, (struct dpmi_vec){.sel=(uint16_t)f->tf.cs, .off=target})) return;
            /* A predicted PC is never proof. Prefer an execute fault; full
             * client DR allocation falls back to checked CPU steps. */
            d->return_steps = DPMI_RETURN_STEPS;
            d->return_hops = DPMI_RETURN_HOPS;
            d->return_tick = g_ticks; d->return_irqs = 0; d->return_replan = false;
            return_arm(v, f, (uint16_t)f->tf.cs, target);
            dosvm_boundary(v, f, "ret_seed");
            return;
        }
        if (op == 0xCF) {
            uint32_t sp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
            if (consumed > 64 || sp > UINT32_MAX - consumed) return;
            uint32_t linear, frame[4] = {0};
            if (!dpmi_ptr(v, (uint16_t)f->tf.user_ss, sp + consumed, 4 * w, false, &linear)) return;
            for (unsigned i = 0; i < 4; i++) if (dosvm_read(v, linear + i * w, &frame[i], w)) return;
            uint16_t selector = (uint16_t)frame[1]; /* IRETD discards the upper half */
            if ((selector & 3) != 3 || ((uint16_t)f->tf.cs & 3) != 3 ||
                selector == (uint16_t)f->tf.cs || /* tracking must start after IRET */
                (frame[2] & (V86_VM | V86_TF | (1u << 14))) ||
                !dpmi_handler_live(v, (struct dpmi_vec){.sel=selector, .off=frame[0]}) ||
                !dpmi_handler_live(v, (struct dpmi_vec){.sel=selector, .off=frame[3]})) return;
            struct dpmi_seg target; uint8_t first;
            if (!dpmi_seg(v, selector, &target) || dosvm_read(v, target.base + frame[0], &first, 1) ||
                first != 0xC3 || (target.big ? 4u : 2u) != w) return;
            d->return_steps = DPMI_RETURN_STEPS;
            d->return_hops = DPMI_RETURN_HOPS;
            d->return_tick = g_ticks; d->return_irqs = 0; d->return_replan = false;
            return_arm(v, f, selector, frame[3]); /* IRET -> RET -> caller */
            dosvm_boundary(v, f, "iret_seed");
            return;
        }
        len = flags_instruction(b, n, cs.big, &decoded, &w);
        if (op == 0x0F && pos + 1 < n && (b[pos + 1] == 0xA1 || b[pos + 1] == 0xA9)) {
            len = pos + 2; consumed += w;
        } else if (op == 0x1F || op == 0x07 || (op >= 0x58 && op <= 0x5F && op != 0x5C)) consumed += w;
        else if (op == 0x61) consumed += 8 * w;
        else if (op == 0x83 && pos + 2 < n && b[pos + 1] == 0xC4 && (int8_t)b[pos + 2] >= 0) {
            /* ADD SP updates only the low half even on a 32-bit stack. A
             * wrap invalidates our monotonically advancing frame offset. */
            if (w == 2 && (uint16_t)(f->tf.user_esp + consumed) > 0xFFFFu - b[pos + 2]) return;
            consumed += b[pos + 2];
        }
        else if (op != 0x90 && (!len || !return_register_instruction(b, pos))) return;
        if (!len || consumed > 64 || at > UINT32_MAX - len) return;
        at += len;
    }
}

/* #GP(0): IOPL-sensitive instructions the monitor virtualizes. */
static int emulate(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_seg cs;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs)) return dpmi_exception(v, f, 13, 0);
    int direct;
    if (dosvm_io_fast(v, f, &cs, &direct)) return direct;
    bool op32 = cs.big, a32 = cs.big, rep = false;
    uint16_t seg = (uint16_t)f->tf.ds;
    unsigned len = 0;
    uint8_t op = 0;
    for (;;) {
        if (len >= 15 || !fetch(v, f, &cs, len, &op)) return dpmi_exception(v, f, 13, 0);
        len++;
        switch (op) {
        case 0x66: op32 = !cs.big; continue;
        case 0x67: a32 = !cs.big; continue;
        case 0xF2: case 0xF3: rep = true; continue;
        case 0x26: seg = (uint16_t)f->tf.es; continue;
        case 0x2E: seg = (uint16_t)f->tf.cs; continue;
        case 0x36: seg = (uint16_t)f->tf.user_ss; continue;
        case 0x3E: seg = (uint16_t)f->tf.ds; continue;
        case 0x64: seg = (uint16_t)f->tf.fs; continue;
        case 0x65: seg = (uint16_t)f->tf.gs; continue;
        default: break;
        }
        break;
    }
    uint32_t next = f->tf.eip + len, mask = cs.big ? 0xFFFFFFFFu : 0xFFFFu;
    unsigned w = op32 ? 4 : 2;
    switch (op) {
    case 0xFA: {
        v->perf.cause = DOSVM_COST_PM_FLAGS;
        /* A fresh CLI starts a fresh ordinary-code grace period even if
         * virtual IF was already clear and an IRQ has long been pending. */
        d->if_waiting = false;
        d->if_steps = 0;
        /* An image armed by an enclosing region and still live keeps its
         * TF and its owner beside the new one (armed_push). */
        struct dpmi_armed planted;
        bool armed = v->monitor.vif && arm_flags_image(v, f, &cs, &planted);
        if (armed) {
            armed_push(v, f, &planted);
            d->stats.if_armed++;
        }
        v->monitor.vif = false;
        dosvm_trace(v, DOSVM_TRACE_FLAGS, f, 0, armed, 0);
        break;
    }
    case 0xFB:
        v->perf.cause = DOSVM_COST_PM_FLAGS;
        if (!v->monitor.vif) {
            /* STI shadow: no IRQ before the next instruction retires. Not
             * single-stepped: TF would leak into a PUSHF image (PUSHF does
             * not trap at IOPL 0 in protected mode). dpmi_pm_trap ends the
             * shadow at the first trap that is not an IRQ at that address. */
            dosvm_vif_restore(v, true, DOSVM_IF_STI);
            f->tf.eip = next & mask;
            d->shadow = true;
            d->shadow_cs = (uint16_t)f->tf.cs;
            d->shadow_eip = f->tf.eip;
            return 0;
        }
        break;
    case 0xF4: v->perf.cause = DOSVM_COST_PM_HLT; v->monitor.state = V86_HALTED; break;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF: {
        uint16_t port = (uint16_t)f->tf.edx;
        if (op < 0xE8) {
            uint8_t p;
            if (!fetch(v, f, &cs, len, &p)) return dpmi_exception(v, f, 13, 0);
            port = p;
            next++;
        }
        unsigned bytes = (op & 1) ? w : 1;
        bool write = op & 2;
        uint32_t value = f->tf.eax;
        int e = v86_io(&v->monitor, port, bytes, write, &value);
        if (e) return e < 0 ? e : -EFAULT;
        if (!write) {
            uint32_t m = bytes == 4 ? 0xFFFFFFFFu : (1u << (bytes * 8)) - 1;
            f->tf.eax = (f->tf.eax & ~m) | (value & m);
        }
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        bool write = op & 2;
        unsigned bytes = (op & 1) ? w : 1;
        uint32_t count = a32 ? f->tf.ecx : (uint16_t)f->tf.ecx;
        if (rep && !count) break;
        uint32_t *index = write ? &f->tf.esi : &f->tf.edi;
        uint16_t sel = write ? seg : (uint16_t)f->tf.es;
        uint32_t off = a32 ? *index : (uint16_t)*index, lin, value = 0;
        if (!dpmi_ptr(v, sel, off, bytes, !write, &lin)) return dpmi_exception(v, f, 13, 0);
        if (write && dosvm_read(v, lin, &value, bytes)) return dpmi_exception(v, f, 13, 0);
        /* f3-46: INS resolves its destination before the port read (the
         * device effect); a contained backing failure stops the VM. */
        if (!write && dosvm_pool_prefault(v, lin, bytes, true)) return -EFAULT;
        int e = v86_io(&v->monitor, (uint16_t)f->tf.edx, bytes, write, &value);
        if (e) return e < 0 ? e : -EFAULT;
        if (!write && dosvm_write(v, lin, &value, bytes)) return dpmi_exception(v, f, 13, 0);
        off += (f->tf.eflags & 0x400) ? 0u - bytes : bytes;
        if (a32) *index = off; else dpmi_low(index, off);
        if (rep) {
            count--;
            if (a32) f->tf.ecx = count; else dpmi_low(&f->tf.ecx, count);
            if (count) next = f->tf.eip;  /* one element per trap, restartable */
        }
        break;
    }
    default:
        return dpmi_exception(v, f, 13, 0);
    }
    f->tf.eip = next & mask;
    return 0;
}

static int debug_trap(struct dosvm *v, struct v86_frame *f, uint32_t dr6)
{
    struct dpmi *d = &v->dpmi;
    dpmi_watch_hits(v, dr6);
    if (dr6 & 15) {
        for (unsigned i = 0; i < DPMI_WATCH; i++)
            if ((dr6 & (1u << i)) && d->watch[i].used && d->watch[i].type == 0) f->tf.eflags |= DPMI_RF;
        if (is_default(d->exc[1], DPMI_PM_EXC + 1, 1)) return 0;  /* recorded for 0B02h */
    }
    return dpmi_exception(v, f, 1, 0);
}
int dpmi_io_repeat(struct dosvm *v, struct v86_frame *f)
{ return emulate(v, f); }

static int pm_trap(struct dosvm *v, struct v86_frame *f, uint32_t cr2, uint32_t dr6)
{
    struct dpmi *d = &v->dpmi;
    if (!d->active) return -EFAULT;
    unsigned vector = f->tf.vector;
    bool irq = vector >= 0x20 && vector < 0x30;
    /* Clear a retired STI shadow before consuming an owned #DB: the
     * early step return below must not extend it through the epilogue. */
    bool execute_fault = vector == 1 && (dr6 & return_debug_bit(d));
    if (d->shadow && !((irq || execute_fault) && (uint16_t)f->tf.cs == d->shadow_cs && f->tf.eip == d->shadow_eip))
        d->shadow = false;
    bool recovery_step = d->if_step;
    if (if_recovery_trap(v, f, dr6)) return 0;
    if (irq) return_unstep(v, f, false);
    if (return_debug_trap(v, f, &dr6)) return 0;
    if (d->return_steps && d->depth == d->return_depth && d->clients == d->return_client && !irq)
        dpmi_return_cancel(v);          /* fault/interrupt, never clear client TF */
    if (irq) return 0;                                      /* physical IRQ, handled */
    /* Before anything else: an instruction trapping right after the POPF
     * (a CLI, an INT, a host stub) sees the restored IF first. */
    if (!recovery_step && flags_restored(v, f, dr6) && vector == 1 && !(dr6 & (15u | (1u << 13) | (1u << 15)))) return 0;
    switch (vector) {
    case 13:
        if ((f->tf.err & 3) == 2) {
            v->perf.cause = DOSVM_COST_PM_INT;
            return software_int(v, f, (uint8_t)(f->tf.err >> 3));
        }
        if (f->tf.err) return dpmi_exception(v, f, 13, f->tf.err);
        if ((uint16_t)f->tf.cs == DPMI_HOST_CS) return dpmi_stub_trap(v, f, f->tf.eip);
        return emulate(v, f);
    case 1: return debug_trap(v, f, dr6);
    case 7: fpu_handle_nm(); return 0;
    case 0x80: return dpmi_pm_interrupt(v, f, 0x80);         /* INT 80h: guest vector, never a syscall */
    case 14:
        /* Host fault resolution first (contract: exceptions undergo host
         * resolution before the client's handler). f3-06: a ring-3 access
         * to the supervisor-only VGA window A0000h-BFFFFh (error code P=1,
         * U=1: protection violation from user mode; CR2 captured before
         * interrupts were enabled) is a trapped device cycle, emulated on
         * the VM's VGA model with the client's 16/32-bit decoding. DOOM's
         * mode 13h/Y frame writes reach the model this way. */
        if (cr2 >= 0xA0000 && cr2 < 0xC0000 && (f->tf.err & 5) == 5) {
            if (dosvm_vga_retry(v, cr2, dpmi_fault_linear(v, f), true)) return 0;
            return dosvm_vga_fault_pm(v, f);
        }
        return dpmi_exception(v, f, 14, f->tf.err);
    default:
        if (vector < 32) return dpmi_exception(v, f, vector, f->tf.err);
        return -EFAULT;
    }
}

/* fx-27 lean port exits (dosvm.c lean_io). True when a #GP(0) at this
 * boundary of a VM with a DPMI client carries no monitor obligation of its
 * own: no STI shadow or IF recovery, no return tracker or client
 * watchpoint, no CLI-seed demand (fx-16 dpmi_seed_wanted), not a host stub
 * or single-step (PM), and not the saved return point of an open
 * hardware-interrupt record (dpmi_irq_resync). An armed flags image
 * (fx-16: a PUSHF image patched with TF at its CLI) is no obligation at an
 * I/O exit: without TF in the frame it has not been loaded yet, so
 * flags_restored does nothing, return_boundary_seed returns before any
 * seed or count, the I/O bursts refuse it themselves, and with no IRQ
 * pending if_recovery_prepare only ends recovery. For such a PM exit
 * dpmi_pm_trap's work around an emulated IN/OUT reduces to the bookkeeping
 * of dpmi_lean_retire; the caller still requires that no virtual IRQ is
 * pending before it skips dpmi_deliver. */
bool dpmi_lean_eligible(const struct dosvm *v, const struct v86_frame *f)
{
    const struct dpmi *d = &v->dpmi;
    if (!d->active || d->shadow || d->if_waiting || d->if_step || d->if_restore_step ||
        d->return_steps || d->return_step || d->return_replan || d->dr7 || dpmi_seed_wanted(v))
        return false;
    if (v->frame_pm && ((uint16_t)f->tf.cs == DPMI_HOST_CS || (f->tf.eflags & (V86_TF | (1u << 14)))))
        return false;
    return dpmi_lean_quiet(v, f);
}
/* dpmi_irq_resync's predicate: true when f is not the saved return point of
 * an open hardware-interrupt record (a native late return to observe). An
 * OUT inside a handler is lean; reaching the interrupted context is not. */
bool dpmi_lean_quiet(const struct dosvm *v, const struct v86_frame *f)
{
    const struct dpmi *d = &v->dpmi;
    for (unsigned n = d->depth; n; n--) {
        const struct dpmi_record *r = &d->rec[n - 1];
        if (!(r->flags & DPMI_RF_HWINT) || (!r->isr_measured && !r->isr_expired) || r->client != d->clients) continue;
        if (r->saved_pm == v->frame_pm && f->tf.cs == r->saved.tf.cs && f->tf.eip == r->saved.tf.eip &&
            f->tf.user_ss == r->saved.tf.user_ss && f->tf.user_esp == r->saved.tf.user_esp) return false;
    }
    return true;
}
/* Exactly the state dpmi_pm_trap and dpmi_deliver leave after such an
 * exit with no deliverable IRQ: return_boundary_seed's skipped-seed count,
 * the decode seed at the returned boundary (no stack snapshot while no
 * seed is wanted), and if_recovery_end through if_recovery_prepare. */
void dpmi_lean_retire(struct dosvm *v, const struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (v->monitor.vif && !d->if_armed) d->cli.seeds_skipped++;
    d->decode_valid = true;
    d->decode_cs = (uint16_t)f->tf.cs;
    d->decode_eip = f->tf.eip;
    d->decode_stack_valid = false;
    struct dpmi_seg ds;
    d->decode_ds_valid = dpmi_seg(v, (uint16_t)f->tf.ds, &ds) && ds.present;
    if (d->decode_ds_valid) d->decode_ds_base = ds.base;
    d->if_waiting = d->if_step = d->if_restore_step = false;
    d->if_steps = 0;
}

int dpmi_pm_trap(struct dosvm *v, struct v86_frame *f, uint32_t cr2, uint32_t dr6)
{
    /* A physical IRQ can land between PUSHF, POP and CLI without changing
     * the guest context. Keep the older boundary only with a forward proof;
     * otherwise the IRQ PC becomes the new seed as before. */
    struct dpmi_seg cs;
    bool irq = f->tf.vector >= 0x20 && f->tf.vector < 0x30;
    /* fx-16: the boundary proof (a forward decode of up to 4096 bytes on
     * every physical IRQ in PM, i.e. most of the T23's 61 us timer exits)
     * only preserves a seed for a CLI proof; skip it while none is wanted. */
    bool seeding = dpmi_seed_wanted(v);
    if (irq) { if (seeding) v->dpmi.cli.irq_proofs++; else v->dpmi.cli.irq_proofs_skipped++; }
    bool keep = irq && seeding && dpmi_seg(v, (uint16_t)f->tf.cs, &cs) &&
                flags_preceding(v, f, &cs, 0, 0, true);
    /* The host IRQ-return stub restores an unchanged interrupted context,
     * including its forward-decode seed. Do not replace it with the CLI PC. */
    bool irq_return = f->tf.vector == 13 && !f->tf.err && (uint16_t)f->tf.cs == DPMI_HOST_CS &&
        f->tf.eip == DPMI_PM_IRQRET && v->dpmi.depth &&
        v->dpmi.rec[v->dpmi.depth - 1].kind == DPMI_REC_PMINT;
    dr6 = dpmi_debug_status(v, dr6);
    bool checkpoint = f->tf.vector == 1 && ((dr6 & return_debug_bit(&v->dpmi)) ||
        (v->dpmi.return_step && (dr6 & (1u << 14))));
    if (irq && v->dpmi.return_steps && v->dpmi.depth == v->dpmi.return_depth &&
        v->dpmi.clients == v->dpmi.return_client) {
        v->dpmi.return_irqs++;
        v->dpmi.return_replan = true;
    }
    int e = pm_trap(v, f, cr2, dr6);
    if (!e && v->frame_pm && f->tf.vector == 13 && !f->tf.err &&
        (v->perf.cause == DOSVM_COST_VGA_IO || (v->perf.cause == DOSVM_COST_PIT_PIC &&
         v->perf.io_port >= 0x40 && v->perf.io_port <= 0x43)))
        e = dosvm_port_bursts(v, f);
    bool replanning = !e && v->frame_pm && v->dpmi.return_replan &&
        v->dpmi.depth == v->dpmi.return_depth && v->dpmi.clients == v->dpmi.return_client;
    /* Preserve a flags save already reached before the IRQ; otherwise the
     * resumed CPU boundary supplies a fresh stack snapshot for its proof. */
    bool keep_resumed = replanning && seeding && dpmi_seg(v, (uint16_t)f->tf.cs, &cs) &&
        flags_preceding(v, f, &cs, 0, 0, true);
    /* The returned PC (including handler entry and emulated fall-through) is
     * a known instruction boundary. Never seed from a backwards guess. */
    if (!e && v->frame_pm && !checkpoint) return_boundary_seed(v, f);
    /* Every checkpoint is an observed boundary: retain its independent
     * proof even while another prediction is in flight. */
    if ((!keep && !irq_return) || e || !v->frame_pm || checkpoint || (replanning && !keep_resumed)) {
        v->dpmi.decode_valid = !e && v->frame_pm;
        if (v->dpmi.decode_valid && (checkpoint || (replanning && !keep_resumed)))
            v->perf.return_reacquired++;
        v->dpmi.decode_cs = (uint16_t)f->tf.cs;
        v->dpmi.decode_eip = f->tf.eip;
        struct dpmi_seg ss, code, ds;
        uint32_t lin;
        v->dpmi.decode_stack_valid = !e && v->frame_pm && dpmi_seed_wanted(v) &&
            dpmi_seg(v, (uint16_t)f->tf.cs, &code) && dpmi_seg(v, (uint16_t)f->tf.user_ss, &ss) &&
            code.big == ss.big && dpmi_ptr(v, (uint16_t)f->tf.user_ss,
                ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp, sizeof(v->dpmi.decode_stack), false, &lin) &&
            !dosvm_read(v, lin, v->dpmi.decode_stack, sizeof(v->dpmi.decode_stack));
        if (v->dpmi.decode_stack_valid) v->dpmi.decode_stack_linear = lin;
        v->dpmi.decode_ds_valid = !e && v->frame_pm && dpmi_seg(v, (uint16_t)f->tf.ds, &ds) && ds.present;
        if (v->dpmi.decode_ds_valid) v->dpmi.decode_ds_base = ds.base;
    }
    return e;
}

/* Asynchronous conventional-memory ESR. Use the same suspended continuation
 * and host real stack as interrupt reflection, but a far-return frame.
 * Unlike an IRQ this does not acknowledge or measure a virtual PIC source. */
int dpmi_to_real_call(struct dosvm *v, struct v86_frame *f, uint16_t cs, uint16_t ip,
                      uint16_t return_ip)
{
    struct dpmi *d = &v->dpmi;
    if (!v->frame_pm || !d->active || !dosvm_range(v, ((uint32_t)cs << 4) + ip, 1, false)) return -EFAULT;
    if (d->depth >= DPMI_DEPTH || d->rstack_top < DPMI_RSTACK_LEVEL) return -ENOSPC;
    /* Do not preserve the host's temporary single-step flag as client state. */
    if (return_unstep(v, f, false)) d->return_replan = true;
    unsigned depth = d->depth;
    struct dpmi_record *r;
    int e = dpmi_push_record(v, f, DPMI_REC_REFLECT, &r);
    if (e) return e;
    r->flags = DPMI_RF_HWINT; /* full register/selector/decode restoration */
    uint16_t ss, sp;
    if ((e = dpmi_rm_stack(v, &ss, &sp))) { dpmi_unwind(v, depth); return e; }
    uint16_t ret[2] = {return_ip, 0xf000};
    if (dosvm_write(v, ((uint32_t)ss << 4) + sp - 4, ret, sizeof(ret))) {
        dpmi_unwind(v, depth); return -EFAULT;
    }
    struct v86_frame n = {0};
    dpmi_gprs(&n, f);
    n.tf.user_ss = ss; n.tf.user_esp = sp - 4;
    n.tf.cs = cs; n.tf.eip = ip;
    n.tf.eflags = (f->tf.eflags & 0x08d5u) | V86_VM | V86_IF | 2u;
    dpmi_set_pm(v, false); v->monitor.vif = false; *f = n;
    return 0;
}

/* Virtual IRQ delivery while a client exists. Hooked vectors reach the
 * protected-mode handler; others keep the default real-mode reflection.
 * Guest IRQ vectors use the PM interrupt table, never the exception table. */
int dpmi_deliver(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    dpmi_irq_resync(v, f);
    return_resume(v, f);
    if_recovery_prepare(v, f);
    if (!v->monitor.vif || v->monitor.shadow || d->shadow) return 0;
    uint32_t flags = irq_save();
    dosvm_pic_cascade(&v->monitor);
    int irq = dosvm_pic_pending(&v->monitor);
    if (irq < 0) { irq_restore(flags); return 0; }
    uint8_t vector = (uint8_t)(v->monitor.pic[irq >> 3].base + (irq & 7));
    struct dpmi_vec h = d->vec[vector];
    bool hooked = !is_default(h, DPMI_PM_INT, 256);
    struct v86_frame from;
    if (!irq && (g_boot.flags & CBI_F_VERBOSE)) from = *f;
    if (!hooked && !v->frame_pm) {
        int e = v86_irq_deliver(&v->monitor, f);
        irq_restore(flags);
        if (!irq && !e && (g_boot.flags & CBI_F_VERBOSE))
            dosvm_trace(v, DOSVM_TRACE_IRQ0, &from, vector, f->tf.cs & 0xFFFF, f->tf.eip);
        return e;
    }
    if (d->depth >= DPMI_DEPTH || (!hooked && d->rstack_top < DPMI_RSTACK_LEVEL)) {
        irq_restore(flags);
        return -ENOSPC;                 /* nesting exhausted: this VM only */
    }
    if (return_unstep(v, f, false)) d->return_replan = true;
    /* Construct the complete frame before INTA. Keep arbitration and
     * commit in one bounded IRQ-disabled transaction: a rejected handler
     * or stack must leave the request pending, with no orphan ISR bit or
     * claimed delivery. Neither constructor sleeps or prints. */
    int e;
    if (!hooked) e = dpmi_to_real_int(v, f, DPMI_REC_REFLECT, (uint8_t)(h.off - DPMI_PM_INT), DPMI_RF_HWINT);
    else e = dpmi_enter_pm_handler(v, f, vector, DPMI_RF_HWINT);
    if (!e) {
        struct dpmi_record *r = &d->rec[d->depth - 1];
        dosvm_isr_begin(v, r, (unsigned)irq);
        dosvm_pic_ack(&v->monitor);
        v->monitor.stats.reflected_irqs++;
        v->monitor.stats.delivered[irq & 15]++;
        v->monitor.stats.last_vector = vector;
        v->monitor.state = V86_RUNNING;
        if (hooked) d->stats.irqs_pm++;
        v->trace.injected_cs = (uint16_t)f->tf.cs;
        v->trace.injected_eip = f->tf.eip;
    }
    irq_restore(flags);
    if (e && hooked && !dpmi_handler_live(v, h))
        dpmi_trace_irq_target(v, f, "pm_irq_invalid", vector, h);
    if (!irq && !e && (g_boot.flags & CBI_F_VERBOSE))
        dosvm_trace(v, DOSVM_TRACE_IRQ0, &from, vector, f->tf.cs & 0xFFFF, f->tf.eip);
    return e;
}
