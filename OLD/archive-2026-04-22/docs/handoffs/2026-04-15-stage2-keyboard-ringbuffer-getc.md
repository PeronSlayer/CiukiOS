# HANDOFF - stage2 keyboard ring buffer + set1 decode + getc

## Date
`2026-04-15`

## Context
Next phase request: evolve keyboard IRQ1 from raw logging to usable input foundation for DOS-like CLI, with minimal `getc` primitives.

## Completed scope
1. Added keyboard ring buffer (IRQ1 producer, runtime consumer).
2. Added minimal Set1 decoder (make/break handling, shift state, basic `0xE0` handling).
3. Added `stage2_keyboard_getc_nonblocking()` and `stage2_keyboard_getc_blocking()` APIs.
4. Integrated runtime input loop in stage2 (`[ input ] ascii=...`).
5. Updated stage2 checkpoint marker for decoder readiness.
6. Updated phase progress with Iteration 7.

## Touched files
1. `stage2/include/keyboard.h`
2. `stage2/src/keyboard.c`
3. `stage2/src/stage2.c`
4. `scripts/test_stage2_boot.sh`
5. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: fixed-size ring buffer (`128`) with overwrite-on-full policy.
Reason: keep IRQ path non-blocking and deterministic.
Impact: newest input is preserved under burst conditions.

2. Decision: decoder scope intentionally minimal (ASCII-focused keys only).
Reason: bootstrap CLI needs letters/digits/basic symbols first.
Impact: function keys and full extended-key semantics are deferred.

3. Decision: protect pop path with IRQ save/restore.
Reason: avoid race between runtime consumer and IRQ1 producer on head/tail indexes.
Impact: stable queue semantics on single-core early runtime.

## ABI/contract changes
1. New keyboard API contract:
   - `i32 stage2_keyboard_getc_nonblocking(void)` returns `-1` if empty.
   - `u8 stage2_keyboard_getc_blocking(void)` waits for next decoded byte.

## Tests executed
1. `make clean && make`
Result: PASS.

2. `make test-boot`
Result: PASS (`test-stage2` + `test-fallback`).

## Current status
1. Stage2 can collect IRQ1 scancodes and expose decoded bytes via `getc` APIs.
2. Runtime loop is now ready for simple CLI scaffolding.
3. Boot stability is preserved on both stage2 and fallback paths.

## Risks / technical debt
1. No CapsLock/Ctrl/Alt state handling yet.
2. Extended keys are minimally handled and mostly ignored.
3. Buffer currently stores decoded bytes only (no rich key event struct).

## Next steps (recommended order)
1. Add command-line input layer on top of `getc_nonblocking` (edit/backspace/enter).
2. Introduce tiny shell loop prototype in stage2 (`help`, `ticks`, `mem`).
3. Define transition boundary between stage2 shell and DOS runtime core.

## Notes for Claude Code
Keep marker strings synchronized with `scripts/test_stage2_boot.sh`. Do not require real keyboard keypress markers (`[ key ]` / `[ input ]`) in automated tests because headless QEMU may not inject input.
