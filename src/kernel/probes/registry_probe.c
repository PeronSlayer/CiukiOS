/* F1 registry evidence, using the production registry and chain algorithm.
 * Research: https://www.kernel.org/doc/html/v6.12/core-api/genericirq.html
 * The live chain API unconditionally unmasks its physical PIC line. To
 * exercise it without asserting hardware, compile the SAME registry.c in
 * this TU with private state and fake PIC/work/register boundaries (as in
 * kernel_sync_test.c). No second chain implementation or live-handler swap.
 * A free live PCI IRQ is exclusively reserved and kept physically masked.
 * Invoke the handler published to the fake dispatcher with a synthesized
 * vector=0x20+irq frame under irq_save; count synthetic EOI after return,
 * then drain its continuation with IF=1. Never call trap_dispatch: doing so
 * would send a physical EOI for an interrupt that never occurred.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/registry.h>
#include <ciuki/work.h>
#include <ciuki/timing.h>
#include <ciuki/init.h>
#include <ciuki/probe.h>
#include <ciuki/task.h>

static struct {
    irq_handler_t handlers[16];
    bool masked[16];
    kwork_fn work;
    void *arg;
    unsigned masks, unmasks, eois, writes, calls_a, calls_b;
    bool asserted, failed;
    char conflict[160];
} fixture_pic;
static struct ciuki_boot_info fixture_boot;
static void fixture_outb(uint16_t p, uint8_t v) { (void)p; (void)v; fixture_pic.writes++; }
static void fixture_outl(uint16_t p, uint32_t v) { (void)p; (void)v; fixture_pic.writes++; }
static uint32_t fixture_inl(uint16_t p) { (void)p; return UINT32_MAX; }
static void fixture_mask(unsigned irq) { fixture_pic.masked[irq] = true; fixture_pic.masks++; }
static void fixture_unmask(unsigned irq) { fixture_pic.masked[irq] = false; fixture_pic.unmasks++; }
static void fixture_handler(unsigned irq, irq_handler_t h) { fixture_pic.handlers[irq] = h; }
static void fixture_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(fixture_pic.conflict, sizeof(fixture_pic.conflict), fmt, ap);
    va_end(ap);
}
static int fixture_work_init(void) { return 0; }
static bool fixture_queue(kwork_fn fn, void *arg)
{
    if (fixture_pic.work) return false;
    fixture_pic.work = fn;
    fixture_pic.arg = arg;
    return true;
}
static void fixture_yield(void) { task_yield(); }
static void fixture_seed(void) { }

/* Namespace the fixture, including static symbols for host inclusion. */
#define res fixture_res
#define nres fixture_nres
#define idle_proven fixture_idle_proven
#define counters fixture_counters
#define pci fixture_pci
#define npci fixture_npci
#define irq_claim_attached fixture_irq_claim_attached
#define irq_quarantine_claim fixture_irq_quarantine_claim
#define overlaps fixture_overlaps
#define matches fixture_matches
#define claim fixture_claim
#define pci_read fixture_pci_read
#define pci_scan fixture_pci_scan
#define dma_init fixture_dma_init
#define irq_member fixture_irq_member
#define irq_chain fixture_irq_chain
#define chains fixture_chains
#define same_owner fixture_same_owner
#define chain_quarantine fixture_chain_quarantine
#define chain_service fixture_chain_service
#define chain_finish fixture_chain_finish
#define chain_resume fixture_chain_resume
#define chain_irq fixture_chain_irq
#define registry_init fixture_registry_init
#define registry_valid fixture_registry_valid
#define registry_claim fixture_registry_claim
#define registry_discover fixture_registry_discover
#define registry_claim_reserved fixture_registry_claim_reserved
#define registry_activate fixture_registry_activate
#define registry_quarantine fixture_registry_quarantine
#define registry_quiesce fixture_registry_quiesce
#define registry_release fixture_registry_release
#define registry_snapshot fixture_registry_snapshot
#define registry_count fixture_registry_count
#define registry_get fixture_registry_get
#define pci_count fixture_pci_count
#define pci_get fixture_pci_get
#define irq_chain_add fixture_irq_chain_add
#define irq_chain_remove fixture_irq_chain_remove
#define irq_chain_snapshot fixture_irq_chain_snapshot
#define outb fixture_outb
#define outl fixture_outl
#define inl fixture_inl
#define pic_mask fixture_mask
#define pic_unmask fixture_unmask
#define irq_set_handler fixture_handler
#define klog fixture_log
#define kwork_init fixture_work_init
#define kwork_queue fixture_queue
#define kwork_yield fixture_yield
#define g_boot fixture_boot
#define stackprot_init fixture_seed
#include "../core/registry.c"
#undef res
#undef nres
#undef idle_proven
#undef counters
#undef pci
#undef npci
#undef irq_claim_attached
#undef irq_quarantine_claim
#undef overlaps
#undef matches
#undef claim
#undef pci_read
#undef pci_scan
#undef dma_init
#undef irq_member
#undef irq_chain
#undef chains
#undef same_owner
#undef chain_quarantine
#undef chain_service
#undef chain_finish
#undef chain_resume
#undef chain_irq
#undef registry_init
#undef registry_valid
#undef registry_claim
#undef registry_discover
#undef registry_claim_reserved
#undef registry_activate
#undef registry_quarantine
#undef registry_quiesce
#undef registry_release
#undef registry_snapshot
#undef registry_count
#undef registry_get
#undef pci_count
#undef pci_get
#undef irq_chain_add
#undef irq_chain_remove
#undef irq_chain_snapshot
#undef outb
#undef outl
#undef inl
#undef pic_mask
#undef pic_unmask
#undef irq_set_handler
#undef klog
#undef kwork_init
#undef kwork_queue
#undef kwork_yield
#undef g_boot
#undef stackprot_init

