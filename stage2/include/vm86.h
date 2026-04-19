/*
 * vm86.h — OPENGEM-016 16-bit execution layer scaffolding.
 *
 * This header defines the ABI types for the v8086 monitor described in
 * docs/opengem-016-design.md. Implementation is currently scaffolding
 * only: no real mode-switch is performed, no guest task is entered.
 * Every function here is observability-only and safe to call from
 * long-mode host context.
 *
 * Phases:
 *   OPENGEM-017: mode-switch scaffold (this file, initial drop).
 *   OPENGEM-018: descriptor table shim data (GDT, TSS, IDT).
 *   OPENGEM-019: VM task descriptor.
 *   OPENGEM-020: INT dispatcher skeleton.
 *
 * Nothing in this header is called from the live boot path yet.
 */
#ifndef STAGE2_VM86_H
#define STAGE2_VM86_H

#include "types.h"

/*
 * Execution mode identifiers used by the three-level architecture
 * described in docs/opengem-016-design.md §5.1.
 */
typedef enum {
    VM86_MODE_HOST_LONG       = 1,  /* stage2 long-mode host         */
    VM86_MODE_COMPAT_PE32     = 2,  /* 32-bit protected-mode bridge  */
    VM86_MODE_GUEST_V8086     = 3   /* virtual-8086 guest            */
} vm86_mode_id;

/*
 * Trap frame exchanged between the PE compatibility host and the
 * INT dispatcher. Layout matches design §5.2.
 *
 * This structure is intentionally 32-bit register width even though
 * the guest is 16-bit: the v8086 task always exposes 32-bit register
 * state to the host (EAX/EBX/…) because the CPU preserves the upper
 * halves across the mode boundary.
 */
typedef struct vm86_trap_frame {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 esi;
    u32 edi;
    u32 ebp;
    u32 esp;
    u32 eip;
    u32 eflags;
    u16 cs;
    u16 ds;
    u16 es;
    u16 fs;
    u16 gs;
    u16 ss;
} vm86_trap_frame;

/*
 * OPENGEM-017 scaffold probe. Emits the four observability markers
 * defined for this phase. Returns 1 on success, 0 on failure.
 */
int vm86_scaffold_probe(void);

/*
 * OPENGEM-018 — descriptor table shim.
 *
 * These types describe the GDT / 32-bit TSS / IDT shim entries that
 * the mode-switch subsystem will populate. Data structures only; no
 * descriptor is installed into the live GDT yet.
 */

/* 8-byte GDT entry (legacy 32-bit segment descriptor layout). */
typedef struct vm86_gdt_entry {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;        /* P, DPL, S, type                */
    u8  limit_hi_flags;/* G, D/B, L, AVL, limit[19:16]   */
    u8  base_hi;
} vm86_gdt_entry;

/* 32-bit TSS layout used for the v8086 host task. */
typedef struct vm86_tss32 {
    u32 link;        /* prev TSS selector (16-bit in low word) */
    u32 esp0;
    u32 ss0;
    u32 esp1;
    u32 ss1;
    u32 esp2;
    u32 ss2;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax;
    u32 ecx;
    u32 edx;
    u32 ebx;
    u32 esp;
    u32 ebp;
    u32 esi;
    u32 edi;
    u32 es;
    u32 cs;
    u32 ss;
    u32 ds;
    u32 fs;
    u32 gs;
    u32 ldt;
    u32 iopb_trap;   /* T bit in low word, IOPB offset in high word */
} vm86_tss32;

/* 8-byte IDT gate (interrupt / trap gate, 32-bit). */
typedef struct vm86_idt_gate {
    u16 offset_lo;
    u16 selector;
    u8  reserved;
    u8  type_attr;
    u16 offset_hi;
} vm86_idt_gate;

/*
 * GDT slot plan. These indices are the contract consumed by the
 * mode-switch routine (OPENGEM-019+). The live long-mode GDT is
 * untouched; this table describes a SEPARATE descriptor set that
 * the compat PE host will load via LGDT during the transition.
 */
