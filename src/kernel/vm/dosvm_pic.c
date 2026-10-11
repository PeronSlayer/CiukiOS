/* Authoritative PIC register model shared with the F1 monitor.
 * Ported from v86.c and reviewed against the legacy cvgp PIC.
 * Research: docs/design/f3-acceptance.md, f3-01.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/v86.h>

static int pic_highest(const struct v86_pic *p, uint8_t bits)
{
    for (unsigned n = 1; n <= 8; n++) {
        unsigned i = (p->priority + n) & 7;
        if (bits & (1u << i))
            return (int)i;
    }
    return -1;
}

int dosvm_pic_eligible(const struct v86_pic *p)
{
    uint8_t pending = p->irr & ~p->imr;
    uint8_t service = p->isr & (p->special_mask ? (uint8_t)~p->imr : 0xFF);
    for (unsigned n = 1; n <= 8; n++) {
        unsigned i = (p->priority + n) & 7;
        if (service & (1u << i))
            break;
        if (pending & (1u << i))
            return (int)i;
    }
    return -1;
}

void dosvm_pic_accept(struct v86_pic *p, unsigned irq)
{
    p->irr &= ~(1u << irq);
    if (!p->auto_eoi)
        p->isr |= 1u << irq;
    else if (p->rotate_auto)
        p->priority = irq;
}

uint8_t dosvm_pic_register(struct v86_pic *p, bool data, bool write, uint8_t value)
{
    if (!write) {
        if (data)
            return p->imr;
        if (p->poll) {
            p->poll = false;
            int irq = dosvm_pic_eligible(p);
            if (irq < 0)
                return 0;
            dosvm_pic_accept(p, (unsigned)irq);
            return 0x80 | (uint8_t)irq;
        }
        return p->read_isr ? p->isr : p->irr;
    }
    if (data) {
        if (p->init == 2) {
            p->base = value & 0xF8;
            p->init = (p->icw1 & 2) ? ((p->icw1 & 1) ? 4 : 0) : 3;
        } else if (p->init == 3) {
            p->icw3 = value;
            p->init = (p->icw1 & 1) ? 4 : 0;
        } else if (p->init == 4) {
            p->auto_eoi = !!(value & 2);
            p->sfnm = !!(value & 0x10);
            p->init = 0;
        } else {
            p->imr = value;
        }
    } else if (value & 0x10) {
        memset(p, 0, sizeof(*p));
        p->icw1 = value;
        p->init = 2;
        p->priority = 7;
    } else if (value & 8) {
        if (value & 2)
            p->read_isr = !!(value & 1);
        if (value & 0x40)
            p->special_mask = !!(value & 0x20);
        p->poll = !!(value & 4);
    } else {
        unsigned op = value >> 5;
        if (op == 0 || op == 4)
            p->rotate_auto = op == 4;
        if (op == 6)
            p->priority = value & 7;
        if (op & 1) {
            int irq = (op & 2) ? (int)(value & 7) : pic_highest(p, p->isr);
            if (irq >= 0) {
                p->isr &= ~(1u << irq);
                if (op & 4)
                    p->priority = (uint8_t)irq;
            }
        }
    }
    return 0;
}

/* IRQ2 is latched, not erased when a slave request disappears before INTA.
 * This is the distinction between slave spurious IRQ15 and master IRQ7. */
void dosvm_pic_cascade(struct v86 *v)
{
    if (dosvm_pic_eligible(&v->pic[1]) >= 0) v->pic[0].irr |= 4;
    else if (!v->track_boundaries) v->pic[0].irr &= ~4u;
}
int dosvm_pic_pending(const struct v86 *v)
{
    struct v86_pic master = v->pic[0];
    int slave = dosvm_pic_eligible(&v->pic[1]);
    if (slave >= 0) master.irr |= 4;
    else if (!v->track_boundaries) master.irr &= ~4u;
    if (master.sfnm && slave >= 0) master.isr &= ~4u;
    int irq = dosvm_pic_eligible(&master);
    return irq == 2 ? (slave < 0 ? 15 : slave + 8) : irq;
}
uint8_t dosvm_pic_ack(struct v86 *v)
{
    dosvm_pic_cascade(v);
    int irq = dosvm_pic_pending(v);
    if (irq < 0) return v->pic[0].base + 7; /* no master ISR7 */
    uint8_t vector = v->pic[irq >> 3].base + (irq & 7);
    dosvm_pic_accept(&v->pic[0], irq >= 8 ? 2u : (unsigned)irq);
    if (irq >= 8 && dosvm_pic_eligible(&v->pic[1]) >= 0)
        dosvm_pic_accept(&v->pic[1], (unsigned)irq - 8);
    dosvm_pic_cascade(v);
    return vector;
}
