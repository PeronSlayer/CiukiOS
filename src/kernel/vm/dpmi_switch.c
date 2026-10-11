/* DPMI 0.9 mode transitions: suspended-context records, host stacks,
 * reflection to V86, host-issued DOS calls, translation services
 * 0300h-0306h, real-mode callbacks and raw switches (DPMI 0.9 sections 3
 * and 11). Every transition is a kernel record; nothing about the host is
 * kept in guest memory except the frames the specification defines.
 * Research and decisions: docs/design/f3-acceptance.md, f3-04.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"


/* Suspend the context in f. A protected-mode context already on the locked
 * stack keeps everything above its ESP: nested entries go below it. */
int dpmi_push_record(struct dosvm *v, const struct v86_frame *f, uint8_t kind, struct dpmi_record **out)
{
    struct dpmi *d = &v->dpmi;
    if (d->depth >= DPMI_DEPTH) return -ENOSPC;
    struct dpmi_record *r = &d->rec[d->depth++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->client = (uint8_t)d->clients;
    r->saved = *f;
    r->saved_pm = v->frame_pm;
    r->vif = v->monitor.vif;
    r->lock_top = d->lock_top;
    r->rstack_top = d->rstack_top;
    dpmi_lock_below(v, f);
    if (d->depth > d->stats.max_depth) d->stats.max_depth = d->depth;
    *out = r;
    return 0;
}
void dpmi_lock_below(struct dosvm *v, const struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (!v->frame_pm || (uint16_t)f->tf.user_ss != DPMI_HOST_SS) return;
    uint32_t esp = d->bits32 ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
    if (esp < d->lock_top) d->lock_top = esp & ~3u;
}
static void pop_record(struct dosvm *v)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_record *r = &d->rec[d->depth - 1];
    d->lock_top = r->lock_top;
    d->rstack_top = r->rstack_top;
    if (r->kind == DPMI_REC_CALLBACK) dpmi_free_ldt(v, DPMI_IDX_TEMP + d->depth - 1);
    d->depth--;
}
/* Drop records above depth without guest help (termination, faults). */
void dpmi_unwind(struct dosvm *v, unsigned depth)
{
    while (v->dpmi.depth > depth) pop_record(v);
}

/* Switch the working frame's mode. The VM's V86 A20 state is preserved
 * across protected-mode execution, which always runs with A20 enabled
 * (dos-dpmi-contract.md, memory services). */
void dpmi_set_pm(struct dosvm *v, bool pm)
{
    struct dpmi *d = &v->dpmi;
    if (pm != v->frame_pm) {
        if (pm) { d->rm_a20 = v->a20; if (!v->a20) dosvm_set_a20(v, true); }
        else if (v->a20 != d->rm_a20) dosvm_set_a20(v, d->rm_a20);
    }
    v->frame_pm = pm;
    v->monitor.shadow = false;
    d->shadow = false;
}

int dpmi_rm_stack(struct dosvm *v, uint16_t *ss, uint16_t *sp)
{
    struct dpmi *d = &v->dpmi;
    if (d->rstack_top < DPMI_RSTACK_LEVEL) return -ENOSPC;
    *ss = DPMI_RSTACK_SEG;
    *sp = d->rstack_top;
    d->rstack_top -= DPMI_RSTACK_LEVEL;
    return 0;
}

static bool rm_push(struct dosvm *v, struct v86_frame *f, uint16_t value)
{
    uint16_t sp = (uint16_t)(f->tf.user_esp - 2);
    if (f->tf.user_ss > 0xFFFF) return false;
    if (dosvm_write(v, ((f->tf.user_ss & 0xFFFFu) << 4) + sp, &value, 2)) return false;
    dpmi_low(&f->tf.user_esp, sp);
    return true;
}
bool dpmi_rm_pop(struct dosvm *v, struct v86_frame *f, uint16_t *value)
{
    uint16_t sp = (uint16_t)f->tf.user_esp;
    if (f->tf.user_ss > 0xFFFF || sp > 0xFFFE) return false;
    if (dosvm_read(v, ((f->tf.user_ss & 0xFFFFu) << 4) + sp, value, 2)) return false;
    dpmi_low(&f->tf.user_esp, sp + 2);
    return true;
}
static bool rm_code(struct dosvm *v, uint32_t cs, uint32_t ip)
{ return cs <= 0xFFFF && ip <= 0xFFFF && dosvm_range(v, (cs << 4) + ip, 1, false); }
static uint16_t flags_image(const struct dosvm *v, uint32_t eflags)
{ return (uint16_t)((eflags & 0x0CD5u) | (v->monitor.vif ? V86_IF : 0) | 2u); }

