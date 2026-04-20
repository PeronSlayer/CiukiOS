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

/*
 * OPENGEM-025 — GDT byte-layout encoder (no LGDT).
 *
 * Builds the 7-slot GDT described in §5.1 as an 8-byte-per-slot
 * flat byte stream laid out exactly the way the CPU will read it
 * under LGDT. No CPU register (GDTR) is touched in this phase;
 * the bytes are written to a host-owned buffer and verified field
 * by field. The buffer is the contract consumed by OPENGEM-028.
 *
 * Access byte encoding (Intel SDM Vol.3A §3.4.5):
 *   bit 7: P   (present)
 *   bits 6-5: DPL
 *   bit 4: S   (1 = code/data, 0 = system)
 *   bit 3: E   (1 = code, 0 = data) for S=1
 *   bit 2: DC  (direction/conforming)
 *   bit 1: RW  (readable for code, writable for data)
 *   bit 0: A   (accessed — CPU may set)
 *
 * Flags nibble (high 4 bits of limit_hi_flags):
 *   bit 7: G   (granularity, 1 = 4 KiB pages)
 *   bit 6: D/B (1 = 32-bit default op size)
 *   bit 5: L   (1 = 64-bit long-mode code, only for code segs)
 *   bit 4: AVL
 *
 * Slot plan (mirrors the enum in §5.1):
 *   0: NULL
 *   1: PE_CODE32  base=0 limit=0xFFFFF G=1 D=1 L=0  AR=0x9A
 *   2: PE_DATA32  base=0 limit=0xFFFFF G=1 D=1 L=0  AR=0x92
 *   3: V86_STACK  base=0 limit=0xFFFFF G=1 D=1 L=0  AR=0x92 (separate slot)
 *   4: V86_TSS    base=<tss_base> limit=sizeof(tss)-1 G=0  AR=0x89 (type 9, available 32-bit TSS)
 *   5: RET_CODE64 base=0 limit=0xFFFFF G=1 D=0 L=1  AR=0x9A
 *   6: RET_DATA64 base=0 limit=0xFFFFF G=1 D=1 L=0  AR=0x92
 */
#define VM86_GDT_BYTES  (VM86_GDT_SLOT_COUNT * 8)

/* Standard access bytes (verified bit-exact by the gate). */
#define VM86_GDT_AR_CODE32  0x9A   /* P=1 DPL=0 S=1 type=0xA (code, exec/read) */
#define VM86_GDT_AR_DATA32  0x92   /* P=1 DPL=0 S=1 type=0x2 (data, read/write) */
#define VM86_GDT_AR_TSS32   0x89   /* P=1 DPL=0 S=0 type=0x9 (32-bit TSS avail) */
#define VM86_GDT_AR_CODE64  0x9A   /* same access byte; L flag distinguishes */

/* Flags nibble values placed into limit_hi_flags[7:4]. */
#define VM86_GDT_FLAGS_32   0xC    /* G=1 D=1 L=0 (32-bit compat) */
#define VM86_GDT_FLAGS_64   0xA    /* G=1 D=0 L=1 (64-bit long)   */
#define VM86_GDT_FLAGS_TSS  0x0    /* G=0 D=0 L=0 (byte-granular) */

/*
 * Encode the full 7-slot GDT into `out` (must be at least
 * VM86_GDT_BYTES). Returns the number of slots encoded on success,
 * or 0 if `out` is null. `tss_base` is the linear address the TSS
 * descriptor should point at; `tss_limit` is sizeof(tss)-1.
 */
u32 vm86_gdt_encode(u8 *out, u32 tss_base, u16 tss_limit);

/*
 * Decode the AR byte of slot `slot` from a previously-encoded
 * buffer. Returns the access byte, or 0 if `slot` is out of range.
 */
u8  vm86_gdt_read_access(const u8 *buf, u32 slot);

/*
 * Extract the 32-bit base from slot `slot`. Slots 4 (TSS) and
 * others follow the legacy 8-byte descriptor layout. Returns 0 on
 * error or on a zero-base descriptor (callers must disambiguate
 * via vm86_gdt_read_access).
 */
u32 vm86_gdt_read_base(const u8 *buf, u32 slot);

/*
 * Extract the 20-bit limit from slot `slot`. Returns 0 on error.
 */
u32 vm86_gdt_read_limit(const u8 *buf, u32 slot);

/*
 * OPENGEM-025 probe. Encodes a reference GDT and verifies every
 * field byte-for-byte against the contract above. Returns 1 on
 * success, 0 on failure. Does NOT touch GDTR.
 */
int vm86_gdt_encoder_probe(void);

/*
 * OPENGEM-026 — IDT gate encoder + v86 IRET stack frame encoder.
 *
 * Two independent byte-layout contracts used by the live switch:
 *
 * (1) IDT gate encoder
 *   The 32-bit PE host installs a 256-entry IDT where each entry
 *   is an 8-byte gate descriptor. Most entries are a generic
 *   "spurious" trampoline; the 10 vectors enumerated in the
 *   VM86_IDT_VEC_* enum point to per-vector stubs. This phase
 *   writes the bytes; no IDTR load happens here.
 *
 *   32-bit interrupt-gate layout (Intel SDM Vol.3A §6.11):
 *     offset[15:0], selector[15:0], 0x00, type_attr, offset[31:16]
 *   type_attr for a 32-bit ring-0 interrupt gate: 0x8E
 *     P=1 DPL=0 S=0 type=0xE (32-bit interrupt gate)
 *
 * (2) v86 IRET stack frame encoder
 *   Entering v8086 mode is done by IRET from ring-0 PE32 with a
 *   stack frame containing, top-to-bottom:
 *     [ESP+32] GS
 *     [ESP+28] FS
 *     [ESP+24] DS
 *     [ESP+20] ES
 *     [ESP+16] SS      (guest real-mode SS)
 *     [ESP+12] ESP     (guest real-mode SP, zero-extended)
 *     [ESP+ 8] EFLAGS  (MUST have VM=1 (bit 17), IOPL=3 (bits 13..12))
 *     [ESP+ 4] CS      (guest real-mode CS)
 *     [ESP+ 0] EIP     (guest real-mode IP, zero-extended)
 *   Total = 36 bytes of DWORD-aligned data.
 *   The CPU consumes this frame atomically on IRET; any byte-wrong
 *   field is a mode-switch-level hazard.
 */
