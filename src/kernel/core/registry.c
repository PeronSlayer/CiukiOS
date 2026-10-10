/* Resource registry: legacy reservations before PCI, read-only PCI
 * inventory through configuration mechanism #1 (no BAR sizing, no
 * activation), transactional claims that fail without side effects on
 * overlap (docs/design/device-firmware-ownership.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/registry.h>
#include <ciuki/arch.h>
#include <ciuki/work.h>
#include <ciuki/timing.h>
#include <ciuki/init.h>

#define MAX_RES 64
#define MAX_PCI 32

static struct resource res[MAX_RES];
static unsigned nres;
static bool idle_proven[MAX_RES];
static struct registry_stats counters;
static struct pci_func pci[MAX_PCI];
static unsigned npci;
static bool irq_claim_attached(int h, gen_t generation);
static void irq_quarantine_claim(int h, gen_t generation);

static bool overlaps(const struct resource *r, enum res_type t, uint32_t s, uint32_t e)
{
    return r->type == t && r->state != RS_RELEASED && s < r->end && r->start < e;
}

static bool matches(int h, gen_t generation)
{
    return h >= 0 && (unsigned)h < nres && gen_matches(res[h].generation, generation);
}

bool registry_valid(int h, gen_t generation)
{
    uint32_t f = irq_save();
    bool valid = matches(h, generation) && res[h].state != RS_RELEASED && res[h].state != RS_QUARANTINED;
    irq_restore(f);
    return valid;
}

static int claim(enum res_type type, uint32_t start, uint32_t end, const char *owner,
                 bool share, enum res_state state)
{
    if (type > RES_MMIO || (int)type < 0 || end <= start || !owner || !*owner ||
        (type == RES_PORT && end > 0x10000) ||
        (type == RES_IRQ && (end > 16 || end - start != 1)) ||
        (type == RES_DMA && end > 8) || (share && type != RES_IRQ))
        return -EINVAL;
    uint32_t f = irq_save();
    unsigned slot = nres;
    for (unsigned i = 0; i < nres; i++) {
        if (slot == nres && res[i].state == RS_RELEASED)
            slot = i;
        bool shared = share && res[i].shareable && start == res[i].start && end == res[i].end &&
                      (res[i].state == RS_CLAIMED || res[i].state == RS_ACTIVE) && state == RS_CLAIMED;
        if (overlaps(&res[i], type, start, end) && !shared) {
            const char *old_owner = res[i].owner;
            counters.conflicts++;
            irq_restore(f);
            klog("[registry] conflict: %s wants %u:%x-%x held by %s reason=overlap",
                 owner, type, start, end, old_owner);
            return -EINVAL;
        }
    }
    if (slot == MAX_RES) {
        irq_restore(f);
        return -ENOMEM;
    }
    gen_t generation = gen_alloc();
    if (!generation) {
        irq_restore(f);
        return -ENOSPC;
    }
    res[slot] = (struct resource){ type, start, end, owner, generation, state, share };
    idle_proven[slot] = false;
    if (slot == nres)
        nres++;
    counters.live++;
    if (state == RS_CLAIMED)
        counters.claims++;
    irq_restore(f);
    return (int)slot;
}

int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share)
{
    return claim(type, start, end, owner, share, RS_CLAIMED);
}

int registry_discover(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share)
{
    return claim(type, start, end, owner, share, RS_DISCOVERED);
}

int registry_claim_reserved(int h, gen_t generation, const char *owner)
{
    if (!owner || !*owner)
        return -EINVAL;
    uint32_t f = irq_save();
    if (!matches(h, generation) || (res[h].state != RS_FIRMWARE && res[h].state != RS_DISCOVERED)) {
        irq_restore(f);
        return -EINVAL;
    }
    gen_t next = gen_alloc();
    if (!next) {
        irq_restore(f);
        return -ENOSPC;
    }
    res[h].owner = owner;
    res[h].generation = next;
    res[h].state = RS_CLAIMED;
    counters.claims++;
    irq_restore(f);
    return 0;
}

int registry_activate(int h, gen_t generation)
{
    uint32_t f = irq_save();
    if (!matches(h, generation) || res[h].state != RS_CLAIMED) {
        irq_restore(f);
        return -EINVAL;
    }
    res[h].state = RS_ACTIVE;
    irq_restore(f);
    return 0;
}

int registry_quarantine(int h, gen_t generation)
{
    uint32_t f = irq_save();
    if (!matches(h, generation) || res[h].state == RS_RELEASED) {
        irq_restore(f);
        return -EINVAL;
    }
    if (res[h].state != RS_QUARANTINED) {
        res[h].state = RS_QUARANTINED;
        idle_proven[h] = false;
        counters.quarantines++;
        irq_quarantine_claim(h, generation);
    }
    irq_restore(f);
    return 0;
}

int registry_quiesce(int h, gen_t generation, registry_idle_proof_fn proof)
{
    if (!proof || !(read_eflags() & 0x200))
        return -EINVAL;
    uint32_t f = irq_save();
    if (!matches(h, generation) || res[h].state != RS_ACTIVE) {
        irq_restore(f);
        return -EINVAL;
    }
    res[h].state = RS_QUIESCING;
    irq_restore(f);
    bool idle = proof(h, generation);
    f = irq_save();
    if (!matches(h, generation) || res[h].state != RS_QUIESCING) {
        irq_restore(f);
        return -EINVAL;
    }
    if (!idle) {
        registry_quarantine(h, generation);
        irq_restore(f);
        return -EFAULT;
    }
    idle_proven[h] = true;
    irq_restore(f);
    return 0;
}

int registry_release(int h, uint32_t generation)
{
    uint32_t f = irq_save();
    if (!matches(h, generation) || res[h].state != RS_QUIESCING || !idle_proven[h] ||
        irq_claim_attached(h, generation)) {
        irq_restore(f);
        return -EINVAL;
    }
    res[h].state = RS_RELEASED;
    idle_proven[h] = false;
    counters.releases++;
    counters.live--;
    irq_restore(f);
    return 0;
}

void registry_snapshot(struct registry_stats *out)
{
    uint32_t f = irq_save();
    *out = counters;
    irq_restore(f);
}

unsigned registry_count(void) { return nres; }
const struct resource *registry_get(unsigned i) { return i < nres ? &res[i] : 0; }
unsigned pci_count(void) { return npci; }
const struct pci_func *pci_get(unsigned i) { return i < npci ? &pci[i] : 0; }

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t f = irq_save();
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                ((uint32_t)fn << 8) | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    irq_restore(f);
    return v;
}

static void pci_scan(void)
{
    /* Bus 0 and firmware-configured secondary buses of PCI-PCI bridges. */
    uint8_t buses[8] = { 0 };
    unsigned nb = 1;
    for (unsigned bi = 0; bi < nb; bi++) {
        uint8_t bus = buses[bi];
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read(bus, dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;
            uint8_t ht0 = (pci_read(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            uint8_t nfn = (ht0 & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF || npci == MAX_PCI)
                    continue;
                struct pci_func *p = &pci[npci++];
                memset(p, 0, sizeof(*p));
                p->bus = bus; p->dev = dev; p->fn = fn;
                p->vendor = id & 0xFFFF;
                p->device = id >> 16;
                uint32_t cr = pci_read(bus, dev, fn, 0x08);
                p->revision = cr & 0xFF;
                p->prog_if = (cr >> 8) & 0xFF;
                p->subclass = (cr >> 16) & 0xFF;
                p->class_code = cr >> 24;
                p->header_type = (pci_read(bus, dev, fn, 0x0C) >> 16) & 0xFF;
                p->command = pci_read(bus, dev, fn, 0x04) & 0xFFFF;
                if ((p->header_type & 0x7F) == 0) {
                    for (int b = 0; b < 6; b++)
                        p->bar[b] = pci_read(bus, dev, fn, 0x10 + b * 4);
                    uint32_t ss = pci_read(bus, dev, fn, 0x2C);
                    p->sub_vendor = ss & 0xFFFF;
                    p->sub_device = ss >> 16;
                    uint32_t il = pci_read(bus, dev, fn, 0x3C);
                    p->irq_line = il & 0xFF;
                    p->irq_pin = (il >> 8) & 0xFF;
                } else if ((p->header_type & 0x7F) == 1) {
                    uint8_t sec = (pci_read(bus, dev, fn, 0x18) >> 8) & 0xFF;
                    bool seen = false;
                    for (unsigned k = 0; k < nb; k++)
                        if (buses[k] == sec)
                            seen = true;
                    if (sec && !seen && nb < ARRAY_SIZE(buses))
                        buses[nb++] = sec;
                }
            }
        }
    }
}