enum {
    VM86_GDT_NULL           = 0,
    VM86_GDT_PE_CODE32      = 1,  /* compat-mode ring 0 code (32-bit PE) */
    VM86_GDT_PE_DATA32      = 2,  /* compat-mode ring 0 data             */
    VM86_GDT_V86_STACK      = 3,  /* host stack segment                  */
    VM86_GDT_V86_TSS        = 4,  /* TSS descriptor for v86 task         */
    VM86_GDT_RETURN_CODE64  = 5,  /* long-mode code for return           */
    VM86_GDT_RETURN_DATA64  = 6,
    VM86_GDT_SLOT_COUNT     = 7
};

/* IDT slot plan — only the vectors the dispatcher cares about. */
enum {
    VM86_IDT_VEC_DE   = 0x00,   /* divide error                   */
    VM86_IDT_VEC_UD   = 0x06,   /* invalid opcode                 */
    VM86_IDT_VEC_NM   = 0x07,   /* device not available           */
    VM86_IDT_VEC_TS   = 0x0A,   /* invalid TSS                    */
    VM86_IDT_VEC_NP   = 0x0B,   /* segment not present            */
    VM86_IDT_VEC_SS   = 0x0C,   /* stack-segment fault            */
    VM86_IDT_VEC_GP   = 0x0D,   /* #GP — primary v8086 trap       */
    VM86_IDT_VEC_PF   = 0x0E,   /* #PF                            */
    VM86_IDT_VEC_SW20 = 0x20,   /* INT 20h — DOS program terminate*/
    VM86_IDT_VEC_SW21 = 0x21,   /* INT 21h — DOS services         */
    VM86_IDT_VEC_SLOT_COUNT = 10
};

/*
 * OPENGEM-018 descriptor shim probe. Emits the descriptor-layout
 * markers defined for this phase. Returns 1 on success, 0 on
 * failure.
 */
int vm86_descriptors_probe(void);

/*
 * OPENGEM-019 — VM task descriptor.
 *
 * Describes a single v8086 guest task that the monitor will host.
 * No task is allocated or entered yet; this structure is the ABI
 * between the mode-switch routine (future OPENGEM-021+) and the
 * INT dispatcher (OPENGEM-020).
 */

typedef enum {
    VM86_TASK_STATE_IDLE      = 0,
    VM86_TASK_STATE_READY     = 1,
    VM86_TASK_STATE_RUNNING   = 2,
    VM86_TASK_STATE_INT_TRAP  = 3,
    VM86_TASK_STATE_FAULTED   = 4,
    VM86_TASK_STATE_EXITED    = 5,
    VM86_TASK_STATE_COUNT     = 6
} vm86_task_state;

typedef enum {
    VM86_EXIT_REASON_NONE      = 0,
    VM86_EXIT_REASON_INT20     = 1,  /* INT 20h terminate              */
    VM86_EXIT_REASON_INT21_4C  = 2,  /* INT 21h AH=4Ch terminate       */
    VM86_EXIT_REASON_FAULT     = 3,  /* unhandled protection fault     */
    VM86_EXIT_REASON_HOST_ABORT = 4, /* host requested shutdown        */
    VM86_EXIT_REASON_COUNT     = 5
} vm86_exit_reason;

/*
 * v8086 guest task descriptor. All register fields are 32-bit wide
 * to match the trap frame convention. Segment registers are 16-bit
 * as they are on the CPU. The conventional-memory window is a host
 * pointer into the 1 MiB region mapped into the guest.
 */
typedef struct vm86_task {
    u32 handle;              /* opaque id, 0 = invalid                 */
    u32 state;               /* vm86_task_state                        */
    u32 exit_reason;         /* vm86_exit_reason, meaningful on EXITED */
    u32 exit_errorlevel;     /* AL at INT 21h AH=4Ch, or 0             */

    /* Entry point declared by the MZ header / COM convention. */
    u16 entry_cs;
    u16 entry_ip;
    u16 entry_ss;
    u16 entry_sp;

    /* Live register snapshot at last exit / at entry. */
    vm86_trap_frame regs;

    /* Host-side window mapped at guest linear 0x00000..0xFFFFF. */
    u64 conventional_base;   /* host virtual address                   */
    u32 conventional_bytes;  /* always 0x100000 for baseline target    */

    /* Observability. */
    u32 int_count;           /* total INT N traps serviced             */
    u32 fault_count;         /* total #GP / #PF / #UD traps            */
} vm86_task;