#define VM86_IDT_ENTRY_COUNT   256
#define VM86_IDT_BYTES         (VM86_IDT_ENTRY_COUNT * 8)

#define VM86_IDT_TYPE_INT32    0x8E   /* P=1 DPL=0 S=0 type=0xE */
#define VM86_IDT_TYPE_TRAP32   0x8F   /* P=1 DPL=0 S=0 type=0xF */

/* v86 IRET frame sizes. */
#define VM86_IRET_FRAME_BYTES  36u    /* 9 dwords */

/* EFLAGS bits that matter for v86 entry. */
#define VM86_EFLAGS_VM         (1u << 17)  /* virtual-8086 mode            */
#define VM86_EFLAGS_IOPL3      (3u << 12)  /* IOPL=3 — guest owns I/O      */
#define VM86_EFLAGS_IF         (1u << 9)   /* interrupt flag enabled       */
#define VM86_EFLAGS_RESERVED1  (1u << 1)   /* reserved, always 1           */

/*
 * Encode a single 32-bit interrupt gate into `dst` (must be 8 bytes).
 * `handler_linear` is the linear address of the PE32 trampoline
 * that services this vector; `cs_selector` is the PE32 code selector.
 */
void vm86_idt_encode_gate(u8 *dst, u32 handler_linear,
                          u16 cs_selector, u8 type_attr);

/*
 * Encode the full 256-entry IDT into `out` (must be at least
 * VM86_IDT_BYTES bytes). Every entry is encoded as a 32-bit
 * interrupt gate. `spurious_handler` fills every vector; the 10
 * vectors listed in the VM86_IDT_VEC_* enum are then overwritten
 * with per-vector `vector_handlers[i]` addresses. Returns the
 * count of entries written on success, 0 on error.
 *
 * `vector_handlers` must have exactly VM86_IDT_VEC_SLOT_COUNT
 * entries, in the same order as the VM86_IDT_VEC_* enum.
 */
u32 vm86_idt_encode(u8 *out,
                    u16 cs_selector,
                    u32 spurious_handler,
                    const u32 *vector_handlers);

/* Read-back helpers for the gate layout. */
u32 vm86_idt_read_offset(const u8 *buf, u32 vector);
u16 vm86_idt_read_selector(const u8 *buf, u32 vector);
u8  vm86_idt_read_type(const u8 *buf, u32 vector);

/*
 * Encode a v86 IRET stack frame into `out` (must be at least
 * VM86_IRET_FRAME_BYTES bytes). Returns VM86_IRET_FRAME_BYTES
 * on success, 0 on error. `eflags` is OR'ed with VM=1|IOPL=3
 * unconditionally — the caller cannot opt out of those bits.
 */
u32 vm86_iret_encode_frame(u8 *out,
                           u16 cs, u16 ip,
                           u16 ss, u16 sp,
                           u32 eflags,
                           u16 ds, u16 es, u16 fs, u16 gs);

/* Read-back helpers for the IRET frame layout. */
u32 vm86_iret_read_eip(const u8 *buf);
u32 vm86_iret_read_cs(const u8 *buf);
u32 vm86_iret_read_eflags(const u8 *buf);
u32 vm86_iret_read_esp(const u8 *buf);
u32 vm86_iret_read_ss(const u8 *buf);
u32 vm86_iret_read_es(const u8 *buf);
u32 vm86_iret_read_ds(const u8 *buf);
u32 vm86_iret_read_fs(const u8 *buf);
u32 vm86_iret_read_gs(const u8 *buf);

/*
 * OPENGEM-026 probe. Encodes a reference IDT and a reference
 * v86 IRET frame, then verifies every field byte-for-byte.
 * Returns 1 on success. Does NOT touch IDTR and does NOT IRET.
 */
int vm86_idt_iret_encoder_probe(void);

/*
 * OPENGEM-028 — live-switch plan.
 *
 * A plan captures all the state the live mode-switch trampoline will
 * eventually consume. Building the plan is pure host-side arithmetic:
 *   - descriptor buffers are staged via vm86_gdt_encode() and
 *     vm86_idt_encode() (OPENGEM-025 / 026)
 *   - the v86 IRET frame is staged via vm86_iret_encode_frame() (026)
 *   - the GDTR / IDTR pseudo-descriptors are computed but NOT loaded
 *
 * The plan is an append-only view over caller-owned buffers. No buffer
 * allocation, no global state, no CPU register touched.
 *
 * Future phases:
 *   OPENGEM-029: vm86_live_switch_arm()/disarm(): flip a runtime gate
 *                that the trampoline consults before executing LGDT,
 *                LIDT, IRET. Default disarmed.
 *   OPENGEM-030: shell.c consumes the plan for gem.exe MZ dispatch
 *                when (and only when) the gate is armed.
 */
#define VM86_LIVE_PLAN_SENTINEL 0x0280u

/*
 * 6-byte / 10-byte pseudo-descriptor used by LGDT / LIDT.
 * We keep the 32-bit form here (6 bytes: limit[15:0] then base[31:0])
 * because the compat-mode trampoline will issue the 32-bit LGDT/LIDT.
 * In long mode the same 10-byte form is used; the trampoline takes
 * care of picking the right operand-size prefix at execution time.
 * For the plan, only limit and base matter.
 */
typedef struct vm86_dtr {
    u16 limit;
    u32 base;
} vm86_dtr;

