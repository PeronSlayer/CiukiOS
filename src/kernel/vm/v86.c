/* Intel SDM Vol. 3B ch. 23 (VME/PVI off, IOPL=0, denied TSS bitmap):
 * https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf
 * Intel 8259A, ICW/OCW and fully nested priorities, pp. 9-18:
 * https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8259A.pdf
 * Independent implementation; no upstream firmware code copied.
 * All memory and physical I/O is behind the same checked boundary in T0
 * and the kernel. One string element per trap bounds monitor work.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/v86.h>

static const struct v86_port_rule input_ports[] = {
    {0x60, 0x60, V86_CONTROLLER}, {0x64, 0x64, V86_CONTROLLER},
    {0x20, 0x21, V86_PIC}, {0xA0, 0xA1, V86_PIC},
    {0x40, 0x43, V86_PIT}, {0x61, 0x61, V86_PIT},
    {0x70, 0x71, V86_RTC},
};

void v86_init(struct v86 *v, const struct v86_ops *ops)
{
    memset(v, 0, sizeof(*v));
    v->ops = *ops;
    v->ports = input_ports;
    v->nports = ARRAY_SIZE(input_ports);
    v->pic[0].base = 8;
    v->pic[1].base = 0x70;
    v->pic[0].imr = (uint8_t)~((1u << 1) | (1u << 2));
    v->pic[1].imr = (uint8_t)~(1u << 4);
    v->pic[0].priority = v->pic[1].priority = 7;
}

int v86_begin(struct v86 *v, uint64_t now, uint32_t ticks)
{
    if (v->state == V86_DISABLED)
        return -V86_EIO;
    if (v->state == V86_RUNNING || v->state == V86_HALTED || !ticks || ticks > 500)
        return -EINVAL;
    v->state = V86_RUNNING;
    v->ticks = now;
    v->deadline = now + ticks;
    v->vif = true;
    v->shadow = false;
    v->result = 0;
    v->stats.calls++;
    return 0;
}

int v86_abort(struct v86 *v, const struct v86_frame *f, int error)
{
    if (v->state == V86_DISABLED && (!f || v->fault.count))
        return v->result;
    v->state = V86_DISABLED;
    v->result = error;
    memset(&v->fault, 0, sizeof(v->fault));
    if (f) {
        v->fault.cs = f->tf.cs;
        v->fault.ip = f->tf.eip;
        v->fault.vector = f->tf.vector;
        for (unsigned i = 0; i < sizeof(v->fault.bytes) && f->tf.eip + i <= 0xFFFF; i++) {
            uint8_t *p = v->ops.memory(v->ops.arg, (f->tf.cs << 4) + f->tf.eip + i, 1, false);
            if (!p)
                break;
            v->fault.bytes[v->fault.count++] = *p;
        }
    }
    return error;
}

int v86_check_deadline(struct v86 *v, uint64_t now)
{
    v->ticks = now;
    if ((v->state == V86_RUNNING || v->state == V86_HALTED) && now - v->deadline < (1ull << 63)) {
        v->stats.timeouts++;
        return v86_abort(v, 0, -V86_ETIMEDOUT);
    }
    return v->state == V86_DISABLED ? v->result : 0;
}

static int highest(const struct v86_pic *p, uint8_t bits)
{
    for (unsigned n = 1; n <= 8; n++) {
        unsigned i = (p->priority + n) & 7;
        if (bits & (1u << i))
            return (int)i;
    }
    return -1;
}

static int eligible(const struct v86_pic *p)
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

static void acknowledge(struct v86_pic *p, unsigned irq)
{
    p->irr &= ~(1u << irq);
    if (!p->auto_eoi)
        p->isr |= 1u << irq;
    else if (p->rotate_auto)
        p->priority = irq;
}

static uint8_t pic_io(struct v86_pic *p, bool data, bool write, uint8_t value)
{
    if (!write) {
        if (data)
            return p->imr;
        if (p->poll) {
            p->poll = false;
            int irq = eligible(p);
            if (irq < 0)
                return 0;
            acknowledge(p, (unsigned)irq);
            return 0x80 | (uint8_t)irq;
        }
        return p->read_isr ? p->isr : p->irr;
    }
    if (data) {
        if (p->init == 2) {
            p->base = value & 0xF8;
            p->init = (p->icw1 & 2) ? ((p->icw1 & 1) ? 4 : 0) : 3;
        } else if (p->init == 3) {
            p->init = (p->icw1 & 1) ? 4 : 0;
        } else if (p->init == 4) {
            p->auto_eoi = !!(value & 2);
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
            int irq = (op & 2) ? (int)(value & 7) : highest(p, p->isr);
            if (irq >= 0) {
                p->isr &= ~(1u << irq);
                if (op & 4)
                    p->priority = (uint8_t)irq;
            }
        }
    }
    return 0;
}

static uint16_t pit_count(const struct v86 *v)
{
    /* Host PIT ratio is 1193 cycles/tick. Virtual BIOS timer is 65536. */
    return (uint16_t)(0u - (uint32_t)v->ticks * 1193u);
}

