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
#include "vm86_switch.h"
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

/* OPENGEM-022 sentinel — static gates grep for this exact token. */
static const char vm86_console_sentinel[] = "OPENGEM-022";

/*
 * Process-wide console sink pointer. Handlers grab this via
 * vm86_console_sink_attach() ahead of dispatch. When null, the
 * handlers silently drop output — the observability markers still
 * describe the attempted write so the dispatch path remains
 * inspectable.
 */
static vm86_console_sink *g_vm86_console_sink = 0;

void vm86_console_sink_attach(vm86_console_sink *sink) {
    g_vm86_console_sink = sink;
}

void vm86_console_sink_reset(vm86_console_sink *sink) {
    if (!sink) {
        return;
    }
    for (u32 i = 0; i < VM86_CONSOLE_SINK_BYTES; i++) {
        sink->buf[i] = 0;
    }
    sink->count    = 0;
    sink->overflow = 0;
}

static void vm86_console_write_byte(u8 b) {
    vm86_console_sink *s = g_vm86_console_sink;
    if (!s) {
        return;
    }
    if (s->count >= VM86_CONSOLE_SINK_BYTES) {
        s->overflow++;
        return;
    }
    s->buf[s->count++] = b;
}

void vm86_int20_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    task->exit_errorlevel = 0;
    task->exit_reason     = VM86_EXIT_REASON_INT20;
    task->state           = VM86_TASK_STATE_EXITED;
    task->int_count++;
}

void vm86_int21_02_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    /* DL = low byte of EDX on x86. */
    u8 dl = (u8)(frame->edx & 0xFFu);
    vm86_console_write_byte(dl);
    task->int_count++;
}

void vm86_int21_09_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    if (!task->conventional_base || !task->conventional_bytes) {
        task->fault_count++;
        return;
    }
    /*
     * DS:DX in a v8086 guest resolves to a 20-bit linear address
     * (DS << 4) + DX, which is always within the 1 MiB conventional
     * window mapped at task->conventional_base.
     */
    u32 seg    = (u32)frame->ds;
    u32 off    = (u32)(frame->edx & 0xFFFFu);
    u32 linear = (seg << 4) + off;
    if (linear >= task->conventional_bytes) {
        task->fault_count++;
        return;
    }
    u8 *base = (u8 *)task->conventional_base;
    /* Bound the scan to the remainder of the conventional window. */
    u32 max_scan = task->conventional_bytes - linear;
    for (u32 i = 0; i < max_scan; i++) {
        u8 c = base[linear + i];
        if (c == (u8)'$') {
            break;
        }
        vm86_console_write_byte(c);
    }
    task->int_count++;
}

int vm86_console_probe(void) {
    vm86_dispatcher   local;
    vm86_task         task;
    vm86_trap_frame   frame;
    vm86_console_sink sink;
    /* Synthetic conventional-memory window. 64 bytes is enough for
     * the "Hi!$" literal placed at DS:DX below. */
    static u8 convbuf[0x40];

    /* Zero dispatcher. */
    for (u32 i = 0; i < VM86_INT_VECTOR_COUNT; i++) {
        local.handler[i] = 0;
    }
    local.registered_count = 0;
    local.unhandled_count  = 0;
    local.handled_count    = 0;

    /* Zero task + frame. */
    u8 *p = (u8 *)&task;
    for (u32 i = 0; i < sizeof(task); i++) {
        p[i] = 0;
    }
    p = (u8 *)&frame;
    for (u32 i = 0; i < sizeof(frame); i++) {
        p[i] = 0;
    }
    for (u32 i = 0; i < sizeof(convbuf); i++) {
        convbuf[i] = 0;
    }

    (void)vm86_console_sentinel;

    serial_write("vm86: console phase=022 status=planned\n");

    /* Attach sink + wire conventional window. */
    vm86_console_sink_reset(&sink);
    vm86_console_sink_attach(&sink);
    task.handle             = 0x1;
    task.state              = VM86_TASK_STATE_RUNNING;
    task.conventional_base  = (u64)(unsigned long)convbuf;
    task.conventional_bytes = (u32)sizeof(convbuf);

    /* Register AH=02h, AH=09h, INT 20h. */
    int r1 = vm86_register_int_handler(&local, 0x21, 0);  /* placeholder   */
    (void)r1;
    /*
     * OPENGEM-020 guarantees append-only registration. We want both
     * AH=02 and AH=09 multiplexed on vector 0x21, but the dispatcher
     * holds one handler per vector. For the probe we re-dispatch by
     * swapping the handler between invocations — sufficient for
     * observability, and representative of the per-AH demux the
     * real PE trap handler will perform.
     */
    local.handler[0x21]     = 0;      /* clear the placeholder above    */
    local.registered_count  = 0;

    if (!vm86_register_int_handler(&local, 0x20, vm86_int20_handler)) {
        serial_write("vm86: console register-failed vec=0x20\n");
        return 0;
    }
    serial_write("vm86: console registered vec=0x20 handler=int20\n");

    if (!vm86_register_int_handler(&local, 0x21, vm86_int21_02_handler)) {
        serial_write("vm86: console register-failed vec=0x21\n");
        return 0;
    }
    serial_write("vm86: console registered vec=0x21 handler=int21-02\n");

    /* ---- AH=02h: write 'H' ---- */
    frame.edx = 0x0248u;   /* DH=0x02 (ignored), DL=0x48 'H' */
    vm86_dispatch_status s1 = vm86_dispatch_int(&local, &task, &frame, 0x21);
    serial_write("vm86: console ah=02 dl=0x48 status=0x");
    serial_write_hex64((u64)s1);
    serial_write(" sink-count=0x");
    serial_write_hex64((u64)sink.count);
    serial_write("\n");

    /* Swap handler for AH=09h. */
    local.handler[0x21] = vm86_int21_09_handler;

    /* Place "i!$" at convbuf[0x10]; DS=0, DX=0x10 -> linear 0x10. */
    convbuf[0x10] = (u8)'i';
    convbuf[0x11] = (u8)'!';
    convbuf[0x12] = (u8)'$';
    convbuf[0x13] = (u8)'X';   /* guard byte: must NOT reach sink */
    frame.ds  = 0;
    frame.edx = 0x0010u;
    vm86_dispatch_status s2 = vm86_dispatch_int(&local, &task, &frame, 0x21);
    serial_write("vm86: console ah=09 ds:dx=0000:0010 status=0x");
    serial_write_hex64((u64)s2);
    serial_write(" sink-count=0x");
    serial_write_hex64((u64)sink.count);
    serial_write("\n");

    /* ---- INT 20h: terminate ---- */
    vm86_dispatch_status s3 = vm86_dispatch_int(&local, &task, &frame, 0x20);
    serial_write("vm86: console int20 status=0x");
    serial_write_hex64((u64)s3);
    serial_write(" task-state=0x");
    serial_write_hex64((u64)task.state);
    serial_write(" exit-reason=0x");
    serial_write_hex64((u64)task.exit_reason);
    serial_write("\n");

    int ok = (s1 == VM86_DISPATCH_HANDLED)
          && (s2 == VM86_DISPATCH_HANDLED)
          && (s3 == VM86_DISPATCH_EXIT)
          && (task.state == VM86_TASK_STATE_EXITED)
          && (task.exit_reason == VM86_EXIT_REASON_INT20)
          && (task.exit_errorlevel == 0u)
          && (sink.count    == 0x3u)
          && (sink.buf[0]   == (u8)'H')
          && (sink.buf[1]   == (u8)'i')
          && (sink.buf[2]   == (u8)'!')
          && (sink.overflow == 0u);

    serial_write("vm86: console sink-bytes=H,i,! overflow=0x");
    serial_write_hex64((u64)sink.overflow);
    serial_write("\n");

    /* Detach before returning so no stale pointer outlives the probe. */
    vm86_console_sink_attach(0);

    if (ok) {
        serial_write("vm86: console complete\n");
    } else {
        serial_write("vm86: console failed\n");
    }
    return ok ? 1 : 0;
}

/* OPENGEM-023 sentinel — static gates grep for this exact token. */
static const char vm86_int10_0e_sentinel[] = "OPENGEM-023";