typedef struct vm86_live_switch_plan {
    u32        sentinel;         /* VM86_LIVE_PLAN_SENTINEL                 */
    u8        *gdt_buf;          /* staged GDT, at least VM86_GDT_BYTES     */
    u32        gdt_bytes;        /* == VM86_GDT_BYTES                       */
    u8        *idt_buf;          /* staged IDT, at least VM86_IDT_BYTES     */
    u32        idt_bytes;        /* == VM86_IDT_BYTES                       */
    u8        *iret_frame;       /* staged v86 IRET frame (36 bytes)        */
    u32        iret_frame_bytes; /* == VM86_IRET_FRAME_BYTES                */
    u32        tss_base;         /* linear address of 32-bit TSS            */
    u16        tss_limit;        /* byte size - 1                           */
    vm86_dtr   gdtr;             /* pseudo-descriptor for LGDT              */
    vm86_dtr   idtr;             /* pseudo-descriptor for LIDT              */
    u16        cs_pe32_selector; /* GDT selector for the PE32 code segment  */
    u16        ss_pe32_selector; /* GDT selector for the PE32 data segment  */
    u16        tss_selector;     /* GDT selector for the 32-bit TSS         */
    u32        guest_entry_cs;   /* v86 guest CS (zero-extended)            */
    u32        guest_entry_ip;   /* v86 guest IP (zero-extended)            */
    u32        flags;            /* bitfield — see VM86_LIVE_PLAN_F_*       */
} vm86_live_switch_plan;

/* Plan status flags (observability). */
#define VM86_LIVE_PLAN_F_BUFFERS_PRESENT   (1u << 0)
#define VM86_LIVE_PLAN_F_GDTR_COMPUTED     (1u << 1)
#define VM86_LIVE_PLAN_F_IDTR_COMPUTED     (1u << 2)
#define VM86_LIVE_PLAN_F_IRET_STAGED       (1u << 3)
#define VM86_LIVE_PLAN_F_VM_BIT_VERIFIED   (1u << 4)  /* EFLAGS.VM=1 in frame */
#define VM86_LIVE_PLAN_F_READY             (1u << 7)  /* all of the above    */

/*
 * Build a live-switch plan over caller-owned buffers. Every pointer
 * must be non-NULL; gdt_buf must be at least VM86_GDT_BYTES; idt_buf
 * at least VM86_IDT_BYTES; iret_frame at least VM86_IRET_FRAME_BYTES.
 *
 * The three buffers MUST already have been populated by the OPENGEM-025
 * / 026 encoders. This function does not touch their contents; it only
 * reads them to cross-check the VM=1 bit in the staged IRET frame.
 *
 * Linear addresses for gdt/idt are taken as (u32)(uintptr_t)buf, which
 * is correct because the live switch runs with a flat identity-mapped
 * PE32 CS. The trampoline (OPENGEM-029) may translate further.
 *
 * Returns 1 on success (plan->flags has F_READY set); 0 on error. Does
 * NOT execute LGDT, LIDT, IRET, or any CPU-state-modifying instruction.
 */
int vm86_live_switch_plan_build(vm86_live_switch_plan *plan,
                                u8 *gdt_buf, u8 *idt_buf,
                                u8 *iret_frame,
                                u32 tss_base, u16 tss_limit,
                                u16 cs_pe32_selector,
                                u16 ss_pe32_selector,
                                u16 tss_selector,
                                u32 guest_entry_cs,
                                u32 guest_entry_ip);

/*
 * Read-back helpers (observability). Stable tokens for gate scripts.
 */
u32 vm86_live_switch_plan_flags(const vm86_live_switch_plan *plan);
u16 vm86_live_switch_plan_gdtr_limit(const vm86_live_switch_plan *plan);
u32 vm86_live_switch_plan_gdtr_base (const vm86_live_switch_plan *plan);
u16 vm86_live_switch_plan_idtr_limit(const vm86_live_switch_plan *plan);
u32 vm86_live_switch_plan_idtr_base (const vm86_live_switch_plan *plan);

/*
 * OPENGEM-028 probe. Stages a full plan end-to-end (encodes GDT, IDT,
 * and a v86 IRET frame into three static host buffers, then builds the
 * plan on top), and cross-checks every computed field. Returns 1 on
 * success. Does NOT execute LGDT / LIDT / IRET.
 */
int vm86_live_switch_plan_probe(void);

/*
 * OPENGEM-029 — armed-but-gated live-switch execute path.
 *
 * The live-switch trampolines declared in stage2/include/vm86_switch.h
 * (long->PE32->v86->PE32->long) now have a single C call site:
 *   vm86_live_switch_execute()
 * That call site is itself guarded by a runtime arm flag. The flag is:
 *   - 0 at boot (default);
 *   - flipped to 1 ONLY by vm86_live_switch_arm() with the correct
 *     magic value AND a plan that is VM86_LIVE_PLAN_F_READY;
 *   - reset to 0 by vm86_live_switch_disarm().
 *
 * vm86_live_switch_execute() refuses to invoke the trampolines unless
 * armed. When armed, it currently invokes the OPENGEM-027 stub bodies
 * (which are all `retq`), so running it is still a no-op at the CPU
 * level. This phase is therefore observability-only: it proves the
 * gating contract without yet running guest code. OPENGEM-030 wires
 * gem.exe to consult the gate; later phases will flesh out the stubs.
 *
 * Design contract:
 *   - The magic value is stable for this phase and becomes the caller's
 *     acknowledgement that it understands live CPU-mutation is possible.
 *   - The gate is never flipped implicitly. No boot path arms it.
 */
#define VM86_LIVE_ARM_MAGIC 0x12860029u

/* Status / reason codes for vm86_live_switch_execute() return + probe. */
typedef enum vm86_live_exec_status {
    VM86_LIVE_EXEC_BLOCKED_NOT_ARMED  = 0,
    VM86_LIVE_EXEC_BLOCKED_NO_PLAN    = 1,
    VM86_LIVE_EXEC_BLOCKED_BAD_PLAN   = 2,
    VM86_LIVE_EXEC_INVOKED_STUBS      = 3   /* executed the retq stubs */
} vm86_live_exec_status;

