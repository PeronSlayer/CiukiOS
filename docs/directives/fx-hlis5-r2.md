# fx-hlis5 round 2: review findings on the burst optical path and the profile TEMP folder

- **Written:** 2026-10-29 00:47 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-hlis5`, branch `wt/fx-hlis5` at `57e083bff`.
- **Input:** the review `build/lead-scratch/fx-hlis5-review-0007.md` in the main checkout, which found no P1 issues.

## Scope

1. **P2: coverage of the 64 KiB optical path.** Add host tests in `tests/host/test_optical.py` and `tests/host/atapi_test.inc`, or in their C harnesses, for:
   - a 32-sector (64 KiB) transfer that crosses the `0xfffe` byte-count limit, split into several DRQ phases;
   - a device error after a successful DRQ phase. The error must reach the caller, the failed fill must publish no data, the demand-buffer canaries must stay unchanged, and the reserved cache blocks must be unhashed;
   - media change or cache-epoch invalidation during a 2048-byte cache fill. Stale generations must be rejected.

   Fix any defect these tests expose. Production changes go only where a test proves a defect.
2. **P3: TEMP creation race.** In `apps/desktop/profile.h`, after `mkdir` returns `EEXIST`, run `stat` again and require a directory. Anything else keeps the desktop diagnostic. Add a deterministic test in `tests/host/desktop/profile_temp_test.c`: a regular file named `Temp` must be reported.
3. Re-run the tests the round-1 report names for the optical, ATA and profile paths, sequentially, with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-hlis5-r2-report.md`.