void vm86_int10_0e_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    /* AL = low byte of EAX. BH (page), BL (fg colour) ignored. */
    u8 al = (u8)(frame->eax & 0xFFu);
    vm86_console_write_byte(al);
    task->int_count++;
}

int vm86_int10_0e_probe(void) {
    vm86_dispatcher   local;
    vm86_task         task;
    vm86_trap_frame   frame;
    vm86_console_sink sink;

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

    (void)vm86_int10_0e_sentinel;

    serial_write("vm86: int10-0e phase=023 status=planned\n");

    vm86_console_sink_reset(&sink);
    vm86_console_sink_attach(&sink);
    task.handle = 0x1;
    task.state  = VM86_TASK_STATE_RUNNING;

    if (!vm86_register_int_handler(&local, 0x10, vm86_int10_0e_handler)) {
        serial_write("vm86: int10-0e register-failed vec=0x10\n");
        vm86_console_sink_attach(0);
        return 0;
    }

    serial_write("vm86: int10-0e registered vec=0x10 handler=teletype\n");

    /* Stream "OK" via two INT 10h AH=0Eh calls. */
    static const u8 payload[] = { (u8)'O', (u8)'K' };
    vm86_dispatch_status s0 = VM86_DISPATCH_UNHANDLED;
    vm86_dispatch_status s1 = VM86_DISPATCH_UNHANDLED;

    /* i=0 */
    frame.eax = (u32)0x0E00u | (u32)payload[0];
    s0 = vm86_dispatch_int(&local, &task, &frame, 0x10);
    /* i=1 */
    frame.eax = (u32)0x0E00u | (u32)payload[1];
    s1 = vm86_dispatch_int(&local, &task, &frame, 0x10);

    serial_write("vm86: int10-0e stream len=0x");
    serial_write_hex64((u64)sizeof(payload));
    serial_write(" sink-count=0x");
    serial_write_hex64((u64)sink.count);
    serial_write(" last-status=0x");
    serial_write_hex64((u64)s1);
    serial_write("\n");

    int ok = (s0 == VM86_DISPATCH_HANDLED)
          && (s1 == VM86_DISPATCH_HANDLED)
          && (sink.count == 0x2u)
          && (sink.buf[0] == (u8)'O')
          && (sink.buf[1] == (u8)'K')
          && (sink.overflow == 0u)
          && (local.handled_count == 0x2u)
          && (task.int_count == 0x2u);

    serial_write("vm86: int10-0e sink-bytes=O,K handled-count=0x");
    serial_write_hex64((u64)local.handled_count);
    serial_write(" int-count=0x");
    serial_write_hex64((u64)task.int_count);
    serial_write("\n");

    vm86_console_sink_attach(0);

    if (ok) {
        serial_write("vm86: int10-0e complete\n");
    } else {
        serial_write("vm86: int10-0e failed\n");
    }
    return ok ? 1 : 0;
}

/* OPENGEM-024 sentinel — static gates grep for this exact token. */
static const char vm86_gem_t0_sentinel[] = "OPENGEM-024";

void vm86_int21_30_handler(vm86_task *task, vm86_trap_frame *frame) {
    if (!task || !frame) {
        return;
    }
    /*
     * DOS INT 21h AH=30h returns:
     *   AL = major version
     *   AH = minor version
     *   BH = OEM number  (we report 0xFF = generic)
     *   BL:CX = serial number (we report 0)
     * We preserve the upper halves of the 32-bit registers and
     * overwrite only the byte lanes the guest inspects.
     */
    u32 eax = frame->eax;
    eax &= 0xFFFF0000u;
    eax |= ((u32)VM86_DOS_VERSION_MINOR << 8) | (u32)VM86_DOS_VERSION_MAJOR;
    frame->eax = eax;

    u32 ebx = frame->ebx;
    ebx &= 0xFFFF0000u;
    ebx |= 0x0000FF00u;  /* BH = 0xFF generic OEM, BL = 0 */
    frame->ebx = ebx;

    frame->ecx &= 0xFFFF0000u;  /* CX = 0 */

    task->int_count++;
}

int vm86_gem_t0_readiness_probe(void) {
    vm86_dispatcher   local;
    vm86_task         task;
    vm86_trap_frame   frame;
    vm86_console_sink sink;
    /* 256-byte synthetic conventional window holds the banner. */
    static u8 convbuf[0x100];

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
    for (u32 i = 0; i < sizeof(convbuf); i++) {
        convbuf[i] = 0;
    }

    (void)vm86_gem_t0_sentinel;

    serial_write("vm86: gem-t0 phase=024 status=planned\n");

    /*
     * Synthetic gem.exe MZ entry coordinates. The real entry
     * CS:IP and SS:SP are produced by the MZ loader (covered by
     * the OpenGEM MZ-probe stack 013..015). OPENGEM-024 records
     * that the v8086 task descriptor receives them verbatim.
     */
    task.handle             = 0xDE;
    task.state              = VM86_TASK_STATE_RUNNING;
    task.entry_cs           = 0x1000u;
    task.entry_ip           = 0x0100u;
    task.entry_ss           = 0x1000u;
    task.entry_sp           = 0xFFFEu;
    task.conventional_base  = (u64)(unsigned long)convbuf;
    task.conventional_bytes = (u32)sizeof(convbuf);

    serial_write("vm86: gem-t0 entry cs=0x");
    serial_write_hex64((u64)task.entry_cs);
    serial_write(" ip=0x");
    serial_write_hex64((u64)task.entry_ip);
    serial_write(" ss=0x");
    serial_write_hex64((u64)task.entry_ss);
    serial_write(" sp=0x");
    serial_write_hex64((u64)task.entry_sp);
    serial_write("\n");

    /* Place banner "GEM$" at linear 0x80 (DS=0, DX=0x80). */
    convbuf[0x80] = (u8)'G';
    convbuf[0x81] = (u8)'E';
    convbuf[0x82] = (u8)'M';
    convbuf[0x83] = (u8)'$';

    /* Attach a fresh sink. */
    vm86_console_sink_reset(&sink);
    vm86_console_sink_attach(&sink);

    /*
     * Register the full T0 handler set. Vector 0x21 holds a single
     * slot, so the probe rotates the slot across the three AH
     * values used by gem.exe startup. The real PE #GP handler in
     * OPENGEM-025+ will perform an AH-keyed demux on the live
     * trap frame; here the rotation is explicit and observable.
     */
    int reg_int20    = vm86_register_int_handler(&local, 0x20, vm86_int20_handler);
    int reg_int10_0e = vm86_register_int_handler(&local, 0x10, vm86_int10_0e_handler);
    int reg_int21_30 = vm86_register_int_handler(&local, 0x21, vm86_int21_30_handler);

    if (!(reg_int20 && reg_int10_0e && reg_int21_30)) {
        serial_write("vm86: gem-t0 register-failed\n");
        vm86_console_sink_attach(0);
        return 0;
    }

    serial_write("vm86: gem-t0 handlers registered count=0x");
    serial_write_hex64((u64)local.registered_count);
    serial_write(" set={int20,int21-30,int21-09-swap,int21-4c-swap,int10-0e}\n");

    /* ---- INT 21h AH=30h: DOS version query ---- */
    frame.eax = 0x3000u;
    vm86_dispatch_status s_ver = vm86_dispatch_int(&local, &task, &frame, 0x21);
    serial_write("vm86: gem-t0 int21-30 status=0x");
    serial_write_hex64((u64)s_ver);
    serial_write(" al=0x");
    serial_write_hex8((u8)(frame.eax & 0xFFu));
    serial_write(" ah=0x");
    serial_write_hex8((u8)((frame.eax >> 8) & 0xFFu));
    serial_write("\n");

    /* ---- INT 21h AH=09h: banner ---- */
    local.handler[0x21] = vm86_int21_09_handler;
    frame.ds  = 0;
    frame.edx = 0x0080u;
    vm86_dispatch_status s_ban = vm86_dispatch_int(&local, &task, &frame, 0x21);
    serial_write("vm86: gem-t0 int21-09 status=0x");
    serial_write_hex64((u64)s_ban);
    serial_write(" sink-count=0x");
    serial_write_hex64((u64)sink.count);
    serial_write("\n");

    /* ---- INT 10h AH=0Eh: one BIOS char after banner ---- */
    frame.eax = 0x0E21u;   /* AH=0Eh, AL='!' */
    vm86_dispatch_status s_tty = vm86_dispatch_int(&local, &task, &frame, 0x10);
    serial_write("vm86: gem-t0 int10-0e status=0x");
    serial_write_hex64((u64)s_tty);
    serial_write(" sink-count=0x");
    serial_write_hex64((u64)sink.count);
    serial_write("\n");

    /* ---- INT 21h AH=4Ch: clean exit, errorlevel 0 ---- */
    local.handler[0x21] = vm86_int21_4c_handler;
    frame.eax = 0x4C00u;
    vm86_dispatch_status s_exit = vm86_dispatch_int(&local, &task, &frame, 0x21);
    serial_write("vm86: gem-t0 int21-4c status=0x");
    serial_write_hex64((u64)s_exit);
    serial_write(" task-state=0x");
    serial_write_hex64((u64)task.state);
    serial_write(" exit-reason=0x");
    serial_write_hex64((u64)task.exit_reason);
    serial_write(" errorlevel=0x");
    serial_write_hex8((u8)task.exit_errorlevel);
    serial_write("\n");

    int ok = (s_ver  == VM86_DISPATCH_HANDLED)
          && (s_ban  == VM86_DISPATCH_HANDLED)
          && (s_tty  == VM86_DISPATCH_HANDLED)
          && (s_exit == VM86_DISPATCH_EXIT)
          && ((frame.eax & 0xFFu)        == (u32)VM86_DOS_VERSION_MAJOR)
          && (((frame.eax >> 8) & 0xFFu) == (u32)VM86_DOS_VERSION_MINOR)
          && (((frame.ebx >> 8) & 0xFFu) == 0xFFu)
          && (sink.count    == 0x4u)
          && (sink.buf[0]   == (u8)'G')
          && (sink.buf[1]   == (u8)'E')
          && (sink.buf[2]   == (u8)'M')
          && (sink.buf[3]   == (u8)'!')
          && (sink.overflow == 0u)
          && (task.state        == VM86_TASK_STATE_EXITED)
          && (task.exit_reason  == VM86_EXIT_REASON_INT21_4C)
          && (task.exit_errorlevel == 0u)
          && (task.int_count == 0x4u);

    serial_write("vm86: gem-t0 sink-bytes=G,E,M,! int-count=0x");
    serial_write_hex64((u64)task.int_count);
    serial_write("\n");

    /*
     * Readiness summary: enumerate the surface ready for live
     * transport and the surface still pending, so the gate output
     * is auditable after every run.
     */
    serial_write("vm86: gem-t0 ready-surface=int20,int10-0e,int21-02,int21-09,int21-30,int21-4c\n");
    serial_write("vm86: gem-t0 pending-surface=mode-switch,pe32-host,gdt-commit,gp-decode,iret-vm\n");

    vm86_console_sink_attach(0);

    if (ok) {
        serial_write("vm86: gem-t0 complete\n");
    } else {
        serial_write("vm86: gem-t0 failed\n");
    }
    return ok ? 1 : 0;
}

