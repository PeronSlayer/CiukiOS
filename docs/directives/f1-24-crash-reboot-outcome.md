# Directive f1-24: `mount-crash-reboot` — orphaned LFN entry after the cut

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`mount-crash` row:
  crash cuts produce no cross-links; the checker reports only declared
  interrupted-state outcomes), `vfs-storage-contract.md` (durable write
  order for create/replace/rename, barriers), the Microsoft FAT
  specification (long-name entries precede their short entry; orphaned
  long-name entries), directive f1-22 and its follow-up.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-crash-outcome`, from `main` at or after `62bd77f`.
  Files: `scripts/test/run.py`, `scripts/test/mount_fixtures.py`,
  `tests/suites/f1-fat32.json` (the `mount-crash-reboot` case),
  `tests/host/test_mount_fixtures.py`, `tests/host/test_runner.py`,
  `src/kernel/fs/fat.c` and `src/kernel/fs/fat_dir.c` (or wherever the
  directory entry write order lives) only if the analysis proves a driver
  ordering defect, with the storage host tests, and one paragraph in
  `docs/design/vfs-storage-contract.md` or `f1-acceptance.md` recording the
  declared interrupted-state outcomes.

## Observed on image `33a68c43…` (commit `62bd77f`, runner case `mount-crash-reboot`, 2026-10-11)

The gate now arms after `ARM action=crash_cut`; the cut happened at the
declared index 2 (`case=cut index=1/2 action=write lba=2072 durable=1`,
next write suspended, guest terminated), the overlay was exported and the
checker ran:

```
fsck.fat -n check-volume.raw  → returncode 1
Orphaned long file name part "F109CUT.BIN"
  Auto-deleting.
Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.
```

The runner classified this as `unclassified interrupted filesystem
discrepancy` and did not attempt boot 2 (`unattempted_boots=1`). The
result and serial log are in `build/f1-24/` of the worktree.

## What to do

1. Explain the state from the driver's write order: `make_file` creates
   `C:/F109CUT.BIN` (why does an 8.3-compatible name get a long-name entry;
   which sector holds the long-name part and which the short entry; which
   writes were durable before the cut at index 2, which were pending),
   then decide:
   - if the long-name part and its short entry can straddle a sector
     boundary or are written in separate operations, so that a cut leaves
     an orphan the checker cannot attribute, and the contract's "declared
     interrupted-state outcomes" should include "orphaned long-name part of
     the workload file, auto-deleted by the checker, no cross-links": add
     that outcome to the declared set, classify it in the runner, record
     the rule in the contract, and let boot 2 run;
   - if the driver's order violates the contract (for example the data or
     the long-name sector made durable before the short entry in a way the
     contract forbids), fix the driver with a storage host test that
     reproduces the cut order.
2. Keep "no cross-links" and the dirty-bit expectation strict; the
   reboot's `case=crash_reboot` records and `crash_refusal` predicates
   stay as f1-22 defined them.
3. Host tests: the classification with the real `fsck.fat` output above,
   and the fake-QEMU sequence through boot 2.

## Acceptance by the lead

Host tests; kernel build; on QEMU `mount-crash-reboot` passes on `qemu-t23`
through both boots. Reply with: files, the write-order analysis, the
decision and its contract text, test output.