/*
 * Arm the gate. `magic` must equal VM86_LIVE_ARM_MAGIC; `plan` must be
 * non-NULL and must have VM86_LIVE_PLAN_F_READY set. Returns 1 on
 * success, 0 on rejection (leaves the gate disarmed).
 */
int  vm86_live_switch_arm(u32 magic, const vm86_live_switch_plan *plan);

/* Clear the arm flag unconditionally. Safe to call at any time. */
void vm86_live_switch_disarm(void);

/* Read-back. */
int  vm86_live_switch_is_armed(void);

/*
 * Read-back for the currently armed plan. NULL while disarmed. The
 * returned pointer aliases the plan passed to vm86_live_switch_arm();
 * callers must not free it.
 */
const vm86_live_switch_plan *vm86_live_switch_get_plan(void);

/*
 * Execute the live switch IF the gate is armed. Returns one of the
 * vm86_live_exec_status values. Never executes the trampolines while
 * disarmed; when armed, invokes the OPENGEM-027 stub bodies (retq),
 * which leaves the CPU state unchanged at this phase.
 */
vm86_live_exec_status vm86_live_switch_execute(void);

/*
 * OPENGEM-029 probe. Drives arm / disarm / execute transitions:
 *   1. disarmed default -> execute returns BLOCKED_NOT_ARMED
 *   2. arm with wrong magic -> rejected, still disarmed
 *   3. arm with NULL plan   -> rejected, still disarmed
 *   4. arm with non-ready plan -> rejected, still disarmed
 *   5. arm with ready plan + correct magic -> armed
 *   6. execute while armed -> INVOKED_STUBS
 *   7. disarm -> execute blocked again
 * Returns 1 on full pass, 0 otherwise. Does NOT execute LGDT / LIDT /
 * IRET (the trampoline bodies are still stubs).
 */
int vm86_live_switch_arm_probe(void);

/*
 * ============================================================
 * OPENGEM-031 — CPU state snapshot + identity-map verification.
 *
 * Observability-only prerequisite for every phase that will
 * mutate CR0/CR3/EFER or reload GDTR/IDTR.
 *
 * Nothing in this block touches CPU control state. The snapshot
 * reads the current values via SGDT/SIDT/MOV-from-CR*; the probe
 * walks the page tables in read-only mode through the current
 * CR3 value. No CR3 write, no invalidation, no LGDT, no LIDT.
 *
 * Until the v8086 guest's 1 MiB window is confirmed
 * identity-mapped by the host paging, no live entry can be
 * attempted without risking a #PF from the guest's very first
 * fetch. This phase proves the invariant.
 * ============================================================
 */

#define VM86_CPU_SNAPSHOT_SENTINEL  0x0310u
#define VM86_PE32_IDENTITY_SENTINEL 0x0311u

/*
 * Structural snapshot of the long-mode host CPU control state.
 * All fields are populated by vm86_cpu_snapshot_capture().
 * Layout is ABI-frozen for test-tool consumption.
 */
typedef struct vm86_cpu_snapshot {
    u32  sentinel;    /* VM86_CPU_SNAPSHOT_SENTINEL on success */
    u32  reserved0;
    u64  cr0;
    u64  cr3;
    u64  cr4;
    u64  efer;
    u16  gdtr_limit;
    u16  reserved1;
    u32  reserved2;
    u64  gdtr_base;
    u16  idtr_limit;
    u16  reserved3;
    u32  reserved4;
    u64  idtr_base;
} vm86_cpu_snapshot;

/*
 * Capture the current host CPU control state into `out`.
 * Returns 1 on success, 0 if `out` is NULL.
 *
 * Reads CR0/CR3/CR4 via MOV-from-CR, EFER via RDMSR(0xC0000080),
 * GDTR/IDTR via SGDT/SIDT. Does not modify any register.
 */
int vm86_cpu_snapshot_capture(vm86_cpu_snapshot *out);

/*
 * Read-only walk of the long-mode page tables starting from the
 * supplied `cr3_phys` value. Verifies that every 4 KiB page in
 * [range_start, range_end) is identity-mapped (virtual == physical)
 * and present. On failure `failing_va_out` (if non-NULL) receives
 * the first virtual address that does not satisfy the invariant.
 *
 * NOTE: requires that the host's current paging has `cr3_phys`
 * reachable via the identity region it already maps. Stage2's
 * UEFI-installed page tables satisfy this for the physical range
 * below 1 GiB.
 *
 * Returns 1 if the whole range is identity-mapped and present,
 * 0 otherwise. Does NOT write to CR3 or any page-table entry.
 */
int vm86_pe32_identity_verify(u64 cr3_phys,
                              u64 range_start,
                              u64 range_end,
                              u64 *failing_va_out);

/*
 * OPENGEM-031 integrated probe.
 *
 * Steps (all observable on serial):
 *   1. Capture the CPU snapshot.
 *   2. Assert EFER.LMA=1, EFER.LME=1 (long mode active).
 *   3. Assert CR0.PE=1, CR0.PG=1 (protected mode + paging).
 *   4. Invoke vm86_pe32_identity_verify on the v8086 window
 *      [0x00000000, 0x00100000) using the captured CR3.
 *   5. Emit a marker summarizing readiness for the forthcoming
 *      OPENGEM-032 live switch.
 *
 * Emits markers prefixed `vm86: pe32-ident ...`. Returns 1 on
 * full readiness, 0 if any invariant fails.
 *
 * SAFETY: does NOT modify control registers or page tables.
 * Safe to invoke from any long-mode host context.
 */
int vm86_pe32_identity_probe(void);

/* ------------------------------------------------------------------ */
/* OPENGEM-032 - v8086 IDT shim builder (observability only).          */
/* ------------------------------------------------------------------ */