/* OPENGEM-025 sentinel — static gates grep for this exact token. */
static const char vm86_gdt_encoder_sentinel[] = "OPENGEM-025";

/*
 * Write the 8 bytes of a single legacy 32-bit segment descriptor
 * into `dst`. See the header for the field encoding.
 */
static void vm86_gdt_write_slot(u8 *dst,
                                u32 base, u32 limit,
                                u8  access, u8 flags_nibble) {
    dst[0] = (u8)(limit & 0xFFu);
    dst[1] = (u8)((limit >> 8) & 0xFFu);
    dst[2] = (u8)(base & 0xFFu);
    dst[3] = (u8)((base >> 8) & 0xFFu);
    dst[4] = (u8)((base >> 16) & 0xFFu);
    dst[5] = access;
    dst[6] = (u8)(((flags_nibble & 0x0Fu) << 4) | ((limit >> 16) & 0x0Fu));
    dst[7] = (u8)((base >> 24) & 0xFFu);
}

u32 vm86_gdt_encode(u8 *out, u32 tss_base, u16 tss_limit) {
    if (!out) {
        return 0;
    }
    /* Slot 0: NULL — all zeros. */
    for (u32 i = 0; i < 8; i++) {
        out[i] = 0;
    }
    /* Slot 1: PE_CODE32. */
    vm86_gdt_write_slot(out + 1 * 8,
                        0u, 0xFFFFFu,
                        VM86_GDT_AR_CODE32, VM86_GDT_FLAGS_32);
    /* Slot 2: PE_DATA32. */
    vm86_gdt_write_slot(out + 2 * 8,
                        0u, 0xFFFFFu,
                        VM86_GDT_AR_DATA32, VM86_GDT_FLAGS_32);
    /* Slot 3: V86_STACK (separate data selector for the host stack). */
    vm86_gdt_write_slot(out + 3 * 8,
                        0u, 0xFFFFFu,
                        VM86_GDT_AR_DATA32, VM86_GDT_FLAGS_32);
    /* Slot 4: V86_TSS — byte-granular, AR=0x89. */
    vm86_gdt_write_slot(out + 4 * 8,
                        tss_base, (u32)tss_limit,
                        VM86_GDT_AR_TSS32, VM86_GDT_FLAGS_TSS);
    /* Slot 5: RETURN_CODE64 — long-mode code. */
    vm86_gdt_write_slot(out + 5 * 8,
                        0u, 0xFFFFFu,
                        VM86_GDT_AR_CODE64, VM86_GDT_FLAGS_64);
    /* Slot 6: RETURN_DATA64. */
    vm86_gdt_write_slot(out + 6 * 8,
                        0u, 0xFFFFFu,
                        VM86_GDT_AR_DATA32, VM86_GDT_FLAGS_32);
    return VM86_GDT_SLOT_COUNT;
}

u8 vm86_gdt_read_access(const u8 *buf, u32 slot) {
    if (!buf || slot >= (u32)VM86_GDT_SLOT_COUNT) {
        return 0;
    }
    return buf[slot * 8 + 5];
}

u32 vm86_gdt_read_base(const u8 *buf, u32 slot) {
    if (!buf || slot >= (u32)VM86_GDT_SLOT_COUNT) {
        return 0;
    }
    const u8 *d = buf + slot * 8;
    u32 base = (u32)d[2]
             | ((u32)d[3] << 8)
             | ((u32)d[4] << 16)
             | ((u32)d[7] << 24);
    return base;
}

u32 vm86_gdt_read_limit(const u8 *buf, u32 slot) {
    if (!buf || slot >= (u32)VM86_GDT_SLOT_COUNT) {
        return 0;
    }
    const u8 *d = buf + slot * 8;
    u32 limit = (u32)d[0]
              | ((u32)d[1] << 8)
              | (((u32)d[6] & 0x0Fu) << 16);
    return limit;
}

