# Directive f0-01: fixes from the review of commit 2003adb

- **Step:** F0 (hardware qualification). **Contracts:** `f0-acceptance.md`
  (a run stops on prerequisite failure and records later probes as not
  run; selector grammar; evidence grammar), `boot-memory.md` (safe mode is
  640×480, flag bit 0). **Input:** the review in `build/codex/review-2003adb.md`
  (copied below as findings 2–9; finding 1, the disk writer, is already
  fixed by the lead).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f0-review-fixes`. Files: `src/kernel/probes/probes.c`,
  `src/kernel/core/console.c`, `src/boot/ciukldr/menu.inc`,
  `src/boot/ciukldr/video.inc`, `scripts/test/run.py`,
  `scripts/test/evidence.py`, `scripts/test/loader_model.py`,
  `tests/suites/f0-*.json`, `tests/host/test_runner.py`,
  `tests/host/test_loader_model.py`, `tests/host/fake_qemu.py`. Nothing
  else. A parallel worktree (`wt/f1-runner`) also edits `parse_selector`,
  the loader's `probe_names` and `run.py`: keep every change minimal and
  local so the lead can merge both.

## Fixes

1. **Stop on failure** (`probes.c`, `probes_main`): each probe returns
   `int`; `core` and `all` ignore it. Stop at the first failing probe,
   emit for every remaining probe one record
   `probe=<name> event=NOT_RUN reason=prerequisite_failed after=<failed-probe>`,
   then page the evidence as today. `evidence.py` must classify such
   probes as `not_run`, never as pass. A single-probe selector is unchanged.
2. **Screen history** (`console.c`, `HIST_LINES` 512): the history must hold
   the worst-case `core` output at 80 columns: 128 E820 entries printed as
   raw and normalized records plus every other record. Derive the bound
   from the real record output (count the records each probe emits), write
   the arithmetic in a comment, and set the constant with margin. Static
   memory stays under 256 KiB.
3. **Selector echo** (`menu.inc`, `.line`/`.backspace`): echo printable
   characters to the screen as well as the UART (BIOS INT 10h AH=0Eh,
   preserving the live registers and the 64-byte limit); backspace erases
   on both (BS, space, BS) and keeps the buffer consistent. The deadline
   handling does not change.
4. **Safe mode ignores the configured mode** (`video.inc`, `mode_rank`):
   with `CBI_F_SAFE_MODE` set, the configured `preferred_mode` gets no
   priority and 640×480 ranks first, as `loader_model.py` already
   specifies. Add the model test case (configured 1024×768 plus safe →
   640×480).
5. **Canonical request construction** (`run.py`): parse each case selector
   into fields and rebuild it as `f0:<probe> run=<id> [platform=e500]
   [safe=1]`, applying the profile platform exactly once; refuse duplicate
   keys; restore the refusal of `loader_options` in suite cases (safe mode
   is expressed only by the selector). The safe-mode case must build
   correctly on `qemu-e500`.
6. **Timer predicates** (`tests/suites/f0-core.json`, and any other suite
   with the same pattern): require `final_tick − ready_tick` ≥ the minimum
   the kernel guarantees for that case and tie `elapsed_pit_cycles` to that
   difference with the kernel's own formula and tolerance (read the probe
   code; do not invent a tolerance). A fabricated record set with both
   ticks zero and claimed cycles must fail `Parser.check()`; add that as a
   host test.
7. **Kernel parser hardening** (`probes.c`, `parse_selector`): reject any
   byte outside the grammar (embedded NUL, non-printable), validate the
   probe name against the probe table plus `all`/`core`, and accept
   `platform=e500` only with `CBI_F_INPUT_FORCED` set and `safe=1` only
   with `CBI_F_SAFE_MODE` set. Check in the loader sources that those flags
   are set together with the suffixes; if they are not, report it instead
   of changing files outside this directive.

## Host tests (mandatory)

`tests/host/test_runner.py`: canonical reconstruction (safe + e500 order,
duplicate platform refusal, `loader_options` refusal); evidence checker
with `NOT_RUN` records; the timer-predicate fabrication above.
`tests/host/test_loader_model.py`: safe ranking with a configured mode.
Keep every existing test green. Run `python3 scripts/build_kernel.py` and
assemble the loader if your sandbox allows; say so if not.

## Acceptance by the lead

Diff review; host tests; kernel build with the FPU audit; on QEMU
`f0-smoke`, `f0-core` (now including the safe-mode case) and `f0-panic`,
plus a manual `core` run with a probe forced to fail to see the `NOT_RUN`
records and the paging. Then a new hardware image replaces
`ciukios-2003adb0.img`. Reply with: files, test output, and for each of
findings 2–9 whether you confirmed it, fixed it, or disagree and why.
