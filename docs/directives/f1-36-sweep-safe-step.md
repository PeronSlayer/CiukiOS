# Directive f1-36: the `safe` step of a hardware sweep and the operator's safe-mode boot

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`safe` row: safe mode
  disables optional devices, no mounts, console and input required; the
  physical `safe-menu-*` cases), `test-architecture.md` (NOT_RUN and
  prerequisite rule, operator-confirmation cases, hardware tier), directives
  f1-28 (sweep cursor), f1-31 (operator prompts), f1-33 and f1-35 (import).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-sweep-safe`, from `main` at or after `21389f9`.
  Files: `src/kernel/probes/safe_probe.c`, `src/kernel/probes/probes.c`
  (sweep accounting only if needed), `scripts/test/physical.py` (case
  mapping; directive f1-35 is changing the boot-level provenance and
  device-exception rules of the same file in parallel: keep your changes
  local to the `safe` mapping), `tests/host/*`, `tests/suites/f1-safe.json`
  only if a record changes, `docs/build-and-run.md` (operator procedure),
  one paragraph in `f1-acceptance.md` and `test-architecture.md`. The
  third sweep's capture is in `build/t23/sweep-77777777.log`.

## Observed on the ThinkPad T23 (sweeps `44444444`, `66666666`, `77777777`)

The sweep boots every step with `BOOT.CFG` `safe=0` because the cursor
for the next step is written to the volume before each step and safe mode
forbids volume writes (the loader writes nothing; the selector's ` safe=1`
is honoured only under QEMU, `selector.c`). The `safe` step therefore
always records `case=option safe_mode=0 … menu=0`, `case=storage mounts=1
writes=0`, `optional_activations=2`, `source=none`, and ends `FAIL
reason=safe_contract` (`subcase=required … errors=1`, where `errors` is
`!ok`). On all three sweeps `console=1 input=1` held. The verdict is
structurally false on a sweep boot: the probe never observed a safe-mode
boot, so neither PASS nor FAIL is evidence. The physical cases
`safe-menu-t23` and `safe-menu-e500` (`tests/suites/f1-safe.json`,
`physical_cases`, `source: menu`, `safe_mode eq 1`) are the hardware
evidence for safe mode and are `not_run` without an operator boot.

## What to do

1. **Sweep step.** When the `safe` probe runs from a sweep cursor and
   the boot is not in safe mode (`!(g_boot.flags & CBI_F_SAFE_MODE)` and
   no safe-mode source), it emits its observations as today, then
   `DATA subcase=required status=not_run reason=safe_mode_not_selected`
   and `END status=NOT_RUN reason=safe_mode_not_selected`, and the sweep
   step counts as `not_run` with that reason (the `SWEEP` record and
   `SWEEP_END`). Outside a sweep (selector or QEMU cases) the verdict is
   unchanged: a `safe` case that expects safe mode and does not get it
   still fails.
2. **Import mapping.** In the physical import, the `safe` QEMU cases map
   to `not_run reason=safe_mode_not_selected` from that record; the
   `safe-menu-<target>` physical case is qualified from a separate
   acquisition of one operator boot in safe mode (its own capture
   directory with `acquisition.json` `selector_source: menu`), with the
   existing predicates (`safe_mode eq 1`, `source=menu`, `optional_activations
   eq 0`, no mounts) and the operator confirmation already required.
3. **Operator procedure.** Document in `docs/build-and-run.md`, from the
   loader source (`src/boot/`), the exact key sequence that boots the
   hardware image in safe mode and runs the `safe` probe with a run
   identity, without rewriting the disk and after a completed sweep
   (`S` then `P` and the selector, or whatever the loader actually
   supports; if the current loader cannot combine safe mode with a probe
   selector on hardware, say so and propose the smallest loader change
   as an open point instead of implementing it).
4. **Host tests:** the sweep-step NOT_RUN records (synthetic sweep
   capture), the `safe` mapping, a synthetic safe-menu acquisition that
   passes, the QEMU `safe` cases unchanged.

## Acceptance by the lead

Host tests; QEMU `f1-safe` unchanged on five profiles; on the next T23
sweep the `safe` step is `not_run reason=safe_mode_not_selected`; the
operator's safe boot, captured and imported, qualifies `safe-menu-t23`.
