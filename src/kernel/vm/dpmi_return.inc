/* f3-34: sparse return-boundary acquisition. Included by dpmi_dispatch.c.
 * Intel SDM Vol. 3B 18.2/18.3; contract amendment in dos-dpmi-contract.md.
 * Predictions only place an execute breakpoint. They never advance a CPU
 * frame, establish retirement, patch code/flags, or change virtual IF.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool return_descriptor(struct dosvm *v, uint16_t sel, uint32_t out[2])
{
    struct dpmi_seg s;
    if (!dpmi_seg(v, sel, &s) || !s.present || s.dpl != 3 || (sel & 3) != 3) return false;
    dpmi_desc_set(out, s.base, s.limit, s.access & ~1u, s.big ? 0x40 : 0);
    return true;                         /* ignore CPU-owned accessed bit */
}
static bool return_descriptors_live(struct dosvm *v)
{
    struct dpmi *d = &v->dpmi;
    uint32_t code[2], stack[2], origin[2];
    return return_descriptor(v, d->return_cs, code) &&
        return_descriptor(v, d->return_ss, stack) &&
        return_descriptor(v, d->return_origin_cs, origin) &&
        !memcmp(code, d->return_code_desc, sizeof(code)) &&
        !memcmp(stack, d->return_stack_desc, sizeof(stack)) &&
        !memcmp(origin, d->return_origin_desc, sizeof(origin)) &&
        dpmi_handler_live(v, (struct dpmi_vec){d->return_eip, d->return_cs});
}
static void return_checkpoint(struct dosvm *, struct v86_frame *);
/* No instruction bytes or saved flags are patched. TF is set only before
 * an observed instruction which cannot save it or inhibit its exception. */
