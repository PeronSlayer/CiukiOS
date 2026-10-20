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