/* 8237 pair: mask every channel except the cascade (4) so no inherited
 * firmware programming can transfer; port 0Ah masks 0-3, D4h masks 4-7. */
static void dma_init(void)
{
    static const uint8_t ch[] = { 0, 1, 2, 3, 5, 6, 7 };
    for (unsigned i = 0; i < ARRAY_SIZE(ch); i++) {
        if (ch[i] < 4)
            outb(0x0A, 0x04 | ch[i]);
        else
            outb(0xD4, 0x04 | (ch[i] - 4));
    }
}

__attribute__((no_stack_protector)) void registry_init(void)
{
    stackprot_init();                 /* before any scheduler/task stack exists */
    dma_init();
    nres = 0;
    npci = 0;
    memset(&counters, 0, sizeof(counters));
    static const struct { uint32_t s, e; const char *o; } legacy[] = {
        { 0x20, 0x22, "pic" }, { 0xA0, 0xA2, "pic" },
        { 0x40, 0x44, "timer" }, { 0x61, 0x62, "timer/speaker" },
        { 0x70, 0x72, "rtc" }, { 0x92, 0x93, "platform-a20" },
        { 0x60, 0x61, "input" }, { 0x64, 0x65, "input" },
        { 0x00, 0x20, "dma" }, { 0x80, 0x90, "dma-page" }, { 0xC0, 0xE0, "dma" },
        { 0xCF8, 0xD00, "pci" },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(legacy); i++) {
        claim(RES_PORT, legacy[i].s, legacy[i].e, legacy[i].o, false, RS_FIRMWARE);
    }
    registry_claim(RES_IRQ, 0, 1, "timer", false);
    registry_claim(RES_IRQ, 2, 3, "pic-cascade", false);
    registry_claim(RES_DMA, 4, 5, "dma-cascade", false);
    if (g_boot.uart_base)
        registry_claim(RES_PORT, g_boot.uart_base, g_boot.uart_base + 8, "uart", false);
    if (g_boot.fb_phys && !(g_boot.flags & CBI_F_TEXT_MODE))
        claim(RES_MMIO, g_boot.fb_phys, g_boot.fb_phys + g_boot.fb_pitch * g_boot.fb_height,
              "boot-framebuffer", false, RS_FIRMWARE);
    /* PCI BIOS reported configuration mechanism #1 (AL bit 0). */
    if ((g_boot.pci_bios & 1) && ((g_boot.pci_bios >> 8) & 1))
        pci_scan();
    klog("[registry] %u reservations, %u PCI functions (read-only)", nres, npci);
}

