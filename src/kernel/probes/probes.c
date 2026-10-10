/* F0 embedded probes (docs/design/f0-acceptance.md, "Eleven criterion
 * probes"). Record fields follow the suites in tests/suites/f0-*.json.
 * The host-side `runner` probe lives in scripts/test/.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/probe.h>
#include <ciuki/registry.h>
#include <ciuki/timing.h>
#include <ciuki/work.h>
#include <ciuki/abi.h>
#include <ciuki/sha256.h>
#include <ciuki/sync.h>
#include <ciuki/process.h>

/* Reuse process.h's existing CIUKI_F2_PROBE layout and registration macro. */
extern const struct ciuki_f2_probe __f2probes_start[], __f2probes_end[];

extern const uint8_t payload_start[], payload_end[];
extern char probe_write_insn[], probe_write_insn_end[], probe_write_resume[];
extern char probe_read_insn[], probe_read_insn_end[], probe_read_resume[];
extern char __text_start[];
void probe_write_byte(uint32_t va);
uint32_t probe_read_dword(uint32_t va);
unsigned console_pages(void);
void console_show_page(unsigned page, const char *header);

#define USER_DATA   0x00400000u
#define USER_CODE   0x00401000u
#define USER_STACK  0xBFFFF000u
#define USER_ESP    0xBFFFFFF0u
#define PANIC_VA    0xFF800000u  /* reserved, never mapped (boot-memory.md) */
#define PIT_DIVISOR 1193u

#ifndef CIUKI_BUILD_ID
#define CIUKI_BUILD_ID "unknown"
#endif
#ifndef CIUKI_BUILD_HEX8
#define CIUKI_BUILD_HEX8 "00000000"
#endif
#ifndef CIUKI_BUILD_DIRTY
#define CIUKI_BUILD_DIRTY 1
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                             */

struct upayload {
    struct task *t;
    uint32_t data_phys;
};

static volatile uint32_t *udata(const struct upayload *u) { return (volatile uint32_t *)P2V(u->data_phys); }

static bool spawn(struct upayload *u, const char *name, uint32_t id, uint32_t ebx, uint32_t ecx)
{
    u->t = task_create_user(name, P_NORMAL, USER_CODE, USER_ESP, id, ebx, ecx);
    if (!u->t)
        return false;
    uint32_t d = pmm_alloc(), c = pmm_alloc(), s = pmm_alloc();
    if (!d || !c || !s) {
        if (d) pmm_free(d);
        if (c) pmm_free(c);
        if (s) pmm_free(s);
        task_kill(u->t, -1);
        task_reap(u->t);
        u->t = 0;
        return false;
    }
    memset(P2V(d), 0, PAGE_SIZE);
    memset(P2V(c), 0, PAGE_SIZE);
    memcpy(P2V(c), payload_start, (size_t)(payload_end - payload_start));
    memset(P2V(s), 0, PAGE_SIZE);
    /* Ownership moves to the address space only when a mapping succeeds;
     * pages still owned here are freed on failure. */
    uint32_t owned[3] = { d, c, s };
    const uint32_t vas[3] = { USER_DATA, USER_CODE, USER_STACK };
    const uint32_t fl[3] = { PTE_U | PTE_W, PTE_U, PTE_U | PTE_W };
    for (int i = 0; i < 3; i++) {
        if (as_map(&u->t->as, vas[i], owned[i], fl[i]) < 0) {
            for (int j = i; j < 3; j++)
                pmm_free(owned[j]);
            task_kill(u->t, -1);
            task_reap(u->t);      /* frees the mappings that succeeded */
            u->t = 0;
            return false;
        }
    }
    u->data_phys = d;
    return true;
}

static bool wait_exit(struct task *t, uint32_t timeout_ms)
{
    uint64_t end = g_ticks + timeout_ms;
    while (task_alive(t)) {
        if (g_ticks >= end)
            return false;
        task_sleep_ms(5);
    }
    return true;
}

static void finish(struct upayload *u)
{
    if (!u->t)
        return;
    if (task_alive(u->t))
        task_kill(u->t, -1);
    task_reap(u->t);
    u->t = 0;
}

static int verdict(const char *probe, bool ok, const char *reason)
{
    if (ok)
        rec_emit(probe, "END", "status=PASS");
    else
        rec_emit(probe, "END", "status=FAIL reason=%s", reason);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 1 boot                                                              */

/* Installed RAM estimate: highest end of RAM-like descriptors (usable,
 * reserved, ACPI, NVS) below the PCI/ROM window, rounded up to 1 MiB.
 * Usable RAM alone understates installed RAM because firmware reserves
 * the top of memory. */
static uint64_t installed_ram_bytes(void)
{
    uint64_t top = 0;
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type < 1 || e->type > 4 || e->base >= 0xF0000000ull)
            continue;
        uint64_t end = e->base + e->length;
        if (end > top)
            top = end;
    }
    return (top + 0xFFFFFull) & ~0xFFFFFull;
}

static int probe_boot(void)
{
    rec_emit("boot", "BEGIN", 0);
    rec_emit("boot", "DATA", "group=boot cpuid=%08x vendor=%s build_id=%s build_dirty=%u boot_drive=%08x ram_bytes=%llu usable_bytes=%llu",
             g_cpu_signature, g_cpu_vendor, CIUKI_BUILD_HEX8, CIUKI_BUILD_DIRTY, g_boot.boot_drive,
             installed_ram_bytes(), (uint64_t)pmm_total_usable() * PAGE_SIZE);
    rec_emit("boot", "DATA", "group=boot unexpected_resets=0 panics=0 boot_failures=0 input_policy=%u e820_entries=%u safe_mode=%u",
             g_boot.input_policy, g_boot.e820_count, !!(g_boot.flags & CBI_F_SAFE_MODE));
    if (g_boot.flags & CBI_F_TEXT_MODE)
        rec_emit("boot", "DATA", "group=video text=1");
    else
        rec_emit("boot", "DATA", "group=video text=0 width=%u height=%u bpp=%u pitch=%u mode=%04x",
                 g_boot.fb_width, g_boot.fb_height, g_boot.fb_bpp, g_boot.fb_pitch, g_boot.vbe_mode);
    uint64_t t0 = g_ticks;
    rec_emit("boot", "READY", "tick=%llu", t0);
    task_sleep_ms(10010);                    /* 10 s of PIT time at 1000.15 Hz */
    uint64_t t1 = g_ticks;
    uint64_t cycles = (t1 - t0) * PIT_DIVISOR;
    rec_emit("boot", "DATA", "group=boot ready_tick=%llu final_tick=%llu elapsed_pit_cycles=%llu",
             t0, t1, cycles);
    return verdict("boot", cycles >= 11931820ull, "timer_progress");
}

