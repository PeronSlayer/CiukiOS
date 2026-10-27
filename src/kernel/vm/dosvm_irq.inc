/* VPICD-style completion guard, separate from the 8259A register model.
 * Research and timeout policy: docs/validation/fx-dospic.md.
 * SPDX-License-Identifier: GPL-2.0-only */
bool dosvm_irq_held(const struct dosvm *v, unsigned irq)
{
    if (irq >= 16) return false;
    uint32_t flags = irq_save();
    uint64_t now = g_ticks;
    bool held = v->irq.rm[irq].active && now - v->irq.rm[irq].tick < DPMI_ISR_WAIT_MS;
    for (unsigned i = 0; !held && i < v->dpmi.depth; i++) {
        const struct dpmi_record *r = &v->dpmi.rec[i];
        held = (r->flags & DPMI_RF_HWINT) && r->isr_measured && !r->isr_expired &&
               r->isr_irq == irq && now - r->isr_tick < DPMI_ISR_WAIT_MS;
    }
    irq_restore(flags);
    return held;
}

/* Called in the same IRQ-disabled transaction as reflection and INTA.
 * Firmware calls keep using v86_irq_deliver directly. */
int dosvm_rm_irq_deliver(struct dosvm *v, struct v86_frame *f)
{
    int irq = dosvm_pic_pending(&v->monitor);
    int e = v86_irq_deliver(&v->monitor, f);
    if (e > 0 && irq >= 0) {
        v->irq.rm[irq].active = true;
        v->irq.rm[irq].tick = g_ticks;
        v->irq.rm[irq].ss = (uint16_t)f->tf.user_ss;
        v->irq.rm[irq].sp = (uint16_t)f->tf.user_esp;
    }
    return e;
}

static void dosvm_rm_irq_return(struct dosvm *v, uint16_t ss, uint16_t sp)
{
    uint32_t flags = irq_save();
    for (unsigned irq = 0; irq < 16; irq++)
        if (v->irq.rm[irq].active && v->irq.rm[irq].ss == ss && v->irq.rm[irq].sp == sp)
            v->irq.rm[irq].active = false;
    irq_restore(flags);
}

void dosvm_irq_release(struct dosvm *v)
{
    uint32_t flags = irq_save();
    uint16_t owed = v->irq.owed;
    while (owed) {
        unsigned irq = (unsigned)__builtin_ctz((unsigned)owed);
        owed &= ~(1u << irq);
        if (!dosvm_irq_held(v, irq)) {
            v->irq.owed &= ~(1u << irq);
            v86_irq_raise(&v->monitor, irq);
            dosvm_idle_wake(v);
        }
    }
    /* A return may release an already paced tick at this very boundary.
     * With no elapsed clocks this cannot create a new PIT edge. */
    if (v->pit.owed && !dosvm_irq_held(v, 0)) dosvm_pit_advance(v, 0);
    irq_restore(flags);
}
