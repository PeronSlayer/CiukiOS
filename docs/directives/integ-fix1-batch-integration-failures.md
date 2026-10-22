# integ-fix1 — Integration failures after the 2026-10-23 batch merge

- **Phase/model:** integration, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/integ-fix1` from `main`.
- **Evidence (lead, 2026-10-23 01:23):** after the batch merge of f2-100, f2-98a, f2-98b, f1-103, f3-63, f2-123, f2-101c, f1-111, f2-88c, f1-109, f1-106 and f1-104, each green in its own worktree, the flat host suite failed on main:
  - `env PYTHONPATH=tests/host python3 -B -m unittest discover -s tests/host -t . -p 'test_*.py'` ran for 2360 s with 16 failures. The full output is `build/lead-scratch/flat.log` in this worktree, from line 2100.
  - The kernel suite fails only on the DOS cadence check fixed separately by f2-98b-fix1, which is not yet on main: ignore that one.

## Required

1. Triage every failure: test_add_remove, test_control_jobs, test_music, test_devices (registry_query, network_settings), test_f3_workloads setUpClass, test_image_payloads, test_about goldens, test_desktop (dosbox_loading, look_reference_frames), test_memory_footprint, test_paint, test_start_orb, test_transients, test_app_gate namespace teardown, and test_credits. For each, establish the cause: a semantic conflict between two merged features, a stale golden, a missing credits row, an environment prerequisite, or a genuine regression.
2. Fix genuine regressions in the code, with the feature owners' intent preserved; read the involved directives and reports in `docs/directives/` and `/home/peronslayer/Desktop/CiukiOS/build/codex/`.
3. Re-record a golden only when the visual change is intended by a merged directive (for example the f2-99 cursor set, the f2-98a/f2-98b update model, or new credits rows). Name the directive and the exact cause in the report, and check that the new frame is correct, not just different.
4. Add the missing credits rows for newly vendored directories (ACPICA etc.), following the existing credits pattern.
5. Environment-only failures: name the missing prerequisite, such as a downloaded archive or a built SDK, and the exact command that provides it. Do not mask them in the test.
6. Rerun each fixed test module individually and report the exact commands and results. A LeakSanitizer exit after all assertions in this sandbox is not an assertion failure; say so.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/integ-fix1-report.md`.
