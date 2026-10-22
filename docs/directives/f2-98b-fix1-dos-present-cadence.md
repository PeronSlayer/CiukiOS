# f2-98b-fix1 — Regression: DOS mode 13h present cadence after the f2-98b merge

- **Phase/model:** F2/F3, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f2-98b-fix1` from `main`.
- **Evidence (lead, 2026-10-23 00:24):** on main at `dbb6dce3`, `scripts/test/host_kernel_tests.sh` fails in `dosvm_test` with `f3-45 flat private staFAIL 810: frames == (hz ? 30u : 35u)`. That is `tests/host/dosvm_speed_test.inc`, `test_present_rates`: 1000 ticks of linear stores in mode 13h at 70 Hz and 60 Hz. The suite was green before the merge of f2-98b (`cefba8dd`). f2-98b changed `src/kernel/vm/dosvm_vga.c`:
  - the planar cache now also applies to chain-4 (`planar_cached`, and `indexed` in the row test);
  - a row callback returning >0 means the destination already holds the scanline;
  - `video_rows` bookkeeping was added.

## Required

1. Find which of these changes alters the counted presents and whether the f3-45 expectation (35 and 30 presents) still describes the required behaviour. That expectation is the contract's half-refresh cadence for a continuously changing mode 13h screen; see the f3-45 and f3-52 documents and directives.
2. Fix the code so that both f2-98b's damage-precision goals and the f3-45 cadence hold. Change the test only if the contract itself requires the new count; then cite that contract text and update the documents.
3. Run the whole `dosvm_test` (as `scripts/test/host_kernel_tests.sh` builds it) and the f2-98b DOS tests (`tests/host/dosvm_wait_present_test.inc`, `dosvm_video_test.inc`). A LeakSanitizer exit in the sandbox is not a failure of the assertions; say so in the report.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Write `build/codex/f2-98b-fix1-report.md`.
