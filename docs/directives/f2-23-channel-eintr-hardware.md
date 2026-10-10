# Directive f2-23: `signals-fault` `channel-eintr` is never injected on the ThinkPad T23

- **Step:** F2. **Contracts:** `posix-subset.md` (EINTR semantics of a
  blocking channel receive; signal delivery to a blocked thread),
  `f2-acceptance.md` (`signals-fault` row, `syscall-interruption`
  cases), directive f2-22 (hardware timing lesson: record output at
  38400 baud consumes probe deadlines).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-channel-eintr-hw`, from `main` at or after
  `857b5b7`. Files: `src/kernel/probes/f2_probes_signals.c`, the channel
  wait path (where `channel_recv` blocks and the task states it uses),
  `tests/host/proc/signal_payload.asm` and `signal_test.c` only if the
  payload changes, `tests/host/*` predicates, `tests/suites/f2-runtime.json`
  only if a record changes. The capture is in
  `build/t23/sweep-77777777.log` of the worktree.

## Observed on the ThinkPad T23 (image `6ed146ab…`, commit `96440d6`, sweep `77777777`, step 33, 2026-10-11)

`fault-repair` now passes (`entries=7 returns=7`: f2-22 confirmed on
hardware). Every other subcase shows `expected == observed`. The only
non-conforming subcase is `channel-eintr` (mode 15):

```
case=channel-eintr part=identity pid=20 tid=20 sender=0
case=channel-eintr part=fault signal=0 vector=0 code=0 trap_error=00000000 address=00000000 eip=00000000
case=channel-eintr part=status expected=0 observed=0 raw_vector=0 corruption=0
case=channel-eintr part=handlers entries=0 returns=0 max_depth=0 order=0
case=channel-eintr part=sleep result=0 elapsed_ms=0 saved_eax=00000000 resumed_eax=00000000
case=syscall-interruption syscall=channel_recv result=0 saved_eax=00000000 resumed_eax=00000000 releases=0 handler_completion=0
case=channel-eintr survivor_pid=3 progress_before=382 progress_after=3403 alive=1
… event=END status=FAIL reason=signal_contract
```

No `part=inject` record exists: the probe emits it only when
`inject_tid` is set, and mode 15 never sets it even when it injects. The
survivor progress jump 382→3403 (about 3000 ticks; the other subcases
take about 22) equals the 2000-tick wait loop plus the 1000-tick zombie
wait: the injection predicate `r.stage == 2 && r.tid &&
proc_thread_find(p, r.tid)->task->state == T_BLOCKED` never held in 2 s,
the payload stayed in `channel_recv`, and the probe stopped it. On QEMU
(`qemu-t23`, `qemu-e500`, `qemu-min128`) the same case passes. Nothing on
the stage-2 path emits records, so serial speed is not the cause this
time. Candidates: the thread's state while blocked in `channel_recv` on
hardware is not `T_BLOCKED` (a timed or yielding wait), the payload's
`TID`/`STAGE` write order versus the probe's `ua_read`, a failed
precondition in the channel-pair path under hardware timing (the `EQ`
after `CHANNEL_PAIR` is only inspected at completion), or the thread
lookup.

## What to do

1. Determine why the predicate never held on hardware: inspect the
   `channel_recv` blocking path and the task states it uses, the
   payload's write order and the probe's polling. Fix the kernel or the
   probe so the injection happens on hardware as on QEMU and the payload
   observes `-EINTR` (`result=-4`, `saved_eax`/`resumed_eax` = `-EINTR`,
   `entries=1`, `returns=1`).
2. Diagnostics regardless of the fix: for modes 15, 16 and 17 emit
   `part=inject` as the other modes do, and when the deadline expires
   without injection emit one `part=wait` record with the last observed
   stage, tid, the thread's task state and the ticks waited, so a future
   hardware failure is attributable from the capture.
3. Host tests: the payload under the host harness unchanged; parsing of
   the new records; a test of the predicate against every task state the
   wait path can produce.

## Acceptance by the lead

Host tests; QEMU `signals-fault` on three profiles PASS; on the next T23
sweep `channel-eintr` reports `entries=1 returns=1 result=-4` and the
probe passes.
