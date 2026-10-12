/* F1-24: conservative boot-framebuffer MTRR allocation, before any LFB alias.
 * Research/combination: docs/design/framebuffer-write-combining.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>

#define MTRR_CAP       0xFEu
#define MTRR_BASE      0x200u
#define MTRR_DEF       0x2FFu
#define PAT_MSR        0x277u
#define MTRR_VALID     (1ull << 11)
#define MTRR_ENABLE    (1ull << 11)
#define MTRR_FIXED     (1ull << 10)
#define MTRR_CAP_FIXED (1ull << 8)
#define MTRR_CAP_WC    (1ull << 10)
#define TYPE_WC        1u
#define TYPE_WB        6u

struct mtrr_pair { uint64_t base, mask; };
struct mtrr_state {
    unsigned bits, count;
    uint64_t cap, def;
    struct mtrr_pair pair[256]; /* VCNT is eight bits; no large stack frame */
};
struct wc_range { uint64_t base, size; unsigned slot; };
struct wc_plan { unsigned count; struct wc_range range[2]; };
static struct mtrr_state state;
static bool attempted, enabled;
static uint32_t framebuffer_phys, framebuffer_bytes;

static bool power_of_two(uint64_t n) { return n && !(n & (n - 1)); }
static uint64_t round_power(uint64_t n)
{
    uint64_t size = PAGE_SIZE;
    while (size < n) size <<= 1;
    return size;
}
static uint64_t address_mask(unsigned bits) { return ((1ull << bits) - 1) & ~0xFFFull; }
static bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return a < b + m && b < a + n;
}
static bool type_valid(unsigned type)
{
    return type == 0 || type == 1 || type == 4 || type == 5 || type == 6;
}

/* Validate even ranges above 4 GiB: silently dropping upper mask bits would
 * mistake a BIOS RAM range for an unused hole. Discontinuous masks fail closed. */
static bool decode_pair(const struct mtrr_state *s, const struct mtrr_pair *p,
                        uint64_t *base, uint64_t *size)
{
    uint64_t am = address_mask(s->bits), pm = am | 0xFFFull;
    if ((p->base & ~(am | 0xFFull)) || (p->mask & ~(am | MTRR_VALID)) ||
        !type_valid((unsigned)(p->base & 0xFF))) return false;
    *base = p->base & am;
    *size = ((~(p->mask & am)) & pm) + 1;
    return power_of_two(*size) && !(*base & (*size - 1));
}

static bool range_safe(const struct mtrr_state *s, uint64_t base, uint64_t size,
                       uint64_t limit)
{
    if (base < 0x100000 || (base & (size - 1)) || base + size > limit) return false;
    for (unsigned i = 0; i < s->count; i++) {
        if (!(s->pair[i].mask & MTRR_VALID)) continue;
        uint64_t b, n;
        if (!decode_pair(s, &s->pair[i], &b, &n) || overlaps(base, size, b, n)) return false;
    }
    for (unsigned i = 0; i < g_boot.e820_count; i++) {
        const struct ciuki_e820 *e = &g_boot.e820[i];
        /* Only ordinary reserved descriptors may describe the PCI aperture. */
        if (!e->length || e->type == 2u) continue; /* E820 reserved */
        if (e->length > UINT64_MAX - e->base || overlaps(base, size, e->base, e->length)) return false;
    }
    return true;
}

/* Pure range planning against a synthetic/BIOS MSR snapshot. No MSR writes
 * occur unless the entire one- or two-pair plan has been accepted. */
