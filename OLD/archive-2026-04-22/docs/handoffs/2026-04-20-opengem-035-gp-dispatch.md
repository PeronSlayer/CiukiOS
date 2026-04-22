# OPENGEM-035 — #GP dispatcher host path (arm-gated, observability only)

**Date:** 2026-04-20
**Branch:** `feature/opengem-035-gp-dispatch` (from `258c5d1`, i.e. tip of `feature/opengem-034-gp-decode`)
**Baseline:** CiukiOS Alpha v0.8.9 (unchanged — no version bump).
**Status:** Landed on feature branch; 19-gate v8086 regression green; **not** merged into `main` (awaiting explicit `fai il merge`).

## Context and goal
Close the `pending-surface=handler-frame-apply,guest-stack-iret` declared at the end of OPENGEM-034. Provide the first host-side entry point that:
1. Classifies a synthetic v8086 `#GP` into a post-decode action (IRETD / HLT / BAD_INPUT).
2. Applies the advanced CPU state back to a caller-supplied 36-byte v86 IRETD stack slot via `vm86_iret_encode_frame()`.
3. Is guarded by its own arm-gate so nothing can reach it from the default boot path.

No CPU control state is touched in this phase: no LIDT / LGDT / IRETD / IRETQ / CR-write is introduced. The ISR symbol is defined but never installed into any live IDT.

## Files touched
1. `stage2/include/vm86.h` — appended the OPENGEM-035 block:
   - `VM86_GP_DISPATCH_SENTINEL = 0x0350u`
   - `VM86_GP_DISPATCH_ARM_MAGIC = 0xC1D39350u`
   - `vm86_gp_dispatch_action` enum: `BLOCKED_NOT_ARMED`, `IRETD`, `HLT`, `BAD_INPUT`
   - `vm86_gp_dispatch_arm/disarm/is_armed/handle/probe` prototypes
2. `stage2/src/vm86.c` — appended the OPENGEM-035 implementation block:
   - Sentinel `vm86_gp_dispatch_c_sentinel[] = "OPENGEM-035"`
   - Arm flag `s_vm86_gp_dispatch_armed = 0` (default disarmed)
   - `vm86_gp_dispatch_handle()` arm-gates, invokes `vm86_gp_decode()`, classifies, and on IRETD writes the 36-byte v86 IRETD frame into the caller-supplied slot
   - `vm86_gp_dispatch_probe()` host-driven probe with ≥14 canned opcode cases + BAD_INPUT / OOB / disarmed-path / arm-magic-reject guards
3. `stage2/src/vm86_gp_dispatch.S` — **new file**: defines `vm86_gp_dispatch_isr_stub` as a deterministic halt-loop landing pad and a `.rodata` `vm86_gp_dispatch_sentinel` ASCII sentinel. No CPU-mutating opcode.
4. `scripts/test_vm86_gp_dispatch.sh` — **new gate** with 62 assertions covering: sentinels, header API, enum tokens, asm shape, forbidden-opcode scan on both asm and C 035 block, arm-flag default, magic enforcement, arm-flag-before-decoder ordering (AWK line-number check), slot-apply via `vm86_iret_encode_frame`, boot-path isolation (C + asm symbols), prior-phase-files untouched, probe surface markers, Makefile target, build artifact.
5. `Makefile` — inserted `test-vm86-gp-dispatch` target immediately after `test-vm86-gp-decode`.

## Decisions made
1. **Separate arm-gate** (new magic `0xC1D39350u`) instead of reusing 029/033 — per the handoff rule "ogni fase che davvero arma ha la sua gate". Default disarmed; magic check in `arm()`; `handle()` returns `BLOCKED_NOT_ARMED` without invoking the decoder when disarmed (gate script enforces the textual ordering).
2. **ISR stub body is `hlt + jmp`** (8-byte aligned, matching `vm86_trap_stubs.S` pattern from OPENGEM-032). Never entered; if ever entered by mistake, the CPU halts in place rather than corrupting host state.
3. **IRETD frame application** uses the existing `vm86_iret_encode_frame()` from OPENGEM-026. EFLAGS.VM=1 and IOPL=3 are enforced by the encoder, so the emitted frame is always valid for a real IRETD back into virtual-8086 regardless of caller-supplied EFLAGS.
4. **Action classification** maps every OPENGEM-034 decoder result into exactly one action: INT/INT3/INTO/IRET/PUSHF/POPF/IN_*/OUT_*/CLI/STI → IRETD; HLT/UNHANDLED → HLT; NULL_ARG/OOB → BAD_INPUT. Slot is written only on IRETD.
5. **Probe never touches the boot path.** Exposed only via its public prototype; the gate greps every `.c` and `.S` file under `stage2/` to confirm neither the C nor the asm new symbols are referenced outside `stage2/src/vm86.c` / `stage2/src/vm86_gp_dispatch.S`.
6. **Pending surface** advertised by the probe: `pe32-isr-wire,live-v86-entry` — wiring the ISR into a real PE32 IDT is deferred to OPENGEM-036+.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios.sh` → clean build, no warnings on the new .S file.
- `make test-vm86-gp-dispatch` → **62 OK / 0 FAIL** → `[PASS] OPENGEM-035 vm86 gp-dispatch gate`.
- Full 19-gate v8086 regression (`test-vm86-scaffold` through `test-vm86-gp-dispatch`) → **19/19 PASS**.
- Boot-path isolation greps green: new C/asm symbols unreferenced outside `vm86.c` / `vm86_gp_dispatch.S`.
- Forbidden-opcode scans green on both the 035 asm file and the 035 C block (no LIDT / LGDT / IRETD / IRETQ / CR-write).

## Risks and next step
### Risks
- **None introduced at the CPU level:** no CR/MSR/GDTR/IDTR mutation; the ISR symbol is a halt loop and is not wired to any live IDT.
- **Probe attack surface** is limited to a 4 KiB static BSS buffer + a 36-byte IRETD slot, both file-local to `vm86.c`. No heap, no dynamic allocation.
- **Encoder dependency:** `vm86_gp_dispatch_handle()` relies on `vm86_iret_encode_frame()` behaving per its 026 contract; any change there would silently alter 035's slot-apply semantics. Gate cross-checks the slot's EIP/CS/SS/ESP/VM/IOPL bits on every IRETD case.

### Next step (OPENGEM-036, proposed)
Wire the PE32 `#GP` ISR (`vm86_gp_dispatch_isr_stub`) into the shim IDT built by OPENGEM-032. The ISR body in .S must be fleshed out to:
- snapshot the hardware-pushed `#GP` frame (error_code, EIP, CS, EFLAGS, ESP, SS, DS, ES, FS, GS) into a `vm86_trap_frame`;
- call `vm86_gp_dispatch_handle()` with a plan-bound guest buffer;
- on `ACTION_IRETD`, reload the trap frame into the hardware-expected slot and `IRETD`;
- on `ACTION_HLT`, fall into a recoverable host trap path (new observability marker `vm86: gp-dispatch hlt exit=<reason>`).
All of the above remains arm-gated via a *separate* arm-gate (do not reuse 033 / 035 magics).

## Invariants preserved
- Alpha v0.8.9 baseline unchanged — no version bump.
- `main` untouched; branch `feature/opengem-035-gp-dispatch` awaiting explicit `fai il merge`.
- OpenGEM observability namespace (`OpenGEM: …`) unchanged; all new markers use the `vm86: …` prefix disjoint from the OpenGEM set.
- 18-gate v8086 history + `test-vm86-gp-dispatch` stack is 19/19 green.
- One branch, one commit, no force-push.