static uint8_t pit_io(struct v86 *v, uint16_t port, bool write, uint8_t value)
{
    if (write) {
        /* Only counter latches affect the read model. Programming is ignored;
         * this F1 read-only clock must never reprogram the physical PIT. */
        if (port == 0x43 && !(value & 0x30) && (value >> 6) < 3) {
            unsigned ch = value >> 6;
            if (!v->pit_latched[ch]) {
                v->pit_latch[ch] = pit_count(v);
                v->pit_latched[ch] = 2;
                v->pit_phase[ch] = 0;
            }
        }
        return 0;
    }
    if (port == 0x61)
        return (uint8_t)(((v->ticks & 1) ? 0x10 : 0) | ((v->ticks & 2) ? 0x20 : 0));
    if (port == 0x43)
        return 0;
    unsigned ch = port - 0x40;
    uint16_t count = v->pit_latched[ch] ? v->pit_latch[ch] : pit_count(v);
    uint8_t result = (uint8_t)(count >> (v->pit_phase[ch] * 8));
    v->pit_phase[ch] ^= 1;
    if (v->pit_latched[ch])
        v->pit_latched[ch]--;
    return result;
}

int v86_io(struct v86 *v, uint16_t port, unsigned width, bool write, uint32_t *value)
{
    /* 8042/8259/PIT/CMOS are byte devices. Decode all CPU widths, but refuse
     * a multi-byte bus transaction rather than split it across device owners. */
    if (width != 1)
        goto denied;
    for (unsigned i = 0; i < v->nports; i++) {
        const struct v86_port_rule *r = &v->ports[i];
        if (port < r->first || port > r->last)
            continue;
        v->stats.io[r->kind]++;
        uint8_t b = (uint8_t)*value;
        switch (r->kind) {
        case V86_CONTROLLER:
            if (write)
                v->ops.out(v->ops.arg, port, b);
            else
                b = v->ops.in(v->ops.arg, port);
            break;
        case V86_PIC: b = pic_io(&v->pic[port >= 0xA0], port & 1, write, b); break;
        case V86_PIT: b = pit_io(v, port, write, b); break;
        case V86_RTC:
            if (write) {
                if (port == 0x70)
                    v->rtc_index = b; /* virtual index only, including NMI bit */
            } else {
                if (!v->rtc_valid)
                    return -V86_EIO; /* no fabricated cached RTC */
                b = port == 0x70 ? v->rtc_index : v->rtc[v->rtc_index & 0x7F];
            }
            break;
        default: goto denied;
        }
        if (!write)
            *value = b;
        return 0;
    }
denied:
    v->stats.disallowed_io++;
    return -V86_EPERM;
}

static void *segmem(struct v86 *v, uint32_t seg, uint32_t off, unsigned n, bool write)
{
    if (seg > 0xFFFF || off > 0xFFFF || n > 0x10000u - off)
        return 0;
    return v->ops.memory(v->ops.arg, (seg << 4) + off, n, write);
}

static uint32_t getword(const uint8_t *p, unsigned width)
{
    uint32_t x = 0;
    for (unsigned i = 0; i < width; i++)
        x |= (uint32_t)p[i] << (i * 8);
    return x;
}

static void putword(uint8_t *p, unsigned width, uint32_t x)
{
    for (unsigned i = 0; i < width; i++)
        p[i] = (uint8_t)(x >> (i * 8));
}

static uint32_t guest_flags(const struct v86 *v, const struct v86_frame *f)
{
    return (f->tf.eflags & 0xCD5u) | 2u | (v->vif ? V86_IF : 0);
}

static void set_flags(struct v86 *v, struct v86_frame *f, uint32_t flags)
{
    v->vif = !!(flags & V86_IF);
    /* IOPL, NT, VM, VIF/VIP and guest TF never control the host return. */
    f->tf.eflags = (flags & 0xCD5u) | V86_VM | V86_IF | 2u;
}

static void setlow(uint32_t *reg, uint32_t value)
{
    *reg = (*reg & 0xFFFF0000u) | (uint16_t)value;
}

