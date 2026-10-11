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
    unsigned w = dpmi_width(v);
    if (d->lock_top < 8 * w + DPMI_HSTACK_RESERVE) { dpmi_unwind(v, d->depth - 1); return -ENOSPC; }
    uint32_t esp = d->lock_top - 8 * w;
    uint32_t frame[8] = {DPMI_PM_EXCRET, DPMI_HOST_CS, error, f->tf.eip, (uint16_t)f->tf.cs,
                         pm_image(v, f->tf.eflags), f->tf.user_esp, (uint16_t)f->tf.user_ss};
    for (unsigned i = 0; i < 8; i++)
        if (dosvm_write(v, d->hstack.base + esp + i * w, &frame[i], w)) { dpmi_unwind(v, d->depth - 1); return -EFAULT; }
    r->vector = (uint8_t)n;
    r->arg[0] = esp;
    d->lock_top = esp;
    struct v86_frame n2 = *f;
    n2.tf.user_ss = DPMI_HOST_SS;
    n2.tf.user_esp = esp;
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
    unsigned w = dpmi_width(v);
    if (d->lock_top < 3 * w + DPMI_HSTACK_RESERVE) { dpmi_unwind(v, d->depth - 1); return -ENOSPC; }
    uint32_t esp = d->lock_top - 3 * w;
    uint32_t frame[3] = {(flags & DPMI_RF_HWINT) ? DPMI_PM_IRQRET : DPMI_PM_SOFTRET, DPMI_HOST_CS,
                         (f->tf.eflags & 0x0CD5u) | (v->monitor.vif ? V86_IF : 0) | 2u};
    for (unsigned i = 0; i < 3; i++)
        if (dosvm_write(v, d->hstack.base + esp + i * w, &frame[i], w)) { dpmi_unwind(v, d->depth - 1); return -EFAULT; }
    r->arg[0] = esp + 3 * w;
    d->lock_top = esp;
    struct v86_frame n;
    if (v->frame_pm) n = *f;
    else { memset(&n, 0, sizeof(n)); dpmi_gprs(&n, f); }
    n.tf.cs = h.sel;
    n.tf.eip = h.off;
    n.tf.user_ss = DPMI_HOST_SS;
    n.tf.user_esp = esp;
    n.tf.eflags = (f->tf.eflags & 0x0CD5u & ~V86_TF) | V86_IF | 2u;
    dpmi_set_pm(v, true);
    *f = n;
    v->monitor.vif = false;
    return 0;
}

/* The interrupted context comes from the kernel record, never from the
 * guest stack, so the handler may IRET from any stack (handlers that copy
 * their frame to a private stack). A return without the matching record
 * is forged or stale and ends the VM. */