int vm86_gdt_encoder_probe(void) {
    /* Host-owned buffer; NOT installed as a live GDT. */
    static u8 gdt[VM86_GDT_BYTES];
    /* Synthetic TSS base: a fixed, plausible linear address for audit. */
    const u32 tss_base  = 0x00200000u;
    const u16 tss_limit = (u16)(sizeof(vm86_tss32) - 1u);

    (void)vm86_gdt_encoder_sentinel;

    for (u32 i = 0; i < VM86_GDT_BYTES; i++) {
        gdt[i] = 0xFFu;  /* prefill so the encoder must overwrite every byte */
    }

    serial_write("vm86: gdt-encode phase=025 status=planned\n");

    u32 slots = vm86_gdt_encode(gdt, tss_base, tss_limit);

    serial_write("vm86: gdt-encode slots=0x");
    serial_write_hex64((u64)slots);
    serial_write(" bytes=0x");
    serial_write_hex64((u64)VM86_GDT_BYTES);
    serial_write("\n");

    /* ---- Slot 0 (NULL): all zeros. ---- */
    int null_ok = 1;
    for (u32 i = 0; i < 8; i++) {
        if (gdt[i] != 0) {
            null_ok = 0;
        }
    }
    serial_write("vm86: gdt-encode slot0-null ok=0x");
    serial_write_hex8((u8)null_ok);
    serial_write("\n");

    /* ---- Slot 1 (PE_CODE32): base=0 limit=0xFFFFF AR=0x9A flags=0xC. ---- */
    u8  ar1  = vm86_gdt_read_access(gdt, VM86_GDT_PE_CODE32);
    u32 b1   = vm86_gdt_read_base  (gdt, VM86_GDT_PE_CODE32);
    u32 l1   = vm86_gdt_read_limit (gdt, VM86_GDT_PE_CODE32);
    u8  fl1  = (u8)((gdt[VM86_GDT_PE_CODE32 * 8 + 6] >> 4) & 0x0Fu);
    serial_write("vm86: gdt-encode pe-code32 ar=0x");
    serial_write_hex8(ar1);
    serial_write(" base=0x");
    serial_write_hex64((u64)b1);
    serial_write(" limit=0x");
    serial_write_hex64((u64)l1);
    serial_write(" flags=0x");
    serial_write_hex8(fl1);
    serial_write("\n");

    /* ---- Slot 2 (PE_DATA32): base=0 limit=0xFFFFF AR=0x92 flags=0xC. ---- */
    u8  ar2  = vm86_gdt_read_access(gdt, VM86_GDT_PE_DATA32);
    u32 b2   = vm86_gdt_read_base  (gdt, VM86_GDT_PE_DATA32);
    u32 l2   = vm86_gdt_read_limit (gdt, VM86_GDT_PE_DATA32);
    serial_write("vm86: gdt-encode pe-data32 ar=0x");
    serial_write_hex8(ar2);
    serial_write(" base=0x");
    serial_write_hex64((u64)b2);
    serial_write(" limit=0x");
    serial_write_hex64((u64)l2);
    serial_write("\n");

    /* ---- Slot 4 (V86_TSS): base=tss_base limit=tss_limit AR=0x89 flags=0. ---- */
    u8  ar4  = vm86_gdt_read_access(gdt, VM86_GDT_V86_TSS);
    u32 b4   = vm86_gdt_read_base  (gdt, VM86_GDT_V86_TSS);
    u32 l4   = vm86_gdt_read_limit (gdt, VM86_GDT_V86_TSS);
    u8  fl4  = (u8)((gdt[VM86_GDT_V86_TSS * 8 + 6] >> 4) & 0x0Fu);
    serial_write("vm86: gdt-encode v86-tss ar=0x");
    serial_write_hex8(ar4);
    serial_write(" base=0x");
    serial_write_hex64((u64)b4);
    serial_write(" limit=0x");
    serial_write_hex64((u64)l4);
    serial_write(" flags=0x");
    serial_write_hex8(fl4);
    serial_write("\n");

    /* ---- Slot 5 (RETURN_CODE64): flags=0xA (G=1,D=0,L=1). ---- */
    u8  ar5  = vm86_gdt_read_access(gdt, VM86_GDT_RETURN_CODE64);
    u8  fl5  = (u8)((gdt[VM86_GDT_RETURN_CODE64 * 8 + 6] >> 4) & 0x0Fu);
    serial_write("vm86: gdt-encode ret-code64 ar=0x");
    serial_write_hex8(ar5);
    serial_write(" flags=0x");
    serial_write_hex8(fl5);
    serial_write("\n");

    int ok = null_ok
          && (slots == (u32)VM86_GDT_SLOT_COUNT)
          && (ar1 == VM86_GDT_AR_CODE32)
          && (b1  == 0u)
          && (l1  == 0xFFFFFu)
          && (fl1 == VM86_GDT_FLAGS_32)
          && (ar2 == VM86_GDT_AR_DATA32)
          && (b2  == 0u)
          && (l2  == 0xFFFFFu)
          && (ar4 == VM86_GDT_AR_TSS32)
          && (b4  == tss_base)
          && (l4  == (u32)tss_limit)
          && (fl4 == VM86_GDT_FLAGS_TSS)
          && (ar5 == VM86_GDT_AR_CODE64)
          && (fl5 == VM86_GDT_FLAGS_64);

    /*
     * GDTR limit audit: the limit the CPU expects under LGDT is
     * (total_bytes - 1). We never LGDT here; we only record what
     * the value WOULD be so OPENGEM-028 can pick it up verbatim.
     */
    u32 gdtr_limit = VM86_GDT_BYTES - 1u;
    serial_write("vm86: gdt-encode gdtr-limit=0x");
    serial_write_hex64((u64)gdtr_limit);
    serial_write(" gdtr-base=<host-buffer>\n");

    serial_write("vm86: gdt-encode ready-surface=bytes-laid\n");
    serial_write("vm86: gdt-encode pending-surface=lgdt-load,pe32-enter,mode-return\n");

    if (ok) {
        serial_write("vm86: gdt-encode complete\n");
    } else {
        serial_write("vm86: gdt-encode failed\n");
    }
    return ok ? 1 : 0;
}

/* OPENGEM-026 sentinel — static gates grep for this exact token. */
static const char vm86_idt_iret_sentinel[] = "OPENGEM-026";

void vm86_idt_encode_gate(u8 *dst, u32 handler_linear,
                          u16 cs_selector, u8 type_attr) {
    if (!dst) {
        return;
    }
    dst[0] = (u8)(handler_linear & 0xFFu);
    dst[1] = (u8)((handler_linear >> 8) & 0xFFu);
    dst[2] = (u8)(cs_selector & 0xFFu);
    dst[3] = (u8)((cs_selector >> 8) & 0xFFu);
    dst[4] = 0;              /* reserved */
    dst[5] = type_attr;
    dst[6] = (u8)((handler_linear >> 16) & 0xFFu);
    dst[7] = (u8)((handler_linear >> 24) & 0xFFu);
}

u32 vm86_idt_encode(u8 *out,
                    u16 cs_selector,
                    u32 spurious_handler,
                    const u32 *vector_handlers) {
    if (!out || !vector_handlers) {
        return 0;
    }
    /* Fill every slot with the spurious trampoline first. */
    for (u32 v = 0; v < (u32)VM86_IDT_ENTRY_COUNT; v++) {
        vm86_idt_encode_gate(out + v * 8,
                             spurious_handler,
                             cs_selector,
                             VM86_IDT_TYPE_INT32);
    }
    /* Overwrite the known vectors in VM86_IDT_VEC_* enum order. */
    u32 vecs[VM86_IDT_VEC_SLOT_COUNT];
    vecs[0] = VM86_IDT_VEC_DE;
    vecs[1] = VM86_IDT_VEC_UD;
    vecs[2] = VM86_IDT_VEC_NM;
    vecs[3] = VM86_IDT_VEC_TS;
    vecs[4] = VM86_IDT_VEC_NP;
    vecs[5] = VM86_IDT_VEC_SS;
    vecs[6] = VM86_IDT_VEC_GP;
    vecs[7] = VM86_IDT_VEC_PF;
    vecs[8] = VM86_IDT_VEC_SW20;
    vecs[9] = VM86_IDT_VEC_SW21;
    for (u32 i = 0; i < (u32)VM86_IDT_VEC_SLOT_COUNT; i++) {
        vm86_idt_encode_gate(out + vecs[i] * 8,
                             vector_handlers[i],
                             cs_selector,
                             VM86_IDT_TYPE_INT32);
    }
    return (u32)VM86_IDT_ENTRY_COUNT;
}

u32 vm86_idt_read_offset(const u8 *buf, u32 vector) {
    if (!buf || vector >= (u32)VM86_IDT_ENTRY_COUNT) {
        return 0;
    }
    const u8 *d = buf + vector * 8;
    return (u32)d[0]
         | ((u32)d[1] << 8)
         | ((u32)d[6] << 16)
         | ((u32)d[7] << 24);
}

u16 vm86_idt_read_selector(const u8 *buf, u32 vector) {
    if (!buf || vector >= (u32)VM86_IDT_ENTRY_COUNT) {
        return 0;
    }
    const u8 *d = buf + vector * 8;
    return (u16)((u16)d[2] | ((u16)d[3] << 8));
}

u8 vm86_idt_read_type(const u8 *buf, u32 vector) {
    if (!buf || vector >= (u32)VM86_IDT_ENTRY_COUNT) {
        return 0;
    }
    return buf[vector * 8 + 5];
}

