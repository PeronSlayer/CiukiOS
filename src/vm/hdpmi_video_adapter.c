/* Host-level protected-mode VGA fault adapter for official HDPMI 3.24.
 *
 * The HDPMI page table deliberately retains CVSESSION's supervisor-only
 * aperture.  Ring-3 accesses therefore arrive here through intr0E and are
 * executed against the same shared cvga_state as the V86 monitor.  This file
 * is freestanding: it allocates nothing and calls neither DOS nor BIOS.
 */
#include "hdpmi_video_adapter.h"
#include "session_video.h"
#include "vga_x86.h"

extern uint32_t cvdpmi_selector_base(uint32_t selector);
extern uint32_t cvdpmi_selector_dbit(uint32_t selector);
/* Linear addresses in HDPMI's address space (0 when unavailable): its own
 * page-table entry for A0000h, the session's shadow entries for A0000h, and
 * the physical page list of the shared video block (page 0 = header). */
extern uint32_t *cvdpmi_aperture_ptes(void);
extern uint32_t *cvdpmi_shadow_ptes(void);
extern uint32_t *cvdpmi_video_pages(void);
extern void cvdpmi_flush_tlb(void);

#define DIRECT_PAGES 16u                 /* A0000-AFFFF */
#define DIRECT_PLANE 7u                  /* pm_direct: mapped plane + 1 */
#define DIRECT_RO 8u                     /* pm_direct: mapped read-only */
#define DIRECT_READING 0x10u             /* pm_direct: read map select written last */
#define DIRECT_FRAME 0x20u               /* pm_direct: CRTC start written (page flip) */
#define PTE_DIRTY 0x40u

/* Pages written through the direct mapping become dirty model granules: the
 * presenter then redraws them exactly as after emulated writes. Clearing a
 * PTE's dirty bit requires the TLB flush the callers perform. */
static int direct_harvest(cvvid_shared *shared, cvga_state *vga, uint32_t *pte)
{
    unsigned plane = (shared->pm_direct & DIRECT_PLANE) - 1u, i, j;
    int harvested = 0;
    for (i = 0; i < DIRECT_PAGES; ++i) {
        if (!(pte[i] & PTE_DIRTY)) continue;
        pte[i] &= ~PTE_DIRTY;
        harvested = 1;
        for (j = 0; j < 4096u / CVGA_DIRTY_GRANULE / 8u; ++j)
            vga->dirty[plane][i * (4096u / CVGA_DIRTY_GRANULE / 8u) + j] = 0xff;
        ++vga->changes;
    }
    return harvested;
}

/* After a VGA register write: expose one plane directly at A0000-AFFFF (no
 * page fault per access), or restore the session's trapping entries. Only
 * HDPMI's own table changes; the V86 view and the shadow stay trapped
 * (cv_guard_ptes skips these entries while pm_direct says so).
 * The last of the map mask (SEQ 2) and read map select (GC 4) written picks
 * the phase: after the map mask, the plane a store writes is mapped writable
 * (Doom's I_UpdateBox); after the read map select, the plane a load reads is
 * mapped read-only, so stores still trap (Doom's I_ReadScreen). A latch copy
 * (write mode 1) traps in both. */
static void direct_update(cvvid_shared *shared, cvga_state *vga, uint16_t port)
{
    uint32_t *pte = cvdpmi_aperture_ptes();
    const uint32_t *shadow = cvdpmi_shadow_ptes();
    const uint32_t *pages = cvdpmi_video_pages();
    uint32_t state = shared->pm_direct, target, flags, i;
    int want;
    if (!pte || !shadow || !pages) return;
    if (port == 0x3cf && vga->gc_index == 4) state |= DIRECT_READING;
    else if (port == 0x3c5 && vga->seq_index == 2) state &= ~DIRECT_READING;
    if (state & DIRECT_READING) {
        want = cvga_direct_read_plane(vga);
        flags = 5u;                      /* present, user, read-only */
    } else {
        want = cvga_direct_plane(vga);
        flags = 7u;                      /* present, user, writable */
    }
    target = want < 0 ? 0u : (uint32_t)(want + 1) | (flags == 5u ? DIRECT_RO : 0u);
    if (target == (state & (DIRECT_PLANE | DIRECT_RO))) {
        shared->pm_direct = state;
        return;
    }
    if ((state & DIRECT_PLANE) && !(state & DIRECT_RO))
        direct_harvest(shared, vga, pte);
    if (target != (state & (DIRECT_PLANE | DIRECT_RO)))
        for (i = 0; i < DIRECT_PAGES; ++i)
            pte[i] = want < 0 ? shadow[i] :
                     (pages[1u + (unsigned)want * DIRECT_PAGES + i] & ~0xfffUL) | flags;
    shared->pm_direct = (state & ~(DIRECT_PLANE | DIRECT_RO)) | target;
    /* A target change always needs invalidation; harvest is folded into it. */
    cvdpmi_flush_tlb();
}