#define VM86_IDT_SHIM_SENTINEL      0x0320u
#define VM86_IDT_SHIM_STUB_COUNT    11u   /* 10 well-known + unexpected */

/*
 * Packed 10-byte IDTR pseudo-descriptor, exactly as LIDT/SIDT
 * consume it. Produced by vm86_idt_shim_idtr_image(); never
 * loaded by this phase.
 */
typedef struct __attribute__((packed)) vm86_idtr_image {
    u16 limit;
    u64 base;
} vm86_idtr_image;

/*
 * Build the static 256-entry IDT shim image backing the v8086
 * host. All slots are filled using vm86_idt_encode(); the ten
 * well-known vectors defined by the VM86_IDT_VEC_* enum receive
 * dedicated trap stub handlers, every other vector is routed to
 * the "unexpected" stub. The CS selector is VM86_GDT_PE_CODE32
 * (see design §5.1).
 *
 * Returns 1 on success, 0 if a stub address does not fit in the
 * 32-bit IDT offset field (the shim cannot be loaded).
 *
 * Idempotent; may be invoked multiple times. Does not mutate any
 * host CPU register.
 */
int vm86_idt_shim_build(void);

/*
 * Populate a pseudo-descriptor matching the shim image. Both
 * `limit_out` and `base_out` must be non-NULL. Returns 1 if the
 * image has been built via vm86_idt_shim_build(), 0 otherwise.
 *
 * Does NOT issue LIDT. The caller is responsible for deciding
 * when the actual CPU mutation happens (OPENGEM-033).
 */
int vm86_idt_shim_idtr_image(u16 *limit_out, u64 *base_out);

/*
 * Read-back verifier. Decodes every gate in the shim image and
 * confirms:
 *   - limit == 0x7FF (256 * 8 - 1);
 *   - selector is VM86_GDT_PE_CODE32;
 *   - type/attr is VM86_IDT_TYPE_INT32 for all 256 slots;
 *   - the 10 well-known vectors point to the dedicated stub
 *     addresses, all distinct and distinct from the unexpected
 *     stub;
 *   - every other vector points to the unexpected stub.
 *
 * Returns 1 on pass, 0 on any mismatch. Does not mutate host
 * CPU state or the image.
 */
int vm86_idt_shim_verify(void);

/*
 * Integrated probe. Builds + verifies + emits
 * `vm86: idt-shim ...` markers summarizing:
 *   - sentinel
 *   - image base (virt)
 *   - limit
 *   - each well-known vector with its offset and CS
 *   - readiness surface (build,verify)
 *   - pending surface (lidt,iretd) — deferred to OPENGEM-033
 *
 * Returns 1 on full success, 0 otherwise. Safe to invoke from
 * any long-mode host context; NO LIDT, NO IRETD.
 */
int vm86_idt_shim_probe(void);

/* ------------------------------------------------------------------ */
/* OPENGEM-033 - LIDT reversible trampoline (arm-gated).              */
/* ------------------------------------------------------------------ */

#define VM86_LIDT_PING_SENTINEL   0x0330u
#define VM86_LIDT_PING_ARM_MAGIC  0xC1036B33u

/*
 * Raw asm trampoline. Performs pushfq;cli;sidt;lidt(new);lidt(saved)
 * ;popfq;ret. Never call directly from application code — go through
 * vm86_lidt_ping_execute(), which enforces the arm-gate.
 *
 * SysV AMD64:
 *   new_idtr  -> const vm86_idtr_image *
 *   saved_out ->       vm86_idtr_image *
 * Returns 1 on success, 0 if either pointer is NULL.
 */
int vm86_lidt_ping_asm(const vm86_idtr_image *new_idtr,
                       vm86_idtr_image *saved_out);

/*
 * Flip the runtime arm-gate. Returns 1 on success, 0 on rejected
 * magic. Default at boot is DISARMED.
 */
int vm86_lidt_ping_arm(u32 magic);
void vm86_lidt_ping_disarm(void);
int  vm86_lidt_ping_is_armed(void);

/*
 * Guarded entry point.
 *   - If disarmed: returns 0 without calling the asm; `saved_out`
 *     untouched. This is the default behaviour and is what every
 *     test and the boot path exercise.
 *   - If armed and both pointers non-NULL: invokes the asm
 *     trampoline, which loads `new_idtr` and IMMEDIATELY restores
 *     the previous IDTR, with IF masked across the window.
 *     Returns the value reported by the asm (1 on success).
 *
 * This is the only way to reach the LIDT opcode from C.
 */
int vm86_lidt_ping_execute(const vm86_idtr_image *new_idtr,
                           vm86_idtr_image *saved_out);

/*
 * Integrated probe. Emits `vm86: lidt-ping ...` markers reporting:
 *   - sentinel
 *   - arm state (must be 0 on default boot)
 *   - magic rejection for a bad value
 *   - would-run result while disarmed (execute returns 0,
 *     saved_out untouched)
 *   - ready-surface=asm,arm-gate
 *   - pending-surface=iretd,gp-handler (deferred to OPENGEM-034)
 *
 * SAFETY: probe NEVER arms the gate. It never runs the asm.
 * Returns 1 if every invariant holds, 0 otherwise.
 */
int vm86_lidt_ping_probe(void);

/* ------------------------------------------------------------------ */
/* OPENGEM-034 - Synthetic #GP opcode decoder (no CPU dispatch).       */
/* ------------------------------------------------------------------ */
/*
 * Decodes the instruction at guest CS:EIP as it would appear after
 * a real v8086 #GP trap. The decoder is tested against a SYNTHETIC
 * trap frame + a caller-supplied 1 MiB conventional-memory buffer;
 * there is no IDT, no LIDT, no IRETD involved. This is the final
 * observability phase before any CPU mutation is wired to a guest.
 *
 * Design reference: docs/opengem-016-design.md §5.2 (INT N → #GP
 * → host dispatcher → handler → IRET path).
 */

