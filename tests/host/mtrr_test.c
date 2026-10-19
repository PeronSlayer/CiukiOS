/* Production MTRR planner/programmer and VMM mappings on synthetic hardware.
 * No privileged instruction is executed. ASan/UBSan host harness.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL mtrr:%d %s\n", __LINE__, #x); failures++; } } while (0)
struct ciuki_boot_info g_boot;
static uint64_t msrs[0x400];
static uint32_t host_cr0, host_cr3, host_cr4, host_flags;
static unsigned msr_reads, msr_writes, cpuid_reads;
static uint32_t revoked_aliases[3];
static uint32_t leaf_max, leaf_features, leaf_ecx, extended_max, physical_bits;
static bool tcg, ignored_writes;
static char log_line[240];
static unsigned logs;
struct event { char kind; uint32_t reg; uint64_t value; };
static struct event events[64];
static unsigned event_count;
static void event(char kind, uint32_t reg, uint64_t value)
{
    CHECK(event_count < ARRAY_SIZE(events));
    if (event_count < ARRAY_SIZE(events)) events[event_count++] = (struct event){kind, reg, value};
}
static uint32_t host_irq_save(void)
{
    uint32_t f = host_flags; host_flags &= ~0x200u; event('I', 0, f); return f;
}
static void host_irq_restore(uint32_t flags) { host_flags = flags; event('i', 0, flags); }
static void host_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    cpuid_reads++; *a = *b = *c = *d = 0;
    if (!leaf) *a = leaf_max;
    else if (leaf == 1) { *d = leaf_features; *c = leaf_ecx; }
    else if (leaf == 0x80000000) *a = extended_max;
    else if (leaf == 0x80000008) *a = physical_bits;
    else if (leaf == 0x40000000 && tcg) {
        *a = 0x40000001; *b = 0x54474354; *c = 0x43544743; *d = 0x47435447;
    }
}
static uint64_t host_rdmsr(uint32_t reg)
{
    CHECK(reg < ARRAY_SIZE(msrs)); msr_reads++; return msrs[reg];
}
static void host_wrmsr(uint32_t reg, uint64_t value)
{
    CHECK(!(host_flags & 0x200) && (host_cr0 & CR0_CD) && !(host_cr0 & CR0_NW));
    CHECK(reg == 0x2FF || (reg >= 0x200 && reg < 0x210));
    for (unsigned i = 0; i < ARRAY_SIZE(revoked_aliases); i++)
        if (revoked_aliases[i]) CHECK(!vmm_kernel_pte(revoked_aliases[i]));
    event('M', reg, value); msr_writes++;
    if (!ignored_writes) msrs[reg] = value;
}
static uint32_t host_read_cr0(void) { return host_cr0; }
static uint32_t host_read_cr3(void) { return host_cr3; }
static uint32_t host_read_cr4(void) { return host_cr4; }
static void host_write_cr0(uint32_t value) { CHECK(!(host_flags & 0x200)); event('0', 0, value); host_cr0 = value; }
static void host_write_cr3(uint32_t value) { event('3', 0, value); host_cr3 = value; }
static void host_write_cr4(uint32_t value) { event('4', 0, value); host_cr4 = value; }
static void host_wbinvd(void) { CHECK(!(host_flags & 0x200)); event('B', 0, 0); }
static unsigned invalidations;
static void host_invlpg(uint32_t va) { (void)va; invalidations++; }
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(log_line, sizeof(log_line), fmt, ap); va_end(ap); logs++;
}
void panic(const char *fmt, ...) { (void)fmt; abort(); }

#undef irq_save
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#define cpuid host_cpuid
#define rdmsr host_rdmsr
#define wrmsr host_wrmsr
#define read_cr0 host_read_cr0
#define read_cr3 host_read_cr3
#define read_cr4 host_read_cr4
#define write_cr0 host_write_cr0
#define write_cr3 host_write_cr3
#define write_cr4 host_write_cr4
#define wbinvd host_wbinvd
#define invlpg host_invlpg
#include "../../src/kernel/core/mtrr.c"

/* Exercise the real framebuffer/MMIO PTE writer, including the console's
 * second alias. Unused VMM functions are removed at link time. */