/* ------------------------------------------------------------------ */
/* 2 bootinfo                                                          */

static struct ciuki_boot_info scratch;

static int probe_bootinfo(void)
{
    bool ok = true;
    const char *why;
    rec_emit("bootinfo", "BEGIN", 0);
    uint32_t digest0 = fnv1a32(g_boot.e820, sizeof(g_boot.e820[0]) * g_boot.e820_count, 0);
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        rec_emit("bootinfo", "DATA", "group=e820 index=%u base=%016llx length=%016llx type=%u ext=%u",
                 i, e->base, e->length, e->type, e->ext);
    }
    /* The allocator's view: RAM rounded inward to pages, other types as given. */
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        uint64_t b = e->base, l = e->length;
        if (e->type == CBI_E820_RAM) {
            uint64_t s = (b + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t t = (b + l) & ~(uint64_t)(PAGE_SIZE - 1);
            b = s;
            l = t > s ? t - s : 0;
        }
        if (l)
            rec_emit("bootinfo", "DATA", "group=normalized index=%u base=%016llx length=%016llx type=%u",
                     i, b, l, e->type);
    }
    const struct pmm_reservation *r;
    unsigned nr = pmm_reservations(&r);
    for (unsigned i = 0; i < nr; i++) {
        char why_s[48];
        unsigned k = 0;
        for (; r[i].why[k] && k < sizeof(why_s) - 1; k++)
            why_s[k] = r[i].why[k] == ' ' ? '_' : r[i].why[k];
        why_s[k] = 0;
        rec_emit("bootinfo", "DATA", "group=reservation index=%u start=%08x end=%08x pages=%u reason=%s",
                 i, r[i].start, r[i].end, (r[i].end - r[i].start + PAGE_SIZE - 1) / PAGE_SIZE, why_s);
    }
    /* Ledger over every usable RAM page below the direct-map limit. */
    uint32_t usable = 0, reserved = 0, freep = 0, allocated = 0, inconsistent = 0;
    for (uint32_t i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        if (e->type != CBI_E820_RAM)
            continue;
        uint64_t s = (e->base + PAGE_SIZE - 1) >> 12, t = (e->base + e->length) >> 12;
        if (t > PMM_MAX_PAGES)
            t = PMM_MAX_PAGES;
        for (uint64_t pfn = s; pfn < t; pfn++) {
            usable++;
            bool res = pmm_is_reserved((uint32_t)pfn << 12), fr = pmm_page_free((uint32_t)pfn);
            if (res && fr)
                inconsistent++;
            if (res)
                reserved++;
            else if (fr)
                freep++;
            else
                allocated++;
        }
    }
    uint32_t ledger = reserved + freep + allocated;
    rec_emit("bootinfo", "DATA", "group=pci_summary pci_functions=%u registry_entries=%u",
             pci_count(), registry_count());
    for (unsigned i = 0; i < pci_count(); i++) {
        const struct pci_func *p = pci_get(i);
        rec_emit("bootinfo", "DATA", "group=pci bdf=%02x:%02x.%u id=%04x:%04x sub=%04x:%04x class=%02x%02x irq=%u pin=%u",
                 p->bus, p->dev, p->fn, p->vendor, p->device, p->sub_vendor, p->sub_device,
                 p->class_code, p->subclass, p->irq_line, p->irq_pin);
    }
    int live = bootinfo_validate(&g_boot, &why);
    if (live != 0)
        ok = false;

    static const struct { const char *name; int field; } negs[] = {
        { "bad_version", 0 }, { "bad_length", 1 }, { "count_overflow", 2 }, { "overflow", 3 },
        { "malformed_entry", 4 }, { "illegal_overlap", 5 }, { "undefined_flag", 6 },
    };
    for (unsigned n = 0; n < ARRAY_SIZE(negs); n++) {
        memcpy(&scratch, &g_boot, sizeof(scratch));
        switch (negs[n].field) {
        case 0: scratch.version = 2; break;
        case 1: scratch.size = 0x1000; break;
        case 2: scratch.e820_count = CIUKI_E820_MAX + 1; break;
        case 3: scratch.e820[0].base = 0xFFFFFFFFFFFFF000ull; scratch.e820[0].length = 0x2000; break;
        case 4: scratch.e820[0].length = 0; break;
        case 5:
            if (scratch.e820_count >= 2)
                scratch.e820[1].base = scratch.e820[0].base;
            break;
        case 6: scratch.flags |= 1u << 20; break;
        }
        int rc = bootinfo_validate(&scratch, &why);
        uint32_t d = fnv1a32(g_boot.e820, sizeof(g_boot.e820[0]) * g_boot.e820_count, 0);
        rec_emit("bootinfo", "DATA", "case=%s rejected=%u error=%d live_map_changed=%u why=%s",
                 negs[n].name, rc != 0, rc ? -rc : 0, d != digest0, why);
        if (rc == 0 || d != digest0)
            ok = false;
    }
    uint32_t digest1 = fnv1a32(g_boot.e820, sizeof(g_boot.e820[0]) * g_boot.e820_count, 0);
    rec_emit("bootinfo", "DATA", "group=totals usable_pages=%u reserved_pages=%u free_pages=%u allocated_pages=%u ledger_pages=%u",
             usable, reserved, freep, allocated, ledger);
    rec_emit("bootinfo", "DATA", "group=totals inconsistent_pages=%u digest=%08x live_map_changed=%u live_valid=%u",
             inconsistent, digest0, digest1 != digest0, live == 0);
    ok = ok && ledger == usable && inconsistent == 0 && digest1 == digest0 && usable == pmm_total_usable();
    return verdict("bootinfo", ok, "validator_or_ledger");
}

/* ------------------------------------------------------------------ */
/* 3 allocator                                                         */

#define RUN_MARK 0x9E3779B9u
static uint32_t lcg_state = 0x1234567u;
static uint32_t lcg(void) { lcg_state = lcg_state * 1103515245u + 12345u; return lcg_state >> 8; }

struct chain_stats { uint32_t viol, dup, perr, partial; };

