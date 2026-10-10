# Directive f1-34: operator-driven input evidence on hardware (`input` stimulus, desktop interaction, desktop exit)

- **Step:** F1/F2. **Contracts:** `f1-acceptance.md` (`input` row: the
  QEMU stimulus is 100 A cycles, 100 moves, 10 clicks with exact totals;
  the physical-transition requirement for T4), `f2-acceptance.md`
  (`crash-isolation` interaction: "redraw and consume a post-fault key and
  mouse/button sequence"), directive f1-31 (operator prompts), f2-12 and
  f2-16 (real desktop in gate mode).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-operator-criteria`, from `main` at or after
  `857b5b7`. Files: `src/kernel/drivers/i8042_probe.c`,
  `src/kernel/probes/f2_probes_desktop.c`, `apps/desktop/*` (gate loop,
  input counting), `apps/demo/demo.c` only if the exit is in the demo,
  `tests/host/*`, `tests/suites/f1-input.json` and `f2-desktop.json` only
  if a record changes, one paragraph in each contract. The capture of the
  third sweep is in `build/t23/sweep-77777777.log` of the worktree.

## Observed on the ThinkPad T23 (image `6ed146ab…`, commit `96440d6`, sweep `77777777`, operator present, 2026-10-11)

`input`: the operator answered the prompt. Counts:
`characters=113 key_transitions=200 button_transitions=74 events=3150
x=3667 y=-4760`; queue `overflow=0 resync=0 duplicates=0 repeats=0
errors=0 stuck_keys=0`; lease retained; verdict `FAIL
reason=stimulus_or_lease`. The verdict in `i8042_probe.c` requires the
exact QEMU totals (`characters == 100`, text digest of 100 `a`,
`key_transitions == 200`, `button_transitions == 20`, `x == 200`,
`y == -100`); the sampling loop stops at its completion thresholds
(≥ 100 characters, ≥ 200 transitions, ≥ 20 button transitions, `x ≥ 200`,
`y ≤ -100`, then 50 ms quiet) before a human has finished, and the prompt
asks for totals a human cannot hit.

`crash-isolation`: 100 cycles pass. Interaction records: `ARM
action=post_fault_input presents=907 input_events=0`; `stage=after
presents=960 input_events=108 changed=0 keys=8 motion=61 buttons=0`;
then `case=step command=6 generation=502 reached=502 live=0
reason=server_exit:256`: `/bin/desktop` exited with status 1 during the
operator's interaction. The probe's interaction thresholds (`keys ≥ 2`,
`motion ≥ 1`, `buttons ≥ 2`) are already thresholds; the failure is
`buttons=0` and the exit. In the same sweep the input probe counted 74
button transitions from the same TrackPoint buttons, so button packets
reach the driver; the desktop's gate counter only counts events of type
`CIUKI_INPUT_BUTTON` (`desktop.c`, interaction branch).

In `desktop.c` gate mode every input event during the interaction sets
`report=1`; each report flushes every client and returns 1 when any
client still has `tx_count` pending (the `if (d->clients[i].tx_count)
return 1;` line), then redraws the whole frame. A human stream of 3150
motion events in a few seconds, with a 38400-baud serial console, is far
denser than the QEMU stimulus this check was written for.

## What to do

1. **Operator criteria for `input`.** When the stimulus comes from the
   operator (`probes_operator_mode`, prompt answered) the verdict uses
   thresholds: at least 10 `a` characters (other characters allowed,
   counted and recorded), `key_transitions ≥ 20`, `button_transitions ≥ 2`
   (at least one press and one release), `|x| + |y| ≥ 50`; sampling ends
   when all thresholds are met and 1 s has passed without events, or at
   the 120 s deadline. The loss, duplicate, stuck-key and lease checks
   are unchanged (`overflow`, `resync`, `duplicates`, `repeats`, `errors`
   unchanged; no keys or buttons down at the end; lease retained). The
   `ARM` and `case=counts` records state `criteria=operator` with the
   thresholds; the prompt text states them ("press A at least ten times,
   move the pointer, click at least once"). The QEMU stimulus is
   unchanged (`criteria=exact`).
2. **Desktop button counting.** Find why the desktop counts `buttons=0`
   while the driver saw 74 button transitions: the event type the input
   device delivers to user space for PS/2 button changes
   (`CIUKI_INPUT_BUTTON` versus a button mask carried in motion events),
   `desk_input` and the gate counters, and the input probe's digest. Fix
   so a button press and release reach `gate_buttons`; host test with the
   event types and counts of the capture.
3. **Desktop exit during interaction.** Make the gate loop survive an
   operator-density event stream: a report per input event must not fail
   on a client with pending transmissions (coalesce reports, bounded
   flush retries, or at most one report per frame), and no other
   `return 1` path may be reachable from ordinary input. Record the reason
   of any gate failure (a `gate_setup_error`-style record or a stderr
   line naming the path) so a future exit is attributable instead of a
   bare status 1. Native harness test replaying a burst of 3000 motion,
   200 key and 74 button events during the interaction.
4. **Interaction criteria.** Keep `keys ≥ 2`, `motion ≥ 1`, `buttons ≥ 2`
   in `f2_probes_desktop.c`; record `criteria=operator` when the
   operator produced the events (prompt answered) so the import can
   distinguish them.

## Acceptance by the lead

Host tests; QEMU `f1-input` (five profiles) and `f2-desktop`
`crash-isolation` unchanged (`criteria=exact`); on the next T23 sweep
with the operator: `input` PASS with `criteria=operator`,
`crash-isolation` interaction PASS with `buttons ≥ 2` and the desktop
alive to the end.