/* Shared INTx flow follows the mask/service/EOI/unmask separation in
 * https://www.kernel.org/doc/html/v6.12/core-api/genericirq.html.
 * Existing trap.c owns EOI; a worker continuation supplies the post-EOI
 * unmask without modifying the F0 dispatcher or issuing a second EOI. */
struct irq_member {
    irq_chain_handler_t handler;
    const char *owner;
    int handle;
    gen_t generation;
};
static struct irq_chain {
    struct irq_member members[IRQ_CHAIN_MAX];
    struct irq_chain_stats stats;
    unsigned next;
    bool servicing, claimed, report;
    char budget_owner[32];
} chains[16];

static bool irq_claim_attached(int h, gen_t generation)
{
    if (res[h].type != RES_IRQ)
        return false;
    struct irq_chain *c = &chains[res[h].start];
    for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++)
        if (c->members[i].handler && c->members[i].handle == h &&
            gen_matches(c->members[i].generation, generation))
            return true;
    return false;
}

static bool same_owner(const char *a, const char *b)
{
    return a && b && strncmp(a, b, strlen(a) + 1) == 0;
}

static void chain_quarantine(unsigned irq)
{
    struct irq_chain *c = &chains[irq];
    pic_mask(irq);
    if (c->stats.quarantined)
        return;
    c->stats.quarantined = true;
    c->stats.quarantines++;
    c->report = true;
    for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++) {
        struct irq_member *m = &c->members[i];
        if (m->handler)
            registry_quarantine(m->handle, m->generation);
    }
}