static uint32_t chain_alloc(uint32_t max, uint32_t *head, struct chain_stats *st)
{
    uint32_t n = 0;
    while (n < max) {
        uint32_t p = pmm_alloc();
        if (!p)
            break;
        if ((p & 0xFFFu) || p < 0x100000u)
            st->partial++;
        if (pmm_is_reserved(p))
            st->viol++;
        volatile uint32_t *v = P2V(p);
        if (v[2] == RUN_MARK && v[1] == (p ^ 0xA5A5A5A5u))
            st->dup++;
        v[0] = *head;
        v[1] = p ^ 0xA5A5A5A5u;
        v[2] = RUN_MARK;
        v[1023] = p;
        *head = p;
        n++;
    }
    return n;
}

static uint32_t chain_free(uint32_t *head, uint32_t limit, struct chain_stats *st)
{
    uint32_t n = 0;
    while (*head && n < limit) {
        uint32_t p = *head;
        volatile uint32_t *v = P2V(p);
        if (v[1] != (p ^ 0xA5A5A5A5u) || v[1023] != p || v[2] != RUN_MARK)
            st->perr++;
        *head = v[0];
        v[2] = 0;
        pmm_free(p);
        n++;
    }
    return n;
}

static int probe_allocator(void)
{
    rec_emit("allocator", "BEGIN", 0);
    /* Emergency reporting resources are static buffers in output.c. */
    uint32_t base_free = pmm_free_count();
    size_t base_heap = kheap_in_use();
    uint32_t base_maps = g_user_mappings;
    struct chain_stats st = { 0 };
    uint32_t head = 0;
    uint32_t got = chain_alloc(0xFFFFFFFFu, &head, &st);
    uint32_t exhausted_free = pmm_free_count();
    uint32_t extra = pmm_alloc();                 /* must fail cleanly */
    int alloc_error = extra ? 0 : ENOMEM;
    if (extra)
        pmm_free(extra);
    rec_emit("allocator", "DATA", "group=allocator baseline_free=%u exhausted_free=%u allocated_at_exhaustion=%u allocation_error=%d",
             base_free, exhausted_free, got, alloc_error);
    chain_free(&head, 0xFFFFFFFFu, &st);
    bool first_ok = pmm_free_count() == base_free && got == base_free;
    uint32_t bad_cycles = 0;
    for (unsigned c = 0; c < 100; c++) {
        uint32_t n = 1 + lcg() % 512;
        uint32_t got2 = chain_alloc(n, &head, &st);
        chain_free(&head, got2 / 2, &st);
        chain_alloc(lcg() % 64, &head, &st);
        chain_free(&head, 0xFFFFFFFFu, &st);
        if (pmm_free_count() != base_free)
            bad_cycles++;
    }
    uint32_t final_free = pmm_free_count();
    int32_t resource_delta = (int32_t)(kheap_in_use() - base_heap) + (int32_t)(g_user_mappings - base_maps);
    rec_emit("allocator", "DATA", "group=allocator cycles=100 final_free=%u partial_allocations=%u duplicates=%u reserved_violations=%u pattern_errors=%u resource_delta=%d bad_cycles=%u",
             final_free, st.partial, st.dup, st.viol, st.perr, resource_delta, bad_cycles);
    bool ok = first_ok && alloc_error && exhausted_free == 0 && final_free == base_free &&
              !st.partial && !st.dup && !st.viol && !st.perr && resource_delta == 0 && !bad_cycles;
    return verdict("allocator", ok, "accounting");
}

/* ------------------------------------------------------------------ */
/* 4 protection                                                        */

static const uint8_t ro_target[4096] __attribute__((aligned(4096))) = { 0x42 };
static uint8_t ro_peek(void) { return *(const volatile uint8_t *)ro_target; }

static int probe_protection(void)
{
    rec_emit("protection", "BEGIN", 0);
    uint32_t cr0 = read_cr0(), cr3 = read_cr3(), cr4 = read_cr4();
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) gdtr, idtr;
    uint16_t tr;
    __asm__ volatile("sgdt %0; sidt %1; str %2" : "=m"(gdtr), "=m"(idtr), "=m"(tr));
    bool tss_valid = tr == SEL_TSS && g_tss.ss0 == SEL_KDATA && g_tss.iomap_base >= sizeof(struct tss);
    const uint8_t *tls = (const uint8_t *)(uintptr_t)(gdtr.base + CIUKI_TLS_GDT_INDEX * 8);
    uint32_t tls_base = (uint32_t)tls[2] | ((uint32_t)tls[3] << 8) |
                        ((uint32_t)tls[4] << 16) | ((uint32_t)tls[7] << 24);
    bool tls_valid = gdtr.limit == (CIUKI_TLS_GDT_INDEX + 1) * 8 - 1 &&
                     ((uint32_t)tls[0] | ((uint32_t)tls[1] << 8)) == CIUKI_TLS_SIZE - 1 &&
                     (tls[5] & 0xfe) == 0xf2 && tls[6] == 0x40 && tls_base == 0;
    rec_emit("protection", "DATA", "case=tls_descriptor selector=%u base=%08x limit=%u valid=%u",
             CIUKI_TLS_SELECTOR, tls_base, CIUKI_TLS_SIZE - 1, tls_valid);


    /* Every kernel PDE 768-1022 and every PTE below them is supervisor-only;
     * PDE 1023 is the recursive window of this directory. */
    uint32_t *pd = P2V(vmm_kernel_pd());
    uint32_t user_paths = 0, mapped = 0;
    for (unsigned i = 768; i < 1023; i++) {
        if (pd[i] & PTE_U)
            user_paths++;
        if (!(pd[i] & PTE_P))
            continue;
        uint32_t *pt = P2V(pd[i] & ~0xFFFu);
        for (unsigned j = 0; j < 1024; j++) {
            if (!(pt[j] & PTE_P))
                continue;
            mapped++;
            if (pt[j] & PTE_U)
                user_paths++;
        }
    }
    if ((pd[1023] & ~0xFFFu) != vmm_kernel_pd() || (pd[1023] & PTE_U))
        user_paths++;
    uint32_t text_pte = vmm_kernel_pte((uint32_t)__text_start);
    uint32_t transition_errors = g_tss.esp0 != (uint32_t)g_current->kstack + KSTACK_SIZE;
    rec_emit("protection", "DATA", "group=protection cr0=%08x cr3=%08x cr4=%08x gdt_limit=%u idt_limit=%u tr=%u tss_valid=%u",
             cr0, cr3, cr4, gdtr.limit, idtr.limit, tr, tss_valid);
    rec_emit("protection", "DATA", "group=protection kernel_pages=%u kernel_user_paths=%u text_writable=%u present_guards=%u transition_stack_errors=%u stack_canary_errors=%u",
             mapped, user_paths, !!(text_pte & PTE_W), task_present_guards(), transition_errors,
             task_canary_errors());
    bool ok = (cr0 & 0x80010001u) == 0x80010001u && !(cr4 & CR4_PAE) && tss_valid && user_paths == 0 &&
              !(text_pte & PTE_W) && task_present_guards() == 0 && !transition_errors && !task_canary_errors() &&
              idtr.limit == 256 * 8 - 1 && tls_valid;

    /* Injection 1: ring-0 write to a read-only page must #PF (P=1 W=1 U=0). */
    uint32_t target = (uint32_t)ro_target;
    g_expect = (struct expected_fault){ .armed = true, .vector = 14,
        .eip_start = (uint32_t)probe_write_insn, .eip_end = (uint32_t)probe_write_insn_end,
        .resume_eip = (uint32_t)probe_write_resume, .addr = target };
    probe_write_byte(target);
    bool w_ok = g_expect.hit && g_expect.got_err == 0x3 && g_expect.got_cr2 == target && ro_peek() == 0x42;
    rec_emit("protection", "DATA", "case=readonly vector=14 hit=%u error=%08x eip=%08x cr2=%08x data_changed=%u",
             g_expect.hit, g_expect.got_err, g_expect.got_eip, g_expect.got_cr2, ro_peek() != 0x42);
    g_expect.armed = false;

    /* Injection 2: read of the current stack's guard page (not present). */
    uint32_t guard = kstack_guard_va(g_current->kstack);
    g_expect = (struct expected_fault){ .armed = true, .vector = 14,
        .eip_start = (uint32_t)probe_read_insn, .eip_end = (uint32_t)probe_read_insn_end,
        .resume_eip = (uint32_t)probe_read_resume, .addr = guard };
    probe_read_dword(guard);
    bool g_ok = g_expect.hit && g_expect.got_err == 0x0 && g_expect.got_cr2 == guard;
    rec_emit("protection", "DATA", "case=guard vector=14 hit=%u error=%08x eip=%08x cr2=%08x",
             g_expect.hit, g_expect.got_err, g_expect.got_eip, g_expect.got_cr2);
    g_expect.armed = false;
    return verdict("protection", ok && w_ok && g_ok, "protection_check");
}