/* Physical timer tick: a client may keep writing one plane without touching
 * a VGA register; its stores still reach the presenter. */
void cvdpmi_video_tick(void *shared_block)
{
    cvvid_shared *shared = (cvvid_shared *)shared_block;
    uint32_t *pte;
    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        !(shared->pm_direct & DIRECT_PLANE) || (shared->pm_direct & DIRECT_RO))
        return;
    pte = cvdpmi_aperture_ptes();
    if (!pte) return;
    if (direct_harvest(shared, (cvga_state *)((uint8_t *)shared + CVVID_MODEL_OFFSET), pte))
        cvdpmi_flush_tlb();
}

/* Before detach: the exact trapping entries again. */
void cvdpmi_video_direct_off(void *shared_block)
{
    cvvid_shared *shared = (cvvid_shared *)shared_block;
    uint32_t *pte = cvdpmi_aperture_ptes();
    const uint32_t *shadow = cvdpmi_shadow_ptes();
    unsigned i;
    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        !(shared->pm_direct & DIRECT_PLANE) || !pte || !shadow)
        return;
    for (i = 0; i < DIRECT_PAGES; ++i) pte[i] = shadow[i];
    shared->pm_direct &= ~(DIRECT_PLANE | DIRECT_RO | DIRECT_READING);
    cvdpmi_flush_tlb();
}

typedef struct video_bus {
    cvga_state *vga;
} video_bus;

static int aperture(uint32_t linear)
{
    return linear >= 0xa0000UL && linear < 0xc0000UL;
}

static int CVGA_CALL read_byte(void *opaque, uint32_t linear, uint8_t *value)
{
    video_bus *bus = (video_bus *)opaque;
    if (aperture(linear)) {
        *value = cvga_read_vram(bus->vga, linear);
        return 0;
    }
    *value = *(volatile uint8_t *)linear;
    return 0;
}

static int CVGA_CALL write_byte(void *opaque, uint32_t linear, uint8_t value)
{
    video_bus *bus = (video_bus *)opaque;
    if (aperture(linear)) {
        cvga_write_vram(bus->vga, linear, value);
        return 0;
    }
    *(volatile uint8_t *)linear = value;
    return 0;
}

static uint32_t CVGA_CALL segment_base(void *opaque, uint16_t selector)
{
    (void)opaque;
    return selector ? cvdpmi_selector_base(selector) : 0;
}

static int CVGA_CALL fetch_byte(void *opaque, uint32_t linear, uint8_t *value)
{
    (void)opaque;
    if (aperture(linear)) return 1;
    *value = *(volatile uint8_t *)linear;
    return 0;
}

