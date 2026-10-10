# Directive f1-31: operator prompts inside the unattended sweep for the input-dependent cases

- **Step:** F1 (sweep infrastructure, used by F1 and F2 cases).
  **Contracts:** `f1-acceptance.md` (`input` row: physical key
  transitions on hardware), `f2-acceptance.md` (`crash-isolation`:
  "an actual desktop MUST redraw and consume a post-fault key and
  mouse/button sequence"; graphical interaction demonstrated by guest
  counters and external observation), `test-architecture.md` (hardware
  tier, operator-confirmation cases), directive f1-28.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-sweep-prompts`, from `main` at or after `abcd677`.
  Files: `src/kernel/probes/probes.c`, `selector.c`, `src/kernel/drivers/i8042_probe.c`
  (the `input` probe's stimulus wait), `src/kernel/probes/f2_probes_desktop.c`
  (the interaction wait), the console output helper used by probes,
  `scripts/test/physical.py` (import of the new not_run reasons),
  `tests/host/*`, `docs/build-and-run.md` (what the operator does), one
  paragraph in `test-architecture.md`.

## Observed on the ThinkPad T23 (image `90477716…`, commit `abcd677`, sweep `66666666`, 2026-10-11)

With the input driver now initialising (`input result=ready`, 15 setup
steps `result=0`), two cases still fail only because nobody is at the
keyboard: `input` ends `FAIL reason=stimulus_or_lease` (no key
transitions arrived), and `crash-isolation` completes its 100 cycles
with the real desktop and then fails the interaction
(`input_events=0 keys=0 motion=0 buttons=0`). On QEMU the runner injects
these through QMP.

## Required behaviour

1. In a sweep boot (selector source `cfg`), a probe that needs operator
   input shows a bounded prompt on the screen and the serial
   (`[operator] press A, move the pointer, click (60 s)`), waits up to
   60 s for the first event, then runs its normal stimulus window; if
   nothing arrives it records `status=not_run reason=operator_absent`
   for that subcase (the probe's other subcases keep their verdicts) and
   the sweep continues. The runner-driven QEMU path is unchanged.
2. The sweep summary counts these as `not_run` with the reason; the
   physical import maps them to the operator-confirmation class.
3. Host tests: the prompt/wait state machine with a fake event source
   (event within the window → normal path; none → not_run), record
   formats, import mapping.

## Acceptance by the lead

Host tests; kernel build; QEMU `f1-input` and `f2-desktop` unchanged; on
the T23 sweep with the operator present, `input` and the interaction
pass; without, both are `not_run reason=operator_absent`.