static const char *plan_wc(const struct mtrr_state *s, uint32_t phys, uint32_t bytes,
                           uint64_t aperture_bytes, struct wc_plan *p)
{
    memset(p, 0, sizeof(*p));
    if (s->bits < 32 || s->bits > 52 || s->count > 255) return "physical-width";
    if (!(s->cap & MTRR_CAP_WC)) return "no-wc";
    if (!(s->cap & MTRR_CAP_FIXED) || (s->def & (MTRR_ENABLE | MTRR_FIXED)) !=
        (MTRR_ENABLE | MTRR_FIXED)) return "mtrrs-disabled";
    if (!bytes || phys < 0x100000 || (phys & (PAGE_SIZE - 1)) ||
        (uint64_t)phys + bytes > 0x100000000ull) return "invalid-range";
    uint64_t needed = ((uint64_t)bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (aperture_bytes < needed) return "aperture-size";
    uint64_t limit = (uint64_t)phys + aperture_bytes;
    if (limit > 0x100000000ull) limit = 0x100000000ull;
    unsigned free_slots[2], free_count = 0;
    for (unsigned i = 0; i < s->count; i++) {
        if (s->pair[i].mask & MTRR_VALID) {
            uint64_t b, n;
            if (!decode_pair(s, &s->pair[i], &b, &n)) return "invalid-bios-range";
        } else if (free_count < 2) free_slots[free_count++] = i;
    }
    if (!free_count) return "no-free-range";
    uint64_t whole = round_power(needed);
    if (range_safe(s, phys, whole, limit)) {
        p->count = 1;
        p->range[0] = (struct wc_range){ phys, whole, free_slots[0] };
        return "ok";
    }
    /* The first range ends exactly at a boundary; only the last may pad. */
    for (uint64_t first = whole >> 1; first >= PAGE_SIZE; first >>= 1) {
        if (first >= needed || (phys & (first - 1))) continue;
        uint64_t second = round_power(needed - first);
        if (!range_safe(s, phys, first, limit) ||
            !range_safe(s, (uint64_t)phys + first, second, limit)) continue;
        if (free_count < 2) return "no-free-range";
        p->count = 2;
        p->range[0] = (struct wc_range){ phys, first, free_slots[0] };
        p->range[1] = (struct wc_range){ (uint64_t)phys + first, second, free_slots[1] };
        return "ok";
    }
    return "unsafe-range";
}

/* IF is already off. Intel SDM §12.11.8 UP sequence, including globals.
 * Preserve CR0, CR4, DEF_TYPE and all BIOS-owned valid pairs. */
static bool program_wc(const struct mtrr_state *s, const struct wc_plan *p)
{
    uint32_t cr0 = read_cr0(), cr4 = read_cr4(), cr3 = read_cr3();
    write_cr0((cr0 | CR0_CD) & ~CR0_NW);
    wbinvd();
    if (cr4 & CR4_PGE) write_cr4(cr4 & ~CR4_PGE);
    else write_cr3(cr3);
    wrmsr(MTRR_DEF, s->def & ~MTRR_ENABLE);
    for (unsigned i = 0; i < p->count; i++) {
        const struct wc_range *r = &p->range[i];
        uint32_t msr = MTRR_BASE + 2 * r->slot;
        wrmsr(msr + 1, 0); /* disable the free pair before replacing stale fields */
        wrmsr(msr, r->base | TYPE_WC);
        wrmsr(msr + 1, (address_mask(s->bits) & ~(r->size - 1)) | MTRR_VALID);
    }
    wrmsr(MTRR_DEF, s->def);
    wbinvd();
    write_cr3(cr3); /* PGE is still clear, so this also flushes global entries */
    write_cr0(cr0);
    if (cr4 & CR4_PGE) write_cr4(cr4);
    bool ok = rdmsr(MTRR_DEF) == s->def;
    for (unsigned i = 0; i < p->count; i++) {
        const struct wc_range *r = &p->range[i];
        uint32_t msr = MTRR_BASE + 2 * r->slot;
        if (rdmsr(msr) != (r->base | TYPE_WC) || rdmsr(msr + 1) !=
            ((address_mask(s->bits) & ~(r->size - 1)) | MTRR_VALID)) ok = false;
    }
    return ok;
}

bool mtrr_framebuffer_enable(uint32_t phys, uint32_t bytes)
{
    if ((g_boot.flags & CBI_F_TEXT_MODE) || phys != g_boot.fb_phys || !phys ||
        (uint64_t)g_boot.fb_pitch * g_boot.fb_height != bytes) return false;
    uint32_t flags = irq_save();
    if (attempted) {
        bool wc = enabled && phys == framebuffer_phys && bytes == framebuffer_bytes;
        irq_restore(flags);
        return wc;
    }
    attempted = true;
    framebuffer_phys = phys; framebuffer_bytes = bytes;
    const char *why = "no-cpuid";
    const char *hypervisor = "none";
    uint32_t a, b, c, d;
    struct wc_plan plan = {0};
    cpuid(0, &a, &b, &c, &d);
    if (a < 1) goto done;
    cpuid(1, &a, &b, &c, &d);
    bool pat = !!(d & (1u << 16));
    if (c & (1u << 31)) {
        cpuid(0x40000000, &a, &b, &c, &d);
        hypervisor = b == 0x54474354 && c == 0x43544743 && d == 0x47435447 ? "qemu-tcg" : "other";
        cpuid(1, &a, &b, &c, &d);
    }
    why = "no-mtrr";
    if (!(d & (1u << 12))) goto done;
    why = "no-msr";
    if (!(d & (1u << 5))) goto done;
    why = "caches-disabled";
    if (read_cr0() & (CR0_CD | CR0_NW)) goto done;
    state.bits = 36;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000008) {
        cpuid(0x80000008, &a, &b, &c, &d);
        state.bits = a & 0xFF;
    }
    why = "physical-width";
    if (state.bits < 32 || state.bits > 52) goto done;
    state.cap = rdmsr(MTRR_CAP);
    state.count = state.cap & 0xFF;
    state.def = rdmsr(MTRR_DEF);
    why = "no-wc";
    if (!(state.cap & MTRR_CAP_WC)) goto done;
    why = "mtrrs-disabled";
    if (!(state.cap & MTRR_CAP_FIXED) || (state.def & (MTRR_ENABLE | MTRR_FIXED)) !=
        (MTRR_ENABLE | MTRR_FIXED)) goto done;
    if (pat) {
        uint64_t value = rdmsr(PAT_MSR);
        why = "pat-layout";
        if ((value & 0xFF) != TYPE_WB || ((value >> 24) & 0xFF) != 0) goto done;
    }
    for (unsigned i = 0; i < state.count; i++) {
        state.pair[i].base = rdmsr(MTRR_BASE + 2 * i);
        state.pair[i].mask = rdmsr(MTRR_BASE + 2 * i + 1);
    }
    uint64_t aperture = ((uint64_t)bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (g_boot.flags & CBI_F_VBE_CTRL_VALID) {
        uint32_t units = g_boot.vbe_ctrl[18] | (uint32_t)g_boot.vbe_ctrl[19] << 8;
        if (units) aperture = (uint64_t)units << 16;
    }
    why = plan_wc(&state, phys, bytes, aperture, &plan);
    if (!plan.count) goto done;
    enabled = program_wc(&state, &plan);
    why = enabled ? "ok" : "readback-failed";
done:
    irq_restore(flags);
    klog("[display] mtrr wc base=%08x size=%u result=%s ranges=%u hypervisor=%s",
         phys, bytes, why, plan.count, hypervisor);
    if (enabled) for (unsigned i = 0; i < plan.count; i++)
        klog("[display] mtrr wc base=%08x size=%llu result=ok slot=%u",
             (uint32_t)plan.range[i].base, plan.range[i].size, plan.range[i].slot);
    return enabled;
}

void framebuffer_write_drain(void) { if (enabled) cpu_write_drain(); }
uint64_t framebuffer_cycles(void) { return read_tsc_serialized(); }