static void *scratch_buffer;
static unsigned scratch_allocated, scratch_freed;
static bool probe_idle(int h, gen_t generation)
{
    const struct resource *r = registry_get((unsigned)h);
    if (!r || r->generation != generation || r->state != RS_QUIESCING) return false;
    if (scratch_buffer) {
        kfree(scratch_buffer);
        scratch_buffer = 0;
        scratch_freed++;
    }
    return true;
}
static bool probe_busy(int h, gen_t generation) { (void)h; (void)generation; return false; }

static bool fixture_a(unsigned irq, const char *owner)
{
    if (!fixture_pic.masked[irq] || strncmp(owner, "fixture-a", 10)) fixture_pic.failed = true;
    fixture_pic.calls_a++;
    return fixture_pic.asserted;
}
static bool fixture_b(unsigned irq, const char *owner)
{
    if (!fixture_pic.masked[irq] || strncmp(owner, "fixture-b", 10)) fixture_pic.failed = true;
    fixture_pic.calls_b++;
    return fixture_pic.asserted;
}
static void fixture_drain(void)
{
    if (!fixture_pic.work) return;
    kwork_fn fn = fixture_pic.work;
    void *arg = fixture_pic.arg;
    fixture_pic.work = 0;
    fn(arg);
}
static bool fixture_pass(unsigned irq)
{
    if (!fixture_pic.handlers[irq] || fixture_pic.masked[irq]) return false;
    struct trap_frame tf = { .vector = 0x20 + irq, .cs = SEL_KCODE };
    uint32_t f = irq_save();
    fixture_pic.handlers[irq](&tf);
    bool masked_line = fixture_pic.masked[irq];
    fixture_pic.eois++;               /* dispatcher epilogue, no physical EOI */
    irq_restore(f);
    fixture_drain();
    return masked_line && !fixture_pic.failed;
}
static bool fixture_idle(int h, gen_t generation)
{
    return fixture_registry_get((unsigned)h)->generation == generation;
}
static bool fixture_release(int h)
{
    gen_t generation = fixture_registry_get((unsigned)h)->generation;
    return !fixture_registry_quiesce(h, generation, fixture_idle) &&
           !fixture_registry_release(h, generation);
}

static int free_irq(void)
{
    static const unsigned candidates[] = { 11, 10, 9, 5 };
    for (unsigned n = 0; n < ARRAY_SIZE(candidates); n++) {
        unsigned irq = candidates[n];
        bool used = false;
        for (unsigned i = 0; i < registry_count(); i++) {
            const struct resource *r = registry_get(i);
            if (r->type == RES_IRQ && r->state != RS_RELEASED && r->start == irq) used = true;
        }
        for (unsigned i = 0; i < pci_count(); i++) {
            const struct pci_func *p = pci_get(i);
            if (p->irq_pin && p->irq_line == irq) used = true;
        }
        if (!used) return (int)irq;
    }
    return -ENOSPC;
}

