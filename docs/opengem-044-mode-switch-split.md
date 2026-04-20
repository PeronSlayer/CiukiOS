# OPENGEM-044 — Long ↔ Legacy PM ↔ v86 Mode-Switch, Multi-Agent Split

**Status:** planned (branches to be opened per task)
**Date:** 2026-04-20
**Baseline:** CiukiOS Alpha v0.8.9 (no version bump tied to this split)
**Parent design:** [docs/opengem-016-design.md](opengem-016-design.md) §0 Errata + §3.3

---

## 0. Why this split

The OPENGEM-043 runtime finding proved that virtual-8086 cannot be reached from IA-32e via a compat-mode host task (Intel SDM 3A §20.1). The user selected **Path 1 (Full legacy mode-switch)** from the §0 Errata because the long-term goal is running CiukiOS on **real retro hardware**, including machines with no VT-x. VMX is therefore rejected as a runtime prerequisite.

The resulting subsystem (long-mode host → exit IA-32e → legacy 32-bit PM host → IRETL into v86 → return symmetrically) is too large for a single session. It is split into three independent sub-tasks with explicit interface contracts so three agents may work in parallel on dedicated branches.

---

## 1. Mandatory cross-task rules

1. Every sub-task runs on its **own branch**. `main` is off-limits for all three.
2. No sub-task merges itself. Only the user says `fai il merge` and controls ordering.
3. Each branch keeps `make test-stage2`, `make test-fallback`, and the 25 existing static gates green.
4. No version bump. Alpha v0.8.9 remains until the user explicitly changes it.
5. Each sub-task maintains **arm-gate default disarmed**. Boot path must never reach any new runtime unless explicitly armed from shell.
6. Each sub-task logs its work to `docs/collab/diario-di-bordo.md` at completion or at every significant checkpoint.
7. Any reusable piece of the OPENGEM-040/041 scaffolding (GDT encoding, TSS32 image, loader, PSP, reloc) MUST be kept if the shape matches; only the inner transition into the guest is invalid. Do not delete 040/041 code paths without explicit agreement.

---

## 2. Task map

```
┌─────────────────────────────────────────────────────────────┐
│ Task A — Mode-switch engine (long ↔ legacy 32-bit PM)       │
│  Branch: feature/opengem-044-A-mode-switch                  │
│  No v86. No guest. Only the round-trip long→PM32→long.      │
└──────────────┬──────────────────────────────────────────────┘
               │ provides: legacy_pm_enter / legacy_pm_return
               ▼
┌─────────────────────────────────────────────────────────────┐
│ Task B — Legacy-PM v86 host (IRETL.VM=1 is legal here)      │
│  Branch: feature/opengem-044-B-legacy-v86-host              │
│  Real TSS32, real 32-bit IDT, real #GP/#PF ISRs in PM.      │
│  Round-trip host_PM32 ↔ v86 guest.                          │
└──────────────┬──────────────────────────────────────────────┘
               │ provides: legacy_v86_enter / legacy_v86_return
               ▼
┌─────────────────────────────────────────────────────────────┐
│ Task C — INT dispatcher bind + loader integration           │
│  Branch: feature/opengem-044-C-dispatch-loader              │
│  Bind INT 10h/21h/16h/33h host callbacks.                   │
│  Rewire shell `gem` / `dosrun` on arm-gate.                 │
└─────────────────────────────────────────────────────────────┘
```

Dependency: A → B → C for final integration. Prototyping may proceed in parallel by stubbing against the contracts in §3.

---

## 3. Interface contracts (binding, changes require logbook entry)

### 3.1 Task A — Mode-switch engine

**Header:** `stage2/include/mode_switch.h`

```c
#define MODE_SWITCH_ARM_MAGIC   0xC1D39440u
#define MODE_SWITCH_SENTINEL    0x0440u

/* Reversible long-mode exit → 32-bit legacy PM → re-entry. */
typedef void (*legacy_pm_body_fn)(void *user);

/* Runs body in legacy 32-bit PM (EFER.LME=0, CR0.PG per impl),
 * returns to long mode with caller state restored. */
int mode_switch_run_legacy_pm(legacy_pm_body_fn body, void *user);

int  mode_switch_arm(uint32_t magic);
void mode_switch_disarm(void);
int  mode_switch_is_armed(void);
int  mode_switch_probe(void);   /* host-driven, no v86 */
```