/*
 * IRET frame layout (Intel SDM Vol.3A §20.2.1 "Entering Virtual-8086 Mode"):
 *   Offset 0  : EIP (guest)
 *   Offset 4  : CS  (zero-extended, high word zero)
 *   Offset 8  : EFLAGS (VM=1 required)
 *   Offset 12 : ESP (guest)
 *   Offset 16 : SS
 *   Offset 20 : ES
 *   Offset 24 : DS
 *   Offset 28 : FS
 *   Offset 32 : GS
 */
static void vm86_iret_write_dword(u8 *dst, u32 value) {
    dst[0] = (u8)(value & 0xFFu);
    dst[1] = (u8)((value >> 8) & 0xFFu);
    dst[2] = (u8)((value >> 16) & 0xFFu);
    dst[3] = (u8)((value >> 24) & 0xFFu);
}

u32 vm86_iret_encode_frame(u8 *out,
                           u16 cs, u16 ip,
                           u16 ss, u16 sp,
                           u32 eflags,
                           u16 ds, u16 es, u16 fs, u16 gs) {
    if (!out) {
        return 0;
    }
    /*
     * Force the bits the CPU REQUIRES for v86 entry. The caller
     * cannot disable VM=1 or IOPL=3 — doing so would silently
     * turn this into a plain 32-bit IRET and drop the guest into
     * ring-0 PE32 code, which is an unrecoverable mode-switch bug.
     */
    u32 flags = eflags
              | VM86_EFLAGS_VM
              | VM86_EFLAGS_IOPL3
              | VM86_EFLAGS_RESERVED1;
    /* Bit 1 is reserved-and-always-1 per §3.4.3; enforce it. */

    vm86_iret_write_dword(out +  0, (u32)ip);
    vm86_iret_write_dword(out +  4, (u32)cs);
    vm86_iret_write_dword(out +  8, flags);
    vm86_iret_write_dword(out + 12, (u32)sp);
    vm86_iret_write_dword(out + 16, (u32)ss);
    vm86_iret_write_dword(out + 20, (u32)es);
    vm86_iret_write_dword(out + 24, (u32)ds);
    vm86_iret_write_dword(out + 28, (u32)fs);
    vm86_iret_write_dword(out + 32, (u32)gs);
    return VM86_IRET_FRAME_BYTES;
}

static u32 vm86_iret_read_dword(const u8 *buf, u32 off) {
    return (u32)buf[off + 0]
         | ((u32)buf[off + 1] << 8)
         | ((u32)buf[off + 2] << 16)
         | ((u32)buf[off + 3] << 24);
}

u32 vm86_iret_read_eip   (const u8 *buf) { return buf ? vm86_iret_read_dword(buf,  0) : 0; }
u32 vm86_iret_read_cs    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf,  4) : 0; }
u32 vm86_iret_read_eflags(const u8 *buf) { return buf ? vm86_iret_read_dword(buf,  8) : 0; }
u32 vm86_iret_read_esp   (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 12) : 0; }
u32 vm86_iret_read_ss    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 16) : 0; }
u32 vm86_iret_read_es    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 20) : 0; }
u32 vm86_iret_read_ds    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 24) : 0; }
u32 vm86_iret_read_fs    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 28) : 0; }
u32 vm86_iret_read_gs    (const u8 *buf) { return buf ? vm86_iret_read_dword(buf, 32) : 0; }

int vm86_idt_iret_encoder_probe(void) {
    static u8 idt[VM86_IDT_BYTES];
    static u8 iret_frame[VM86_IRET_FRAME_BYTES];

    (void)vm86_idt_iret_sentinel;

    serial_write("vm86: idt-iret phase=026 status=planned\n");

    /* ---- IDT encoding ---- */
    const u16 cs_pe32 = 0x08;   /* PE_CODE32 selector = slot 1 << 3 */
    const u32 spurious_handler = 0xDEAD0000u;
    u32 vector_handlers[VM86_IDT_VEC_SLOT_COUNT];
    vector_handlers[0] = 0x1000u;  /* DE   */
    vector_handlers[1] = 0x1006u;  /* UD   */
    vector_handlers[2] = 0x1007u;  /* NM   */
    vector_handlers[3] = 0x100Au;  /* TS   */
    vector_handlers[4] = 0x100Bu;  /* NP   */
    vector_handlers[5] = 0x100Cu;  /* SS   */
    vector_handlers[6] = 0x100Du;  /* GP — primary v8086 trap         */
    vector_handlers[7] = 0x100Eu;  /* PF                              */
    vector_handlers[8] = 0x1020u;  /* SW20 — INT 20h fast path        */
    vector_handlers[9] = 0x1021u;  /* SW21 — INT 21h fast path        */

    for (u32 i = 0; i < VM86_IDT_BYTES; i++) {
        idt[i] = 0xFFu;  /* prefill — encoder must overwrite every byte */
    }

    u32 entries = vm86_idt_encode(idt, cs_pe32, spurious_handler, vector_handlers);
    serial_write("vm86: idt-iret idt-entries=0x");
    serial_write_hex64((u64)entries);
    serial_write(" bytes=0x");
    serial_write_hex64((u64)VM86_IDT_BYTES);
    serial_write("\n");

    /* Verify a representative unclaimed vector got the spurious handler. */
    u32 spur_off = vm86_idt_read_offset(idt, 0x50);
    u16 spur_sel = vm86_idt_read_selector(idt, 0x50);
    u8  spur_typ = vm86_idt_read_type(idt, 0x50);
    serial_write("vm86: idt-iret spurious vec=0x50 off=0x");
    serial_write_hex64((u64)spur_off);
    serial_write(" sel=0x");
    serial_write_hex64((u64)spur_sel);
    serial_write(" type=0x");
    serial_write_hex8(spur_typ);
    serial_write("\n");

    /* Verify #GP vector (0x0D) got the dedicated GP handler. */
    u32 gp_off = vm86_idt_read_offset(idt, VM86_IDT_VEC_GP);
    u16 gp_sel = vm86_idt_read_selector(idt, VM86_IDT_VEC_GP);
    u8  gp_typ = vm86_idt_read_type(idt, VM86_IDT_VEC_GP);
    serial_write("vm86: idt-iret gp-vec=0x0D off=0x");
    serial_write_hex64((u64)gp_off);
    serial_write(" sel=0x");
    serial_write_hex64((u64)gp_sel);
    serial_write(" type=0x");
    serial_write_hex8(gp_typ);
    serial_write("\n");

    /* Verify INT 21h software vector got its dedicated handler. */
    u32 sw21_off = vm86_idt_read_offset(idt, VM86_IDT_VEC_SW21);
    u32 sw20_off = vm86_idt_read_offset(idt, VM86_IDT_VEC_SW20);
    serial_write("vm86: idt-iret sw-vec sw20=0x");
    serial_write_hex64((u64)sw20_off);
    serial_write(" sw21=0x");
    serial_write_hex64((u64)sw21_off);
    serial_write("\n");

    /* ---- IRET frame encoding ---- */
    for (u32 i = 0; i < VM86_IRET_FRAME_BYTES; i++) {
        iret_frame[i] = 0xFFu;
    }

    /*
     * gem.exe synthetic entry: CS=0x1000 IP=0x0100 SS=0x1000 SP=0xFFFE
     * matches the entry seeded in the OPENGEM-024 readiness probe.
     */
    u32 frame_bytes = vm86_iret_encode_frame(iret_frame,
                                             0x1000u, 0x0100u,
                                             0x1000u, 0xFFFEu,
                                             VM86_EFLAGS_IF,
                                             0x0000u, 0x0000u,
                                             0x0000u, 0x0000u);
    serial_write("vm86: idt-iret frame-bytes=0x");
    serial_write_hex64((u64)frame_bytes);
    serial_write("\n");

    u32 f_eip = vm86_iret_read_eip(iret_frame);
    u32 f_cs  = vm86_iret_read_cs(iret_frame);
    u32 f_efl = vm86_iret_read_eflags(iret_frame);
    u32 f_esp = vm86_iret_read_esp(iret_frame);
    u32 f_ss  = vm86_iret_read_ss(iret_frame);

    serial_write("vm86: idt-iret frame eip=0x");
    serial_write_hex64((u64)f_eip);
    serial_write(" cs=0x");
    serial_write_hex64((u64)f_cs);
    serial_write(" eflags=0x");
    serial_write_hex64((u64)f_efl);
    serial_write(" esp=0x");
    serial_write_hex64((u64)f_esp);
    serial_write(" ss=0x");
    serial_write_hex64((u64)f_ss);
    serial_write("\n");

    int vm_bit    = (f_efl & VM86_EFLAGS_VM)     ? 1 : 0;
    int iopl3     = ((f_efl & VM86_EFLAGS_IOPL3) == VM86_EFLAGS_IOPL3) ? 1 : 0;
    int if_bit    = (f_efl & VM86_EFLAGS_IF)     ? 1 : 0;
    int r1_bit    = (f_efl & VM86_EFLAGS_RESERVED1) ? 1 : 0;

    serial_write("vm86: idt-iret eflags-bits vm=0x");
    serial_write_hex8((u8)vm_bit);
    serial_write(" iopl3=0x");
    serial_write_hex8((u8)iopl3);
    serial_write(" if=0x");
    serial_write_hex8((u8)if_bit);
    serial_write(" r1=0x");
    serial_write_hex8((u8)r1_bit);
    serial_write("\n");

    /*
     * Robustness audit: even when the caller passes eflags=0,
     * VM=1 | IOPL=3 | reserved-1 MUST still be present. A missing
     * VM=1 on IRET would drop the guest into ring-0 PE32 code —
     * an unrecoverable mode-switch bug. We simulate and verify.
     */
    static u8 iret_zero[VM86_IRET_FRAME_BYTES];
    for (u32 i = 0; i < VM86_IRET_FRAME_BYTES; i++) {
        iret_zero[i] = 0;
    }
    vm86_iret_encode_frame(iret_zero,
                           0x1000u, 0x0100u,
                           0x1000u, 0xFFFEu,
                           0u,
                           0u, 0u, 0u, 0u);
    u32 zflags = vm86_iret_read_eflags(iret_zero);
    int zvm    = (zflags & VM86_EFLAGS_VM)    ? 1 : 0;
    int ziopl3 = ((zflags & VM86_EFLAGS_IOPL3) == VM86_EFLAGS_IOPL3) ? 1 : 0;

    serial_write("vm86: idt-iret eflags-force zero-in vm=0x");
    serial_write_hex8((u8)zvm);
    serial_write(" iopl3=0x");
    serial_write_hex8((u8)ziopl3);
    serial_write("\n");

    int ok = (entries == (u32)VM86_IDT_ENTRY_COUNT)
          && (frame_bytes == VM86_IRET_FRAME_BYTES)
          && (spur_off == spurious_handler)
          && (spur_sel == cs_pe32)
          && (spur_typ == VM86_IDT_TYPE_INT32)
          && (gp_off == vector_handlers[6])
          && (gp_sel == cs_pe32)
          && (gp_typ == VM86_IDT_TYPE_INT32)
          && (sw20_off == vector_handlers[8])
          && (sw21_off == vector_handlers[9])
          && (f_eip == 0x0100u)
          && (f_cs  == 0x1000u)
          && (f_esp == 0xFFFEu)
          && (f_ss  == 0x1000u)
          && vm_bit && iopl3 && if_bit && r1_bit
          && zvm && ziopl3;

    /* IDTR limit audit for OPENGEM-028. */
    u32 idtr_limit = VM86_IDT_BYTES - 1u;
    serial_write("vm86: idt-iret idtr-limit=0x");
    serial_write_hex64((u64)idtr_limit);
    serial_write("\n");

    serial_write("vm86: idt-iret ready-surface=idt-bytes,iret-frame,eflags-forced\n");
    serial_write("vm86: idt-iret pending-surface=lidt-load,trap-stubs,iret-exec\n");

    if (ok) {
        serial_write("vm86: idt-iret complete\n");
    } else {
        serial_write("vm86: idt-iret failed\n");
    }
    return ok ? 1 : 0;
}

