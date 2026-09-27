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
    for (i = 0; i < 6; ++i) {
        cpu.sreg[i] = selectors[i];
        cpu.seg_base[i] = selectors[i] ? cvdpmi_selector_base(selectors[i]) : 0;
    }
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
    if (!shared || shared->magic != CVVID_SHARED_MAGIC ||
        shared->version != 0x0100 || shared->bytes != CVVID_BLOCK_BYTES ||
        shared->fatal)
        return;
    vga = (cvga_state *)((uint8_t *)shared + CVVID_MODEL_OFFSET);
    ++shared->pm_port_writes;
    cvga_write_port(vga, port, value);
}