**Ownership (files only Task A may create/modify):**
- `stage2/include/mode_switch.h` (new)
- `stage2/src/mode_switch.c` (new)
- `stage2/src/mode_switch_asm.S` (new — the actual trampoline)
- `scripts/test_mode_switch.sh` (new gate)
- `Makefile` (add `test-mode-switch` target; do not change existing targets)

**Contract guarantees:**
- `mode_switch_run_legacy_pm` must be a **round-trip**: on return the caller observes identical long-mode CR3, GDTR/IDTR, and callee-saved regs.
- `body` is invoked in legacy 32-bit PM with CS=code32_flat, DS/SS=data32_flat, EFLAGS.VM=0, interrupts disabled, stack provided by A.
- If `body` returns normally, A re-enters long mode and returns 0.
- If `body` faults before calling A's explicit return hook, the engine halts with a deterministic marker (not triple-fault). Recovery is Task B's concern.
- Default disarmed. Arm-gate verified before the engine touches any mode register.
- No boot-path caller permitted. Gate verifies.

**Probe (`mode_switch_probe`) minimum cases:**
1. Armed → run a body that writes a marker to serial in PM32 and returns → engine returns OK, marker observed.
2. Disarmed → engine refuses immediately, no mode register touched.
3. Wrong magic → refused.
4. Body-NULL → refused.

### 3.2 Task B — Legacy-PM v86 host

**Header:** `stage2/include/legacy_v86.h`

```c
#define LEGACY_V86_ARM_MAGIC   0xC1D39450u
#define LEGACY_V86_SENTINEL    0x0450u

typedef struct {
    uint16_t cs, ip;
    uint16_t ss, sp;
    uint16_t ds, es, fs, gs;
    uint32_t eflags;    /* VM=1 and IOPL=3 forced by host */
    uint32_t reserved[4];
} legacy_v86_frame_t;

typedef enum {
    LEGACY_V86_EXIT_NORMAL = 0,   /* INT 20h or INT 21h AH=4Ch from guest */
    LEGACY_V86_EXIT_GP_INT,       /* guest issued INT N → dispatcher callback */
    LEGACY_V86_EXIT_HALT,
    LEGACY_V86_EXIT_FAULT,
} legacy_v86_exit_reason_t;

typedef struct {
    legacy_v86_exit_reason_t reason;
    uint8_t  int_vector;          /* valid when reason == GP_INT */
    legacy_v86_frame_t frame;     /* guest state at exit */
    uint32_t fault_code;
} legacy_v86_exit_t;

int legacy_v86_enter(const legacy_v86_frame_t *entry, legacy_v86_exit_t *out);

int  legacy_v86_arm(uint32_t magic);
void legacy_v86_disarm(void);
int  legacy_v86_is_armed(void);
int  legacy_v86_probe(void);
```

**Ownership (files only Task B may create/modify):**
- `stage2/include/legacy_v86.h` (new)
- `stage2/src/legacy_v86.c` (new)
- `stage2/src/legacy_v86_pm32.S` (new — 32-bit code executed inside the mode-switch body)
- `scripts/test_legacy_v86.sh`
- `Makefile` (add `test-legacy-v86` target)

**Dependency:** consumes `mode_switch_run_legacy_pm` from Task A. May stub it locally under `#ifndef HAVE_MODE_SWITCH` for independent development.