int v86_reflect(struct v86 *v, struct v86_frame *f, uint8_t vector)
{
    uint8_t *ivt = v->ops.memory(v->ops.arg, (uint32_t)vector * 4, 4, false);
    uint16_t sp = (uint16_t)(f->tf.user_esp - 6);
    uint8_t *ip_slot = segmem(v, f->tf.user_ss, sp, 2, true);
    uint8_t *cs_slot = segmem(v, f->tf.user_ss, (uint16_t)(sp + 2), 2, true);
    uint8_t *fl_slot = segmem(v, f->tf.user_ss, (uint16_t)(sp + 4), 2, true);
    if (!ivt || !ip_slot || !cs_slot || !fl_slot)
        return v86_abort(v, f, -EFAULT);
    uint32_t ip = getword(ivt, 2), cs = getword(ivt + 2, 2);
    if (!segmem(v, cs, ip, 1, false))
        return v86_abort(v, f, -EFAULT);
    putword(ip_slot, 2, f->tf.eip);
    putword(cs_slot, 2, f->tf.cs);
    putword(fl_slot, 2, guest_flags(v, f));
    setlow(&f->tf.user_esp, sp);
    f->tf.eip = ip;
    f->tf.cs = cs;
    v->vif = false;
    v->shadow = false;
    f->tf.eflags &= ~V86_TF;
    v->state = V86_RUNNING;
    return 0;
}

void v86_irq_raise(struct v86 *v, unsigned irq)
{
    if (irq < 16)
        v->pic[irq >> 3].irr |= 1u << (irq & 7);
}

int v86_irq_deliver(struct v86 *v, struct v86_frame *f)
{
    if (!v->vif || v->shadow)
        return 0;
    int slave = eligible(&v->pic[1]);
    if (slave >= 0)
        v->pic[0].irr |= 4;
    else
        v->pic[0].irr &= ~4u;
    int master = eligible(&v->pic[0]);
    if (master < 0)
        return 0;
    uint8_t vector = master == 2 ? v->pic[1].base + (uint8_t)slave : v->pic[0].base + master;
    int rc = v86_reflect(v, f, vector);
    if (rc)
        return rc;
    acknowledge(&v->pic[0], (unsigned)master);
    if (master == 2)
        acknowledge(&v->pic[1], (unsigned)slave);
    v->stats.reflected_irqs++;
    return 1;
}

int v86_debug_step(struct v86 *v, struct v86_frame *f)
{
    if (!v->shadow)
        return v86_abort(v, f, -EFAULT);
    v->shadow = false;
    f->tf.eflags &= ~V86_TF;
    return 0;
}

