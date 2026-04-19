/*
 * vm86.c — OPENGEM-016 16-bit execution layer scaffolding.
 *
 * OPENGEM-017: mode-switch scaffold. Observability only.
 *
 * No real mode-switch is performed here. The code exists so that:
 *   1. The ABI types in stage2/include/vm86.h are compiled under the
 *      real stage2 toolchain flags and survive -Wall -Wextra.
 *   2. Static gates can assert the presence of the four scaffold
 *      markers described in docs/opengem-016-design.md §5.5.
 *   3. Later phases (OPENGEM-018+) have a host file to extend.
 *
 * Nothing in this translation unit is called from the live boot path
 * yet. The probe function is reachable from test code via extern
 * linkage and remains safe to invoke from long-mode host context.
 */

#include "vm86.h"
#include "serial.h"
#include "types.h"

/* OPENGEM-017 sentinel — static gates grep for this exact token. */
static const char vm86_scaffold_sentinel[] = "OPENGEM-017";

int vm86_scaffold_probe(void) {
    /*
     * Size and alignment self-check. The trap frame is a frozen ABI
     * contract — if the layout changes, downstream phases must update
     * the dispatcher in lockstep.
     */
    const u32 frame_bytes = (u32)sizeof(vm86_trap_frame);

    (void)vm86_scaffold_sentinel;

    serial_write("vm86: scaffold phase=017 status=planned\n");
    serial_write("vm86: scaffold host-mode=long compat-mode=pe32 guest-mode=v8086\n");

    serial_write("vm86: scaffold frame-bytes=0x");
    serial_write_hex64((u64)frame_bytes);
    serial_write("\n");

    serial_write("vm86: scaffold complete\n");
    return 1;
}

/* OPENGEM-018 sentinel — static gates grep for this exact token. */
static const char vm86_descriptors_sentinel[] = "OPENGEM-018";

int vm86_descriptors_probe(void) {
    const u32 gdt_entry_bytes = (u32)sizeof(vm86_gdt_entry);
    const u32 tss32_bytes     = (u32)sizeof(vm86_tss32);
    const u32 idt_gate_bytes  = (u32)sizeof(vm86_idt_gate);
    const u32 gdt_slots       = (u32)VM86_GDT_SLOT_COUNT;
    const u32 idt_slots       = (u32)VM86_IDT_VEC_SLOT_COUNT;

    (void)vm86_descriptors_sentinel;

    serial_write("vm86: descriptors phase=018 status=planned\n");

    serial_write("vm86: descriptors gdt-slots=0x");
    serial_write_hex64((u64)gdt_slots);
    serial_write(" idt-slots=0x");
    serial_write_hex64((u64)idt_slots);
    serial_write("\n");

    serial_write("vm86: descriptors gdt-entry-bytes=0x");
    serial_write_hex64((u64)gdt_entry_bytes);
    serial_write(" tss32-bytes=0x");
    serial_write_hex64((u64)tss32_bytes);
    serial_write(" idt-gate-bytes=0x");
    serial_write_hex64((u64)idt_gate_bytes);
    serial_write("\n");

    serial_write("vm86: descriptors gdt-layout=pe-code32,pe-data32,v86-stack,v86-tss,ret-code64,ret-data64\n");
    serial_write("vm86: descriptors idt-layout=de,ud,nm,ts,np,ss,gp,pf,sw20,sw21\n");

    serial_write("vm86: descriptors complete\n");
    return 1;
}

/* OPENGEM-019 sentinel — static gates grep for this exact token. */
static const char vm86_task_sentinel[] = "OPENGEM-019";

int vm86_task_probe(void) {
    const u32 task_bytes       = (u32)sizeof(vm86_task);
    const u32 state_count      = (u32)VM86_TASK_STATE_COUNT;
    const u32 exit_reason_cnt  = (u32)VM86_EXIT_REASON_COUNT;
    const u32 conv_window_bytes = 0x100000u;

    (void)vm86_task_sentinel;

    serial_write("vm86: task phase=019 status=planned\n");

    serial_write("vm86: task bytes=0x");
    serial_write_hex64((u64)task_bytes);
    serial_write(" state-count=0x");
    serial_write_hex64((u64)state_count);
    serial_write(" exit-reason-count=0x");
    serial_write_hex64((u64)exit_reason_cnt);
    serial_write("\n");

    serial_write("vm86: task fields=handle,state,exit-reason,exit-errorlevel,entry-cs,entry-ip,entry-ss,entry-sp,regs,conventional-base,conventional-bytes,int-count,fault-count\n");

    serial_write("vm86: task states=idle,ready,running,int-trap,faulted,exited\n");
    serial_write("vm86: task exit-reasons=none,int20,int21-4c,fault,host-abort\n");

    serial_write("vm86: task conventional-window-bytes=0x");
    serial_write_hex64((u64)conv_window_bytes);
    serial_write("\n");

    serial_write("vm86: task complete\n");
    return 1;
}