/*
 * OPENGEM-019 VM task probe. Emits task-layout markers and verifies
 * the descriptor is laid out within the bounds expected by the
 * dispatcher. Returns 1 on success, 0 on failure.
 */
int vm86_task_probe(void);

/*
 * OPENGEM-020 — INT dispatcher skeleton.
 *
 * Design §5.2 specifies that all guest INT N instructions trap via
 * #GP into the PE compatibility host, which decodes the vector and
 * hands the trap frame to a host-side handler table indexed by the
 * 8-bit vector number. This phase provides:
 *   - the handler prototype,
 *   - the 256-slot table declaration,
 *   - a registration API,
 *   - a "decode and dispatch" entry point that future phases will
 *     wire into the real #GP handler once mode-switching lands.
 * No handler is registered in this phase; the table is entirely
 * empty and the dispatcher is not invoked from the live boot path.
 */

typedef void (*vm86_int_handler)(vm86_task *task, vm86_trap_frame *frame);

#define VM86_INT_VECTOR_COUNT  0x100

typedef enum {
    VM86_DISPATCH_UNHANDLED = 0,
    VM86_DISPATCH_HANDLED   = 1,
    VM86_DISPATCH_EXIT      = 2,  /* handler requested task exit  */
    VM86_DISPATCH_FAULT     = 3   /* handler reported a fault     */
} vm86_dispatch_status;

typedef struct vm86_dispatcher {
    vm86_int_handler handler[VM86_INT_VECTOR_COUNT];
    u32 registered_count;
    u32 unhandled_count;
    u32 handled_count;
} vm86_dispatcher;

/* Installs a handler for vector `vec`. Returns 1 on success. */
int vm86_register_int_handler(vm86_dispatcher *d, u8 vec, vm86_int_handler h);

/* Dispatches a decoded INT N into the handler table. */
vm86_dispatch_status vm86_dispatch_int(vm86_dispatcher *d,
                                       vm86_task *task,
                                       vm86_trap_frame *frame,
                                       u8 vec);

/*
 * OPENGEM-020 dispatcher probe. Allocates a local dispatcher on
 * the host stack (no live state is modified), exercises the
 * registration and dispatch paths with an empty table, and emits
 * the dispatcher-layout markers. Returns 1 on success, 0 on
 * failure.
 */
int vm86_dispatcher_probe(void);

/*
 * OPENGEM-021 — INT 21h AH=4Ch terminate handler.
 *
 * First real INT handler. When the guest issues `MOV AH,4Ch; MOV
 * AL,errorlevel; INT 21h`, the v8086 monitor must:
 *   1. read AL from the trap frame;
 *   2. write it into task->exit_errorlevel;
 *   3. set task->exit_reason = VM86_EXIT_REASON_INT21_4C;
 *   4. set task->state = VM86_TASK_STATE_EXITED.
 *
 * The dispatcher inspects task->state after handler invocation and
 * reports VM86_DISPATCH_EXIT / VM86_DISPATCH_FAULT accordingly (see
 * extension documented in vm86.c). No live task is entered yet; this
 * phase wires only the handler and its tests.
 */
void vm86_int21_4c_handler(vm86_task *task, vm86_trap_frame *frame);

/*
 * OPENGEM-021 probe. Builds a local dispatcher + task, registers
 * the INT 21h AH=4Ch handler, simulates the guest INT and verifies
 * the post-handler state transition. Returns 1 on success.
 */
int vm86_int21_4c_probe(void);

/*
 * OPENGEM-022 — INT 20h terminate + INT 21h AH=02h putchar +
 * INT 21h AH=09h $-terminated string write.
 *
 * Handlers are invoked via the dispatcher. For AH=02h/09h the
 * output is captured into a host-side console sink so the probe can
 * assert byte-level correctness without needing a running TTY.
 * AH=09h walks the guest's conventional memory starting at DS:DX
 * until it finds a '$' terminator (DOS convention) or the sink
 * fills up.
 *
 * The sink is a host-only observability object; real console
 * routing to the CiukiOS services bridge lands in a later phase.
 */

