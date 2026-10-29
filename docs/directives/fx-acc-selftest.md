# fx-acc-selftest: the self-test boot as an acc-t23 case on QEMU

- **Written:** 2026-10-29 11:19 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-acc-selftest`, branch `wt/fx-acc-selftest` from `main`.
- **Owner decision** (dev diary 2026-10-28-01): the 1.0 Beta prerelease is published once `acc-t23` **and the self-test** are green on QEMU. Today no QEMU case runs the self-test boot (`docs/design/self-test.md`); the lead would have to drive it by hand.

## Scope

1. **New case `acc-selftest`** in `tests/suites/acc-t23.json`, through the existing acceptance machinery (`scripts/test/acceptance*.py`, the case generator if the suite is generated). The case:
   1. boots the canonical image on `qemu-t23`;
   2. selects `T` at the loader menu, the same way the other cases send loader keys;
   3. waits for the self-test end record defined in `docs/design/self-test.md`;
   4. extracts `C:\SYSTEM\LOGS\SELFTEST.LOG` from the run overlay with the existing FAT reader the runner already uses for disk checks;
   5. parses both the log and the serial `probe=selftest` records with `scripts/test/selftest_report.py`.
2. **Verdict.**
   - The case passes only if every check is PASS or a SKIP that the contract allows for the profile. A QEMU device that does not exist on the twin, such as the ThinkPad EC, counts as an allowed SKIP.
   - A QEMU deviation must be declared as `PASS*` with its directive, following the existing KNOWN_FAIL convention, and listed in the summary. It is never silently skipped.
   - The summary line shows the counts by status and the first failing check.
3. **Budget.** Give the case its timeout. The self-test is about 5 minutes on hardware; derive the TCG figure from its own records. Respect the `test_owner_security_items_in_acc_t23` budget rule: do not raise constants; report if the case does not fit.
4. **Host tests.** Add tests in `tests/host/test_acceptance.py` for:
   - the case definition;
   - the verdict mapping (PASS, allowed SKIP, PASS*, FAIL), using recorded fixture logs;
   - the summary line.

   Check that `scripts/test/acceptance.sh --list` shows the case. Run `tests/host/test_acceptance.py`, the self-test parser tests and `tests/host/test_runner*` sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds; the lead runs the case.
- Write the report to `build/codex/fx-acc-selftest-report.md`.