#define VM86_GP_DECODE_SENTINEL      0x0340u

/*
 * Decode outcome. Describes which opcode family was recognized
 * at guest CS:EIP.
 *
 * UNHANDLED_OPCODE = opcode seen but not yet implemented by the
 * minimal subset. The caller is free to ignore.
 */
typedef enum {
    VM86_GP_RESULT_NONE          = 0, /* pre-dispatch sentinel     */
    VM86_GP_RESULT_INT           = 1, /* INT N (CD ib) dispatched  */
    VM86_GP_RESULT_INTO          = 2, /* INTO (CE) — dispatched #4 */
    VM86_GP_RESULT_INT3          = 3, /* INT3 (CC) — dispatched #3 */
    VM86_GP_RESULT_IRET          = 4, /* IRET (CF)                 */
    VM86_GP_RESULT_PUSHF         = 5, /* PUSHF (9C)                */
    VM86_GP_RESULT_POPF          = 6, /* POPF  (9D)                */
    VM86_GP_RESULT_IN_IMM        = 7, /* IN AL/AX, imm8            */
    VM86_GP_RESULT_OUT_IMM       = 8, /* OUT imm8, AL/AX           */
    VM86_GP_RESULT_IN_DX         = 9, /* IN AL/AX, DX              */
    VM86_GP_RESULT_OUT_DX        = 10,/* OUT DX, AL/AX             */
    VM86_GP_RESULT_CLI           = 11,
    VM86_GP_RESULT_STI           = 12,
    VM86_GP_RESULT_HLT           = 13,
    VM86_GP_RESULT_UNHANDLED     = 14,
    VM86_GP_RESULT_NULL_ARG      = 15,
    VM86_GP_RESULT_OOB           = 16  /* CS:EIP outside guest mem  */
} vm86_gp_decode_result;

/*
 * The decoder consumes the guest memory window as a flat 1 MiB
 * byte buffer laid out at physical 0. It never touches CiukiOS
 * kernel memory. Callers pass the host-side pointer covering
 * that buffer; real-mode CS:EIP is translated linearly as
 * (cs << 4) + eip and bounds-checked against `guest_bytes`.
 *
 * On VM86_GP_RESULT_INT / INTO / INT3 the decoder also dispatches
 * the vector into the supplied `vm86_dispatcher`, increments
 * frame->eip past the INT instruction, and propagates the
 * dispatcher status via `dispatch_status_out` (non-NULL required
 * only when one of those three results can occur).
 *
 * The decoder does NOT mutate the guest memory buffer and does
 * NOT rewrite CPU state outside of frame->eip (and the dispatcher
 * side effects on handler-observable fields).
 */
vm86_gp_decode_result vm86_gp_decode(const u8             *guest_bytes,
                                     u32                   guest_size,
                                     vm86_trap_frame      *frame,
                                     vm86_dispatcher      *disp,
                                     vm86_task            *task,
                                     vm86_dispatch_status *dispatch_status_out);

/*
 * OPENGEM-034 integrated probe.
 *
 * Builds a 256 KiB synthetic conventional buffer, writes a handful
 * of canned instruction encodings at known CS:EIP pairs, runs
 * vm86_gp_decode() against each, and asserts both the classified
 * result and (for INT N) the dispatcher side-effects.
 *
 * Emits `vm86: gp-decode ...` markers on serial. Returns 1 on
 * full success, 0 otherwise. Does NOT alter any host CPU state.
 */
int vm86_gp_decode_probe(void);

/* ------------------------------------------------------------------ */
/* OPENGEM-035 - #GP dispatcher host path (arm-gated, observability). */
/* ------------------------------------------------------------------ */
/*
 * Closes the `pending-surface=handler-frame-apply,guest-stack-iret`
 * declared by OPENGEM-034. The host-side C entry point is
 * vm86_gp_dispatch_handle(): given a trap frame and a guest-memory
 * window, it invokes the OPENGEM-034 decoder, classifies the result
 * into an action, and (on IRETD) writes an IRETD-shaped v86 stack
 * frame into a caller-supplied slot via vm86_iret_encode_frame().
 *
 * The ISR symbol `vm86_gp_dispatch_isr_stub` (defined in
 * stage2/src/vm86_gp_dispatch.S) is the future PE32 #GP landing
 * pad. It is NEVER installed into any live IDT by this phase; the
 * body is a deterministic halt loop so even an accidental entry is
 * a contained failure. The arm-gate below guards the C path from
 * ever running on the default boot path.
 *
 * Safety invariants (gate-enforced):
 *   - arm flag defaults to 0 and is not flipped implicitly;
 *   - magic constant is required to flip the gate;
 *   - vm86_gp_dispatch_handle() returns BLOCKED_NOT_ARMED while
 *     disarmed, without invoking the decoder;
 *   - no file below vm86.c references the new C symbols;
 *   - no file below vm86_gp_dispatch.S references the new asm
 *     symbols;
 *   - no LIDT / LGDT / IRETD / IRETQ / CR-write is introduced by
 *     this phase.
 */

#define VM86_GP_DISPATCH_SENTINEL   0x0350u
#define VM86_GP_DISPATCH_ARM_MAGIC  0xC1D39350u

typedef enum vm86_gp_dispatch_action {
    VM86_GP_DISPATCH_ACTION_BLOCKED_NOT_ARMED = 0,
    VM86_GP_DISPATCH_ACTION_IRETD             = 1,
    VM86_GP_DISPATCH_ACTION_HLT               = 2,
    VM86_GP_DISPATCH_ACTION_BAD_INPUT         = 3
} vm86_gp_dispatch_action;

/*
 * Flip the arm-gate. `magic` must equal VM86_GP_DISPATCH_ARM_MAGIC;
 * any other value leaves the gate disarmed. Returns 1 on success,
 * 0 on rejection.
 */
int  vm86_gp_dispatch_arm(u32 magic);