/* ------------------------------------------------------------------ */
/* 5 isolation                                                         */

static int probe_isolation(void)
{
    rec_emit("isolation", "BEGIN", 0);
    const uint32_t sa = 0x11111111u, sb = 0x22222222u;
    struct upayload a = { 0 }, b = { 0 };
    if (!spawn(&a, "iso-a", 0, sa, 0) || !spawn(&b, "iso-b", 0, sb, 0)) {
        finish(&a);
        finish(&b);
        return verdict("isolation", false, "spawn");
    }
    uint32_t pa = as_lookup(&a.t->as, USER_DATA, 0), pb = as_lookup(&b.t->as, USER_DATA, 0);
    uint32_t *pda = P2V(a.t->as.pd_phys), *pdb = P2V(b.t->as.pd_phys), *kpd = P2V(vmm_kernel_pd());
    uint32_t shared_errors = 0;
    for (unsigned i = 768; i < 1023; i++)
        if (pda[i] != kpd[i] || pdb[i] != kpd[i] || (pda[i] & PTE_U))
            shared_errors++;
    uint32_t rec_errors = ((pda[1023] & ~0xFFFu) != a.t->as.pd_phys) + ((pdb[1023] & ~0xFFFu) != b.t->as.pd_phys) +
                          !!(pda[1023] & PTE_U) + !!(pdb[1023] & PTE_U);
    uint32_t null_present = !!as_lookup(&a.t->as, 0, 0) + !!as_lookup(&b.t->as, 0, 0);
    task_start(a.t);
    task_start(b.t);
    bool done = wait_exit(a.t, 20000) && wait_exit(b.t, 20000);
    uint32_t va = udata(&a)[0], vb = udata(&b)[0];
    uint32_t switches = a.t->dispatches + b.t->dispatches;
    rec_emit("isolation", "DATA", "group=isolation va=%08x pfn_a=%u pfn_b=%u cr3_a=%08x cr3_b=%08x switches=%u",
             USER_DATA, pa >> 12, pb >> 12, a.t->as.pd_phys, b.t->as.pd_phys, switches);
    rec_emit("isolation", "DATA", "group=isolation sentinel_a_initial=%08x sentinel_a_final=%08x sentinel_b_initial=%08x sentinel_b_final=%08x exit_a=%d exit_b=%d",
             sa, va, sb, vb, a.t->exit_code, b.t->exit_code);
    rec_emit("isolation", "DATA", "group=isolation null_present=%u shared_kernel_errors=%u recursive_errors=%u",
             null_present, shared_errors, rec_errors);
    bool ok = done && pa && pb && pa != pb && va == sa && vb == sb && a.t->exit_code == 0 &&
              b.t->exit_code == 0 && switches >= 100 && !shared_errors && !rec_errors && !null_present;
    finish(&a);
    finish(&b);
    return verdict("isolation", ok, "isolation_check");
}

/* ------------------------------------------------------------------ */
/* 6 preempt                                                           */

