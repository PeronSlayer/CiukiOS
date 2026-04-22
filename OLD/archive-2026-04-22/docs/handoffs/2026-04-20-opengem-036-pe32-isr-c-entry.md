# OPENGEM-036 — PE32 #GP ISR C-side entry (arm-gated, observability only)

**Date:** 2026-04-20
**Branch:** `feature/opengem-036-pe32-isr-c-entry` (from `main@0f79062`, i.e. post-035 merge)
**Baseline:** CiukiOS Alpha v0.8.9 (unchanged — no version bump).
**Status:** Landed on feature branch; 20-gate v8086 regression green; **not** merged into `main` (awaits explicit `fai il merge`).

## Context and goal
First half of the `pe32-isr-wire` pending surface advertised at the end of OPENGEM-035. The PE32 `#GP` ISR (whose body is still the halt-loop from 035) needs a C-side entry point so the future asm body (OPENGEM-037) can tail-call into it after snapshotting the hardware-pushed trap frame. This phase delivers that C entry under its own independent arm-gate, observability-only.

## Files touched
1. `stage2/include/vm86.h` — appended OPENGEM-036 block:
   - `VM86_GP_ISR_C_SENTINEL = 0x0360u`
   - `VM86_GP_ISR_ARM_MAGIC  = 0xC1D39360u` (dedicated; disjoint from 029/033/035 magics)
   - `vm86_gp_isr_c_arm / _disarm / _is_armed / _entry / _probe` prototypes
2. `stage2/src/vm86.c` — appended OPENGEM-036 implementation block:
   - Sentinel `vm86_gp_isr_c_sentinel[] = "OPENGEM-036"`
   - Arm flag `s_vm86_gp_isr_c_armed = 0` (default disarmed)
   - `vm86_gp_isr_c_entry()` — validates inputs (NULL in_frame / NULL guest_base / zero guest_size → BAD_INPUT), arm-gates (disarmed → BLOCKED, decoder not invoked, out_frame/slot untouched), copies `in_frame` into a local working frame, routes through `vm86_gp_dispatch_handle()` from 035, writes the post-decode frame into `out_frame` only on non-BLOCKED paths. Input `in_frame` is never mutated.
   - `vm86_gp_isr_c_probe()` — 6-case host-driven probe: (A) INT21h armed path → IRETD + EIP advance + slot VM/IOPL3 verified; (B) HLT → ACTION_HLT + slot untouched; (C) NULL in_frame → BAD_INPUT; (D) NULL guest_base → BAD_INPUT; (E) zero guest_size → BAD_INPUT; (F) gate independence (disarm 036 only, 035 stays armed, entry still returns BLOCKED_NOT_ARMED).
3. `scripts/test_vm86_gp_isr_c.sh` — **new gate** with 57 assertions.
4. `Makefile` — `test-vm86-gp-isr-c` target after `test-vm86-gp-dispatch`.

## Decisions made
1. **Separate arm-gate** with magic `0xC1D39360u`, disjoint from 029 / 033 / 035 magics. Both 035 and 036 gates must be armed independently to reach the decoder — gate verifies this via Case F.
2. **asm stub file UNTOUCHED** — `stage2/src/vm86_gp_dispatch.S` remains the 035 halt-loop. Filling the asm body (frame-capture + tail-call into `vm86_gp_isr_c_entry`) is the OPENGEM-037 scope.
3. **No LIDT / LGDT / IRETD / IRETQ / CR-write** introduced by 036 C block — forbidden-opcode scan enforces this.
4. **Defensive copy of in_frame** into a local working frame before dispatch, so the caller's hardware-captured frame is never mutated in place. Mirrors the contract the PE32 asm stub will follow once written.
5. **out_frame is skipped on BLOCKED** — preserves the "disarmed path leaves caller state pristine" invariant shared by 029/033/035.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios.sh` → clean build.
- `make test-vm86-gp-isr-c` → **57 OK / 0 FAIL** → `[PASS] OPENGEM-036 vm86 gp-isr-c gate`.
- Full 20-gate v8086 regression (017..036) → **20/20 PASS**.
- Boot-path isolation greps: every new C symbol (`vm86_gp_isr_c_*`) unreferenced outside `stage2/src/vm86.c`.
- Forbidden-opcode scan on the 036 C block: no LIDT / LGDT / IRETD / IRETQ / CR-write.
- Prior-phase asm files (`vm86_switch.S`, `vm86_lidt_ping.S`, `vm86_trap_stubs.S`, `vm86_snapshot.S`, `vm86_gp_dispatch.S`) do not reference any 036 symbol.

## Risks and next step
### Risks
- **None introduced at the CPU level.** No CR / MSR / GDTR / IDTR mutation. ISR asm body is still the 035 halt-loop.
- **Probe attack surface** limited to a 4 KiB static BSS buffer + 36-byte IRETD slot, both file-local to `vm86.c`. Independent of the 035 probe's buffers.

### Next step (OPENGEM-037, proposed)
Write the real asm body of `vm86_gp_dispatch_isr_stub` in `stage2/src/vm86_gp_dispatch.S`:
- Push all guest GPRs and segment selectors into a `vm86_trap_frame`-layout stack frame.
- Pass the frame pointer (plus plan-bound `guest_base`, `guest_size`, and `disp`/`task`) to `vm86_gp_isr_c_entry()`.
- On return, branch on the action:
  - `ACTION_IRETD` → reload the trap frame into the hardware-expected slot and `IRETD`.
  - `ACTION_HLT` / `ACTION_BAD_INPUT` / `ACTION_BLOCKED_NOT_ARMED` → fall into a deterministic halt path emitting `vm86: gp-isr-asm hlt-exit=<reason>`.
That phase WILL introduce `IRETD` in the asm file, so the gate's forbidden-opcode scan must scope `iretd` as permitted only in `vm86_gp_dispatch.S` from 037 onward.

Installation into a LIVE PE32 IDT is a *separate* downstream phase (OPENGEM-038), guarded by yet another independent arm-gate.

## Invariants preserved
- Alpha v0.8.9 baseline unchanged — no version bump.
- `main` untouched; branch awaits explicit `fai il merge`.
- `vm86: …` marker namespace; disjoint from OpenGEM.
- 19-gate v8086 history + `test-vm86-gp-isr-c` stack is **20/20 green**.
- One branch, one commit, no force-push.