/* Clear the arm flag unconditionally. */
void vm86_gp_dispatch_disarm(void);

/* Read-back. */
int  vm86_gp_dispatch_is_armed(void);

/*
 * Host-side #GP dispatch entry point.
 *
 * Preconditions (all enforced, failures map to BAD_INPUT):
 *   - guest_bytes non-NULL and guest_size > 0
 *   - frame non-NULL
 *
 * Behaviour:
 *   - If the arm flag is 0, returns BLOCKED_NOT_ARMED immediately.
 *     The decoder is NOT invoked, `guest_iret_slot` is NOT written,
 *     `decode_out` (if non-NULL) receives VM86_GP_RESULT_NONE.
 *   - If armed, invokes vm86_gp_decode() on the frame. The decoder
 *     advances frame->eip past the recognised instruction and
 *     (for INT/INT3/INTO) routes the vector into `disp`/`task`.
 *   - The decode result is classified:
 *       INT, INT3, INTO, IRET, PUSHF, POPF,
 *       IN_IMM, OUT_IMM, IN_DX, OUT_DX, CLI, STI
 *           => ACTION_IRETD
 *       HLT, UNHANDLED
 *           => ACTION_HLT
 *       NULL_ARG, OOB
 *           => ACTION_BAD_INPUT
 *   - When the action is IRETD and `guest_iret_slot` is non-NULL,
 *     a 36-byte v86 IRETD stack frame is written at that address
 *     via vm86_iret_encode_frame() using the post-decode
 *     frame->eip / cs / eflags / esp / ss / ds / es / fs / gs.
 *     EFLAGS is OR'ed with VM=1 | IOPL=3 | reserved-bit-1 by the
 *     encoder. The slot is untouched on any other action.
 *
 * Returns the action. `decode_out` (optional) receives the raw
 * decoder result.
 */
vm86_gp_dispatch_action vm86_gp_dispatch_handle(
    const u8              *guest_bytes,
    u32                    guest_size,
    vm86_trap_frame       *frame,
    vm86_dispatcher       *disp,
    vm86_task             *task,
    u8                    *guest_iret_slot,
    vm86_gp_decode_result *decode_out);

/*
 * OPENGEM-035 integrated probe. Host-driven; NEVER invoked from the
 * live boot path.
 *
 * Drives all dispatch-action classes through a synthetic guest
 * buffer and a 36-byte IRETD slot, verifying:
 *   - disarmed default -> BLOCKED_NOT_ARMED, slot untouched;
 *   - wrong magic rejected, still disarmed;
 *   - INT 21h -> IRETD, dispatcher hit, slot EIP advanced, VM+IOPL3
 *     bits set in slot EFLAGS;
 *   - INT3, INTO -> IRETD, per-vector hit counters;
 *   - IRET, PUSHF, POPF, IN_*, OUT_*, CLI, STI -> IRETD;
 *   - HLT -> ACTION_HLT;
 *   - BOUND (unhandled) -> ACTION_HLT;
 *   - NULL / OOB inputs -> ACTION_BAD_INPUT;
 *   - disarm cleanly after the probe (leaves gate disarmed).
 *
 * Emits `vm86: gp-dispatch ...` markers. Returns 1 on full
 * success, 0 otherwise. Does NOT touch any CPU control state.
 */
int vm86_gp_dispatch_probe(void);

/* ================================================================== */
/* OPENGEM-036 - PE32 #GP ISR C-side entry (arm-gated, observability). */
/*                                                                    */
/* This phase adds the C function that a future real PE32 #GP handler */
/* will call after capturing the hardware-pushed trap frame. The asm  */
/* ISR stub defined by OPENGEM-035 remains a halt-loop; the wiring    */
/* of the asm body is the OPENGEM-037 pending surface. This phase is  */
/* therefore still strictly observability-only:                       */
/*   - no LIDT / LGDT is issued;                                      */
/*   - no IRETD / IRETQ is emitted in C or asm;                       */
/*   - the new C entry is reachable only via its public prototype;    */
/*   - the new arm-gate is default-disarmed and requires a dedicated  */
/*     magic constant disjoint from the 029 / 033 / 035 magics.       */
/* ================================================================== */

#define VM86_GP_ISR_C_SENTINEL   0x0360u
#define VM86_GP_ISR_ARM_MAGIC    0xC1D39360u

/*
 * Flip / read / clear the OPENGEM-036 arm-gate. The gate is
 * completely independent of every earlier arm-gate: arming 029 /
 * 033 / 035 does NOT arm 036, and vice-versa.
 */
int  vm86_gp_isr_c_arm(u32 magic);
void vm86_gp_isr_c_disarm(void);
int  vm86_gp_isr_c_is_armed(void);

/*
 * Host-side PE32 #GP ISR C entry.
 *
 * This is the function a real PE32 #GP handler (OPENGEM-037+) will
 * invoke once it has captured the hardware-pushed trap frame into a
 * `vm86_trap_frame`. The entry:
 *   - validates its inputs (NULL/OOB -> ACTION_BAD_INPUT);
 *   - enforces the dedicated arm-gate (disarmed -> BLOCKED_NOT_ARMED,
 *     decoder NOT invoked, slot/out_frame untouched);
 *   - copies `in_frame` into a local working frame, routes it through
 *     vm86_gp_dispatch_handle(), and on return:
 *       * writes the post-decode working frame into `out_frame`
 *         (register-for-register), provided `out_frame` is non-NULL;
 *       * the 36-byte IRETD slot handling is delegated to
 *         vm86_gp_dispatch_handle()'s existing contract.
 *
 * Parameters:
 *   in_frame      -- non-NULL; hardware-captured trap frame.
 *   guest_base    -- non-NULL; base of the v8086 guest's visible
 *                    memory window (host-linear).
 *   guest_size    -- bytes in the guest window; must be > 0.
 *   disp / task   -- optional; passed through to
 *                    vm86_gp_dispatch_handle().
 *   guest_iret_slot -- optional 36-byte slot for the post-decode
 *                      IRETD stack image. May be NULL.
 *   out_frame     -- optional; if non-NULL, receives the post-decode
 *                    working frame (same layout as in_frame).
 *   decode_out    -- optional; receives the raw decoder result.
 *
 * Returns the dispatch action.
 */