/* ========================================================================
 * OPENGEM-028 — live-switch plan builder (no LGDT / LIDT / IRET).
 *
 * The plan aggregates caller-owned staged buffers and produces the
 * GDTR / IDTR pseudo-descriptors the compat-mode trampoline will load
 * in OPENGEM-029. It is pure host-side arithmetic with read-only
 * cross-checks on the IRET frame's VM/IOPL bits.
 * ========================================================================
 */

int vm86_live_switch_plan_build(vm86_live_switch_plan *plan,
                                u8 *gdt_buf, u8 *idt_buf,
                                u8 *iret_frame,
                                u32 tss_base, u16 tss_limit,
                                u16 cs_pe32_selector,
                                u16 ss_pe32_selector,
                                u16 tss_selector,
                                u32 guest_entry_cs,
                                u32 guest_entry_ip) {
    if (!plan) return 0;
    plan->sentinel         = VM86_LIVE_PLAN_SENTINEL;
    plan->gdt_buf          = gdt_buf;
    plan->gdt_bytes        = VM86_GDT_BYTES;
    plan->idt_buf          = idt_buf;
    plan->idt_bytes        = VM86_IDT_BYTES;
    plan->iret_frame       = iret_frame;
    plan->iret_frame_bytes = VM86_IRET_FRAME_BYTES;
    plan->tss_base         = tss_base;
    plan->tss_limit        = tss_limit;
    plan->cs_pe32_selector = cs_pe32_selector;
    plan->ss_pe32_selector = ss_pe32_selector;
    plan->tss_selector     = tss_selector;
    plan->guest_entry_cs   = guest_entry_cs;
    plan->guest_entry_ip   = guest_entry_ip;
    plan->flags            = 0;
    plan->gdtr.limit       = 0;
    plan->gdtr.base        = 0;
    plan->idtr.limit       = 0;
    plan->idtr.base        = 0;

    if (!gdt_buf || !idt_buf || !iret_frame) {
        serial_write("vm86: live-plan buffers missing\n");
        return 0;
    }
    plan->flags |= VM86_LIVE_PLAN_F_BUFFERS_PRESENT;

    /*
     * GDTR / IDTR pseudo-descriptors: limit = bytes - 1; base is the
     * linear address of the caller-owned buffer, truncated to 32 bits
     * because the compat-mode LGDT/LIDT expects a 32-bit base.
     */
    plan->gdtr.limit = (u16)(VM86_GDT_BYTES - 1u);
    plan->gdtr.base  = (u32)(u64)(unsigned long)gdt_buf;
    plan->flags |= VM86_LIVE_PLAN_F_GDTR_COMPUTED;

    plan->idtr.limit = (u16)(VM86_IDT_BYTES - 1u);
    plan->idtr.base  = (u32)(u64)(unsigned long)idt_buf;
    plan->flags |= VM86_LIVE_PLAN_F_IDTR_COMPUTED;

    plan->flags |= VM86_LIVE_PLAN_F_IRET_STAGED;

    /*
     * Cross-check the staged IRET frame forces VM=1 and IOPL=3. This is
     * the single safety invariant on which the whole live entry depends:
     * an IRET with VM=0 would silently drop into ring-0 PE32 code
     * instead of the 16-bit guest.
     */
    u32 frame_eflags = vm86_iret_read_eflags(iret_frame);
    int has_vm    = (frame_eflags & VM86_EFLAGS_VM) ? 1 : 0;
    int has_iopl3 = ((frame_eflags & VM86_EFLAGS_IOPL3) == VM86_EFLAGS_IOPL3) ? 1 : 0;
    if (has_vm && has_iopl3) {
        plan->flags |= VM86_LIVE_PLAN_F_VM_BIT_VERIFIED;
    }

    u32 required = VM86_LIVE_PLAN_F_BUFFERS_PRESENT
                 | VM86_LIVE_PLAN_F_GDTR_COMPUTED
                 | VM86_LIVE_PLAN_F_IDTR_COMPUTED
                 | VM86_LIVE_PLAN_F_IRET_STAGED
                 | VM86_LIVE_PLAN_F_VM_BIT_VERIFIED;
    if ((plan->flags & required) == required) {
        plan->flags |= VM86_LIVE_PLAN_F_READY;
    }

    serial_write("vm86: live-plan begin sentinel=0x");
    serial_write_hex64((u64)plan->sentinel);
    serial_write("\n");
    serial_write("vm86: live-plan gdtr limit=0x");
    serial_write_hex64((u64)plan->gdtr.limit);
    serial_write(" base=0x");
    serial_write_hex64((u64)plan->gdtr.base);
    serial_write("\n");
    serial_write("vm86: live-plan idtr limit=0x");
    serial_write_hex64((u64)plan->idtr.limit);
    serial_write(" base=0x");
    serial_write_hex64((u64)plan->idtr.base);
    serial_write("\n");
    serial_write("vm86: live-plan tss base=0x");
    serial_write_hex64((u64)plan->tss_base);
    serial_write(" limit=0x");
    serial_write_hex64((u64)plan->tss_limit);
    serial_write(" sel=0x");
    serial_write_hex64((u64)plan->tss_selector);
    serial_write("\n");
    serial_write("vm86: live-plan selectors cs=0x");
    serial_write_hex64((u64)plan->cs_pe32_selector);
    serial_write(" ss=0x");
    serial_write_hex64((u64)plan->ss_pe32_selector);
    serial_write("\n");
    serial_write("vm86: live-plan guest cs=0x");
    serial_write_hex64((u64)plan->guest_entry_cs);
    serial_write(" ip=0x");
    serial_write_hex64((u64)plan->guest_entry_ip);
    serial_write("\n");
    serial_write("vm86: live-plan flags=0x");
    serial_write_hex64((u64)plan->flags);
    serial_write((plan->flags & VM86_LIVE_PLAN_F_READY) ? " ready=1\n" : " ready=0\n");

    return (plan->flags & VM86_LIVE_PLAN_F_READY) ? 1 : 0;
}