static bool return_fallback(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    uint16_t target_cs = (uint16_t)f->tf.cs;
    if (!d->return_steps || (f->tf.eflags & (V86_TF | (1u << 14)))) return false;
    if (!return_step_safe(v, f)) {
        struct dpmi_seg cs;
        uint8_t b[15]; unsigned w;
        if (!dpmi_seg(v, target_cs, &cs) || f->tf.eip > cs.limit) return false;
        unsigned n = cs.limit - f->tf.eip >= 14 ? 15 : cs.limit - f->tf.eip + 1;
        if (dosvm_read(v, cs.base + f->tf.eip, b, n)) return false;
        unsigned pos = flags_opcode(b, n, cs.big, &w);
        if (pos == n || b[pos] != 0xCF) return false;
        for (unsigned i = 0; i < pos; i++) if (b[i] != 0x66) return false;
        struct v86_frame stack = *f;
        uint32_t ip, sel, flags;
        if (!dpmi_pop(v, &stack, &ip, w) || !dpmi_pop(v, &stack, &sel, w) ||
            !dpmi_pop(v, &stack, &flags, w) || (sel & 3) != 3 ||
            (flags & (V86_VM | V86_TF | (1u << 14))) ||
            !dpmi_handler_live(v, (struct dpmi_vec){ip, (uint16_t)sel})) return false;
        target_cs = (uint16_t)sel; /* SDM: BS even if IRET clears entry TF */
    }
    d->return_step_cs = (uint16_t)f->tf.cs;
    d->return_step_target_cs = target_cs;
    d->return_step = true;
    d->return_steps--;
    f->tf.eflags |= V86_TF;
    dpmi_reload_debug(v); /* no private execute fault may mask this BS */
    return true;
}
static bool return_unstep(struct dosvm *v, struct v86_frame *f, bool retired)
{
    struct dpmi *d = &v->dpmi;
    if (!d->return_step || d->depth != d->return_depth || d->clients != d->return_client ||
        (uint16_t)f->tf.user_ss != d->return_ss ||
        (uint16_t)f->tf.cs != (retired ? d->return_step_target_cs : d->return_step_cs)) return false;
    f->tf.eflags &= ~V86_TF;
    d->return_step = false;
    return true;
}
static bool return_arm(struct dosvm *v, struct v86_frame *f, uint16_t cs, uint32_t ip)
{
    struct dpmi *d = &v->dpmi;
    uint32_t linear;
    if (!d->return_steps || !d->return_hops ||
        (cs == (uint16_t)f->tf.cs && ip == f->tf.eip) ||
        !dpmi_handler_live(v, (struct dpmi_vec){ip, cs}) ||
        !dpmi_ptr(v, cs, ip, 1, false, &linear) ||
        !return_descriptor(v, cs, d->return_code_desc) ||
        !return_descriptor(v, (uint16_t)f->tf.user_ss, d->return_stack_desc) ||
        !return_descriptor(v, (uint16_t)f->tf.cs, d->return_origin_desc)) {
        dpmi_return_cancel(v);
        return false;
    }
    d->return_depth = (uint8_t)d->depth; d->return_client = (uint8_t)d->clients;
    d->return_cs = cs; d->return_ss = (uint16_t)f->tf.user_ss;
    d->return_origin_cs = (uint16_t)f->tf.cs;
    d->return_eip = ip; d->return_linear = linear;
    dpmi_reload_debug(v);
    if (d->return_slot < DPMI_WATCH) v->perf.return_armed++;
    if (d->return_slot == DPMI_WATCH && !return_fallback(v, f)) dpmi_return_cancel(v);
    return d->return_steps != 0 || d->return_step;
}
static void return_resume(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    if (!d->return_steps && !d->return_step) return;
    if (d->clients != d->return_client || d->depth != d->return_depth) {
        d->return_replan = true;
        if (d->clients < d->return_client || d->depth < d->return_depth) dpmi_return_cancel(v);
        return;
    }
    bool expired = g_ticks - d->return_tick >= 100 || d->return_irqs >= 64;
    if (expired) v->perf.return_expired++;
    if (expired || !v->frame_pm || !v->monitor.vif ||
        (f->tf.eflags & ((d->return_step ? 0 : V86_TF) | (1u << 14))) ||
        (uint16_t)f->tf.user_ss != d->return_ss ||
        ((uint16_t)f->tf.cs != d->return_cs && (uint16_t)f->tf.cs != d->return_origin_cs) ||
        !return_descriptors_live(v)) {
        return_unstep(v, f, false);
        dpmi_return_cancel(v);
        return;
    }
    if (d->return_replan) {
        d->return_replan = false;
        return_unstep(v, f, false);
        /* Nested code can edit the stack or a decoded branch. Recompute
         * from the current CPU boundary, never resurrect the old target. */
        return_checkpoint(v, f);
    } else if (d->return_slot == DPMI_WATCH && !d->return_step && !return_fallback(v, f))
        dpmi_return_cancel(v);
}

static uint32_t return_register(const struct v86_frame *f, unsigned reg)
{
    switch (reg & 7) {
    case 0: return f->tf.eax; case 1: return f->tf.ecx;
    case 2: return f->tf.edx; case 3: return f->tf.ebx;
    case 4: return f->tf.user_esp; case 5: return f->tf.ebp;
    case 6: return f->tf.esi; default: return f->tf.edi;
    }
}
static bool return_condition(unsigned cc, uint32_t flags)
{
    bool cf = flags & 1, pf = flags & 4, zf = flags & 0x40, sf = flags & 0x80, of = flags & 0x800;
    bool yes;
    switch (cc >> 1) {
    case 0: yes = of; break; case 1: yes = cf; break;
    case 2: yes = zf; break; case 3: yes = cf || zf; break;
    case 4: yes = sf; break; case 5: yes = pf; break;
    case 6: yes = sf != of; break; default: yes = zf || sf != of; break;
    }
    return cc & 1 ? !yes : yes;
}
/* Decode only the near indirect operand at the *current CPU boundary*.
 * Operand reads use the ordinary guest-copy checks, including segment
 * overrides, SIB/displacements and 16-bit address wrapping. */
