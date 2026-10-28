# fx-vmclock: per-VM DOS audio clock IRQ budget regression

- **Written:** 2026-10-27 02:34 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main 41732d190.
- **Failure.** `python3 -B tests/host/irq_work_replay.py` on main fails with `f0-46 IRQ model handler=DOS-audio-clock-per-VM samples=84389 max_us=252 budget_us=250 FAIL`, then `IRQ model failures: vm-clock`. It started after today's merges of wt/fx-biosvm, wt/fx-doomfs2 (lean exits for non-VGA ports in native full screen), wt/fx-dosmouse (INT 33h, DPMI mouse callbacks, mouse capture) and wt/fx-acpioff (main 41732d190).
- **History.** The same model went from 253 to 250 µs in 1.0.1. The fix then was an inline range test in `transfer()` (src/kernel/vm/dosvm.c), which lets DMA and low-memory copies skip the windowed-VBE and LFB lookups.

## Scope
1. Find which merged change put new work on the per-VM audio clock path. Bisect between 6dd6049fd^1 and main with the single replay, or read the instrumented trace that the replay prints.
2. Move that work off the IRQ path, or make it cheaper, without changing what it does, until the model is at or below 250 µs again with margin. Do not raise the budget.
3. Run irq_work_replay.py and the DOS, audio and kernel latency suites, one at a time with nice -n 19.

Report to build/codex/fx-vmclock-report.md. Leave changes uncommitted.
