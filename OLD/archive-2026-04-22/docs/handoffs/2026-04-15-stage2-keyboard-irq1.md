# HANDOFF - stage2 keyboard irq1 logging

## Date
`2026-04-15`

## Context
Requested next step after timer IRQ0: enable keyboard IRQ1 pipeline with basic scancode logging while keeping stage2 boot stable.

## Completed scope
1. Added keyboard module with IRQ1 handler and raw scancode serial logging.
2. Added IDT vector `0x21` route to dedicated IRQ1 stub.
3. Updated PIC mask to unmask IRQ0 and IRQ1 (`0xFC` on master PIC).
4. Added stage2 checkpoint marker for keyboard readiness.
5. Updated stage2 boot test required markers for the new checkpoint and interrupt enable message.
6. Updated phase progress with Iteration 6 entry.

## Touched files
1. `stage2/include/keyboard.h`
2. `stage2/src/keyboard.c`
3. `stage2/include/serial.h`
4. `stage2/src/serial.c`
5. `stage2/src/interrupts.c`
6. `stage2/src/interrupt_stub.S`
7. `stage2/src/timer.c`
8. `stage2/src/stage2.c`
9. `scripts/test_stage2_boot.sh`
10. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: keep keyboard path as raw scancode logger first (no keymap yet).
Reason: verify IRQ flow before introducing layout/state complexity.
Impact: immediate visibility for hardware path with minimal risk.

2. Decision: unmask IRQ0 + IRQ1 only on master PIC.
Reason: preserve deterministic environment and avoid noisy IRQ lines.
Impact: timer and keyboard are active; other lines remain masked.

3. Decision: add compact hex8 serial formatter for scancode output.
Reason: scancodes are byte-sized and clearer with two-digit hex logging.
Impact: cleaner debug output (`[ key ] scancode=0xNN`).

## ABI/contract changes
1. Stage2 interrupt readiness contract now includes keyboard path:
   - IDT vector `0x21` installed before `sti`.
   - PIC master mask must keep IRQ1 unmasked.

## Tests executed
1. `make clean && make`
Result: PASS.

2. `make test-boot`
Result: PASS (`test-stage2` + `test-fallback`).

## Current status
1. Stage2 receives IRQ0 timer and is ready to receive IRQ1 keyboard interrupts.
2. Keyboard IRQ handler logs raw scancodes when input occurs.
3. Existing fallback path remains stable.

## Risks / technical debt
1. No scancode set decode/state machine yet (make/break/extended keys).
2. No ring buffer yet; keyboard data is only logged to serial.
3. No consumer API for higher-level CLI input pipeline yet.

## Next steps (recommended order)
1. Add keyboard ring buffer (`push` on IRQ1, `pop` from runtime code).
2. Implement basic Set-1 decoder (make/break + extended `0xE0` handling).
3. Expose minimal `getc` primitive for DOS-like CLI shell scaffolding.

## Notes for Claude Code
Keep test markers in `scripts/test_stage2_boot.sh` synchronized with stage2 log strings. Do not require `[ key ]` marker in automated tests because headless runs may not inject key events.