/* Enter the real-mode handler of vector through the IVT with an IRET frame
 * returning to the host stub; INT semantics clear virtual IF and TF. */
int dpmi_real_int(struct dosvm *v, struct v86_frame *n, uint8_t vector, uint16_t image)
{
    uint16_t ivt[2];
    if (dosvm_read(v, (uint32_t)vector * 4, ivt, 4) || !rm_code(v, ivt[1], ivt[0])) return -EFAULT;
    if (!rm_push(v, n, image) || !rm_push(v, n, 0xF000) || !rm_push(v, n, DPMI_RM_RETURN)) return -EFAULT;
    n->tf.cs = ivt[1];
    n->tf.eip = ivt[0];
    n->tf.eflags = (image & 0x0CD5u & ~V86_TF) | V86_VM | V86_IF | 2u;
    v->monitor.vif = false;
    return 0;
}

/* Default reflection of a PM interrupt (0.9 section 3.2): EAX-EBP and flags
 * cross unchanged, segment registers are undefined (zero), the host real
 * mode stack carries the IRET frame. kind is REFLECT; flags may request a
 * complete restore of the interrupted PM frame (hardware interrupts). */
int dpmi_to_real_int(struct dosvm *v, struct v86_frame *f, uint8_t kind, uint8_t vector, uint8_t flags)
{
    struct dpmi_record *r;
    int e = dpmi_push_record(v, f, kind, &r);
    if (e) return e;
    r->vector = vector;
    r->flags = flags;
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    dpmi_gprs(&n, f);
    uint16_t ss, sp;
    if ((e = dpmi_rm_stack(v, &ss, &sp))) { pop_record(v); return e; }
    n.tf.user_ss = ss;
    n.tf.user_esp = sp;
    uint16_t image = flags_image(v, f->tf.eflags);
    bool vif = v->monitor.vif;
    if ((e = dpmi_real_int(v, &n, vector, image))) { v->monitor.vif = vif; pop_record(v); return e; }
    dpmi_set_pm(v, false);
    *f = n;
    if (flags & DPMI_RF_HWINT) v->dpmi.stats.irqs_reflected++;
    else v->dpmi.stats.soft_reflected++;
    return 0;
}

int dpmi_hostcall(struct dosvm *v, struct v86_frame *f, uint8_t op, const struct v86_frame *regs)
{
    struct dpmi_record *r;
    int e = dpmi_push_record(v, f, DPMI_REC_HOSTCALL, &r);
    if (e) return e;
    r->op = op;
    struct v86_frame n = *regs;
    uint16_t ss, sp;
    if ((e = dpmi_rm_stack(v, &ss, &sp))) { pop_record(v); return e; }
    n.tf.user_ss = ss;
    n.tf.user_esp = sp;
    bool vif = v->monitor.vif;
    if ((e = dpmi_real_int(v, &n, 0x21, flags_image(v, 0)))) { v->monitor.vif = vif; pop_record(v); return e; }
    dpmi_set_pm(v, false);
    *f = n;
    v->dpmi.stats.hostcalls++;
    return 0;
}