u32 vm86_live_switch_plan_flags(const vm86_live_switch_plan *plan) {
    return plan ? plan->flags : 0u;
}
u16 vm86_live_switch_plan_gdtr_limit(const vm86_live_switch_plan *plan) {
    return plan ? plan->gdtr.limit : (u16)0;
}
u32 vm86_live_switch_plan_gdtr_base(const vm86_live_switch_plan *plan) {
    return plan ? plan->gdtr.base : 0u;
}
u16 vm86_live_switch_plan_idtr_limit(const vm86_live_switch_plan *plan) {
    return plan ? plan->idtr.limit : (u16)0;
}
u32 vm86_live_switch_plan_idtr_base(const vm86_live_switch_plan *plan) {
    return plan ? plan->idtr.base : 0u;
}

int vm86_live_switch_plan_probe(void) {
    serial_write("vm86: live-plan probe begin OPENGEM-028\n");

    /* Static host buffers (zeroed explicitly, no array initializer). */
    static u8 gdt[VM86_GDT_BYTES];
    static u8 idt[VM86_IDT_BYTES];
    static u8 iret[VM86_IRET_FRAME_BYTES];
    for (u32 i = 0; i < VM86_GDT_BYTES; i++)       gdt[i]  = 0;
    for (u32 i = 0; i < VM86_IDT_BYTES; i++)       idt[i]  = 0;
    for (u32 i = 0; i < VM86_IRET_FRAME_BYTES; i++) iret[i] = 0;

    /* Stage descriptors using 025/026 encoders. */
    u32 tss_base  = 0x00200000u;
    u16 tss_limit = (u16)(104u - 1u);
    u32 gdt_bytes_written = vm86_gdt_encode(gdt, tss_base, tss_limit);
    if (gdt_bytes_written != VM86_GDT_BYTES) {
        serial_write("vm86: live-plan probe gdt-encode-failed\n");
        return 0;
    }

    u16 cs_pe32   = 0x0008u; /* GDT index 1 */
    u16 ss_pe32   = 0x0010u; /* GDT index 2 */
    u16 tss_sel   = 0x0020u; /* GDT index 4 */

    /* Dummy handler addresses; live switch replaces them in 029. */
    u32 vector_handlers[VM86_IDT_VEC_SLOT_COUNT];
    for (u32 i = 0; i < VM86_IDT_VEC_SLOT_COUNT; i++) {
        vector_handlers[i] = 0x00100000u + (i * 0x20u);
    }
    u32 spurious = 0x00100800u;
    u32 idt_entries = vm86_idt_encode(idt, cs_pe32, spurious, vector_handlers);
    if (idt_entries != (u32)VM86_IDT_ENTRY_COUNT) {
        serial_write("vm86: live-plan probe idt-encode-failed\n");
        return 0;
    }

    u32 iret_bytes = vm86_iret_encode_frame(iret,
                                            0x1000u, 0x0100u,
                                            0x1000u, 0xFFFEu,
                                            0u,
                                            0u, 0u, 0u, 0u);
    if (iret_bytes != VM86_IRET_FRAME_BYTES) {
        serial_write("vm86: live-plan probe iret-encode-failed\n");
        return 0;
    }

    /* Build the plan. */
    vm86_live_switch_plan plan;
    plan.sentinel = 0;
    int built = vm86_live_switch_plan_build(&plan,
                                            gdt, idt, iret,
                                            tss_base, tss_limit,
                                            cs_pe32, ss_pe32, tss_sel,
                                            0x1000u, 0x0100u);
    if (!built) {
        serial_write("vm86: live-plan probe build-failed\n");
        return 0;
    }

    int ok = 1;
    if (plan.sentinel != VM86_LIVE_PLAN_SENTINEL) ok = 0;
    if (plan.gdt_bytes != VM86_GDT_BYTES) ok = 0;
    if (plan.idt_bytes != VM86_IDT_BYTES) ok = 0;
    if (plan.iret_frame_bytes != VM86_IRET_FRAME_BYTES) ok = 0;
    if (plan.gdtr.limit != (u16)(VM86_GDT_BYTES - 1u)) ok = 0;
    if (plan.idtr.limit != (u16)(VM86_IDT_BYTES - 1u)) ok = 0;
    if (plan.gdtr.base != (u32)(u64)(unsigned long)gdt) ok = 0;
    if (plan.idtr.base != (u32)(u64)(unsigned long)idt) ok = 0;
    if (plan.cs_pe32_selector != cs_pe32) ok = 0;
    if (plan.ss_pe32_selector != ss_pe32) ok = 0;
    if (plan.tss_selector     != tss_sel) ok = 0;
    if (plan.tss_base         != tss_base) ok = 0;
    if (plan.tss_limit        != tss_limit) ok = 0;
    if (plan.guest_entry_cs   != 0x1000u) ok = 0;
    if (plan.guest_entry_ip   != 0x0100u) ok = 0;
    if (!(plan.flags & VM86_LIVE_PLAN_F_READY)) ok = 0;
    if (!(plan.flags & VM86_LIVE_PLAN_F_VM_BIT_VERIFIED)) ok = 0;

    /* Verify read-back helpers agree with the direct field reads. */
    if (vm86_live_switch_plan_flags(&plan)      != plan.flags)      ok = 0;
    if (vm86_live_switch_plan_gdtr_limit(&plan) != plan.gdtr.limit) ok = 0;
    if (vm86_live_switch_plan_gdtr_base(&plan)  != plan.gdtr.base)  ok = 0;
    if (vm86_live_switch_plan_idtr_limit(&plan) != plan.idtr.limit) ok = 0;
    if (vm86_live_switch_plan_idtr_base(&plan)  != plan.idtr.base)  ok = 0;

    /* Null-guard sanity. */
    if (vm86_live_switch_plan_flags(0)      != 0u) ok = 0;
    if (vm86_live_switch_plan_gdtr_limit(0) != 0)  ok = 0;
    if (vm86_live_switch_plan_gdtr_base(0)  != 0u) ok = 0;
    if (vm86_live_switch_plan_idtr_limit(0) != 0)  ok = 0;
    if (vm86_live_switch_plan_idtr_base(0)  != 0u) ok = 0;

    /* Contract: a plan built with NULL buffers must fail. */
    vm86_live_switch_plan bad;
    bad.sentinel = 0;
    int bad_built = vm86_live_switch_plan_build(&bad, 0, idt, iret,
                                                tss_base, tss_limit,
                                                cs_pe32, ss_pe32, tss_sel,
                                                0x1000u, 0x0100u);
    if (bad_built) ok = 0;
    if (bad.flags & VM86_LIVE_PLAN_F_READY) ok = 0;

    serial_write("vm86: live-plan probe ready-surface=buffers,gdtr,idtr,iret,vm-bit\n");
    serial_write("vm86: live-plan probe pending-surface=lgdt-exec,lidt-exec,iret-exec,gp-decode\n");
    if (ok) {
        serial_write("vm86: live-plan probe complete\n");
    } else {
        serial_write("vm86: live-plan probe failed\n");
    }
    return ok ? 1 : 0;
}