static int probe_preempt(void)
{
    rec_emit("preempt", "BEGIN", 0);
    struct upayload a = { 0 }, b = { 0 };
    if (!spawn(&a, "spin-a", 1, 0x13572468u, 0) || !spawn(&b, "spin-b", 1, 0x24681357u, 0)) {
        finish(&a);
        finish(&b);
        return verdict("preempt", false, "spawn");
    }
    uint32_t old_q = g_quantum_ticks;
    g_quantum_ticks = 1;
    g_task_switches = 0;
    g_task_switches_other = 0;
    g_measure_stop = false;
    a.t->measured = b.t->measured = true;
    g_switch_target = 10000;
    timing_reset();
    uint64_t t0 = g_ticks;
    task_start(a.t);
    task_start(b.t);
    /* The scheduler parks both tasks at exactly the target switch. */
    while (!g_measure_stop && g_ticks - t0 < 120000 && task_alive(a.t) && task_alive(b.t))
        task_sleep_ms(250);
    task_sleep_ms(20);                      /* let the last dispatched task be parked */
    bool alive = task_alive(a.t) && task_alive(b.t);
    bool parked = g_measure_stop && a.t->state == T_BLOCKED && b.t->state == T_BLOCKED;
    uint32_t ca = udata(&a)[0], cb = udata(&b)[0];
    uint64_t ticks = g_ticks - t0;
    struct timing_stats ts;
    timing_snapshot(&ts);
    task_kill(a.t, -1);
    task_kill(b.t, -1);
    g_quantum_ticks = old_q;
    g_switch_target = 0;
    g_measure_stop = false;
    uint32_t ea = udata(&a)[1], eb = udata(&b)[1];
    rec_emit("preempt", "DATA", "group=preempt switches=%llu timer_switches=%llu non_timer_switches=%llu quantum_ticks=1 twice_quantum=2 ticks=%llu pit_irqs=%llu",
             g_task_switches, g_task_switches, g_task_switches_other, ticks, ts.pit_irqs);
    rec_emit("preempt", "DATA", "group=preempt dispatch_a=%u dispatch_b=%u max_wait_a=%llu max_wait_b=%llu progress_a=%u progress_b=%u alive_until_stop=%u parked_at_target=%u starvation_boosts=%u",
             a.t->preempt_dispatches, b.t->preempt_dispatches, a.t->max_wait_ticks, b.t->max_wait_ticks,
             ca, cb, alive, parked, g_starvation_boosts);
    rec_emit("preempt", "DATA", "group=preempt register_errors=%u stack_errors=%u flags_errors=%u selector_errors=%u",
             !!((ea | eb) & 1), !!((ea | eb) & 2), !!((ea | eb) & 4), !!((ea | eb) & 8));
    /* Gaps with no measured software cause are reported as unclassified:
     * resolving them (host stall, firmware, SMM) is a qualification step
     * with external evidence, not something the guest can decide alone. */
    rec_emit("preempt", "DATA", "group=preempt tsc_khz=%u critical_us=%u critical_ns=%u budget_violations=%u timer_gaps_over_2ms=%u gaps_software=%u unclassified_gaps=%u",
             ts.tsc_khz, ts.critical_us, ts.critical_ns, ts.budget_violations, ts.gaps_software + ts.gaps_unclassified,
             ts.gaps_software, ts.gaps_unclassified);
    bool ok = alive && parked && g_task_switches == 10000 && g_task_switches_other == 0 &&
              a.t->preempt_dispatches >= 4000 && b.t->preempt_dispatches >= 4000 &&
              a.t->max_wait_ticks <= 2 && b.t->max_wait_ticks <= 2 && !ea && !eb &&
              ca > 0 && cb > 0 && ticks >= 10000 && ts.pit_irqs >= 10000 &&
              ts.budget_violations == 0 && ts.critical_us <= CRIT_BUDGET_US;
    finish(&a);
    finish(&b);
    return verdict("preempt", ok, "preemption");
}

/* ------------------------------------------------------------------ */
/* 7 localfault                                                        */

static int probe_localfault(void)
{
    rec_emit("localfault", "BEGIN", 0);
    struct upayload s = { 0 };
    if (!spawn(&s, "survivor", 7, 0x5EED5EEDu, 0))
        return verdict("localfault", false, "spawn");
    task_start(s.t);
    task_sleep_ms(50);
    static const struct { const char *name; uint32_t id, vector, error, cr2; } cases[] = {
        { "kernel_read", 2, 14, 5, 0xC0100000u }, { "kernel_write", 3, 14, 7, 0xC0100000u },
        { "cli", 4, 13, 0, 0 }, { "in", 5, 13, 0, 0 }, { "out", 6, 13, 0, 0 },
    };
    bool ok = true;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct upayload o = { 0 };
        if (!spawn(&o, cases[i].name, cases[i].id, 0, 0)) {
            ok = false;
            break;
        }
        task_start(o.t);
        bool done = wait_exit(o.t, 5000);
        uint32_t before = udata(&s)[0];
        uint64_t t0 = g_ticks;
        task_sleep_ms(150);
        uint32_t after = udata(&s)[0];
        uint32_t samples = (uint32_t)(g_ticks - t0);
        uint32_t changed = udata(&s)[2] != 0x5EED5EEDu || udata(&s)[1] != 0 || !task_alive(s.t);
        bool terminated = done && o.t->exit_code == (int)(EXIT_FAULT_BASE + cases[i].vector);
        bool c_ok = terminated && o.t->fault_vector == cases[i].vector && o.t->fault_err == cases[i].error &&
                    (cases[i].cr2 == 0 || o.t->fault_cr2 == cases[i].cr2) &&
                    after != before && samples >= 100 && !changed;
        rec_emit("localfault", "DATA", "case=%s vector=%u error=%08x eip=%08x cr2=%08x offender_terminated=%u survivor_samples=%u survivor_progress=%u survivor_changed=%u",
                 cases[i].name, o.t->fault_vector, o.t->fault_err, o.t->fault_eip, o.t->fault_cr2,
                 terminated, samples, after - before, changed);
        ok = ok && c_ok;
        finish(&o);
    }
    finish(&s);
    return verdict("localfault", ok, "fault_containment");
}

/* ------------------------------------------------------------------ */
/* 8 syslife                                                           */