#define VM86_CONSOLE_SINK_BYTES 0x100

typedef struct vm86_console_sink {
    u8  buf[VM86_CONSOLE_SINK_BYTES];
    u32 count;
    u32 overflow;
} vm86_console_sink;

/* Install / clear the process-wide console sink pointer. */
void vm86_console_sink_attach(vm86_console_sink *sink);
void vm86_console_sink_reset(vm86_console_sink *sink);

/* INT 20h terminate (errorlevel = 0, reason = INT20). */
void vm86_int20_handler(vm86_task *task, vm86_trap_frame *frame);

/* INT 21h AH=02h: write DL to stdout. */
void vm86_int21_02_handler(vm86_task *task, vm86_trap_frame *frame);

/* INT 21h AH=09h: write $-terminated string at DS:DX to stdout. */
void vm86_int21_09_handler(vm86_task *task, vm86_trap_frame *frame);

/*
 * OPENGEM-022 probe. Drives all three handlers end-to-end with a
 * host-side conventional memory window and console sink, verifying
 * byte-accurate output plus INT 20h exit semantics. Returns 1 on
 * success.
 */
int vm86_console_probe(void);

/*
 * OPENGEM-023 — INT 10h AH=0Eh teletype (video BIOS write-char).
 *
 * Legacy BIOS teletype output. Writes AL as a character at the
 * current cursor, auto-advances, and wraps at the end of the line.
 * BH (page number) and BL (foreground colour for graphics modes)
 * are ignored by this observability stub; the byte is routed to
 * the same host-side console sink used by INT 21h.
 *
 * This is the third and final write-path handler required before
 * OPENGEM-024 can attempt gem.exe first-INT observability.
 */
void vm86_int10_0e_handler(vm86_task *task, vm86_trap_frame *frame);

/*
 * OPENGEM-023 probe. Streams a short string via repeated INT 10h
 * AH=0Eh invocations and asserts byte-accurate capture in the
 * console sink. Returns 1 on success.
 */
int vm86_int10_0e_probe(void);

/*
 * OPENGEM-024 — gem.exe T0 readiness.
 *
 * The v8086 live transport (long-mode -> PE32 -> v86 mode-switch,
 * GDT/TSS commit, #GP-based INT decode, IRET) is the deliverable
 * of OPENGEM-025+. Before that work begins, this phase assembles
 * every write-side handler that gem.exe needs to reach its first
 * meaningful exit, wires them into a single dispatcher, and
 * simulates the guest INT sequence that a normal gem.exe startup
 * would issue: INT 21h AH=30h (DOS version query), followed by
 * INT 21h AH=09h "GEM$" banner, followed by INT 21h AH=4Ch.
 *
 * The probe asserts that every call through the dispatcher
 * produces the correct task mutation and sink contents. If this
 * readiness probe is green, the only remaining obstacle to T0 is
 * the live transport itself.
 *
 * Adds one new INT handler so the simulation is honest:
 *   vm86_int21_30_handler — INT 21h AH=30h, returns a pinned DOS
 *   version identity in AL:AH. Version pinning is a deliberate
 *   compatibility decision: gem.exe accepts any DOS >= 2.0, and
 *   reporting 5.00 matches the reference environment under which
 *   the symbiotic FreeDOS pipeline is validated.
 */
void vm86_int21_30_handler(vm86_task *task, vm86_trap_frame *frame);

/* Pinned DOS identity returned by vm86_int21_30_handler. */
#define VM86_DOS_VERSION_MAJOR 0x05
#define VM86_DOS_VERSION_MINOR 0x00

/*
 * OPENGEM-024 probe. Builds a full dispatcher with every handler
 * registered (INT 20h, INT 21h AH={02,09,30,4C}, INT 10h AH=0Eh),
 * seeds a task with synthetic gem.exe MZ entry coordinates, and
 * runs the four-INT startup simulation. Returns 1 on success.
 */
int vm86_gem_t0_readiness_probe(void);

#endif /* STAGE2_VM86_H */