static uint8_t ram[2 * 1024 * 1024] __attribute__((aligned(4096)));
#undef P2V
#define P2V(p) ((void *)(ram + (uint32_t)(p)))
void kpreempt_point(void) { }
#include "../../src/kernel/core/vmm.c"

static void bios_range(unsigned slot, uint64_t base, uint64_t size, unsigned type)
{
    msrs[MTRR_BASE + 2 * slot] = base | type;
    msrs[MTRR_BASE + 2 * slot + 1] = (address_mask(physical_bits) & ~(size - 1)) | MTRR_VALID;
}
static void fresh(void)
{
    memset(&g_boot, 0, sizeof(g_boot)); memset(msrs, 0, sizeof(msrs));
    memset(revoked_aliases, 0, sizeof(revoked_aliases)); memset(&state, 0, sizeof(state)); memset(&owned, 0, sizeof(owned)); memset(original, 0, sizeof(original)); attempted = enabled = false;
    host_cr0 = CR0_PE | CR0_PG | CR0_WP; host_cr3 = 0x1000; host_cr4 = CR4_PGE | CR4_PSE;
    host_flags = 0x202; leaf_max = 1; leaf_features = (1u << 5) | (1u << 12) | (1u << 16);
    leaf_ecx = 0; extended_max = 0x80000008; physical_bits = 36; tcg = ignored_writes = false;
    msr_reads = msr_writes = cpuid_reads = event_count = logs = 0; log_line[0] = 0;
    msrs[MTRR_CAP] = 8 | MTRR_CAP_FIXED | MTRR_CAP_WC;
    msrs[MTRR_DEF] = MTRR_ENABLE | MTRR_FIXED | TYPE_WB;
    msrs[PAT_MSR] = 0x0007040600070406ull;
    msrs[0x259] = 0; /* BIOS UC VGA fixed range; must never be written */
    bios_range(0, 0, 128u << 20, TYPE_WB);
    g_boot.flags = CBI_F_VBE_CTRL_VALID;
    g_boot.fb_phys = 0xE0000000; g_boot.fb_pitch = 4096; g_boot.fb_height = 768;
    g_boot.vbe_ctrl[18] = 128; /* 8 MiB aperture */
    g_boot.e820_count = 2;
    g_boot.e820[0] = (struct ciuki_e820){.base = 0, .length = 128u << 20, .type = 1};
    g_boot.e820[1] = (struct ciuki_e820){.base = 0xE0000000, .length = 256u << 20, .type = 2};
    memset(ram, 0, sizeof(ram)); kpd_phys = 0x1000; kpd = P2V(kpd_phys); mmio_next = MMIO_REGION;
    for (unsigned i = 768; i < 1023; i++) kpd[i] = (0x10000u + (i - 768) * PAGE_SIZE) | PTE_P | PTE_W;
}
static struct mtrr_state snapshot(void)
{
    struct mtrr_state s = {.bits = physical_bits, .count = 8, .cap = msrs[MTRR_CAP], .def = msrs[MTRR_DEF]};
    for (unsigned i = 0; i < s.count; i++) s.pair[i] = (struct mtrr_pair){msrs[0x200 + 2 * i], msrs[0x201 + 2 * i]};
    return s;
}
static void expect_plan(uint32_t base, uint32_t bytes, uint64_t aperture,
                        unsigned count, uint64_t first, uint64_t second)
{
    struct mtrr_state s = snapshot(); struct wc_plan p;
    CHECK(!strcmp(plan_wc(&s, base, bytes, aperture, &p), "ok")); CHECK(p.count == count);
    CHECK(p.range[0].base == base && p.range[0].size == first && p.range[0].slot == 1);
    if (count == 2) CHECK(p.range[1].base == (uint64_t)base + first && p.range[1].size == second && p.range[1].slot == 2);
}
static void arithmetic_tests(void)
{
    fresh();
    expect_plan(0xE0000000, 3u << 20, 8u << 20, 1, 4u << 20, 0);
    expect_plan(0xE0000000, PAGE_SIZE, PAGE_SIZE, 1, PAGE_SIZE, 0);
    expect_plan(0xE0000000, PAGE_SIZE - 1, PAGE_SIZE, 1, PAGE_SIZE, 0);
    expect_plan(0xE0100000, 3u << 20, 8u << 20, 2, 1u << 20, 2u << 20);
    expect_plan(0xE0200000, 3u << 20, 8u << 20, 2, 2u << 20, 1u << 20);
    expect_plan(0xE0300000, 5u << 20, 8u << 20, 2, 1u << 20, 4u << 20);
    expect_plan(0xFFFFE000, 8192, 8192, 1, 8192, 0); /* exclusive end at 4 GiB */
    struct mtrr_state s = snapshot(); struct wc_plan p;
    CHECK(!strcmp(plan_wc(&s, 0xE0100000, 5u << 20, 8u << 20, &p), "unsafe-range") && !p.count);
    CHECK(!strcmp(plan_wc(&s, 0xE0000001, 1, PAGE_SIZE, &p), "invalid-range"));
    CHECK(!strcmp(plan_wc(&s, 0xA0000, 0x20000, 0x20000, &p), "invalid-range"));
    CHECK(!strcmp(plan_wc(&s, 0xE0000000, 0, 0, &p), "invalid-range"));
    CHECK(!strcmp(plan_wc(&s, 0xFFFFF000, 8192, 8192, &p), "invalid-range"));
    CHECK(!strcmp(plan_wc(&s, 0xE0000000, 3u << 20, 2u << 20, &p), "aperture-size"));
    expect_plan(0xE0000000, 3u << 20, 3u << 20, 2, 2u << 20, 1u << 20);
    bios_range(3, 0xE0300000, 1u << 20, 0); /* one range pads into BIOS UC; split avoids it */
    expect_plan(0xE0000000, 3u << 20, 8u << 20, 2, 2u << 20, 1u << 20);
    bios_range(3, 0xE0200000, 1u << 20, 0); s = snapshot();
    CHECK(!strcmp(plan_wc(&s, 0xE0000000, 3u << 20, 8u << 20, &p), "unsafe-range") && !p.count);
    fresh(); bios_range(3, 0x100000000ull, 1u << 20, TYPE_WB); /* upper bits preserved */
    expect_plan(0xE0000000, 3u << 20, 8u << 20, 1, 4u << 20, 0);
    msrs[0x207] ^= 1u << 22; s = snapshot();
    CHECK(!strcmp(plan_wc(&s, 0xE0000000, 3u << 20, 8u << 20, &p), "invalid-bios-range"));
    fresh(); msrs[0x200] |= 1ull << 50; s = snapshot();
    CHECK(!strcmp(plan_wc(&s, 0xE0000000, PAGE_SIZE, PAGE_SIZE, &p), "invalid-bios-range"));
    fresh();
    for (unsigned type = 1; type <= 4; type++) {
        if (type == 2) continue;
        g_boot.e820[1] = (struct ciuki_e820){.base = 0xE0200000, .length = 1u << 20, .type = type}; s = snapshot();
        CHECK(!strcmp(plan_wc(&s, 0xE0000000, 3u << 20, 8u << 20, &p), "unsafe-range"));
    }
}
static void expect_event(unsigned *at, char kind, uint32_t reg, uint64_t value)
{
    CHECK(*at < event_count);
    if (*at < event_count) CHECK(events[*at].kind == kind && events[*at].reg == reg && events[*at].value == value);
    (*at)++;
}
static void sequence_tests(bool global, bool interrupts, bool split)
{
    fresh(); if (!global) host_cr4 &= ~CR4_PGE; if (!interrupts) host_flags &= ~0x200u;
    if (split) g_boot.fb_phys = 0xE0100000;
    uint32_t cr0 = host_cr0, cr4 = host_cr4, flags = host_flags;
    uint64_t bios_base = msrs[0x200], bios_mask = msrs[0x201], pat = msrs[PAT_MSR], def = msrs[MTRR_DEF];
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20));
    unsigned at = 0;
    expect_event(&at, 'I', 0, flags);
    expect_event(&at, '0', 0, cr0 | CR0_CD);
    expect_event(&at, 'B', 0, 0);
    expect_event(&at, global ? '4' : '3', 0, global ? cr4 & ~CR4_PGE : 0x1000);
    expect_event(&at, 'M', MTRR_DEF, def & ~MTRR_ENABLE);
    expect_event(&at, 'M', 0x203, 0);
    expect_event(&at, 'M', 0x202, (uint64_t)g_boot.fb_phys | TYPE_WC);
    expect_event(&at, 'M', 0x203, split ? 0xFFFF00800ull : 0xFFFC00800ull);
    if (split) {
        expect_event(&at, 'M', 0x205, 0);
        expect_event(&at, 'M', 0x204, 0xE0200001);
        expect_event(&at, 'M', 0x205, 0xFFFE00800ull);
    }
    expect_event(&at, 'M', MTRR_DEF, def);
    expect_event(&at, 'B', 0, 0);
    expect_event(&at, '3', 0, 0x1000);
    expect_event(&at, '0', 0, cr0);
    if (global) expect_event(&at, '4', 0, cr4);
    expect_event(&at, 'i', 0, flags); CHECK(at == event_count);
    CHECK(host_cr0 == cr0 && host_cr4 == cr4 && host_flags == flags);
    CHECK(msrs[0x200] == bios_base && msrs[0x201] == bios_mask && msrs[PAT_MSR] == pat && msrs[0x259] == 0);
    CHECK(msr_writes == (split ? 8u : 5u));
    unsigned writes = msr_writes, reads = msr_reads, records = logs;
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && msr_writes == writes && msr_reads == reads && logs == records);
}
static void gating_tests(void)
{
    for (unsigned n = 0; n < 11; n++) {
        fresh();
        const char *reason;
        switch (n) {
        case 0: leaf_max = 0; reason = "no-cpuid"; break;
        case 1: leaf_features &= ~(1u << 12); reason = "no-mtrr"; break;
        case 2: leaf_features &= ~(1u << 5); reason = "no-msr"; break;
        case 3: msrs[MTRR_CAP] &= ~MTRR_CAP_WC; reason = "no-wc"; break;
        case 4: msrs[MTRR_DEF] &= ~MTRR_ENABLE; reason = "mtrrs-disabled"; break;
        case 5: msrs[MTRR_DEF] &= ~MTRR_FIXED; reason = "mtrrs-disabled"; break;
        case 6: msrs[PAT_MSR] &= ~0xFFull; reason = "pat-layout"; break;
        case 7: physical_bits = 0; reason = "physical-width"; break;
        case 8: physical_bits = 53; reason = "physical-width"; break;
        case 9: for (unsigned i = 1; i < 8; i++) bios_range(i, (uint64_t)i << 30, 1u << 20, TYPE_WB); reason = "no-free-range"; break;
        default: host_cr0 |= CR0_CD; reason = "caches-disabled"; break;
        }
        CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && !msr_writes);
        CHECK(strstr(log_line, reason) && host_flags == 0x202);
        if (n < 3 || n == 7 || n == 8 || n == 10) CHECK(!msr_reads);
    }
    fresh(); leaf_ecx = 1u << 31; tcg = true; leaf_features &= ~(1u << 12);
    CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && !msr_reads && !msr_writes);
    CHECK(strstr(log_line, "hypervisor=qemu-tcg") && strstr(log_line, "result=no-mtrr"));
    fresh(); leaf_ecx = 1u << 31; tcg = true;
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && msr_writes == 5);
    fresh(); leaf_ecx = 1u << 31; leaf_features &= ~(1u << 12);
    CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && !msr_reads && strstr(log_line, "hypervisor=other"));
    fresh(); ignored_writes = true; leaf_ecx = 1u << 31; tcg = true;
    CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && strstr(log_line, "readback-failed") && !enabled);
    fresh(); extended_max = 0x80000000; physical_bits = 36; leaf_features &= ~(1u << 16);
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && state.bits == 36 && msrs[0x203] == 0xFFFC00800ull);
    fresh(); physical_bits = 40; bios_range(0, 0, 128u << 20, TYPE_WB);
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && msrs[0x203] == 0xFFFFC00800ull);
    fresh(); physical_bits = 32; bios_range(0, 0, 128u << 20, TYPE_WB);
    CHECK(mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && msrs[0x203] == 0xFFC00800ull);
    fresh(); g_boot.flags |= CBI_F_TEXT_MODE;
    CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && !cpuid_reads && !msr_reads);
    fresh(); CHECK(!mtrr_framebuffer_enable(0xA0000, 3u << 20) && !cpuid_reads && !msr_reads);
    fresh(); g_boot.fb_phys = 0xE0100000;
    for (unsigned i = 2; i < 8; i++) bios_range(i, (uint64_t)i << 30, 1u << 20, TYPE_WB);
    CHECK(!mtrr_framebuffer_enable(g_boot.fb_phys, 3u << 20) && !msr_writes && strstr(log_line, "no-free-range"));
}
static void mapping_tests(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        fresh();
        if (mode == 1) leaf_features &= ~(1u << 12);
        if (mode == 2) ignored_writes = true;
        void *first = vmm_map_framebuffer(g_boot.fb_phys, 3u << 20);
        CHECK(first != 0);
        void *second = vmm_map_framebuffer(g_boot.fb_phys, 3u << 20);
        CHECK(second && second != first);
        uint32_t cache = mode ? PTE_PCD | PTE_PWT : 0;
        for (unsigned i = 0; i < 768; i++) {
            uint32_t va = (uint32_t)(uintptr_t)first + i * PAGE_SIZE;
            CHECK(vmm_kernel_pte(va) == (0xE0000000 + i * PAGE_SIZE) + (PTE_P | PTE_W | cache));
            CHECK(vmm_kernel_pte((uint32_t)(uintptr_t)second + i * PAGE_SIZE) == vmm_kernel_pte(va));
        }
        CHECK(!vmm_kernel_pte((uint32_t)(uintptr_t)first + (3u << 20)));
        void *vga = vmm_map_mmio(0xA0000, 0x20000, true);
        CHECK((vmm_kernel_pte((uint32_t)(uintptr_t)vga) & 0xFFF) == (PTE_P | PTE_W | PTE_PCD | PTE_PWT));
    }
    fresh(); mmio_next = MMIO_END - PAGE_SIZE;
    CHECK(!vmm_map_framebuffer(g_boot.fb_phys, 3u << 20) && !msr_reads && !msr_writes);
}
/* Boot aliases, new pitch/base, slot retirement, repeated restore and UC
 * fallback all use the production range planner and PTE writer. */