static bool return_indirect(struct dosvm *v, const struct v86_frame *f, const uint8_t *b,
                            unsigned n, unsigned pos, unsigned w, bool big, uint32_t *target)
{
    unsigned at = pos + 1;
    if (at >= n) return false;
    unsigned m = b[at++], mod = m >> 6, rm = m & 7;
    if (mod == 3) { *target = return_register(f, rm) & (w == 2 ? 0xFFFFu : UINT32_MAX); return true; }
    bool a32 = big, stack = false;
    uint32_t override = UINT32_MAX;
    for (unsigned i = 0; i < pos; i++) switch (b[i]) {
    case 0x67: a32 = !big; break;
    case 0x26: override = (uint16_t)f->tf.es; break;
    case 0x2E: override = (uint16_t)f->tf.cs; break;
    case 0x36: override = (uint16_t)f->tf.user_ss; break;
    case 0x3E: override = (uint16_t)f->tf.ds; break;
    case 0x64: override = (uint16_t)f->tf.fs; break;
    case 0x65: override = (uint16_t)f->tf.gs; break;
    default: break;
    }
    uint32_t off = 0;
    unsigned disp = mod == 1 ? 1 : mod == 2 ? (a32 ? 4 : 2) : 0;
    if (a32) {
        unsigned base = rm;
        if (rm == 4) {
            if (at >= n) return false;
            unsigned sib = b[at++], index = (sib >> 3) & 7;
            base = sib & 7;
            if (index != 4) off = return_register(f, index) << (sib >> 6);
        }
        if (!mod && base == 5) disp = 4;
        else { off += return_register(f, base); stack = base == 4 || base == 5; }
    } else {
        static const uint8_t first[8] = {3,3,5,5,6,7,5,3};
        if (!mod && rm == 6) disp = 2;
        else {
            off = return_register(f, first[rm]); stack = first[rm] == 5;
            if (rm < 4) off += return_register(f, rm & 1 ? 7 : 6);
        }
    }
    if (at + disp > n || at + disp > 15) return false;
    if (disp) off += disp == 1 ? (uint32_t)(int32_t)(int8_t)b[at] :
                    disp == 2 ? vm_u16(b + at) : vm_u32(b + at);
    if (!a32) off &= 0xFFFFu;
    uint16_t sel = override != UINT32_MAX ? (uint16_t)override : (uint16_t)(stack ? f->tf.user_ss : f->tf.ds);
    uint32_t linear;
    *target = 0;
    return dpmi_ptr(v, sel, off, w, false, &linear) && !dosvm_read(v, linear, target, w);
}

/* Additional lengths needed by the sparse scanner, separate from the
 * stricter flag-image proof decoder. Unknown safe encodings use a bounded CPU step.
 * The CPU alone executes and checks these instructions. */
static unsigned return_length(const uint8_t *b, unsigned n, bool big, unsigned *pos, unsigned *w)
{
    uint8_t clean[15], op;
    unsigned k = 0, removed = 0, i = 0;
    *w = big ? 4 : 2;
    for (; i < n; i++) {
        op = b[i];
        if (op == 0x66) *w = big ? 2 : 4;
        else if (op == 0xF0 || op == 0xF2 || op == 0xF3) { removed++; continue; }
        else if (op != 0x67 && op != 0x26 && op != 0x2E && op != 0x36 && op != 0x3E && op != 0x64 && op != 0x65) break;
        clean[k++] = op;
    }
    *pos = i;
    if (i == n) return 0;
    unsigned cp = k;
    while (i < n) clean[k++] = b[i++];
    op = clean[cp];
    unsigned len = 0;
    if ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) || op == 0xEB) len = *pos + 2;
    else if (op == 0xE8 || op == 0xE9) len = *pos + 1 + *w;
    else if (op == 0xC2) len = *pos + 3;
    else if (op == 0xC3 || op == 0xC9 || (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9)) len = *pos + 1;
    else if (op == 0x0F && cp + 1 < k && clean[cp + 1] >= 0x80 && clean[cp + 1] <= 0x8F) len = *pos + 2 + *w;
    else if (op == 0x0F && cp + 1 < k && (clean[cp + 1] == 0xA1 || clean[cp + 1] == 0xA9)) len = *pos + 2;
    else {
        unsigned imm = 0;
        if (op == 0xFF && cp + 1 < k) clean[cp + 1] &= ~0x38u;
        if (op >= 0xD8 && op <= 0xDF) clean[cp] = 0x8B; /* same ModRM address length */
        if (op == 0x0F && cp + 1 < k &&
            (clean[cp + 1] == 0xA4 || clean[cp + 1] == 0xA5 || clean[cp + 1] == 0xAC || clean[cp + 1] == 0xAD)) {
            imm = !(clean[cp + 1] & 1);
            clean[cp + 1] = 0xAF;       /* same ModRM, plus optional imm8 */
        }
        unsigned ignored;
        len = flags_instruction(clean, k, big, &op, &ignored);
        if (len) len += removed + imm;
    }
    return len <= n && len <= 15 ? len : 0;
}
static bool return_transfer(const uint8_t *b, unsigned n, unsigned pos)
{
    unsigned op = b[pos];
    return op == 0xC2 || op == 0xC3 || op == 0xE8 || op == 0xE9 || op == 0xEB ||
        (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) ||
        (op == 0x0F && pos + 1 < n && b[pos + 1] >= 0x80 && b[pos + 1] <= 0x8F) ||
        (op == 0xFF && pos + 1 < n && (((b[pos + 1] >> 3) & 7) == 2 || ((b[pos + 1] >> 3) & 7) == 4));
}