int cvdpmi_video_execute(cvdpmi_fault_frame *f, void *shared_block)
{
    cvvid_shared *shared = (cvvid_shared *)shared_block;
    video_bus context;
    cvx_bus bus;
    cvx_cpu cpu;
    cvx_result result;
    uint16_t selectors[6];
    uint32_t i;
    int status;

    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        shared->version != 0x0100 || shared->bytes != CVVID_BLOCK_BYTES ||
        shared->fatal)
        return 1;
    context.vga = (cvga_state *)((uint8_t *)shared + CVVID_MODEL_OFFSET);
    bus.read = read_byte;
    bus.write = write_byte;
    bus.fetch = fetch_byte;
    bus.context = &context;
    bus.segment_base = segment_base;

    cpu.gpr[CVX_EAX] = f->eax; cpu.gpr[CVX_ECX] = f->ecx;
    cpu.gpr[CVX_EDX] = f->edx; cpu.gpr[CVX_EBX] = f->ebx;
    cpu.gpr[CVX_ESP] = f->esp; cpu.gpr[CVX_EBP] = f->ebp;
    cpu.gpr[CVX_ESI] = f->esi; cpu.gpr[CVX_EDI] = f->edi;
    cpu.eip = f->eip;
    cpu.eflags = f->eflags;
    selectors[CVX_ES] = (uint16_t)f->es;
    selectors[CVX_CS] = (uint16_t)f->cs;
    selectors[CVX_SS] = (uint16_t)f->ss;
    selectors[CVX_DS] = (uint16_t)f->ds;
    selectors[CVX_FS] = (uint16_t)f->fs;
    selectors[CVX_GS] = (uint16_t)f->gs;
    for (i = 0; i < 6; ++i) cpu.sreg[i] = selectors[i];
    cpu.seg_base[CVX_CS] = segment_base(0, selectors[CVX_CS]);
    cpu.base_valid = 1u << CVX_CS;   /* the others on first use */
    cpu.code32 = (uint8_t)cvdpmi_selector_dbit(selectors[CVX_CS]);

    status = cvx_execute(&cpu, &bus, 4096, &result);
    if (status != CVX_DONE && status != CVX_PARTIAL) {
        ++shared->pm_unsupported;
        if (!shared->fatal)
            shared->fatal = status == CVX_BUS_FAULT ? CVVID_FATAL_BUS :
                            status == CVX_DIVIDE_ERROR ? CVVID_FATAL_DIVIDE :
                            CVVID_FATAL_INSTRUCTION;
        return 1;
    }

    f->eax = cpu.gpr[CVX_EAX]; f->ecx = cpu.gpr[CVX_ECX];
    f->edx = cpu.gpr[CVX_EDX]; f->ebx = cpu.gpr[CVX_EBX];
    f->ebp = cpu.gpr[CVX_EBP]; f->esi = cpu.gpr[CVX_ESI];
    f->edi = cpu.gpr[CVX_EDI];
    f->eip = cpu.eip;
    f->eflags = (f->eflags & ~0x8d5UL) | (cpu.eflags & 0x8d5UL);
    ++shared->pm_instructions;
    shared->pm_elements += result.elements;
    return 0;
}

uint8_t cvdpmi_video_port_read(void *shared_block, uint16_t port,
                               uint8_t status1)
{
    cvvid_shared *shared = (cvvid_shared *)shared_block;
    cvga_state *vga;
    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        shared->version != 0x0100 || shared->bytes != CVVID_BLOCK_BYTES ||
        shared->fatal)
        return 0xff;
    vga = (cvga_state *)((uint8_t *)shared + CVVID_MODEL_OFFSET);
    ++shared->pm_port_reads;
    return cvga_read_port(vga, port, status1);
}

void cvdpmi_video_port_write(void *shared_block, uint16_t port, uint8_t value)
{
    cvvid_shared *shared = (cvvid_shared *)shared_block;
    cvga_state *vga;
    uint8_t old_crtc_value = 0;
    int check_crtc_start = 0;
    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        shared->version != 0x0100 || shared->bytes != CVVID_BLOCK_BYTES ||
        shared->fatal)
        return;
    vga = (cvga_state *)((uint8_t *)shared + CVVID_MODEL_OFFSET);
    ++shared->pm_port_writes;
    if ((port == 0x3d5 || port == 0x3b5) &&
        (vga->crtc_index == 0x0c || vga->crtc_index == 0x0d)) {
        old_crtc_value = vga->crtc[vga->crtc_index];
        check_crtc_start = 1;
    }
    cvga_write_port(vga, port, value);
    if (check_crtc_start && old_crtc_value != vga->crtc[vga->crtc_index]) {
        if ((shared->pm_direct & DIRECT_PLANE) && !(shared->pm_direct & DIRECT_RO)) {
            uint32_t *pte = cvdpmi_aperture_ptes();
            if (pte && direct_harvest(shared, vga, pte)) cvdpmi_flush_tlb();
        }
        shared->pm_direct |= DIRECT_FRAME;   /* present after an actual start change */
    }
    /* Recompute direct access only for registers that affect its address,
     * plane or permission, including the ordering phase selected by GC4/SR2. */
    if ((port == 0x3c2) || (port == 0x3c3) ||
        (port == 0x3c5 && (vga->seq_index == 2 || vga->seq_index == 4)) ||
        (port == 0x3cf && (vga->gc_index == 1 || vga->gc_index == 3 ||
                           vga->gc_index == 4 || vga->gc_index == 5 ||
                           vga->gc_index == 6 || vga->gc_index == 8)))
        direct_update(shared, vga, port);
}
