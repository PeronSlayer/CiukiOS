# HANDOFF - stage2 mini shell commands (help/ticks/mem)

## Date
`2026-04-15`

## Context
User asked to proceed after keyboard `getc` foundation: add first usable command-line shell loop in stage2.

## Completed scope
1. Introduced dedicated shell module with DOS-like prompt `A:\>`.
2. Added line editing essentials (`enter`, `backspace`, tab->space, max line bound).
3. Implemented command parser (first-token, case-insensitive).
4. Added internal commands:
   - `help`
   - `ticks`
   - `mem`
5. Integrated shell start in stage2 runtime after interrupt enable.
6. Updated stage2 boot test with shell readiness and shell loop markers.
7. Updated phase progress with Iteration 8.

## Touched files
1. `stage2/include/shell.h`
2. `stage2/src/shell.c`
3. `stage2/src/stage2.c`
4. `scripts/test_stage2_boot.sh`
5. `docs/phase-0-kickoff.md`

## Technical decisions
1. Decision: split shell logic into its own module (`shell.c`).
Reason: keep `stage2.c` focused on bootstrap sequencing.
Impact: easier future expansion (more commands, history, parser improvements).

2. Decision: parse only first token for command dispatch.
Reason: minimal reliable baseline before adding argument parsing complexity.
Impact: stable command handling for first milestone commands.

3. Decision: continue using non-blocking keyboard polling + `hlt` idle.
Reason: integrates naturally with IRQ-driven ring buffer and avoids busy wait.
Impact: low overhead while still responsive to keyboard/timer interrupts.

## ABI/contract changes
1. New internal shell entrypoint:
   - `void stage2_shell_run(boot_info_t*, handoff_v0_t*)`
2. Stage2 runtime contract now includes shell markers:
   - `[ ok ] stage2 mini shell ready (help/ticks/mem)`
   - `[ shell ] mini command loop active`

## Tests executed
1. `make clean && make`
Result: PASS.

2. `make test-boot`
Result: PASS (`test-stage2` + `test-fallback`).

## Current status
1. Stage2 now offers an interactive minimal shell over serial.
2. Keyboard path is functional end-to-end: IRQ1 -> decode -> queue -> command loop.
3. Bootstrap stability is preserved.

## Risks / technical debt
1. No command history/edit cursor yet (only basic line editing).
2. No argument parsing beyond first token.
3. `mem` output is raw diagnostic data, not yet formatted as DOS-like utilities.

## Next steps (recommended order)
1. Add basic CLI editing upgrades (left/right optional, ctrl shortcuts optional).
2. Add first DOS-style shell internal commands (`cls`, `ver`, `echo`).
3. Define command execution boundary for future `.COM` loader handoff.

## Notes for Claude Code
Keep shell readiness markers synchronized with `scripts/test_stage2_boot.sh`. Do not make shell command interaction mandatory in automated tests because headless runs may not provide keyboard input.
