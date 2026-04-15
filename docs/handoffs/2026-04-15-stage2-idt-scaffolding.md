# HANDOFF - stage2 local IDT scaffolding

## Date
`2026-04-15`

## Context
Phase progression toward DOS-compatible runtime: reduce dependency on firmware CPU state by loading a stage2-owned IDT baseline.

## Completed scope
1. Added `stage2` interrupt subsystem skeleton (`interrupts.c/.h`).
2. Added assembly default interrupt stub (`interrupt_stub.S`) with safe halt loop.
3. Stage2 now initializes and loads a local 256-entry IDT.
4. Added boot log checkpoint: `[ ok ] stage2 local idt is active`.
5. Updated automated stage2 boot test to assert the new checkpoint.

## Touched files
1. `stage2/include/interrupts.h`
2. `stage2/src/interrupts.c`
3. `stage2/src/interrupt_stub.S`
4. `stage2/src/stage2.c`
5. `scripts/test_stage2_boot.sh`
6. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: initialize all 256 IDT vectors to one default stub.
Reason: keep scaffolding minimal and deterministic while no per-vector policy exists.
Impact: stage2 now owns IDT setup and can evolve vector-by-vector in next steps.

2. Decision: default handler loops in `cli; hlt`.
Reason: fail-stop behavior is safer than undefined returns during early bring-up.
Impact: unexpected interrupts/exceptions stop predictably for debug.

3. Decision: resolve CS selector at runtime (`mov %cs`) when creating gates.
Reason: avoid hardcoding firmware selector values.
Impact: more robust across boot environments.

## ABI/contract changes
None.

## Tests executed
1. `make clean && make`
Result: PASS.

2. `make test-stage2`
Result: PASS.

3. `make test-fallback`
Result: PASS.

## Current status
1. Stage2 now loads a local IDT before entering idle loop.
2. Boot log includes explicit IDT readiness checkpoint.
3. Interrupt policy and vector-specific handlers are not implemented yet.

## Risks / technical debt
1. Default interrupt stub is fail-stop only; no diagnostics per vector yet.
2. No dedicated GDT/TSS ownership in stage2 yet.
3. PIC/PIT/keyboard wiring is still pending (Phase 2).

## Next steps (recommended order)
1. Add vector-aware exception stubs (at least #GP/#PF/#UD markers).
2. Introduce stage2-owned minimal GDT/TSS and validate transition.
3. Start PIC remap + timer ISR skeleton with serial tick marker.

## Notes for Claude Code
Preserve current checkpoint strings used by `scripts/test_stage2_boot.sh`. If new checkpoints are added, extend `required_patterns` in the same change set.