static void return_checkpoint(struct dosvm *v, struct v86_frame *f)
{
    struct dpmi *d = &v->dpmi;
    struct dpmi_seg cs;
    if (!v->monitor.vif || !d->return_hops || !d->return_steps ||
        (f->tf.eflags & (V86_TF | (1u << 14))) || !dpmi_seg(v, (uint16_t)f->tf.cs, &cs)) {
        dpmi_return_cancel(v); return;
    }
    bool spare = false;
    for (unsigned i = 0; i < DPMI_WATCH; i++) spare |= !d->watch[i].used;
    if (v->return_tracker_tf || !spare) {
        if (!return_fallback(v, f)) dpmi_return_cancel(v);
        return;
    }
    struct v86_frame scan = *f;
    bool may_write = false;
    while (d->return_steps && scan.tf.eip <= cs.limit) {
        uint8_t b[15]; unsigned pos, w;
        unsigned n = cs.limit - scan.tf.eip >= 14 ? 15 : cs.limit - scan.tf.eip + 1;
        uint32_t linear;
        if (!dpmi_ptr(v, (uint16_t)f->tf.cs, scan.tf.eip, n, false, &linear) ||
            dosvm_read(v, linear, b, n)) break;
        unsigned len = return_length(b, n, cs.big, &pos, &w);
        bool barrier = !return_step_safe(v, &scan);
        if (len && !barrier && !may_write && (b[pos] == 0xE8 || b[pos] == 0xE9 || b[pos] == 0xEB)) {
            /* Follow static transfers only while preceding decoded work
             * cannot write their encoding. A store requires an observed
             * transfer boundary before reading its new displacement. */
            unsigned iw = b[pos] == 0xEB ? 1 : w;
            int32_t rel = iw == 1 ? (int8_t)b[len - 1] : iw == 2 ? (int16_t)vm_u16(b + len - 2) : (int32_t)vm_u32(b + len - 4);
            uint32_t target = scan.tf.eip + len + (uint32_t)rel;
            if (iw == 2 || (iw == 1 && !cs.big)) target &= 0xFFFFu;
            if (target == scan.tf.eip || target == f->tf.eip) break;
            /* CALL writes a return slot, potentially through a code alias. */
            may_write = b[pos] == 0xE8;
            d->return_steps--; scan.tf.eip = target;
            continue;
        }
        if (barrier || !len || return_transfer(b, n, pos)) {
            if (scan.tf.eip != f->tf.eip) {
                return_arm(v, f, (uint16_t)f->tf.cs, scan.tf.eip);
                return;
            }
            if (barrier) break;
            if (!len) {
                if (return_fallback(v, f)) return;
                break;
            }
            /* Live register/stack values are meaningful only here. No
             * future branch direction or indirect operand is guessed. */
            uint32_t next = f->tf.eip + len, target = next;
            unsigned op = b[pos];
            if (op == 0xC2 || op == 0xC3) {
                struct v86_frame stack = *f;
                if (!dpmi_pop(v, &stack, &target, w)) break;
            } else if (op == 0xFF) {
                if (!return_indirect(v, f, b, n, pos, w, cs.big, &target)) break;
            } else {
                unsigned iw = op == 0xE8 || op == 0xE9 || op == 0x0F ? w : 1;
                int32_t rel = iw == 1 ? (int8_t)b[len - 1] : iw == 2 ? (int16_t)vm_u16(b + len - 2) : (int32_t)vm_u32(b + len - 4);
                bool take = true;
                if ((op >= 0x70 && op <= 0x7F) || op == 0x0F)
                    take = return_condition((op == 0x0F ? b[pos + 1] : op) & 15, f->tf.eflags);
                if (op >= 0xE0 && op <= 0xE3) {
                    bool a32 = cs.big;
                    for (unsigned j = 0; j < pos; j++) if (b[j] == 0x67) a32 = !cs.big;
                    uint32_t count = (f->tf.ecx - (op == 0xE3 ? 0 : 1)) & (a32 ? UINT32_MAX : 0xFFFFu);
                    take = op == 0xE3 ? !count : count != 0;
                    if (op == 0xE0) take &= !(f->tf.eflags & V86_ZF);
                    if (op == 0xE1) take &= (f->tf.eflags & V86_ZF) != 0;
                }
                if (take) target = next + (uint32_t)rel;
                if (iw == 2 && take) target &= 0xFFFFu;
            }
            if (!cs.big && (op == 0xEB || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3))) target &= 0xFFFFu;
            d->return_steps--;
            if (target == f->tf.eip) break;
            may_write |= op == 0xFF && ((b[pos + 1] >> 3) & 7) == 2;
            scan.tf.eip = target;
            continue;
        }
        /* This deliberately conservative classifier shares the epilogue's
         * complete register-only encodings. Unknown memory effects fence
         * subsequent direct transfers, without stepping ordinary code. */
        may_write |= b[pos] != 0x90 && !return_register_instruction(b, pos);
        d->return_steps--;
        if (scan.tf.eip > UINT32_MAX - len || (!cs.big && scan.tf.eip + len > 0xFFFFu)) break;
        scan.tf.eip += len;
    }
    dpmi_return_cancel(v);
}