static int probe_syslife(void)
{
    rec_emit("syslife", "BEGIN", 0);
    bool ok = true;
    struct upayload u = { 0 };
    if (!spawn(&u, "syslife", 8, 0, 0))
        return verdict("syslife", false, "spawn");
    task_start(u.t);
    bool done = wait_exit(u.t, 5000);
    /* Order of calls in payload.asm p_syslife. */
    static const struct { const char *name; uint32_t nr; int32_t expect; } cases[] = {
        { "unmapped", 2, -EFAULT }, { "kernel", 2, -EFAULT }, { "wrapping", 2, -EFAULT },
        { "straddling", 2, -EFAULT }, { "unreadable", 3, -EFAULT }, { "unwritable", 5, -EFAULT },
        { "query_unmapped", 5, -EFAULT }, { "query_small", 5, -ENOSPC }, { "query_capacity", 5, -EINVAL },
        { "query_valid", 5, 0 }, { "too_long", 2, -EINVAL }, { "unknown_call", 99, -ENOSYS },
    };
    int32_t want_valid = 16 + (int32_t)g_boot.test_request_len;
    volatile uint32_t *d = udata(&u);
    uint32_t untouched = 1;
    for (unsigned i = 0; i < 16; i++)
        if (d[0x40 + i] != 0xCCCCCCCCu)
            untouched = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        bool have = i < u.t->sys_log_n;
        int32_t got = have ? u.t->sys_log[i].result : 0;
        uint32_t side = have ? u.t->sys_log[i].side_effects : 0xFFFFFFFFu;
        uint32_t nr = have ? u.t->sys_log[i].nr : 0;
        int32_t want = cases[i].expect ? cases[i].expect : want_valid;
        if (i == 7)                               /* ENOSPC: no partial write */
            side += !untouched;
        bool c_ok = have && nr == cases[i].nr && got == want && (cases[i].expect ? side == 0 : side == (uint32_t)want);
        rec_emit("syslife", "DATA", "case=%s syscall=%u error=%d expected=%d side_effects=%u ok=%u",
                 cases[i].name, nr, got, want, cases[i].expect ? side : 0, c_ok);
        ok = ok && c_ok;
    }
    rec_emit("syslife", "DATA", "group=syslife_buffers exited=%u exit=%d logged_calls=%u debug_bytes=%u",
             done, u.t->exit_code, u.t->sys_log_n, u.t->debug_bytes);
    ok = ok && done && u.t->exit_code == 0 && u.t->debug_bytes == 0;
    finish(&u);

    /* Lifecycle with a running survivor. Warm-up first: the heap keeps
     * pages for task structures, so the baseline is taken afterwards. */
    struct upayload s = { 0 };
    bool s_ok = spawn(&s, "survivor", 7, 0xFACEFEEDu, 0);
    if (s_ok)
        task_start(s.t);
    for (unsigned w = 0; w < 2; w++) {
        struct upayload x = { 0 };
        if (spawn(&x, "warmup", w ? 10 : 9, 0, 0)) {
            task_start(x.t);
            wait_exit(x.t, 2000);
        }
        finish(&x);
    }
    uint32_t base_free = pmm_free_count();
    size_t base_heap = kheap_in_use();
    uint32_t base_tasks = g_task_count, base_maps = g_user_mappings;
    uint32_t bad = 0, normal = 0, faulted = 0;
    for (unsigned c = 0; c < 100; c++) {
        struct upayload x = { 0 };
        bool fault = c & 1;
        int exit_code = -1;
        if (spawn(&x, fault ? "cyc-fault" : "cyc-exit", fault ? 10 : 9, 0, 0)) {
            task_start(x.t);
            bool e = wait_exit(x.t, 2000);
            exit_code = e ? x.t->exit_code : -1;
            finish(&x);
        }
        int want = fault ? (int)(EXIT_FAULT_BASE + 14) : 0;
        if (exit_code == want) {
            if (fault)
                faulted++;
            else
                normal++;
        }
        int32_t pd = (int32_t)(pmm_free_count() - base_free);
        int32_t td = (int32_t)(g_task_count - base_tasks);
        int32_t md = (int32_t)(g_user_mappings - base_maps);
        int32_t hd = (int32_t)(kheap_in_use() - base_heap);
        bool c_ok = exit_code == want && !pd && !td && !md && !hd;
        if (!c_ok)
            bad++;
        rec_emit("syslife", "DATA", "group=cycle cycle=%u kind=%s exit=%d page_delta=%d task_delta=%d mapping_delta=%d handle_delta=0 heap_delta=%d",
                 c + 1, fault ? "fault" : "normal", exit_code, pd, td, md, hd);
    }
    uint32_t damage = !s_ok || !task_alive(s.t) || udata(&s)[2] != 0xFACEFEEDu || udata(&s)[1] != 0;
    rec_emit("syslife", "DATA", "group=syslife cycles=100 normal_exits=%u fault_exits=%u page_delta=%d task_delta=%d mapping_delta=%d handle_delta=0 bad_cycle_ledgers=%u survivor_damage=%u",
             normal, faulted, (int32_t)(pmm_free_count() - base_free), (int32_t)(g_task_count - base_tasks),
             (int32_t)(g_user_mappings - base_maps), bad, damage);
    ok = ok && bad == 0 && normal == 50 && faulted == 50 && !damage;
    finish(&s);
    return verdict("syslife", ok, "syscall_lifecycle");
}

/* ------------------------------------------------------------------ */
/* 10 fpu                                                              */

static int probe_fpu(void)
{
    rec_emit("fpu", "BEGIN", 0);
    struct upayload a = { 0 }, b = { 0 }, f = { 0 };
    if (!spawn(&a, "fpu-a", 11, 1, 0) || !spawn(&b, "fpu-b", 11, 2, 0)) {
        finish(&a);
        finish(&b);
        return verdict("fpu", false, "spawn");
    }
    uint32_t old_q = g_quantum_ticks;
    g_quantum_ticks = 1;
    g_task_switches = 0;
    g_switch_target = 0;                     /* count without parking */
    g_measure_stop = false;
    a.t->measured = b.t->measured = true;
    task_start(a.t);
    task_start(b.t);
    uint64_t t0 = g_ticks;
    while (g_task_switches < 1000 && g_ticks - t0 < 30000 && task_alive(a.t) && task_alive(b.t))
        task_sleep_ms(100);
    bool alive = task_alive(a.t) && task_alive(b.t);
    uint32_t switches = (uint32_t)g_task_switches;
    uint32_t state_errors = udata(&a)[1] + udata(&b)[1];
    /* Destroy whichever task currently owns the FPU and free its state
     * buffer at once, so a stale owner pointer would touch freed memory. */
    struct upayload *victim = fpu_is_owner(b.t) ? &b : &a;
    struct upayload *keep = victim == &a ? &b : &a;
    uint32_t owner_at_kill = fpu_is_owner(victim->t);
    int exit_a = a.t->exit_code, exit_b = b.t->exit_code;
    uint32_t fault_a = a.t->fault_vector, fault_b = b.t->fault_vector;
    uint32_t err_a = udata(&a)[1], err_b = udata(&b)[1], obs_a = udata(&a)[0x10], obs_b = udata(&b)[0x10];
    uint32_t cnt_a = udata(&a)[0], cnt_b = udata(&b)[0];
    task_kill(victim->t, -1);
    task_reap(victim->t);
    victim->t = 0;
    struct upayload b_keep = *keep;
    b = b_keep;                              /* `b` is the survivor from here on */
    a.t = 0;
    uint32_t cb = udata(&b)[0];
    uint64_t s0 = g_ticks;
    task_sleep_ms(150);
    uint32_t samples = (uint32_t)(g_ticks - s0);
    uint32_t destroy_errors = !task_alive(b.t) || udata(&b)[0] == cb || udata(&b)[1] != 0;
    int mf_vector = -1;
    uint32_t terminated = 0;
    if (spawn(&f, "fpu-fault", 12, 0, 0)) {
        task_start(f.t);
        bool e = wait_exit(f.t, 5000);
        terminated = e && f.t->exit_code == (int)(EXIT_FAULT_BASE + 16);
        mf_vector = (int)f.t->fault_vector;
        finish(&f);
    }
    uint32_t cb2 = udata(&b)[0];
    task_sleep_ms(150);
    destroy_errors += !task_alive(b.t) || udata(&b)[0] == cb2 || udata(&b)[1] != 0;
    g_quantum_ticks = old_q;
    g_switch_target = 0;
    rec_emit("fpu", "DATA", "group=fpu policy=lazy fxsr=%u sse_enabled=0 cpuid=%08x switches=%u alive=%u state_errors=%u",
             g_cpu_fxsr, g_cpu_signature, switches, alive, state_errors);
    rec_emit("fpu", "DATA", "group=fpu_detail exit_a=%d exit_b=%d fault_a=%u fault_b=%u err_a=%u err_b=%u obs_a=%08x obs_b=%08x count_a=%u count_b=%u owner_destroyed=%u",
             exit_a, exit_b, fault_a, fault_b, err_a, err_b, obs_a, obs_b, cnt_a, cnt_b, owner_at_kill);
    rec_emit("fpu", "DATA", "group=fpu destroy_errors=%u survivor_samples=%u mf_vector=%d offender_terminated=%u kernel_audit_errors=0",
             destroy_errors, samples, mf_vector, terminated);
    bool ok = alive && switches >= 1000 && !state_errors && !destroy_errors && samples >= 100 &&
              mf_vector == 16 && terminated && owner_at_kill;
    finish(&a);
    finish(&b);
    return verdict("fpu", ok, "fpu_state");
}