static void irq_quarantine_claim(int h, gen_t generation)
{
    /* A late failed proof must also prevent an already queued rearm from
     * reopening this line. chain_quarantine publishes its flag before
     * visiting members, so cascading quarantine is idempotent. */
    if (irq_claim_attached(h, generation) && !chains[res[h].start].stats.quarantined)
        chain_quarantine(res[h].start);
}

static void chain_service(unsigned irq, bool hard_irq)
{
    struct irq_chain *c = &chains[irq];
    uint64_t start = ktime_cycles();
    c->servicing = true;
    while (c->next < IRQ_CHAIN_MAX && !c->stats.quarantined) {
        struct irq_member *m = &c->members[c->next++];
        if (!m->handler)
            continue;
        if (!registry_valid(m->handle, m->generation) ||
            (res[m->handle].state != RS_ACTIVE && res[m->handle].state != RS_QUIESCING)) {
            chain_quarantine(irq);
            break;
        }
        uint32_t before = read_eflags();
        if (m->handler(irq, m->owner))
            c->claimed = true;
        if ((read_eflags() ^ before) & 0x200)
            panic("irq: handler changed IF irq=%u owner=%s", irq, m->owner);
        if (hard_irq && ktime_elapsed_us(start, CRIT_BUDGET_US)) {
            c->stats.budget_violations++;
            uint64_t us = (ktime_cycles() - start) * 1000 / g_tsc_per_ms;
            if (us > c->stats.max_service_us)
                c->stats.max_service_us = us;
            unsigned k = 0;
            for (; k < sizeof(c->budget_owner) - 1 && m->owner[k]; k++)
                c->budget_owner[k] = m->owner[k];
            c->budget_owner[k] = 0;
            break;                      /* finish the pass after EOI, in thread context */
        }
        if (!hard_irq)
            kwork_yield();               /* each participant is independently bounded */
    }
    c->servicing = false;
}

static void chain_finish(unsigned irq)
{
    struct irq_chain *c = &chains[irq];
    if (c->claimed) {
        c->stats.handled++;
        c->stats.consecutive_unclaimed = 0;
    } else {
        c->stats.unclaimed++;
        if (++c->stats.consecutive_unclaimed == IRQ_CHAIN_STUCK_PASSES)
            chain_quarantine(irq);
    }
}

static void chain_resume(void *arg)
{
    unsigned irq = (unsigned)(uintptr_t)arg;
    struct irq_chain *c = &chains[irq];
    if (c->next < IRQ_CHAIN_MAX && !c->stats.quarantined) {
        chain_service(irq, false);
        chain_finish(irq);
    }
    if (c->budget_owner[0]) {
        klog("[irq] budget exceeded irq=%u owner=%s us=%llu pending=%u", irq,
             c->budget_owner, c->stats.max_service_us, IRQ_CHAIN_MAX - c->next);
        c->budget_owner[0] = 0;
    }
    if (c->report) {
        c->report = false;
        for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++)
            if (c->members[i].handler)
                klog("[irq] quarantined irq=%u owner=%s unclaimed=%u", irq,
                     c->members[i].owner, c->stats.consecutive_unclaimed);
    }
    uint32_t f = irq_save();
    c->stats.deferred = false;
    if (!c->stats.quarantined && c->stats.participants)
        pic_unmask(irq);
    irq_restore(f);
}