static bool shared_fixture(void)
{
    int irq = free_irq();
    if (irq < 0) {
        rec_emit("registry", "DATA", "case=shared_irq error=%d reason=no_unused_line", irq);
        return false;
    }
    int live = registry_claim(RES_IRQ, (unsigned)irq, (unsigned)irq + 1, "registry-fixture", false);
    if (live < 0) return false;
    gen_t live_gen = registry_get((unsigned)live)->generation;
    bool ok = !registry_activate(live, live_gen);
    pic_mask((unsigned)irq);          /* this fixture NEVER physically unmasks */
    memset(&fixture_pic, 0, sizeof(fixture_pic));
    for (unsigned i = 0; i < 16; i++) fixture_pic.masked[i] = true;
    int a = fixture_registry_claim(RES_IRQ, (unsigned)irq, (unsigned)irq + 1, "fixture-a", true);
    int b = fixture_registry_claim(RES_IRQ, (unsigned)irq, (unsigned)irq + 1, "fixture-b", true);
    if (a < 0 || b < 0) ok = false;
    else {
        ok = !fixture_registry_activate(a, fixture_registry_get((unsigned)a)->generation) && ok;
        ok = !fixture_registry_activate(b, fixture_registry_get((unsigned)b)->generation) && ok;
        ok = !fixture_irq_chain_add((unsigned)irq, fixture_a, "fixture-a") && ok;
        ok = !fixture_irq_chain_add((unsigned)irq, fixture_b, "fixture-b") && ok;
        fixture_pic.asserted = true;
        ok = fixture_pass((unsigned)irq) && ok;
        ok = fixture_pic.calls_a == 1 && fixture_pic.calls_b == 1 && ok;
        /* Remove while its deferred rearm is pending, exercising the fence. */
        struct trap_frame tf = { .vector = 0x20 + (unsigned)irq, .cs = SEL_KCODE };
        uint32_t f = irq_save();
        if (fixture_pic.handlers[irq]) fixture_pic.handlers[irq](&tf);
        else ok = false;
        fixture_pic.eois++;
        irq_restore(f);
        ok = !fixture_irq_chain_remove((unsigned)irq, fixture_a, "fixture-a") && ok;
        ok = fixture_release(a) && ok;
        fixture_drain();
        ok = fixture_pass((unsigned)irq) && ok;
        ok = fixture_pic.calls_a == 2 && fixture_pic.calls_b == 3 && ok;
        fixture_pic.asserted = false;
        for (unsigned i = 0; i < IRQ_CHAIN_STUCK_PASSES; i++) {
            if (!fixture_pass((unsigned)irq)) { ok = false; break; }
            if (!(i % 32)) task_yield();
        }
        struct irq_chain_stats s;
        fixture_irq_chain_snapshot((unsigned)irq, &s);
        ok = s.quarantined && s.unclaimed == IRQ_CHAIN_STUCK_PASSES && s.passes == 1003 &&
             s.participants == 1 && s.quarantines == 1 && fixture_pic.masked[irq] &&
             fixture_registry_get((unsigned)b)->state == RS_QUARANTINED && !fixture_pic.writes && ok;
        rec_emit("registry", "DATA", "case=shared_irq irq=%u boundary=production_shadow frame=synthetic physical_unmasks=0 device_writes=%u callbacks_a=%u callbacks_b=%u",
                 (unsigned)irq, fixture_pic.writes, fixture_pic.calls_a, fixture_pic.calls_b);
        rec_emit("registry", "DATA", "case=stuck_irq owner=fixture-b generation=%u passes=%llu unclaimed=%llu quarantined=%u eois=%u removed_a=1 masked=%u ok=%u",
                 fixture_registry_get((unsigned)b)->generation, s.passes, s.unclaimed, s.quarantined,
                 fixture_pic.eois, fixture_pic.masked[irq], ok);
        ok = !fixture_irq_chain_remove((unsigned)irq, fixture_b, "fixture-b") && ok;
    }
    ok = !registry_quiesce(live, live_gen, probe_idle) && ok;
    ok = !registry_release(live, live_gen) && ok;
    return ok;
}