vm86_gp_dispatch_action vm86_gp_isr_c_entry(
    const vm86_trap_frame *in_frame,
    const u8              *guest_base,
    u32                    guest_size,
    vm86_dispatcher       *disp,
    vm86_task             *task,
    u8                    *guest_iret_slot,
    vm86_trap_frame       *out_frame,
    vm86_gp_decode_result *decode_out);

/*
 * OPENGEM-036 integrated probe. Host-driven; never invoked from the
 * live boot path. Exercises:
 *   - default-disarmed: handle returns BLOCKED_NOT_ARMED, out_frame
 *     is not populated, iret slot is not touched;
 *   - wrong magic rejected;
 *   - armed path over the same synthetic guest buffer as 034/035:
 *     INT21h -> IRETD, HLT -> ACTION_HLT;
 *   - NULL input_frame / NULL guest_base / zero guest_size all map
 *     to ACTION_BAD_INPUT with out_frame/slot untouched;
 *   - disarms cleanly on exit.
 *
 * Returns 1 on success, 0 otherwise.
 */
int vm86_gp_isr_c_probe(void);

/* ================================================================== */
/* OPENGEM-037 - PE32 #GP ISR real asm body (arm-gated, never-installed). */
/*                                                                    */
/* This phase adds the REAL 32-bit asm ISR that captures the HW trap */
/* frame into a static area and halts. It is declared in its own     */
/* file (stage2/src/vm86_gp_isr_body.S) and never installed in any   */
/* IDT in 037: no caller exists outside that file. Own arm-gate,     */
/* own magic, own sentinel constant. The vm86_gp_dispatch.S halt    */
/* stub from OPENGEM-035 is NOT modified.                            */
/*                                                                    */
/* Observability only:                                                */
/*   - no LIDT / LGDT is issued;                                      */
/*   - no IRETD / IRETQ is emitted (yet);                             */
/*   - no CR-write is introduced;                                     */
/*   - the new symbols are reachable only via their prototypes.       */
/* ================================================================== */

#define VM86_GP_ISR_REAL_SENTINEL 0x0370u
#define VM86_GP_ISR_REAL_ARM_MAGIC 0xC1D39370u

/* Capture area written by vm86_gp_isr_real_entry on #GP entry. */
extern u8  vm86_gp_isr_capture_area[64];
extern u8  vm86_gp_isr_capture_flag;
extern u32 vm86_gp_isr_capture_seq;

/*
 * Arm-gate for OPENGEM-037. Independent from 029/033/035/036. Arming
 * this gate alone does NOT install the ISR in any IDT -- installation
 * is the OPENGEM-038 pending surface.
 */
int  vm86_gp_isr_real_arm(u32 magic);
void vm86_gp_isr_real_disarm(void);
int  vm86_gp_isr_real_is_armed(void);

/*
 * OPENGEM-037 host-side probe. Exercises static invariants only:
 *   - default-disarmed;
 *   - wrong magic rejected;
 *   - capture flag default 0, sequence default 0;
 *   - capture area initialised to zero;
 *   - sentinel string is "OPENGEM-037";
 *   - vm86_gp_isr_real_entry symbol address is non-NULL.
 *
 * Returns 1 on success, 0 otherwise. Never invokes the ISR (it is
 * unreachable in 037 by design).
 */
int vm86_gp_isr_real_probe(void);

/* ================================================================== */
/* OPENGEM-038 - PE32 IDT install + live-arm shell gate.              */
/*                                                                    */
/* This phase wires the INSTALL operation: on explicit arm, rewrite   */
/* vector 0x0D in the PE32 shim IDT image to point at the 037 real    */
/* ISR. It does NOT execute LIDT (that is 039 pending) and does NOT   */
/* enter v8086 mode. The install is reversible via uninstall().       */
/*                                                                    */
/* Arming the 038 gate is done ONLY via the shell command             */
/* `vm86-arm-live` -- the default boot path never invokes it.         */
/* ================================================================== */

#define VM86_GP_ISR_INSTALL_SENTINEL  0x0380u
#define VM86_GP_ISR_INSTALL_ARM_MAGIC 0xC1D39380u

int  vm86_gp_isr_install_arm(u32 magic);
void vm86_gp_isr_install_disarm(void);
int  vm86_gp_isr_install_is_armed(void);

/*
 * Install / uninstall the real ISR into the PE32 shim IDT at vector
 * 0x0D. Returns 1 on success, 0 if the gate is disarmed or the shim
 * IDT is not yet built.
 *
 *   install()    -- writes vector 0x0D of s_vm86_idt_shim_bytes to
 *                   point at vm86_gp_isr_real_entry with the PE32
 *                   code32 selector; caches the prior bytes so that
 *                   uninstall() can restore the 032 default.
 *   uninstall()  -- restores the cached bytes unconditionally (does
 *                   not require the gate to be armed).
 *   is_installed() -- 1 iff the last operation was install() without
 *                   a subsequent uninstall().
 */
int  vm86_gp_isr_install(u32 magic);
int  vm86_gp_isr_uninstall(void);
int  vm86_gp_isr_is_installed(void);

/*
 * OPENGEM-038 probe. Host-driven; verifies the install/uninstall
 * round-trip leaves the shim IDT byte-identical to the pre-install
 * state, that install is refused when the gate is disarmed, and
 * that vector 0x0D correctly targets vm86_gp_isr_real_entry after
 * install.
 *
 * Returns 1 on success, 0 otherwise. Never executes LIDT.
 */
int vm86_gp_isr_install_probe(void);

#endif /* STAGE2_VM86_H */