static void chain_irq(struct trap_frame *tf)
{
    unsigned irq = tf->vector - 0x20;
    struct irq_chain *c = &chains[irq];
    pic_mask(irq);
    if (c->stats.quarantined || c->stats.deferred)
        return;
    c->stats.passes++;
    c->next = 0;
    c->claimed = false;
    chain_service(irq, true);
    if (c->next == IRQ_CHAIN_MAX)
        chain_finish(irq);
    c->stats.deferred = true;
    if (!kwork_queue(chain_resume, (void *)(uintptr_t)irq)) {
        c->stats.deferred = false;
        chain_quarantine(irq);          /* no unsafe unmask when the ring is full */
    }
}

int irq_chain_add(unsigned irq, irq_chain_handler_t handler, const char *owner)
{
    /* These are the chipset-routable legacy PCI lines. No ELCR mutation. */
    if (irq >= 16 || irq == 0 || irq == 1 || irq == 2 || irq == 8 || irq == 13 ||
        !handler || !owner || !*owner || !(read_eflags() & 0x200) || !g_cpu_tsc || !g_tsc_per_ms)
        return -EINVAL;
    struct irq_chain *c = &chains[irq];
    if (c->servicing || c->stats.quarantined || c->stats.participants == IRQ_CHAIN_MAX)
        return -EINVAL;
    int h = -1;
    for (unsigned i = 0; i < nres; i++)
        if (res[i].type == RES_IRQ && res[i].start == irq && res[i].end == irq + 1 &&
            res[i].shareable && res[i].state == RS_ACTIVE && same_owner(res[i].owner, owner)) {
            h = (int)i;
            break;
        }
    if (h < 0)
        return -EINVAL;
    for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++)
        if (c->members[i].handler && c->members[i].handle == h)
            return -EINVAL;
    int err = kwork_init();
    if (err)
        return err;
    uint32_t f = irq_save();
    /* An IRQ could quarantine the chain during validation/allocation. */
    if (c->stats.quarantined || res[h].state != RS_ACTIVE) {
        irq_restore(f);
        return -EINVAL;
    }
    for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++) {
        if (c->members[i].handler)
            continue;
        c->members[i] = (struct irq_member){ handler, owner, h, res[h].generation };
        c->stats.participants++;
        break;
    }
    irq_set_handler(irq, chain_irq);
    if (!c->stats.deferred)
        pic_unmask(irq);
    irq_restore(f);
    return 0;
}

int irq_chain_remove(unsigned irq, irq_chain_handler_t handler, const char *owner)
{
    if (irq >= 16 || !handler || !owner || !(read_eflags() & 0x200))
        return -EINVAL;
    struct irq_chain *c = &chains[irq];
    /* Kernel callbacks cannot be preempted; only a callback itself can
     * observe servicing here. Refuse self-removal and its lifetime hazard. */
    if (c->servicing)
        return -EINVAL;
    uint32_t f = irq_save();
    for (unsigned i = 0; i < IRQ_CHAIN_MAX; i++) {
        struct irq_member *m = &c->members[i];
        if (m->handler != handler || !same_owner(m->owner, owner))
            continue;
        memset(m, 0, sizeof(*m));
        if (!--c->stats.participants)
            pic_mask(irq);
        irq_restore(f);
        return 0;
    }
    irq_restore(f);
    return -EINVAL;
}

bool irq_chain_snapshot(unsigned irq, struct irq_chain_stats *out)
{
    if (irq >= 16 || !out)
        return false;
    uint32_t f = irq_save();
    *out = chains[irq].stats;
    irq_restore(f);
    return true;
}