/* 50-byte real-mode call structure, 0.9 section 11. */
static void struct_read(const uint8_t *s, struct v86_frame *n)
{
    n->tf.edi = vm_u32(s); n->tf.esi = vm_u32(s + 4); n->tf.ebp = vm_u32(s + 8);
    n->tf.ebx = vm_u32(s + 0x10); n->tf.edx = vm_u32(s + 0x14);
    n->tf.ecx = vm_u32(s + 0x18); n->tf.eax = vm_u32(s + 0x1C);
    n->es = vm_u16(s + 0x22); n->ds = vm_u16(s + 0x24); n->fs = vm_u16(s + 0x26); n->gs = vm_u16(s + 0x28);
    n->tf.eip = vm_u16(s + 0x2A); n->tf.cs = vm_u16(s + 0x2C);
    n->tf.user_esp = vm_u16(s + 0x2E); n->tf.user_ss = vm_u16(s + 0x30);
}
static void struct_write(struct dosvm *v, uint8_t *s, const struct v86_frame *f, bool all)
{
    vm_w32(s, f->tf.edi); vm_w32(s + 4, f->tf.esi); vm_w32(s + 8, f->tf.ebp);
    vm_w32(s + 0x10, f->tf.ebx); vm_w32(s + 0x14, f->tf.edx);
    vm_w32(s + 0x18, f->tf.ecx); vm_w32(s + 0x1C, f->tf.eax);
    vm_w16(s + 0x20, flags_image(v, f->tf.eflags));
    vm_w16(s + 0x22, (uint16_t)f->es); vm_w16(s + 0x24, (uint16_t)f->ds);
    vm_w16(s + 0x26, (uint16_t)f->fs); vm_w16(s + 0x28, (uint16_t)f->gs);
    if (all) {
        vm_w16(s + 0x2A, (uint16_t)f->tf.eip); vm_w16(s + 0x2C, (uint16_t)f->tf.cs);
        vm_w16(s + 0x2E, (uint16_t)f->tf.user_esp); vm_w16(s + 0x30, (uint16_t)f->tf.user_ss);
    }
}

/* 0300h/0301h/0302h. Returns 0 switched, 1 failure (CF), <0 fault. */
int dpmi_translation(struct dosvm *v, struct v86_frame *f, unsigned ax)
{
    struct dpmi *d = &v->dpmi;
    uint16_t sel = (uint16_t)f->tf.es;
    uint32_t off = dpmi_off(v, f->tf.edi), lin, src = 0;
    uint8_t s[0x32];
    if (!dpmi_ptr(v, sel, off, sizeof(s), true, &lin) || dosvm_read(v, lin, s, sizeof(s))) return 1;
    unsigned words = (uint16_t)f->tf.ecx;
    struct dpmi_seg ss;
    if (!dpmi_seg(v, (uint16_t)f->tf.user_ss, &ss)) return 1;
    uint32_t esp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
    if (words && !dpmi_ptr(v, (uint16_t)f->tf.user_ss, esp, words * 2, false, &src)) return 1;
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    struct_read(s, &n);
    uint8_t vector = (uint8_t)f->tf.ebx;
    if (ax == 0x0300) {
        uint16_t ivt[2];
        if (dosvm_read(v, (uint32_t)vector * 4, ivt, 4)) return 1;
        n.tf.eip = ivt[0]; n.tf.cs = ivt[1];
    }
    if (!rm_code(v, n.tf.cs, n.tf.eip)) return 1;
    bool host = !n.tf.user_ss && !n.tf.user_esp;
    unsigned need = words * 2 + (ax == 0x0301 ? 4 : 6);
    if (!host && ((uint16_t)n.tf.user_esp < need ||
                  !dosvm_range(v, (n.tf.user_ss << 4) + (uint16_t)n.tf.user_esp - need, need, true)))
        return 1;
    /* A protected-mode program terminating through the translation service
     * (0.9 section 6: INT 21h/4Ch in protected mode) is cleaned up first. */
    uint8_t ah = (uint8_t)(n.tf.eax >> 8);
    if (ax == 0x0300 && d->clients &&
        ((vector == 0x21 && (ah == 0x4C || ah == 0x00 || ah == 0x31)) || vector == 0x20 || vector == 0x27)) {
        return dpmi_terminate_int(v, f, vector, &n);
    }
    struct dpmi_record *r;
    if (dpmi_push_record(v, f, DPMI_REC_REALCALL, &r)) return 1;
    r->sel = sel; r->off = off; r->vector = vector;
    r->arg[0] = ax;
    if (host) {
        uint16_t hss, hsp;
        if (dpmi_rm_stack(v, &hss, &hsp)) { pop_record(v); return 1; }
        n.tf.user_ss = hss; n.tf.user_esp = hsp;
    }
    uint16_t sp = (uint16_t)n.tf.user_esp - (uint16_t)(words * 2);
    if (words) {
        uint8_t buf[256];
        for (unsigned done = 0; done < words * 2; ) {
            unsigned k = words * 2 - done > sizeof(buf) ? sizeof(buf) : words * 2 - done;
            if (dosvm_read(v, src + done, buf, k) || dosvm_write(v, (n.tf.user_ss << 4) + sp + done, buf, k)) {
                pop_record(v); return 1;
            }
            done += k;
        }
    }
    dpmi_low(&n.tf.user_esp, sp);
    uint16_t image = (uint16_t)((vm_u16(s + 0x20) & 0x0FD5u) | 2u);
    if ((ax != 0x0301 && !rm_push(v, &n, image)) || !rm_push(v, &n, 0xF000) || !rm_push(v, &n, DPMI_RM_RETURN)) {
        pop_record(v); return 1;
    }
    /* 0300h/0302h: entered with IF and TF clear (0.9 section 11.1/11.3);
     * 0301h: the structure's flags. */
    v->monitor.vif = ax == 0x0301 ? (image & V86_IF) != 0 : false;
    n.tf.eflags = (image & 0x0CD5u & ~V86_TF) | V86_VM | V86_IF | 2u;
    dpmi_set_pm(v, false);
    *f = n;
    d->stats.realcalls++;
    return 0;
}

