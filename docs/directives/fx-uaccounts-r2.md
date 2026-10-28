# fx-uaccounts round 2: review findings on the Control Panel job adoption

- **Written:** 2026-10-28 22:47 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: branch `wt/fx-uaccounts` at a82997db1, the Claude agent's fix for User Accounts stuck on "Reading the accounts...".
- **Inputs:**
  - its report `build/codex/fx-uaccounts-report.md`;
  - the design-conformance review `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/fx-uaccounts-review-2201.md`. Read it first.

## Scope

Fix the five review findings. For each one, add a host test that fails without the fix.

1. **P1, `apps/control/accounts.inc:270`.** Adoption uses the current selection `account_uid` instead of the uid the request was submitted for. Keep the submitted uid with the request and use it during adoption, including for the PASSWORD notice.
2. **P1, `apps/control/main.c:1169`.** Close joins the accounts worker without adopting its result. A committed first password can then never reach the desktop and LOGON. Defer the normal close until the account completion is adopted and its desktop notification is queued, as Windows XP's User Accounts finishes a change before its window closes.
3. **P2, `main.c:622`.** Game adoption calls `cp_job_finish(&m->menu_job)` and consumes the menu job. Remove that unrelated join.
4. **P2, `main.c:886`.** `cp_tick()` runs before the guarded `cp_jobs_poll()`, and the heartbeat and pending-navigation paths call `cp_tick()` unguarded. Enforce the dialog and pointer-capture guard inside the shared adoption function so that every caller observes it.
5. **P2, `tests/host/security/control_panel_test.c:42` and `:106`.** The harness rebuilds startup and replaces `du_window_queue()`.
   - Run the production initialisation and the real window queuing, with fakes only at the kernel boundary.
   - Assert LOGON's resulting state after a password change, including the close, selection-change, modal and capture cases.

## Acceptance

- The new and existing tests pass, including `tests/host/security/test_control_panel_accounts.py` and `control_jobs_test`.
- The security and desktop suites gain no failure against a82997db1. Failures that only LeakSanitizer causes in the sandbox are listed separately.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-uaccounts`.
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Append a "Round 2" section to `build/codex/fx-uaccounts-report.md`. Leave the changes uncommitted.