static int pmint_return(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_record *r = d->depth ? &d->rec[d->depth - 1] : 0;
    if (!r || r->kind != DPMI_REC_PMINT ||
        (r->flags & DPMI_RF_HWINT ? f->tf.eip != DPMI_PM_IRQRET : f->tf.eip != DPMI_PM_SOFTRET)) {
        d->stats.stale++;
        return -EFAULT;
    }
    struct dpmi_record rec = *r;
    struct v86_frame result = *f;
    dpmi_unwind(v, d->depth - 1);
    dpmi_set_pm(v, rec.saved_pm);
    *f = rec.saved;
    if (rec.flags & DPMI_RF_RESULTS) {
        dpmi_gprs(f, &result);
        f->tf.eflags = (f->tf.eflags & ~0x08D5u) | (result.tf.eflags & 0x08D5u);
    }
    v->monitor.vif = rec.vif;           /* the virtual state saved at delivery */
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

/* #GP(0): IOPL-sensitive instructions the monitor virtualizes. */
static int emulate(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_seg cs;
    if (!dpmi_seg(v, (uint16_t)f->tf.cs, &cs)) return dpmi_exception(v, f, 13, 0);
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
    case 0xFA: v->monitor.vif = false; break;
    case 0xFB:
        if (!v->monitor.vif) {
            /* STI shadow: no IRQ before the next instruction retires. Not
             * single-stepped: TF would leak into a PUSHF image (PUSHF does
             * not trap at IOPL 0 in protected mode). dpmi_pm_trap ends the
             * shadow at the first trap that is not an IRQ at that address. */
            v->monitor.vif = true;
            f->tf.eip = next & mask;
            d->shadow = true;
            d->shadow_cs = (uint16_t)f->tf.cs;
            d->shadow_eip = f->tf.eip;
            return 0;
        }
        break;
    case 0xF4: v->monitor.state = V86_HALTED; break;
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

int dpmi_pm_trap(struct dosvm *v, struct v86_frame *f, uint32_t cr2, uint32_t dr6)
{
    (void)cr2;
    struct dpmi *d = &v->dpmi;
    if (!d->active) return -EFAULT;
    unsigned vector = f->tf.vector;
    bool irq = vector >= 0x20 && vector < 0x30;
    /* An IRQ taken before the shadowed instruction keeps the STI shadow;
     * any other trap means that instruction retired or is being handled. */
    if (d->shadow && !(irq && (uint16_t)f->tf.cs == d->shadow_cs && f->tf.eip == d->shadow_eip))
        d->shadow = false;
    if (irq) return 0;                                      /* physical IRQ, handled */
    switch (vector) {
    case 13:
        if ((f->tf.err & 3) == 2) return software_int(v, f, (uint8_t)(f->tf.err >> 3));
        if (f->tf.err) return dpmi_exception(v, f, 13, f->tf.err);
        if ((uint16_t)f->tf.cs == DPMI_HOST_CS) return dpmi_stub_trap(v, f, f->tf.eip);
        return emulate(v, f);
    case 1: return debug_trap(v, f, dr6);
    case 7: fpu_handle_nm(); return 0;
    case 0x80: return dpmi_pm_interrupt(v, f, 0x80);         /* INT 80h: guest vector, never a syscall */
    case 14:
        /* Host fault resolution first: the only host-owned faults are the
         * trapped devices of later rows (f3-02 VGA aperture); none here. */
        return dpmi_exception(v, f, 14, f->tf.err);
    default:
        if (vector < 32) return dpmi_exception(v, f, vector, f->tf.err);
        return -EFAULT;
    }
}

/* Virtual IRQ delivery while a client exists. Hooked vectors reach the
 * protected-mode handler; others keep the default real-mode reflection.
 * Guest IRQ vectors use the PM interrupt table, never the exception table. */
int dpmi_deliver(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (!v->monitor.vif || v->monitor.shadow || d->shadow) return 0;
    uint32_t flags = irq_save();
    dosvm_pic_cascade(&v->monitor);
    int irq = dosvm_pic_pending(&v->monitor);
    if (irq < 0) { irq_restore(flags); return 0; }
    uint8_t vector = (uint8_t)(v->monitor.pic[irq >> 3].base + (irq & 7));
    struct dpmi_vec h = d->vec[vector];
    bool hooked = !is_default(h, DPMI_PM_INT, 256);
    if (!hooked && !v->frame_pm) {
        int e = v86_irq_deliver(&v->monitor, f);
        irq_restore(flags);
        return e;
    }
    if (d->depth >= DPMI_DEPTH || (!hooked && d->rstack_top < DPMI_RSTACK_LEVEL)) {
        irq_restore(flags);
        return -ENOSPC;                 /* nesting exhausted: this VM only */
    }
    dosvm_pic_ack(&v->monitor);
    v->monitor.stats.reflected_irqs++;
    v->monitor.stats.delivered[irq & 15]++;
    v->monitor.stats.last_vector = vector;
    irq_restore(flags);
    v->monitor.state = V86_RUNNING;
    if (!hooked) return dpmi_to_real_int(v, f, DPMI_REC_REFLECT, (uint8_t)(h.off - DPMI_PM_INT), DPMI_RF_HWINT);
    d->stats.irqs_pm++;
    return dpmi_enter_pm_handler(v, f, vector, DPMI_RF_HWINT);
}