static void runtime_mapping_tests(void)
{
    fresh();
    /* An invalid pair can contain stale fields; retirement restores them. */
    msrs[0x204] = 0xD0000006;
    msrs[0x205] = address_mask(36) & ~((1u << 20) - 1);
    uint64_t spare_base = msrs[0x204], spare_mask = msrs[0x205];
    g_boot.version = 2;
    g_boot.display.tag = CBI_DISPLAY_TAG;
    g_boot.display.size = sizeof(g_boot.display);
    g_boot.display.vram_bytes = 8u << 20;
    g_boot.flags &= ~CBI_F_VBE_CTRL_VALID; /* v2 extent supplies padding bounds */
    void *console = vmm_map_framebuffer(g_boot.fb_phys, 3u << 20);
    void *presenter = vmm_map_framebuffer(g_boot.fb_phys, 3u << 20);
    CHECK(console && presenter && enabled);
    vmm_revoke_mmio(console, 3u << 20);
    vmm_revoke_mmio(presenter, 3u << 20);
    CHECK(!vmm_kernel_pte((uint32_t)(uintptr_t)console));
    CHECK(!vmm_kernel_pte((uint32_t)(uintptr_t)presenter));
    void *window = vmm_reserve_mmio(9u << 20);
    CHECK(window != 0);
    revoked_aliases[0] = (uint32_t)(uintptr_t)console;
    revoked_aliases[1] = (uint32_t)(uintptr_t)presenter;
    revoked_aliases[2] = (uint32_t)(uintptr_t)window;
    uint64_t bios_base = msrs[0x200], bios_mask = msrs[0x201];
    const uint32_t bases[] = {0xE0000000, 0xE0100000, 0xE0000000, 0xE0100000, 0xE0000000};
    const uint32_t sizes[] = {5u << 20, 3u << 20, 1u << 20, 3u << 20, 3u << 20};
    for (unsigned n = 0; n < ARRAY_SIZE(bases); n++) {
        event_count = 0;
        CHECK(vmm_replace_framebuffer(window, 9u << 20, bases[n], sizes[n]) == window);
        CHECK(enabled && owned.count == (n == 1 || n == 3 ? 2u : 1u));
        for (unsigned i = 0; i < sizes[n]; i += PAGE_SIZE)
            CHECK(vmm_kernel_pte((uint32_t)(uintptr_t)window + i) == (bases[n] + i + PTE_P + PTE_W));
        CHECK(!vmm_kernel_pte((uint32_t)(uintptr_t)window + sizes[n]));
        CHECK(msrs[0x200] == bios_base && msrs[0x201] == bios_mask);
        CHECK(!(msrs[0x205] & MTRR_VALID) || owned.count == 2);
        if (owned.count == 1) CHECK(msrs[0x204] == spare_base && msrs[0x205] == spare_mask);
        CHECK(owned.range[0].slot == 1 && !(msrs[0x207] & MTRR_VALID));
        CHECK(host_flags == 0x202 && !(host_cr0 & (CR0_CD | CR0_NW)));
    }
    unsigned writes = msr_writes;
    event_count = 0;
    CHECK(vmm_replace_mmio(window, 9u << 20, 0xE0000000, PAGE_SIZE) == window);
    CHECK(msr_writes == writes && (vmm_kernel_pte((uint32_t)(uintptr_t)window) & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT));
    event_count = 0;
    CHECK(vmm_replace_framebuffer(window, 9u << 20, 0xE0000003, PAGE_SIZE));
    CHECK(!enabled && msr_writes == writes);
    CHECK((vmm_kernel_pte((uint32_t)(uintptr_t)window) & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT));
    event_count = 0;
    CHECK(vmm_replace_framebuffer(window, 9u << 20, 0x100000, PAGE_SIZE));
    CHECK(!enabled && msr_writes == writes); /* BIOS RAM + E820 refuse WC */
    event_count = 0;
    CHECK(vmm_replace_framebuffer(window, 9u << 20, 0xE0000000, 3u << 20) && enabled);
    writes = msr_writes;
    msrs[0x202] = 0xE0000000; /* firmware changed owned WC into UC */
    event_count = 0;
    CHECK(vmm_replace_framebuffer(window, 9u << 20, 0xE1000000, 3u << 20));
    CHECK(!enabled && msr_writes == writes && msrs[0x202] == 0xE0000000);
    CHECK(strstr(log_line, "owned-range-changed"));
    CHECK((vmm_kernel_pte((uint32_t)(uintptr_t)window) & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT));
    fresh(); leaf_features &= ~(1u << 12);
    window = vmm_reserve_mmio(4u << 20);
    CHECK(vmm_replace_framebuffer(window, 4u << 20, 0xE0000000, 3u << 20));
    CHECK(!enabled && !msr_reads && !msr_writes);
    CHECK((vmm_kernel_pte((uint32_t)(uintptr_t)window) & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT));
    fresh(); window = vmm_reserve_mmio(PAGE_SIZE);
    CHECK(!vmm_replace_framebuffer(window, PAGE_SIZE, 0xE0000000, 3u << 20) && !msr_reads && !msr_writes);
}
static void firmware_mapping_tests(void)
{
    fresh();
    const uint32_t addresses[]={0x1ff7c000,0x1ff7cff0,0x40000ff0,0xfffffffau};
    const uint32_t lengths[]={1717,1717,UINT16_MAX,6};
    for(unsigned n=0;n<ARRAY_SIZE(addresses);n++) {
        uint32_t start=mmio_next,phys=addresses[n],bytes=lengths[n];
        uint32_t off=phys&(PAGE_SIZE-1),span=PAGE_ALIGN_UP(off+bytes);
        const void *mapped=vmm_map_physical_ro(phys,bytes);
        CHECK((uintptr_t)mapped==start+off && mmio_next==start+span+PAGE_SIZE);
        for(uint32_t i=0;i<span;i+=PAGE_SIZE)
            CHECK(vmm_kernel_pte(start+i)==(PAGE_ALIGN_DOWN(phys)+i+PTE_P));
        CHECK(!vmm_kernel_pte(start+span)); /* guard */
        unsigned before=invalidations;
        vmm_revoke_mmio((void *)mapped,bytes);
        CHECK(invalidations-before==span/PAGE_SIZE);
        for(uint32_t i=0;i<span+PAGE_SIZE;i+=PAGE_SIZE)CHECK(!vmm_kernel_pte(start+i));
    }
    CHECK(!msr_reads && !msr_writes); /* no framebuffer cache-policy changes */
    uint32_t next=mmio_next;
    CHECK(!vmm_map_physical_ro(0x1ff7c000,0) && mmio_next==next);
    CHECK(!vmm_map_physical_ro(0xfffffffa,7) && mmio_next==next);
    mmio_next=MMIO_END-2*PAGE_SIZE;
    CHECK(!vmm_map_physical_ro(0x1ff7cfff,2)); /* two pages plus guard do not fit */
    CHECK(mmio_next==MMIO_END-2*PAGE_SIZE && !vmm_kernel_pte(mmio_next));
    const void *last=vmm_map_physical_ro(0x1ff7c000,PAGE_SIZE);
    CHECK(last && mmio_next==MMIO_END);
    CHECK(!vmm_map_physical_ro(0x1ff7c000,1));
    vmm_revoke_mmio((void *)last,PAGE_SIZE);
    CHECK(!vmm_kernel_pte(MMIO_END-2*PAGE_SIZE) && !vmm_kernel_pte(MMIO_END-PAGE_SIZE));
    puts("VMM firmware window: bounded read-only supervisor PTEs, offsets, guard, revoke/TLB invalidation, 4 GiB bounds and arena exhaustion PASS");
}
int main(void)
{
    arithmetic_tests();
    for (unsigned g = 0; g < 2; g++) for (unsigned i = 0; i < 2; i++) for (unsigned s = 0; s < 2; s++)
        sequence_tests(g, i, s);
    gating_tests(); mapping_tests(); runtime_mapping_tests(); firmware_mapping_tests();
    printf("MTRR host tests: %s (%u checks, %u failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