int v86_emulate(struct v86 *v, struct v86_frame *f)
{
    bool op32 = false, addr32 = false, rep = false;
    uint32_t seg = f->ds;
    unsigned len = 0;
    uint8_t op;
    for (;;) {
        uint8_t *p = len < 15 ? segmem(v, f->tf.cs, f->tf.eip + len, 1, false) : 0;
        if (!p)
            return v86_abort(v, f, -EFAULT);
        op = *p;
        len++;
        switch (op) {
        case 0x66: op32 = true; continue;
        case 0x67: addr32 = true; continue;
        case 0xF2: case 0xF3: rep = true; continue;
        case 0x26: seg = f->es; continue;
        case 0x2E: seg = f->tf.cs; continue;
        case 0x36: seg = f->tf.user_ss; continue;
        case 0x3E: seg = f->ds; continue;
        case 0x64: seg = f->fs; continue;
        case 0x65: seg = f->gs; continue;
        default: break;
        }
        break;
    }
    unsigned width = op32 ? 4 : 2;
    uint32_t next = (uint16_t)(f->tf.eip + len), value;
    uint16_t sp = (uint16_t)f->tf.user_esp;
    uint8_t *p;
    bool old_shadow = v->shadow;
    int rc = 0;
    switch (op) {
    case 0xCD: case 0xCC: case 0xCE: {
        uint8_t vector = op == 0xCC ? 3 : 4;
        if (op == 0xCD) {
            p = len < 15 ? segmem(v, f->tf.cs, f->tf.eip + len, 1, false) : 0;
            if (!p)
                goto fault;
            vector = *p;
            next = (uint16_t)(next + 1);
        }
        if (op == 0xCE && !(f->tf.eflags & 0x800))
            break;
        /* Firmware-internal INTs use the IVT, including INT15/4F keyboard
         * hooks. biosvm_call enforces the kernel-entry allowlist; the call
         * deadline, mapped memory and I/O policy still bound this execution. */
        v->stats.insn[V86_INT]++;
        f->tf.eip = next;
        return v86_reflect(v, f, vector);
    }
    case 0xCF: {
        p = segmem(v, f->tf.user_ss, sp, width, false);
        uint8_t *cs_slot = segmem(v, f->tf.user_ss, (uint16_t)(sp + width), width, false);
        uint8_t *fl_slot = segmem(v, f->tf.user_ss, (uint16_t)(sp + width * 2), width, false);
        if (!p || !cs_slot || !fl_slot)
            goto fault;
        uint32_t ip = getword(p, width), cs = getword(cs_slot, width);
        value = getword(fl_slot, width);
        if (ip > 0xFFFF || cs > 0xFFFF)
            goto fault;
        v->stats.insn[V86_IRET]++;
        f->tf.eip = ip;
        f->tf.cs = cs;
        setlow(&f->tf.user_esp, sp + width * 3);
        set_flags(v, f, value);
        v->shadow = false;
        if (cs == v->sentinel_cs && ip == v->sentinel_ip &&
            f->tf.user_ss == v->sentinel_ss && (uint16_t)f->tf.user_esp == v->sentinel_sp) {
            v->state = V86_DONE;
            return 1;
        }
        return 0;
    }
    case 0x9C:
        sp = (uint16_t)(sp - width);
        p = segmem(v, f->tf.user_ss, sp, width, true);
        if (!p)
            goto fault;
        putword(p, width, guest_flags(v, f));
        setlow(&f->tf.user_esp, sp);
        v->stats.insn[V86_PUSHF]++;
        break;
    case 0x9D:
        p = segmem(v, f->tf.user_ss, sp, width, false);
        if (!p)
            goto fault;
        set_flags(v, f, getword(p, width));
        setlow(&f->tf.user_esp, sp + width);
        v->stats.insn[V86_POPF]++;
        break;
    case 0xFA:
        v->vif = false;
        v->stats.insn[V86_CLI]++;
        break;
    case 0xFB:
        v->shadow = !v->vif;
        v->vif = true;
        if (v->shadow)
            f->tf.eflags |= V86_TF; /* retire the next instruction before injecting */
        else
            f->tf.eflags &= ~V86_TF;
        v->stats.insn[V86_STI]++;
        f->tf.eip = next;
        return 0;
    case 0xF4:
        v->state = V86_HALTED;
        v->stats.insn[V86_HLT]++;
        rc = 2;
        break;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xEC: case 0xED: case 0xEE: case 0xEF: {
        uint16_t port = (uint16_t)f->tf.edx;
        if (op < 0xE8) {
            p = len < 15 ? segmem(v, f->tf.cs, f->tf.eip + len, 1, false) : 0;
            if (!p)
                goto fault;
            port = *p;
            next = (uint16_t)(next + 1);
        }
        if (!(op & 1))
            width = 1;
        bool write = !!(op & 2);
        value = f->tf.eax;
        rc = v86_io(v, port, width, write, &value);
        if (rc)
            return v86_abort(v, f, rc);
        if (!write) {
            uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
            f->tf.eax = (f->tf.eax & ~mask) | (value & mask);
        }
        v->stats.insn[write ? V86_OUT : V86_IN]++;
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        bool write = !!(op & 2);
        uint32_t count = addr32 ? f->tf.ecx : (uint16_t)f->tf.ecx;
        if (rep && !count)
            break;
        if (!(op & 1))
            width = 1;
        uint32_t *index = write ? &f->tf.esi : &f->tf.edi;
        uint32_t off = addr32 ? *index : (uint16_t)*index;
        p = segmem(v, write ? seg : f->es, off, width, !write);
        if (!p)
            goto fault; /* validate INS destination before its physical read */
        value = write ? getword(p, width) : 0;
        rc = v86_io(v, (uint16_t)f->tf.edx, width, write, &value);
        if (rc)
            return v86_abort(v, f, rc);
        if (!write)
            putword(p, width, value);
        off += (f->tf.eflags & 0x400) ? 0u - width : width;
        if (addr32)
            *index = off;
        else
            setlow(index, off);
        if (rep) {
            if (addr32)
                f->tf.ecx = --count;
            else
                setlow(&f->tf.ecx, --count);
            if (count)
                next = f->tf.eip;
        }
        v->stats.insn[write ? V86_OUTS : V86_INS]++;
        break;
    }
    default: goto fault;
    }
    f->tf.eip = next;
    if (old_shadow) {
        v->shadow = false;
        f->tf.eflags &= ~V86_TF;
    }
    return rc;
fault:
    return v86_abort(v, f, -EFAULT);
}
