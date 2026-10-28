# integ7-merge: resolve the sec-user-06 merge on integration 7

- **Written:** 2026-10-28 15:22 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree and branches:** `/home/peronslayer/Desktop/CiukiOS-wt-integ7`, branch `wt/integ7` = `wt/integ6` (main + sec-runtime1 + fx-games20 + f7-26) + sec-k08 (merged cleanly). The lead has started `git merge wt/sec-user06`; nine files carry conflict markers (`git diff --name-only --diff-filter=U`).
- **Inputs:**
  - the reports `../CiukiOS-wt-sec-user06/build/codex/sec-user06-report.md`, `../CiukiOS-wt-sec-k08/build/codex/sec-k08-report.md` and `../CiukiOS-wt-sec-runtime1/build/codex/sec-runtime1-report.md`;
  - `docs/directives/sec-lead-decisions.md`, in particular U6-1..U6-6 and K8-*.

## Scope

1. Resolve every marker and keep every side's behaviour:
   - sec-runtime1's first-boot predicates and actions in `tests/suites/acc-t23.json`: add them to `acc-accounts`' autologon check as well, as sec-user-06 proposed;
   - sec-user-06's `acc-accounts` case, the LOGON-aware templates and the power-cut action in `scripts/test/acceptance.py`, `scripts/test/acceptance.sh` and `scripts/test/run.py`;
   - the LOGON records of all three branches in `apps/logon/logon.h`.
2. **U6-6:** keep one `security.kdf` self-test check, sec-k08's implementation with an 8 s bound. The total self-test budget must stay within 600 s. Update `docs/design/self-test.md`, `src/kernel/core/selftest_checks.c` and both self-test host tests to the merged list.
3. **U6-4:** `/Programs/sectest` stays in the image and is hidden from the Start menu and from Add/Remove Programs.
4. Compare each resolved file with both parents (`git diff wt/integ6 -- <file>`, `git diff wt/sec-user06 -- <file>`) and justify every removed line in the report.
5. Run:
   - the security suite (`python3 -B -m unittest discover -s tests/host/security -t .`);
   - `tests.host.test_acceptance`;
   - `tests.host.test_selftest`;
   - the self-test checks host test;
   - `python3 scripts/gen_api_reference.py --check`;
   - `scripts/test/acceptance.sh --list`.

## Rules

- Never commit or stage; the lead commits the merge.
- Never run QEMU, `systemd-run` or image builds.
- Run tests sequentially with `nice -n 19`.
- Report to `build/codex/integ7-merge-report.md`.
