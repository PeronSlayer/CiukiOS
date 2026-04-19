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

/* OPENGEM-020 sentinel — static gates grep for this exact token. */
static const char vm86_dispatcher_sentinel[] = "OPENGEM-020";

int vm86_register_int_handler(vm86_dispatcher *d, u8 vec, vm86_int_handler h) {
    if (!d || !h) {
        return 0;
    }
    if (d->handler[vec]) {
        return 0;   /* already registered; append-only per phase */
    }
    d->handler[vec] = h;
    d->registered_count++;
    return 1;
}

vm86_dispatch_status vm86_dispatch_int(vm86_dispatcher *d,
                                       vm86_task *task,
                                       vm86_trap_frame *frame,
                                       u8 vec) {
    if (!d || !task || !frame) {
        return VM86_DISPATCH_FAULT;
    }
    vm86_int_handler h = d->handler[vec];
    if (!h) {
        d->unhandled_count++;
        return VM86_DISPATCH_UNHANDLED;
    }
    h(task, frame);
    d->handled_count++;
    /*
     * OPENGEM-021 extension: inspect post-handler task state so a
     * handler can promote a plain HANDLED outcome to EXIT (terminate)
     * or FAULT (host abort) without changing its own signature.
     */
    if (task->state == VM86_TASK_STATE_EXITED) {
        return VM86_DISPATCH_EXIT;
    }
    if (task->state == VM86_TASK_STATE_FAULTED) {
        return VM86_DISPATCH_FAULT;
    }
    return VM86_DISPATCH_HANDLED;
}

int vm86_dispatcher_probe(void) {
    /*
     * Build a throwaway dispatcher on the host stack. We never feed
     * it a real task; we verify only the registration/dispatch ABI.
     */
    vm86_dispatcher local;
    vm86_task       dummy_task;
    vm86_trap_frame dummy_frame;

    for (u32 i = 0; i < VM86_INT_VECTOR_COUNT; i++) {
        local.handler[i] = 0;
    }
    local.registered_count = 0;
    local.unhandled_count  = 0;
    local.handled_count    = 0;

    /* Zero the dummies without leaking uninitialized state. */
    u8 *p = (u8 *)&dummy_task;
    for (u32 i = 0; i < sizeof(dummy_task); i++) {
        p[i] = 0;
    }
    p = (u8 *)&dummy_frame;
    for (u32 i = 0; i < sizeof(dummy_frame); i++) {
        p[i] = 0;
    }

    (void)vm86_dispatcher_sentinel;

    serial_write("vm86: dispatcher phase=020 status=planned\n");

    serial_write("vm86: dispatcher vector-count=0x");
    serial_write_hex64((u64)VM86_INT_VECTOR_COUNT);
    serial_write(" slot-bytes=0x");
    serial_write_hex64((u64)sizeof(vm86_int_handler));
    serial_write("\n");

    serial_write("vm86: dispatcher status-codes=unhandled,handled,exit,fault\n");

    /* Unhandled path — verify empty-table behaviour. */
    vm86_dispatch_status s1 = vm86_dispatch_int(&local, &dummy_task, &dummy_frame, 0x21);
    serial_write("vm86: dispatcher empty-probe vec=0x21 status=0x");
    serial_write_hex64((u64)s1);
    serial_write(" unhandled-count=0x");
    serial_write_hex64((u64)local.unhandled_count);
    serial_write("\n");

    serial_write("vm86: dispatcher registered-count=0x");
    serial_write_hex64((u64)local.registered_count);
    serial_write(" handled-count=0x");
    serial_write_hex64((u64)local.handled_count);
    serial_write("\n");

    serial_write("vm86: dispatcher complete\n");
    return (s1 == VM86_DISPATCH_UNHANDLED) ? 1 : 0;
}

/* OPENGEM-021 sentinel — static gates grep for this exact token. */
static const char vm86_int21_4c_sentinel[] = "OPENGEM-021";

void vm86_int21_4c_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    /* AL occupies the low byte of EAX on x86. */
    u8 errorlevel = (u8)(frame->eax & 0xFFu);
    task->exit_errorlevel = (u32)errorlevel;
    task->exit_reason     = VM86_EXIT_REASON_INT21_4C;
    task->state           = VM86_TASK_STATE_EXITED;
    task->int_count++;
}

int vm86_int21_4c_probe(void) {
    vm86_dispatcher local;
    vm86_task       task;
    vm86_trap_frame frame;

    /* Zero state. */
    for (u32 i = 0; i < VM86_INT_VECTOR_COUNT; i++) {
        local.handler[i] = 0;
    }
    local.registered_count = 0;
    local.unhandled_count  = 0;
    local.handled_count    = 0;

    u8 *p = (u8 *)&task;
    for (u32 i = 0; i < sizeof(task); i++) {
        p[i] = 0;
    }
    p = (u8 *)&frame;
    for (u32 i = 0; i < sizeof(frame); i++) {
        p[i] = 0;
    }

    (void)vm86_int21_4c_sentinel;

    serial_write("vm86: int21-4c phase=021 status=planned\n");

    /* Simulate the guest state produced by `MOV AH,4Ch; MOV AL,0x42; INT 21h`. */
    task.handle = 0x1;
    task.state  = VM86_TASK_STATE_RUNNING;
    frame.eax   = 0x4C42u;    /* AH=4Ch, AL=42h */

    if (!vm86_register_int_handler(&local, 0x21, vm86_int21_4c_handler)) {
        serial_write("vm86: int21-4c register-failed\n");
        return 0;
    }

    serial_write("vm86: int21-4c registered vec=0x21 registered-count=0x");
    serial_write_hex64((u64)local.registered_count);
    serial_write("\n");

    serial_write("vm86: int21-4c invoke ah=0x4c al=0x");
    serial_write_hex8((u8)(frame.eax & 0xFFu));
    serial_write("\n");

    vm86_dispatch_status s = vm86_dispatch_int(&local, &task, &frame, 0x21);

    serial_write("vm86: int21-4c post-dispatch status=0x");
    serial_write_hex64((u64)s);
    serial_write(" task-state=0x");
    serial_write_hex64((u64)task.state);
    serial_write(" exit-reason=0x");
    serial_write_hex64((u64)task.exit_reason);
    serial_write(" errorlevel=0x");
    serial_write_hex8((u8)task.exit_errorlevel);
    serial_write("\n");

    int ok = (s == VM86_DISPATCH_EXIT)
          && (task.state == VM86_TASK_STATE_EXITED)
          && (task.exit_reason == VM86_EXIT_REASON_INT21_4C)
          && (task.exit_errorlevel == 0x42u)
          && (local.handled_count == 0x1u);

    if (ok) {
        serial_write("vm86: int21-4c complete\n");
    } else {
        serial_write("vm86: int21-4c failed\n");
    }
    return ok ? 1 : 0;
}
