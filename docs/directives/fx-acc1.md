# fx-acc1: first acceptance run (acc-t23) findings

- **Written:** 2026-10-27 20:05 by the lead. Implementer: Claude agent. Base: main df22c1d49 or later.
- **Evidence:** the lead's first full `scripts/test/acceptance.sh` run on image fbc785e6… (main 225710f20): `build/test-runs/acc-t23/summary.txt` and the per-case directories under `build/test-runs/acc-t23/` in the lead's checkout (serial.log, screenshots .ppm, result.json). Verdict BLOCKED.

## Findings and scope

1. **acc-quake3 (driver defect):** the VISE installer stayed 3000 s on its first page, titled "Welcome", with Next focused (`c634b7c6/screen-fail.ppm`): the suite presses keys only on untitled pages. Drive every page of the installer the way the H1 replay does (Welcome, licence, destination, components, copy, Finished), with a focus/title-based state machine and a per-page timeout that names the page it stopped on.
2. **acc-dos-games:** full screen is a known QEMU refusal (f3-29), but afterwards the close step never saw `end task dialog kind=dos` and the VM never reached `lifecycle stage=cleanup` for every game. Find from the serial logs whether CiukiOS fails to close a DOS window after a refused full-screen switch (fx-biosvm2 changed the park/resume path) or the suite's close stimulus is wrong; fix the defect where it is, with a host regression if it is in CiukiOS.
3. **acc-safe-restart-shutdown (CiukiOS defect, matches the owner's T23 report "il restart non funziona"):** Restart from a Safe Mode session fails at `[power] native quiesce failed: -5`. Find which native device quiesce returns -EIO in Safe Mode, fix it the way Windows 98's Safe Mode restart works, host regression.
4. **acc-netsurf-site-2:** `predicate failed: status eq 200`. Check the fixture's mapping of the second snapshot (`build/downloads/ns-sites-2026-10-26`, never committed) and NetSurf's records; fix the suite or NetSurf.
5. **acc-installshield5:** fx-is5boot is now merged: write the case with a generated InstallShield 5 test medium (no commercial bytes) and the bootstrap records.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-acc1` (branch `wt/fx-acc1`); never commit, merge or push; never run QEMU or image builds (the lead re-runs the suite and sends you new evidence); run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-acc1-report.md` in the worktree. Leave changes uncommitted.