/* Callback (0303h) invoked from real mode at F000:DPMI_RM_CALLBACK+4*index. */
int dpmi_callback_enter(struct dosvm *v, struct v86_frame *f, unsigned index)
{
    struct dpmi *d = &v->dpmi;
    unsigned c = index / DPMI_CALLBACKS, k = index % DPMI_CALLBACKS;
    if (c >= DPMI_CLIENTS || !d->client[c].used || !d->client[c].cb[k].used) {
        d->stats.stale++;
        return -EFAULT;                 /* stale real-mode pointer into a freed callback */
    }
    struct dpmi_callback *cb = &d->client[c].cb[k];
    uint32_t lin;
    uint8_t s[0x32];
    if (!dpmi_ptr(v, cb->struct_sel, cb->struct_off, sizeof(s), true, &lin)) { d->stats.stale++; return -EFAULT; }
    struct v86_frame rm = *f;
    rm.tf.eip = (uint16_t)(f->tf.eip - 2);   /* CS:IP = the callback address itself */
    struct_write(v, s, &rm, true);
    if (dosvm_write(v, lin, s, sizeof(s))) return -EFAULT;
    if (!dpmi_handler_live(v, (struct dpmi_vec){cb->off, cb->sel})) { d->stats.stale++; return -EFAULT; }
    struct dpmi_record *r;
    if (dpmi_push_record(v, f, DPMI_REC_CALLBACK, &r)) return -ENOSPC;
    r->arg[1] = index;
    unsigned temp = DPMI_IDX_TEMP + d->depth - 1;
    d->kind[temp] = DPMI_HOST; d->owner[temp] = 0;
    dpmi_desc_set(d->ldt[temp], (f->tf.user_ss & 0xFFFFu) << 4, 0xFFFF, 0xF2, 0);
    unsigned w = dpmi_width(v);
    if (d->lock_top < 3 * w + DPMI_HSTACK_RESERVE) { pop_record(v); return -ENOSPC; }
    uint32_t esp = d->lock_top - 3 * w;
    uint32_t frame[3] = {DPMI_PM_CBRET, DPMI_HOST_CS, flags_image(v, f->tf.eflags)};
    for (unsigned i = 0; i < 3; i++)
        if (dosvm_write(v, d->hstack.base + esp + i * w, &frame[i], w)) { pop_record(v); return -EFAULT; }
    r->arg[0] = esp + 3 * w;
    d->lock_top = esp;
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    dpmi_gprs(&n, f);
    n.tf.ds = DPMI_SEL(temp);
    n.tf.esi = (uint16_t)f->tf.user_esp;
    n.tf.es = cb->struct_sel;
    n.tf.edi = cb->struct_off;
    n.tf.cs = cb->sel;
    n.tf.eip = cb->off;
    n.tf.user_ss = DPMI_HOST_SS;
    n.tf.user_esp = esp;
    n.tf.eflags = (f->tf.eflags & 0x0CD5u & ~V86_TF) | V86_IF | 2u;
    dpmi_set_pm(v, true);
    *f = n;
    v->monitor.vif = false;
    d->stats.callbacks++;
    return 0;
}
int dpmi_callback_return(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_record *r = d->depth ? &d->rec[d->depth - 1] : 0;
    uint32_t lin;
    /* The procedure may IRET from any stack; ES:(E)DI names the structure. */
    if (!r || r->kind != DPMI_REC_CALLBACK) { d->stats.stale++; return -EFAULT; }
    uint8_t s[0x32];
    if (!dpmi_client_ptr(v, f, (uint16_t)f->tf.es, f->tf.edi, sizeof(s), false, &lin) || dosvm_read(v, lin, s, sizeof(s)))
        return -EFAULT;
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    struct_read(s, &n);
    if (!rm_code(v, n.tf.cs, n.tf.eip)) return -EFAULT;
    uint16_t image = vm_u16(s + 0x20);
    n.tf.eflags = (image & 0x0CD5u) | V86_VM | V86_IF | 2u;
    pop_record(v);
    dpmi_set_pm(v, false);
    *f = n;
    v->monitor.vif = (image & V86_IF) != 0;
    return 0;
}

