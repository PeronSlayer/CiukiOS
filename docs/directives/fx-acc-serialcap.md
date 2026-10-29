# fx-acc-serialcap: acc-t23 item 2, Quake III case stopped by the serial log cap

- **Written:** 2026-10-29 10:44 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-acc-serialcap`, branch `wt/fx-acc-serialcap` from `main`.
- **Evidence:** image `7c01ad46…`, case `acc-quake3`. The serial log is `/home/peronslayer/Desktop/CiukiOS/build/test-runs/acc-t23/f627080e/serial.log` (4,194,277 bytes, the runner cap), with `export.log`, `result.json` and `screen-fail.ppm` in the same directory.

## Defect

The case failed with `serial/stderr log cap reached` before the installer finished. The log is dominated by diagnostic records:

| Records | Kind |
|---|---|
| 6,744 | `[cpu] task=desktop#N class=2 base=7 effective=7 input=0 io=0 …` |
| 1,930 | `[mem] ledger snapshot=` |
| 1,443 | `[cpu] task=storage-#…` |
| 1,150 | `[mem] activity snapshot=` |
| 585 | `[sched] inherit from=storage-writer#…` |

On 2026-10-27, before the security and CPU-scheduling integrations, the same case reached the installer's end without the cap.

## Scope

1. Find which merged change made these records periodic or per-event at this rate. Count records per second from the log timestamps or ordering, and attribute each emitter in the source.
   - Is the desktop really changing priority state (input or io boosts) thousands of times, which would be a scheduler defect costing CPU on the Pentium III?
   - Or does an unchanged state get re-logged?
2. Fix the cause.
   - A diagnostic record is emitted on a state change, or rate-limited as the project's earlier rule requires: full statistics at most once a minute (diary 2026-10-25-03).
   - If the scheduler really churns, fix the churn, not only the log.
3. Add a host test bounding the record rate in the reproduced scenario. Check that no other emitter of `[cpu]`, `[mem]` or `[sched]` records can exceed the bound during a long install.
4. Keep the records that acceptance predicates and the self-test parse. List them, and check that they keep their format.
5. Run the kernel host tests touching the scheduler, memory ledger and storage, sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds; the lead reruns the case.
- Write the report to `build/codex/fx-acc-serialcap-report.md`.
