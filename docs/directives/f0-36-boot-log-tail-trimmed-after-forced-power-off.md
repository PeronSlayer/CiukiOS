# f0-36 — after a forced power-off the boot log's unpublished tail is trimmed instead of recovered

- **Phase:** F0 (filesystem). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-36` from main `684033e6`.
- **Authority:** `src/kernel/fs/fat.c` (f0-25 log recovery, f0-32 tail-trim repair, `fat_write_reserved`/`fat_publish_reserved`), `src/kernel/core/bootlog.c` (metadata publication cadence), `tests/host/fs/test_powercuts.c`, `tests/host/fs/test_mount_budget.c`, `docs/design/vfs-storage-contract.md` (amend).

## Evidence (T23, image a79f840c, `legacy/local/physical/a79f840c/`, read-only)

Second boot (L, stick inserted): the desktop stalls in `autoplay-service` (f2-78) and the owner powers off. Its black-box replay, found at the head of the third boot's `BOOT.LOG`, shows the log sink had written the boot's log to disk:

```
[blackbox] [kernel] bootlog writes=12 bytes=393216 metadata=0 rotations=0 dropped=0
```

`metadata=0`: in more than 40 s the log's directory entry (size) was never published (the five-second metadata cadence of f0-34 did not run: the desktop's stalled settings read holds the namespace, or the publication was never due). Third boot:

```
[storage] repaired logs=1 lost=0 copies=0 dirty=cleared elapsed_ms=426 mount_ms=296 error=0
[storage] kind=2 disk=0 part=1 mode=rw error=0 writes=13 qualified=0 reason=dirty_recovered activation_seq=6
```

and after rotation `BOOT1.LOG` (the second boot's log) is **0 bytes**. Before f0-32 (image a395ffa8 and earlier) the f0-25 recovery accepted the EOC/link disagreement at the published EOF as an interrupted extension and the log kept its written data; f0-32's repair "trims excess regular-file tails after the complete ownership scan" and counts the boot logs it trims as `repaired_logs` (`fat.c`, `repair_directory_tails` / the f1-70 reconciliation). The evidence of a freeze is exactly that unpublished tail.

## Required

1. Boot logs (`/system/logs/BOOT*.LOG`, the reserved-chain log files) are recovered, never trimmed: after a cut the published size is extended to the last sector of the owned chain that carries valid log data (the f0-25 rule, with the existing sector validity checks), and the repair record says `extended` with the recovered byte count; ordinary regular files keep the f0-32 trim.
2. Metadata publication of the log must not depend on user-space lock holders: either the storage task publishes the log's directory size on its own lock path, or the reserved-chain layout makes the size recoverable without the directory (preferred: both). Record the actual cause of `metadata=0` during the stall in the report (the host harness can reproduce a stalled namespace holder).
3. Host coverage: power cut after N drained batches with no metadata publication → the next mount recovers N × 32 KiB; the same with a stalled namespace holder; the f0-32 crash matrix unchanged for ordinary files. Run `scripts/test/host_fs_tests.sh`, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f0-36-report.md`. The lead runs the f1-fat32 bootlog cases on QEMU.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
