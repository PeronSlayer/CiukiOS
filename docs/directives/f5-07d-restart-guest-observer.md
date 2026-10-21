# f5-07D — guest observer for the f5-07B restart cases: `w32-restart` fixture and probe

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead (f5-07B authored the suite case `w32-restart-qemu-t23` unrun; f5-21/f5-21b registered it as pending with this owner)
- **Worktree:** `../CiukiOS-wt-f5-07d`, branch `wt/f5-07d`

## Required

1. **Fixture** `restart-exercise.exe` in `src/probes/win32_restart/` on the result-block protocol at the free page `0x10050000`: (a) PROGMAN DDE: `CreateGroup`, `AddItem`, `ReplaceItem`, `DeleteItem`, `DeleteGroup` through raw DDE and DDEML, verifying the `.lnk` files and the `Groups` request answer; (b) end-session: a second window with `WM_QUERYENDSESSION` veto then accept, `ExitWindowsEx(EWX_LOGOFF)` coordination through w32srv, with the desktop confirming automatically under the probe; (c) `WININIT.INI [rename]` moves and NUL deletes prepared by the fixture, then a virtual restart requested with `EWX_REBOOT`, observed after the restart through the journal and the resulting files; (d) `RunServicesOnce`/`RunOnce` entries executed once in order by `runonce.exe` with their consumption proven by the registry; (e) recovery: an interrupted restart (journal present at boot) resumed by the service, no double execution. The fixture writes each case's `checked=1 errors=N` into the result block; the virtual restart boundary is survived by writing the pending state to a file under `C:\WINDOWS\TEMP` that the second run reads.
2. **Probe** `f5:w32-restart` in `src/kernel/probes/` emitting exactly the suite's predicates (`progman_dde`, `end_session`, `wininit_restart`, `runonce_restart`, `restart_recovery`, each `errors=0 checked=1`) from kernel-observed facts (the files, the registry through w32srv's directory, the journal, the process records), using the desktop fixture gate (f5-21b's `GATE_W32_FIXTURE` if merged; otherwise the f5-10C3 pattern with the C: write gate and the registry round-trip wait, to be unified at merge); registration in the three tables and the loader model; removal from the known-pending lists.
3. **Tests.** Host: the fixture's logic against the f5-07B host shell/session harness (`tests/host/win32/shell`), the probe's predicate dry-run, the loader size bounds, `make CAP= apps` once. No QEMU (the lead runs the probe).

## Constraints

`rg`/`sed` only; no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; English. Report `build/codex/f5-07d-report.md`.

## Amendment 1 (lead, 2026-10-21, from the lead's suite run on the delivery; logs in `build/lead/` of this worktree)

1. `tests/host/test_desktop_gate.py` (`test_native_desktop_gate_loop`, `test_native_handshake_reports_through_kernel_hook`): the desktop no longer compiles in the gate harness after the fallback gate change — compiler output: `clang: error: linker command failed with exit code 1 (use -v to see invocation)`. Fix the production code or the harness include so both tests pass.
2. `scripts/test/host_kernel_tests.sh`: `FAIL evidence record scope: probes/f5_restart_observer.inc:70, probes/f5_restart_observer.inc:73, probes/f5_restart_observer.inc:75, probes/f5_restart_observer.inc:76, probes/f5_restart_observer.inc:169, probes/f5_restart_observer.inc:192, probes/f5_restart_observer.inc:194, probes/f5_restart_observ` — the new probe emits evidence records outside the allowed scope; follow the pattern of the registered probes (records only from the probe's own scope).
3. `tests/host/win32/test_kernel_core.py:110`: the case `w32-restart-qemu-t23` lost `integration_pending` on registration; the test still reads it. Update the test to the registered state ("authored, unrun" stays in the case as the other registered-but-unrun cases do) rather than dropping the field.
Rerun `tests.host.win32.test_kernel_core`, the gate, and the kernel suite as far as the sandbox allows.

## Amendment 2 (lead, 2026-10-21): merge of current `main` into this branch

`main` now contains f5-21 and f5-21b (the shared desktop fixture gate `GATE_W32_FIXTURE`, the ten core observers, the probe registration tables and the known-pending lists updated). The lead ran `git merge main` in this worktree and left the conflict markers in the twelve files listed by `git status`. Resolve them so that: the restart probe uses the shared gate of f5-21b instead of this branch's fallback gate (delete the fallback); the registration of `w32-restart` appears once in each table (kernel `f5_names`, loader `f5_probe_names`, `loader_model.F5_PROBES`), the known-pending lists keep only `w32-installer-nsis` (owner f5-15A2); the result page `0x10050000` does not collide with f5-21b's pages (`0x10030000 + n·0x10000`, n = 0..9 → use `0x100d0000`); the suite and the tests reflect the merged state. Then rerun `tests.host.win32.test_kernel_core`, `test_probe_registry`, `test_kernel_loader`, `tests/host/win32/shell`, `test_desktop_gate test_runner_selectors test_boot_modes`, `scripts/test/host_kernel_tests.sh` as far as the sandbox allows, `make CAP= apps` once. Do not commit: report in `build/codex/f5-07d-report.md` ("Amendment 2").