/* Raw switches, 0.9 section 11.7: AX=DS CX=ES DX=SS (E)BX=(E)SP SI=CS
 * (E)DI=(E)IP; (E)BP preserved, FS=GS=0, other registers undefined. */
int dpmi_raw_to_real(struct dosvm *v, struct v86_frame *f)
{
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    dpmi_gprs(&n, f);
    n.ds = (uint16_t)f->tf.eax; n.es = (uint16_t)f->tf.ecx;
    n.tf.user_ss = (uint16_t)f->tf.edx; n.tf.user_esp = (uint16_t)f->tf.ebx;
    n.tf.cs = (uint16_t)f->tf.esi; n.tf.eip = (uint16_t)f->tf.edi;
    if (!rm_code(v, n.tf.cs, n.tf.eip)) return -EFAULT;
    n.tf.eflags = (f->tf.eflags & 0x0CD5u) | V86_VM | V86_IF | 2u;
    bool vif = v->monitor.vif;
    dpmi_lock_below(v, f);
    dpmi_set_pm(v, false);
    *f = n;
    v->monitor.vif = vif;
    v->dpmi.stats.raw_switches++;
    return 0;
}
int dpmi_raw_to_pm(struct dosvm *v, struct v86_frame *f)
{
    struct v86_frame n;
    memset(&n, 0, sizeof(n));
    dpmi_gprs(&n, f);
    n.tf.ds = (uint16_t)f->tf.eax; n.tf.es = (uint16_t)f->tf.ecx;
    n.tf.user_ss = (uint16_t)f->tf.edx;
    n.tf.user_esp = dpmi_off(v, f->tf.ebx);
    n.tf.cs = (uint16_t)f->tf.esi;
    n.tf.eip = dpmi_off(v, f->tf.edi);
    n.tf.eflags = (f->tf.eflags & 0x0CD5u & ~V86_TF) | V86_IF | 2u;
    bool vif = v->monitor.vif;
    dpmi_set_pm(v, true);
    if (!dpmi_frame_check(v, &n)) { dpmi_set_pm(v, false); return -EFAULT; }
    *f = n;
    v->monitor.vif = vif;
    v->dpmi.stats.raw_switches++;
    return 0;
}

/* State save/restore entries (0305h): the host keeps no single "other mode"
 * state that a raw switch overwrites (every suspended context is a kernel
 * record), so the buffer size is zero and both routines just return. */