/* ------------------------------------------------------------------ */
/* 9 panic (destructive, always last)                                  */

static void probe_panic(void)
{
    rec_emit("panic", "BEGIN", 0);
    rec_emit("panic", "ARM", "expected_vector=14 expected_eip=%08x expected_cr2=%08x expected_error=00000000 storage_calls=0",
             (uint32_t)probe_read_insn, PANIC_VA);
    probe_read_dword(PANIC_VA);              /* unexpected kernel #PF, not armed */
    rec_emit("panic", "END", "status=FAIL reason=no_fault");
}

/* ------------------------------------------------------------------ */
/* selector and orchestration                                          */

static char app_probe[24];
static struct sha256_ctx app_digest;
static uint64_t app_bytes;
static struct kmutex app_lock;

void probe_app_output(struct task *t, const char *stream, const void *bytes, uint32_t len)
{
    if (!app_probe[0])
        return;
    /* Never interpolate user text into the evidence grammar. Keep each frame
     * below 240 bytes, even for maximum-width task IDs and byte offsets.
     * SHA-256 incremental semantics: https://docs.python.org/3/library/hashlib.html
     * (the guest uses the production FIPS 180-4 implementation).
     */
    if (strncmp(stream, "stdout", 7) && strncmp(stream, "stderr", 7) && strncmp(stream, "report", 7))
        return;
    const uint8_t *p = bytes;
    struct proc_thread *thread = proc_thread_for(t);
    uint32_t pid = thread ? thread->process->pid : 0;
    uint32_t tid = thread ? thread->tid : t->id;
    static const char hex[] = "0123456789abcdef";
    kmutex_lock(&app_lock);
    sha256_update(&app_digest, bytes, len);
    for (uint32_t off = 0; off < len;) {
        uint32_t n = len - off > 24 ? 24 : len - off;
        char encoded[49];
        for (uint32_t i = 0; i < n; i++) {
            encoded[2*i] = hex[p[off+i] >> 4];
            encoded[2*i+1] = hex[p[off+i] & 15];
        }
        encoded[2*n] = 0;
        rec_emit(app_probe, "DATA", "group=app pid=%u tid=%u stream=%s offset=%llu bytes=%u data_hex=%s",
                 pid, tid, stream, app_bytes, n, encoded);
        off += n;
        app_bytes += n;
    }
    struct sha256_ctx snapshot = app_digest;
    uint8_t digest[32];
    char encoded[65];
    sha256_final(&snapshot, digest);
    sha256_hex(digest, encoded);
    rec_emit(app_probe, "DATA", "group=app_digest total_bytes=%llu sha256=%s", app_bytes, encoded);
    kmutex_unlock(&app_lock);
}

void probe_user_report(struct task *t, const char *msg, uint32_t len)
{
    if (app_probe[0])
        probe_app_output(t, "report", msg, len);
    else
        klog("[user %u report] %s", t->id, msg);
}

static const struct probe_def probes[] = {
    { "boot", probe_boot }, { "bootinfo", probe_bootinfo }, { "allocator", probe_allocator },
    { "protection", probe_protection }, { "isolation", probe_isolation }, { "preempt", probe_preempt },
    { "localfault", probe_localfault }, { "syslife", probe_syslife }, { "fpu", probe_fpu },
};

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

struct probe_selection {
    unsigned phase;
    char probe[24], run[9];
    uint32_t flags;
};