int probe_registry(void)
{
    rec_emit("registry", "BEGIN", 0);
    struct registry_stats before, after;
    registry_snapshot(&before);
    bool ok = true;
    gen_t last = 0;
    unsigned writes = 0, cycles = 0;
    for (; cycles < 100; cycles++) {
        int h = registry_claim(RES_PORT, 0xE00, 0xE10, "registry-scratch", false);
        if (h < 0) { ok = false; break; }
        gen_t generation = registry_get((unsigned)h)->generation;
        scratch_buffer = kzalloc(64);
        if (!scratch_buffer) {
            registry_quarantine(h, generation);
            ok = false;
            break;
        }
        scratch_allocated++;
        ok = generation > last && !registry_activate(h, generation) && ok;
        int conflict = registry_claim(RES_PORT, 0xE08, 0xE10, "registry-conflict", false);
        int unknown = registry_claim(RES_PORT, 0xE00, 0xE00, "registry-unknown", false);
        int stale = registry_activate(h, last);
        /* A device write is allowed ONLY for an accepted request. No port
         * I/O occurs in this scratch fixture, including successful cycles. */
        if (conflict >= 0 || unknown >= 0 || !stale) writes++;
        ok = conflict == -EINVAL && unknown == -EINVAL && stale == -EINVAL && ok;
        if (!cycles || cycles == 99)
            rec_emit("registry", "DATA", "case=cycle cycle=%u owner=registry-scratch other_owner=registry-conflict generation=%u stale_generation=%u conflict=%d unknown=%d stale=%d device_writes=%u",
                     cycles + 1, generation, last, conflict, unknown, stale, writes);
        if (!cycles) {
            rec_emit("registry", "DATA", "case=unknown_size owner=registry-unknown other_owner=registry-scratch error=%d device_writes=%u",
                     unknown, writes);
            rec_emit("registry", "DATA", "case=stale owner=registry-stale other_owner=registry-scratch generation=%u supplied=%u error=%d device_writes=%u",
                     generation, last, stale, writes);
        }
        ok = !registry_quiesce(h, generation, probe_idle) && ok;
        ok = !registry_release(h, generation) && ok;
        last = generation;
    }
    registry_snapshot(&after);
    ok = cycles == 100 && !writes && before.live == after.live &&
         after.claims - before.claims == 100 && after.releases - before.releases == 100 &&
         scratch_allocated == 100 && scratch_freed == 100 && !scratch_buffer && ok;
    rec_emit("registry", "DATA", "case=ledger cycles=%u live_before=%u live_after=%u claims=%llu releases=%llu device_writes=%u mappings=0 buffers=0 ok=%u",
             cycles, before.live, after.live, after.claims - before.claims, after.releases - before.releases, writes, ok);
    rec_emit("registry", "DATA", "case=resources buffers_allocated=%u buffers_freed=%u mappings=0 callbacks_pending=0 states=claimed,active,quiescing,released",
             scratch_allocated, scratch_freed);
    int h = registry_claim(RES_PORT, 0xE00, 0xE10, "registry-quarantine", false);
    bool held = false;
    if (h < 0) ok = false;
    else {
        gen_t generation = registry_get((unsigned)h)->generation;
        int active = registry_activate(h, generation);
        int idle = registry_quiesce(h, generation, probe_busy);
        int release = registry_release(h, generation);
        int retry = registry_claim(RES_PORT, 0xE00, 0xE10, "registry-retry", false);
        held = !active && idle == -EFAULT && release == -EINVAL && retry == -EINVAL &&
               registry_get((unsigned)h)->state == RS_QUARANTINED;
        rec_emit("registry", "DATA", "case=quarantine owner=registry-quarantine other_owner=registry-retry generation=%u idle=%d release=%d retry=%d retained=%u device_writes=0",
                 generation, idle, release, retry, held);
        ok = held && ok;
    }
    bool shared = shared_fixture();
    ok = shared && ok;
    rec_emit("registry", "DATA", "group=registry cycles=%u claim_delta=%d mapping_delta=0 buffer_delta=%d conflict_writes=%u stale_writes=%u unknown_size_writes=%u",
             cycles, (int)after.live - (int)before.live, (int)scratch_allocated - (int)scratch_freed,
             writes, writes, writes);
    rec_emit("registry", "DATA", "group=registry quarantine_retained=%u second_claim_refused=%u irq_owner_errors=%u",
             held, held, !shared);
    rec_emit("registry", "DATA", "group=metadata subcase=lifecycle owner=registry-scratch generation=%u errors=%u gate=registry timing_domain=%s",
             last, !ok, (g_boot.flags & CBI_F_SMBIOS_QEMU) ? "icount" : "hardware");
    rec_emit("registry", "END", "status=%s reason=%s", ok ? "PASS" : "FAIL", ok ? "lifecycle" : "registry_contract");
    return ok ? 0 : 1;
}
CIUKI_F1_PROBE("registry", probe_registry);