**Contract guarantees:**
- `legacy_v86_enter` wraps `mode_switch_run_legacy_pm(body, &ctx)`. The body sets up legacy 32-bit GDT (code32, data32, tss32, code16, data16), legacy 32-bit IDT with #GP ISR, loads TR, pushes the v86 IRETL frame, and executes `IRETL`.
- Return from guest is via #GP ISR in PM32 that captures the guest frame, analyses the faulting opcode (or INT vector), marshals `legacy_v86_exit_t`, and returns out of the body.
- Host IA-32e state must be untouched on exit (Task A's guarantee transitively preserved).
- Default disarmed. Requires **both** Task A and Task B armed to enter.

**Probe (`legacy_v86_probe`) minimum cases:**
1. Disarmed → refused, no PM32 entered.
2. Armed, entry with tiny canned INT3 sequence at 0x1000:0x0000 → exit reason GP_INT vector=3, frame preserved.
3. Armed, entry with HLT → exit reason HALT.
4. Armed, entry with INT 20h → exit reason NORMAL.

### 3.3 Task C — INT dispatcher bind + loader integration

**Header:** `stage2/include/v86_dispatch.h`

```c
#define V86_DISPATCH_ARM_MAGIC   0xC1D39460u
#define V86_DISPATCH_SENTINEL    0x0460u

typedef enum {
    V86_DISPATCH_CONT,     /* guest may resume */
    V86_DISPATCH_EXIT_OK,  /* guest requested termination, value in al */
    V86_DISPATCH_EXIT_ERR,
} v86_dispatch_result_t;

v86_dispatch_result_t v86_dispatch_int(uint8_t vector, legacy_v86_frame_t *frame);

int  v86_dispatch_arm(uint32_t magic);
void v86_dispatch_disarm(void);
int  v86_dispatch_is_armed(void);
int  v86_dispatch_probe(void);
```

**Ownership:**
- `stage2/include/v86_dispatch.h` (new)
- `stage2/src/v86_dispatch.c` (new)
- `stage2/src/shell.c` (ONLY the `gem` and `dosrun` commands may be rewired; no other command may be edited)
- `scripts/test_v86_dispatch.sh`
- `Makefile` (add `test-v86-dispatch` target)

**Dependency:** consumes Task B. No stubbing recommended; C lands last.

**Contract guarantees:**
- INT 21h handlers already present in stage2 must be reused — C only wires them to this dispatcher, not reimplemented.
- INT 10h AH=00/AL=0x13 must route to existing `gfx_modes` mode13h setup; other INT 10h vectors return `CF=1 AH=0x86` initially.
- INT 16h → existing keyboard service.
- INT 33h → mouse service if armed, else `CF=1`.
- `gem` command must be the exact entry point replacing the broken 041-path invocation: arm cascade now 038→044A→044B→044C, frame-fill, `legacy_v86_enter()`, loop on `V86_DISPATCH_CONT`.

---

## 4. Shared invariants

1. All three tasks use the arm-cascade discipline established in 017..043. No single gate is removed; new gates are additive.
2. Magic numbers 0xC1D39440 / 50 / 60 and sentinels 0x0440 / 50 / 60 are reserved for A / B / C respectively.
3. No sub-task may modify `vm86_compat_entry*.S`, `vm86.c`, `vm86.h` from the 040/041 tree except to add `#ifdef` guarded stubs. The compat-mode path is kept as historical reference and may be removed only after C is merged and verified.
4. `docs/opengem-016-design.md` §0 Errata remains authoritative. When C lands, add a note closing the errata.

---

## 5. Handoff contract

Each sub-task at completion must publish:
- `docs/handoffs/YYYY-MM-DD-opengem-044-[A|B|C]-*.md` with the standard five fields.
- Diary entry in `docs/collab/diario-di-bordo.md` tagged `OPENGEM-044-A/B/C`.
- Make target name, static gate count, probe output marker strings (for cross-agent grep).

---

## 6. Assignments (proposed)

- **Task A — Mode-switch engine:** claimed by Claude Opus 4.7 on branch `feature/opengem-044-A-mode-switch`.
- **Task B — Legacy-PM v86 host:** unassigned. Can start in parallel by stubbing A.
- **Task C — Dispatcher + loader:** unassigned. Should start after B's contract stabilizes.

The user may reassign at any time. Each agent must check this section before claiming.