static uint32_t return_debug_bit(const struct dpmi *d)
{
    return d->return_steps && d->return_slot < DPMI_WATCH &&
        !d->watch[d->return_slot].used && (d->dr7 & (1u << (2 * d->return_slot))) ?
        1u << d->return_slot : 0;
}
static bool return_debug_trap(struct dosvm *v, struct v86_frame *f, uint32_t *dr6)
{
    struct dpmi *d = &v->dpmi;
    uint32_t bit = return_debug_bit(d);
    if (d->return_step) {
        bool bs = f->tf.vector == 1 && (*dr6 & (1u << 14));
        if (return_unstep(v, f, bs)) {
            if (bs) {
                *dr6 &= ~(1u << 14);
                v->perf.return_fallbacks++;
                if (!(*dr6 & (15u | (1u << 13) | (1u << 15)))) {
                    return_checkpoint(v, f);
                    return true;
                }
            }
            dpmi_return_cancel(v);
        }
    }
    if (f->tf.vector != 1 || !(*dr6 & bit)) return false;
    *dr6 &= ~bit;                        /* never becomes a client .hit */
    bool arrived = return_context(d, f) && f->tf.eip == d->return_eip && return_descriptors_live(v);
    if (arrived && !(*dr6 & (15u | (1u << 13) | (1u << 14) | (1u << 15)))) {
        v->perf.return_hits++;
        struct dpmi_seg cs;
        if (dpmi_seg(v, (uint16_t)f->tf.cs, &cs) && cs.base) v->perf.return_segmented++;
        d->return_hops--;
        return_checkpoint(v, f);
    } else dpmi_return_cancel(v);        /* alias hit or simultaneous debugger */
    /* The slot is removed or moved before retry. No RF is needed for our
     * fault; leave a client's RF intact. A client execute hit sets its own. */
    return !(*dr6 & (15u | (1u << 13) | (1u << 14) | (1u << 15)));
}