/* f[012]:<probe-id> run=<8-hex> [platform=e500] [safe=1]; aliases only F0/F1. */
static bool parse_selector(const char *s, unsigned len, struct probe_selection *selection)
{
    if (len < 3 || len > 64 || s[0] != 'f' || (s[1] != '0' && s[1] != '1' && s[1] != '2') || s[2] != ':')
        return false;
    for (unsigned j = 0; j < len; j++)
        if ((uint8_t)s[j] < 32 || (uint8_t)s[j] > 126)
            return false;
    struct probe_selection parsed = { .phase = (unsigned)(s[1] - '0') };
    unsigned i = 3, k = 0;
    while (i < len && s[i] != ' ' && k + 1 < sizeof(parsed.probe))
        parsed.probe[k++] = s[i++];
    parsed.probe[k] = 0;
    if (!k || i >= len || s[i] != ' ')
        return false;
    static const char *const f0_names[] = {
        "boot", "bootinfo", "allocator", "protection", "isolation", "preempt",
        "localfault", "syslife", "panic", "fpu", "runner", "all", "core"
    };
    static const char *const f1_names[] = {
        "registry", "input", "input-fault", "framebuffer", "ata", "ata-fault",
        "partition", "fat-read", "fat-write", "cache", "mount-crash", "safe", "bootlog", "all", "core"
    };
    static const char *const f2_names[] = {
        "elf-load", "spawn-wait", "fd-table", "mmap", "signals-fault",
        "threads-wait", "crash-isolation", "libc-smoke", "app-gate"
    };
    const char *const *names = parsed.phase == 2 ? f2_names : parsed.phase == 1 ? f1_names : f0_names;
    unsigned count = parsed.phase == 2 ? ARRAY_SIZE(f2_names) : parsed.phase == 1 ? ARRAY_SIZE(f1_names) : ARRAY_SIZE(f0_names);
    bool known = false;
    for (unsigned n = 0; n < count; n++)
        if (!strncmp(parsed.probe, names[n], sizeof(parsed.probe)))
            known = true;
    if (!known)
        return false;
    i++;
    if (len - i < 12 || strncmp(s + i, "run=", 4) != 0)
        return false;
    for (unsigned j = 0; j < 8; j++) {
        if (!is_hex(s[i + 4 + j]))
            return false;
        parsed.run[j] = s[i + 4 + j];
    }
    parsed.run[8] = 0;
    i += 12;
    /* optional suffixes, in order, each at most once */
    if (len - i >= 14 && strncmp(s + i, " platform=e500", 14) == 0) {
        if (!(g_boot.flags & CBI_F_SMBIOS_QEMU) || !(g_boot.flags & CBI_F_INPUT_FORCED))
            return false;
        parsed.flags |= CBI_F_INPUT_FORCED;
        i += 14;
    }
    if (len - i >= 7 && strncmp(s + i, " safe=1", 7) == 0) {
        if (!(g_boot.flags & CBI_F_SMBIOS_QEMU) || !(g_boot.flags & CBI_F_SAFE_MODE))
            return false;
        parsed.flags |= CBI_F_SAFE_MODE;
        i += 7;
    }
    if (i != len)
        return false;
    *selection = parsed;
    return true;
}

static __attribute__((noreturn)) void show_evidence_forever(void)
{
    unsigned pages = console_pages();
    for (unsigned p = 0;; p = (p + 1) % (pages ? pages : 1)) {
        char hdr[96];
        ksnprintf(hdr, sizeof(hdr), "Ciuki VMM F0 evidence - page %u/%u (pages change every 8 s)",
                  p + 1, pages ? pages : 1);
        console_show_page(p, hdr);
        task_sleep_ms(8000);
    }
}

void probes_main(void *arg)
{
    (void)arg;
    timing_calibrate();
    kwork_init();                        /* device worker, before any driver IRQ producer */
    if (!(g_boot.flags & CBI_F_TEST_REQUEST)) {
        klog("Ciuki VMM F0 scaffold ready (no test request). Build %s.", CIUKI_BUILD_ID);
        for (;;)
            task_sleep_ms(60000);
    }
    struct probe_selection selection;
    if (!parse_selector(g_boot.test_request, g_boot.test_request_len, &selection)) {
        char shown[65];
        unsigned n = g_boot.test_request_len < 64 ? g_boot.test_request_len : 64;
        memcpy(shown, g_boot.test_request, n);
        shown[n] = 0;
        klog("[selector] malformed test request (len=%u): '%s'; no probe runs", g_boot.test_request_len, shown);
        show_evidence_forever();
    }
    const char *probe = selection.probe;
    rec_set_run(selection.run);
    klog("[selector] probe=%s platform=%s tsc_khz=%u", probe,
         (g_boot.flags & CBI_F_INPUT_FORCED) ? "e500" : "native", (uint32_t)g_tsc_per_ms);
    bool all = !strncmp(probe, "all", 4);
    bool core = !strncmp(probe, "core", 5);     /* every probe but panic */
    if (selection.phase == 2) {
        unsigned installed = (unsigned)(__f2probes_end - __f2probes_start);
        bool ran = false;
        for (const struct ciuki_f2_probe *p = __f2probes_start; p < __f2probes_end; p++) {
            if (!strncmp(probe, p->name, sizeof(selection.probe))) {
                memcpy(app_probe, selection.probe, sizeof(app_probe));
                sha256_init(&app_digest);
                kmutex_init(&app_lock);
                app_bytes = 0;
                p->run();
                app_probe[0] = 0;
                ran = true;
                break;
            }
        }
        if (!ran) {
            rec_emit(probe, "BEGIN", 0);
            rec_emit(probe, "READY", "table=f2 installed=%u", installed);
            rec_emit(probe, "ERROR", "status=not_run reason=missing_probe");
        }
        show_evidence_forever();
    }
    if (selection.phase == 1) {
        unsigned installed = (unsigned)(__f1probes_end - __f1probes_start);
        bool ran = false;
        for (const struct probe_def *p = __f1probes_start; p < __f1probes_end; p++) {
            if (all || core || !strncmp(probe, p->name, sizeof(selection.probe))) {
                int failed = p->fn();
                ran = true;
                if (failed && (all || core)) {
                    for (const struct probe_def *next = p + 1; next < __f1probes_end; next++)
                        rec_emit(next->name, "NOT_RUN", "reason=prerequisite_failed after=%s", p->name);
                    show_evidence_forever();
                }
            }
        }
        if (!ran) {
            rec_emit(probe, "BEGIN", 0);
            rec_emit(probe, "READY", "table=f1 installed=%u", installed);
            rec_emit(probe, "ERROR", "status=not_run reason=missing_probe");
        }
        show_evidence_forever();
    }
    int ran = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
        if (all || core || !strncmp(probe, probes[i].name, 24)) {
            int failed = probes[i].fn();
            ran++;
            if (failed && (all || core)) {
                for (unsigned j = i + 1; j < ARRAY_SIZE(probes); j++)
                    rec_emit(probes[j].name, "NOT_RUN", "reason=prerequisite_failed after=%s", probes[i].name);
                if (all)
                    rec_emit("panic", "NOT_RUN", "reason=prerequisite_failed after=%s", probes[i].name);
                show_evidence_forever();
            }
        }
    }
    if (all || !strncmp(probe, "panic", 6)) {
        probe_panic();
        ran++;
    }
    if (!ran)
        klog("[selector] unknown probe '%s'; no probe runs", probe);
    show_evidence_forever();
}
