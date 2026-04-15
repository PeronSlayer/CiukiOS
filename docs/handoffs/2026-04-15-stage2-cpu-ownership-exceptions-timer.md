# HANDOFF - stage2 cpu ownership + exceptions + timer irq

## Date
`2026-04-15`

## Context
User requested to execute all three next bootstrap steps in one iteration:
1. dedicated exception stubs (`#UD/#GP/#PF`) with clear markers,
2. stage2-owned minimal `GDT/TSS`,
3. PIC remap + PIT + IRQ0 timer ticks over serial.

## Completed scope
1. Added stage2-owned `GDT/TSS` initialization and activation (`lgdt`, segment reload, `ltr`).
2. Extended IDT setup with dedicated vectors for `#UD/#GP/#PF` and IRQ0 (`0x20`).
3. Added exception stubs in assembly and C panic reporter with deterministic log line.
4. Added PIC/PIT timer module and IRQ0 handler with serial tick checkpoints.
5. Updated stage2 flow: `GDT/TSS -> IDT -> PIC/PIT -> STI -> HLT loop`.
6. Extended automated stage2 boot test with new required markers.
7. Updated phase progress doc with Iteration 5.

## Touched files
1. `stage2/include/cpu_tables.h`
2. `stage2/include/interrupts.h`
3. `stage2/include/timer.h`
4. `stage2/src/cpu_tables.c`
5. `stage2/src/gdt_flush.S`
6. `stage2/src/interrupts.c`
7. `stage2/src/interrupt_stub.S`
8. `stage2/src/timer.c`
9. `stage2/src/stage2.c`
10. `scripts/test_stage2_boot.sh`
11. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: use one IST-backed interrupt stack in TSS (`ist1`) for critical exceptions.
Reason: safer baseline for early bring-up and better resilience in fault paths.
Impact: `#UD/#GP/#PF` can run on known-good stack even if runtime stack is damaged.

2. Decision: route all unspecified IDT vectors to a fail-stop default stub.
Reason: deterministic behavior during incremental interrupt bring-up.
Impact: unexpected vectors halt cleanly instead of causing undefined flow.

3. Decision: remap PIC to `0x20/0x28` and unmask only IRQ0.
Reason: keep timer path minimal while avoiding unrelated IRQ noise.
Impact: deterministic first interrupt pipeline for validation.

4. Decision: emit first timer tick marker and then every 100 ticks.
Reason: prove IRQ flow without flooding serial output.
Impact: stable logs and test-friendly checkpoints.

## ABI/contract changes
1. New internal stage2 bootstrap sequence contract:
   `stage2_init_gdt_tss()` must run before `stage2_init_idt()` to ensure valid selectors/TSS.
2. New ISR contract:
   `stage2_timer_on_irq0()` must send PIC EOI on every IRQ0.

## Tests executed
1. `make clean && make`
Result: PASS.

2. `make test-boot`
Result: PASS (`test-stage2` + `test-fallback`).

3. Stage2 required markers now include:
- `[ ok ] stage2 local gdt+tss is active`
- `[ ok ] pic remapped and pit started`
- `[ ok ] interrupts enabled (timer irq0)`
- `[ tick ] irq0 #0000000000000001`

## Current status
1. Stage2 owns core CPU tables (`GDT/TSS/IDT`) and can receive timer IRQ0.
2. Exception diagnostics are deterministic for `#UD/#GP/#PF`.
3. Boot path remains stable both with and without stage2 fallback.

## Risks / technical debt
1. Exception path currently logs and halts, without register/frame dump.
2. Only IRQ0 is active; keyboard and other IRQ lines are still masked.
3. No dedicated scheduler or time source abstraction yet (ticks only).

## Next steps (recommended order)
1. Add exception frame capture (RIP/CS/RFLAGS/error-code report) for `#UD/#GP/#PF`.
2. Implement keyboard IRQ1 path with simple scancode logging.
3. Start IVT compatibility layer scaffolding aligned with DOS interrupt model.

## Notes for Claude Code
Preserve existing stage2 test markers in `scripts/test_stage2_boot.sh`. Any rename of marker strings must be synchronized in the same commit with test updates.