/* ========================================================================
 * OPENGEM-029 — armed-but-gated live-switch execute path.
 *
 * A single module-private flag guards the only C call site that
 * invokes the vm86_switch.S trampolines. Default = 0 (disarmed).
 * Default is never flipped implicitly — only vm86_live_switch_arm()
 * with the magic value AND a ready plan flips it.
 *
 * While the trampoline bodies remain OPENGEM-027 stubs (retq), invoking
 * them is a CPU-level no-op. This phase proves the gating contract
 * without yet running any guest code.
 * ========================================================================
 */

static int                            vm86_live_switch_armed_flag = 0;
static const vm86_live_switch_plan   *vm86_live_switch_armed_plan = 0;

int vm86_live_switch_arm(u32 magic, const vm86_live_switch_plan *plan) {
    if (magic != VM86_LIVE_ARM_MAGIC) {
        serial_write("vm86: live-switch arm rejected reason=bad-magic\n");
        return 0;
    }
    if (!plan) {
        serial_write("vm86: live-switch arm rejected reason=null-plan\n");
        return 0;
    }
    if (!(plan->flags & VM86_LIVE_PLAN_F_READY)) {
        serial_write("vm86: live-switch arm rejected reason=plan-not-ready\n");
        return 0;
    }
    vm86_live_switch_armed_plan = plan;
    vm86_live_switch_armed_flag = 1;
    serial_write("vm86: live-switch armed magic=ok plan-ready=1\n");
    return 1;
}

void vm86_live_switch_disarm(void) {
    vm86_live_switch_armed_flag = 0;
    vm86_live_switch_armed_plan = 0;
    serial_write("vm86: live-switch disarmed\n");
}

int vm86_live_switch_is_armed(void) {
    return vm86_live_switch_armed_flag ? 1 : 0;
}

const vm86_live_switch_plan *vm86_live_switch_get_plan(void) {
    return vm86_live_switch_armed_plan;
}

vm86_live_exec_status vm86_live_switch_execute(void) {
    if (!vm86_live_switch_armed_flag) {
        serial_write("vm86: live-switch execute blocked reason=not-armed\n");
        return VM86_LIVE_EXEC_BLOCKED_NOT_ARMED;
    }
    if (!vm86_live_switch_armed_plan) {
        serial_write("vm86: live-switch execute blocked reason=no-plan\n");
        return VM86_LIVE_EXEC_BLOCKED_NO_PLAN;
    }
    if (!(vm86_live_switch_armed_plan->flags & VM86_LIVE_PLAN_F_READY)) {
        serial_write("vm86: live-switch execute blocked reason=bad-plan\n");
        return VM86_LIVE_EXEC_BLOCKED_BAD_PLAN;
    }

    /*
     * Armed-but-stubbed path. Each call below is still a retq at
     * OPENGEM-027; no CPU state changes. When the trampolines gain
     * real bodies in a later phase, this site will already be wired.
     */
    serial_write("vm86: live-switch execute begin mode=stub\n");
    serial_write("vm86: live-switch execute long-to-pe32\n");
    vm86_switch_long_to_pe32();
    serial_write("vm86: live-switch execute enter-v86\n");
    vm86_switch_enter_v86_via_iret();
    serial_write("vm86: live-switch execute pe32-to-long\n");
    vm86_switch_pe32_to_long();
    serial_write("vm86: live-switch execute complete mode=stub stubs=3\n");
    return VM86_LIVE_EXEC_INVOKED_STUBS;
}

int vm86_live_switch_arm_probe(void) {
    serial_write("vm86: live-switch arm-probe begin OPENGEM-029\n");

    vm86_live_switch_disarm();
    if (vm86_live_switch_is_armed()) return 0;
    if (vm86_live_switch_get_plan()) return 0;

    int ok = 1;

    vm86_live_exec_status s1 = vm86_live_switch_execute();
    if (s1 != VM86_LIVE_EXEC_BLOCKED_NOT_ARMED) ok = 0;

    vm86_live_switch_plan plan;
    plan.sentinel = 0;
    plan.flags    = 0;
    int r2 = vm86_live_switch_arm(0xDEADBEEFu, &plan);
    if (r2) ok = 0;
    if (vm86_live_switch_is_armed()) ok = 0;

    int r3 = vm86_live_switch_arm(VM86_LIVE_ARM_MAGIC, 0);
    if (r3) ok = 0;
    if (vm86_live_switch_is_armed()) ok = 0;

    int r4 = vm86_live_switch_arm(VM86_LIVE_ARM_MAGIC, &plan);
    if (r4) ok = 0;
    if (vm86_live_switch_is_armed()) ok = 0;

    static u8 gdt_p29[VM86_GDT_BYTES];
    static u8 idt_p29[VM86_IDT_BYTES];
    static u8 iret_p29[VM86_IRET_FRAME_BYTES];
    for (u32 i = 0; i < VM86_GDT_BYTES; i++)        gdt_p29[i]  = 0;
    for (u32 i = 0; i < VM86_IDT_BYTES; i++)        idt_p29[i]  = 0;
    for (u32 i = 0; i < VM86_IRET_FRAME_BYTES; i++) iret_p29[i] = 0;
    vm86_gdt_encode(gdt_p29, 0x00200000u, (u16)(104u - 1u));
    u32 vh[VM86_IDT_VEC_SLOT_COUNT];
    for (u32 i = 0; i < VM86_IDT_VEC_SLOT_COUNT; i++) vh[i] = 0x00100000u + (i * 0x20u);
    vm86_idt_encode(idt_p29, 0x0008u, 0x00100800u, vh);
    vm86_iret_encode_frame(iret_p29, 0x1000u, 0x0100u, 0x1000u, 0xFFFEu, 0u, 0u, 0u, 0u, 0u);

    vm86_live_switch_plan ready;
    ready.sentinel = 0;
    int built = vm86_live_switch_plan_build(&ready, gdt_p29, idt_p29, iret_p29,
                                            0x00200000u, (u16)(104u - 1u),
                                            0x0008u, 0x0010u, 0x0020u,
                                            0x1000u, 0x0100u);
    if (!built) ok = 0;
    if (!(ready.flags & VM86_LIVE_PLAN_F_READY)) ok = 0;

    int r5 = vm86_live_switch_arm(VM86_LIVE_ARM_MAGIC, &ready);
    if (!r5) ok = 0;
    if (!vm86_live_switch_is_armed()) ok = 0;
    if (vm86_live_switch_get_plan() != &ready) ok = 0;

    vm86_live_exec_status s6 = vm86_live_switch_execute();
    if (s6 != VM86_LIVE_EXEC_INVOKED_STUBS) ok = 0;

    vm86_live_switch_disarm();
    if (vm86_live_switch_is_armed()) ok = 0;
    if (vm86_live_switch_get_plan()) ok = 0;
    vm86_live_exec_status s7 = vm86_live_switch_execute();
    if (s7 != VM86_LIVE_EXEC_BLOCKED_NOT_ARMED) ok = 0;

    if (vm86_switch_stub_sentinel != VM86_SWITCH_SENTINEL) ok = 0;

    serial_write("vm86: live-switch arm-probe ready-surface=arm,disarm,execute-gate\n");
    serial_write("vm86: live-switch arm-probe pending-surface=trampoline-bodies,real-lgdt,real-iret\n");
    if (ok) {
        serial_write("vm86: live-switch arm-probe complete\n");
    } else {
        serial_write("vm86: live-switch arm-probe failed\n");
    }
    return ok ? 1 : 0;
}