int dpmi_far_return(struct dosvm *v, struct v86_frame *f)
{
    if (!v->frame_pm) {
        uint16_t ip, cs;
        struct v86_frame n = *f;
        if (!dpmi_rm_pop(v, &n, &ip) || !dpmi_rm_pop(v, &n, &cs) || !rm_code(v, cs, ip)) return -EFAULT;
        n.tf.eip = ip; n.tf.cs = cs;
        *f = n;
        return 0;
    }
    uint32_t eip = 0, cs = 0;
    struct v86_frame n = *f;
    if (!dpmi_pop(v, &n, &eip, dpmi_width(v)) || !dpmi_pop(v, &n, &cs, dpmi_width(v))) return dpmi_exception(v, f, 12, 0);
    n.tf.eip = eip; n.tf.cs = (uint16_t)cs;
    *f = n;
    return 0;
}

bool dpmi_push(struct dosvm *v, struct v86_frame *f, uint32_t value, unsigned w)
{
    struct dpmi_seg s;
    uint16_t ss = (uint16_t)f->tf.user_ss;
    if (!dpmi_seg(v, ss, &s)) return false;
    uint32_t esp = s.big ? f->tf.user_esp - w : (uint16_t)(f->tf.user_esp - w), lin;
    if (!dpmi_ptr(v, ss, esp, w, true, &lin) || dosvm_write(v, lin, &value, w)) return false;
    if (s.big) f->tf.user_esp = esp; else dpmi_low(&f->tf.user_esp, esp);
    return true;
}
bool dpmi_pop(struct dosvm *v, struct v86_frame *f, uint32_t *value, unsigned w)
{
    struct dpmi_seg s;
    uint16_t ss = (uint16_t)f->tf.user_ss;
    if (!dpmi_seg(v, ss, &s)) return false;
    uint32_t esp = s.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp, lin;
    *value = 0;
    if (!dpmi_ptr(v, ss, esp, w, false, &lin) || dosvm_read(v, lin, value, w)) return false;
    if (s.big) f->tf.user_esp = esp + w; else dpmi_low(&f->tf.user_esp, esp + w);
    return true;
}

/* INT FEh at F000:DPMI_RM_RETURN: a real-mode routine started by the host
 * returned. The top record identifies the continuation. */
int dpmi_v86_return(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (!d->depth) { d->stats.stale++; return -EFAULT; }
    struct dpmi_record rec = d->rec[d->depth - 1];
    if (rec.kind != DPMI_REC_REALCALL && rec.kind != DPMI_REC_REFLECT && rec.kind != DPMI_REC_HOSTCALL)
        return -EFAULT;
    struct v86_frame result = *f;
    bool result_vif = v->monitor.vif;
    pop_record(v);
    dpmi_set_pm(v, rec.saved_pm);
    *f = rec.saved;
    v->monitor.vif = rec.vif;
    if (rec.kind == DPMI_REC_HOSTCALL) {
        if (rec.op == DPMI_OP_ENTRY) return dpmi_entry_complete(v, f, (uint16_t)result.tf.ebx);
        if (rec.op == DPMI_OP_TERMCHECK) {
            if (d->clients && (uint16_t)result.tf.ebx == d->client[d->clients - 1].psp) dpmi_terminate(v, false);
            return v86_reflect(&v->monitor, f, rec.vector) < 0 ? -EFAULT : 0;
        }
        return dpmi_dos_complete(v, f, &rec, &result);
    }
    if (rec.kind == DPMI_REC_REALCALL) {
        uint32_t lin;
        uint8_t s[0x32];
        if (!dpmi_ptr(v, rec.sel, rec.off, sizeof(s), true, &lin) || dosvm_read(v, lin, s, sizeof(s)))
            return -EFAULT;
        struct_write(v, s, &result, false);
        /* struct_write took IF from the restored caller; report the flags
         * the real-mode routine returned with. */
        vm_w16(s + 0x20, (uint16_t)((result.tf.eflags & 0x0CD5u) | 2u | (result_vif ? V86_IF : 0)));
        if (dosvm_write(v, lin, s, sizeof(s))) return -EFAULT;
        dpmi_carry(f, false);
        return 0;
    }
    /* REFLECT: registers and arithmetic flags come back (0.9 section 3.2),
     * except for hardware interrupts, which restore the interrupted frame. */
    if (!(rec.flags & DPMI_RF_HWINT)) {
        dpmi_gprs(f, &result);
        f->tf.eflags = (f->tf.eflags & ~0x08D5u) | (result.tf.eflags & 0x08D5u);
    }
    return 0;
}
