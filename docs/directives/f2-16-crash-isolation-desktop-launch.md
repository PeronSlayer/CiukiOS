# Directive f2-16: `crash-isolation` desktop mode fails at the launch handshake on QEMU

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`crash-isolation` row and
  the desktop qualification paragraph), directive f2-12 and its scope
  amendments, `execution-abi.md` F2 extension (call 3 bounded reports,
  mmap placement), `posix-subset.md` (surfaces, channels, grants, spawn
  groups).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-desktop-launch`, from `main` at or after `9a7fcc2`.
  Files: `src/kernel/probes/f2_probes_desktop.c`, `src/kernel/probes/selector.c`,
  `src/kernel/proc/supervisor.c`, `apps/desktop/desktop.c`, `apps/desktop/gate.h`,
  `apps/demo/demo.c`, `tests/host/proc/desktop_test.c`,
  `tests/host/desktop/desktop_gate_test.c`, `tests/host/desktop/demo_gate_test.c`,
  `tests/host/test_desktop_gate.py`, `tests/suites/f2-desktop.json` only if a
  record the fix adds must be declared, `scripts/test/host_kernel_tests.sh`.
  Identity assets, layout and copy stay untouched.

## Observed on image `81e8c43c…` (commit `6050606`, runner case `crash-isolation-normal-qemu-t23`, 2026-10-11)

```
[selector] probe=crash-isolation platform=native tsc_khz=999978
… event=BEGIN server=desktop
… event=DATA case=payload server=desktop path=/bin/desktop clients=/bin/demo
[supervisor] desktop stopped; console ready deaths=1
… event=DATA case=ledger server=desktop objects_equal=1 processes_equal=1 descriptions=0 surfaces=0 pages=0 channels=0 messages=0 grants=0
… event=END server=desktop status=FAIL reason=desktop_contract
```

The runner reports "terminal evidence before declared stimuli completed".
The desktop was spawned (no `desktop_launch` error) and the probe failed
before the `identity` record: the first handshake (`survivor_stage == 2`
within 10 s, then `GATE_SNAPSHOT`) did not complete. The "desktop stopped"
line is logged when the probe's cleanup stops the desktop, so the desktop
was alive until then. Nothing in the records says which step failed: the
probe emits no diagnostic when `native_reports.invalid` is set, when a wait
times out, or when the desktop exits by itself. The guest serial log and
the records are in `build/f2records/desktop-launch/` of the worktree. The
host tests passed because they feed the validator synthetic reports.

## What to do

1. **Diagnosability first (contract: evidence must explain a failure).**
   Emit, in desktop mode, a `case=launch` record after the handshake attempt
   with the desktop PID, the reported survivor PID, the control-page
   address, the survivor stage, the elapsed ticks and a `reason` field
   naming the first failed condition (`invalid_report:<check>`,
   `survivor_timeout`, `snapshot_timeout`, `server_exit:<status>`, …); on
   every later failure emit a `case=step` record with the command,
   generation reached and liveness. Keep the records ≤ 240 bytes.
2. **Find and fix the cause.** Candidates to check against the real code
   paths, not synthetic inputs: the validator's `control >= CIUKI_MMAP_BASE`
   and `ua_range` conditions against where the SDK's `mmap(MAP_ANONYMOUS)`
   actually places a page; the survivor's first `stage=2` report (it needs
   CONFIGURE from a desktop whose heartbeat PINGs are suppressed in gate
   mode); the `generation`/`cycle` monotonic checks on the very first
   report; `report_unsigned` on `survivor=%d`/`victim=%d`; the
   `c->ppid == p->pid` check after the probe re-parents the desktop; the
   desktop's `_exit(126)` paths when `ciuki_raw_probe_report` fails.
3. **Host test through the real path.** Build the desktop and demo natively
   as the existing harnesses do, capture the exact report lines they emit
   in gate mode, and drive them through `probe_f2_libc_report` →
   `native_report` with a process table that mirrors the guest (desktop
   re-parented to the controller, survivor spawned with a new group);
   assert that the handshake validates. Add the mmap-placement check.

## Acceptance by the lead

Host tests; kernel build; `make build-full`; on QEMU the runner case
`crash-isolation-normal-qemu-t23` passes with `server=desktop`, then the
other four desktop profiles. Reply with: files, the cause and the proof,
the new records, test output.
