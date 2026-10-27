# fx-autorun: AutoPlay runs Windows CD autoruns; one-line stderr records for every native program

- **Written:** 2026-10-27 21:50 by the lead. Implementer: Claude agent. Base: main 9e2df1875 or later.
- **Evidence:** fx-acc1 found that AutoPlay cannot run a Windows program named by a CD's `AUTORUN.INF` (`OPEN=`); period game CDs (the owner's Half-Life CD has `autorun.exe`, PE32) start that way on Windows 98. fx-acc1 also found that newlib writes each part of an `fprintf(stderr, ...)` separately, so native programs' log records arrive split over several serial lines (fixed only in the desktop).

## Scope

1. AutoPlay follows Windows 98 for CDs (research and cite: Microsoft's AUTORUN.INF reference, the Windows 98 Resource Kit): `OPEN=` runs a Win32 program (and through the native InstallShield 5 bootstrap an NE setup stub) from the disc root with the disc as current directory, honouring `ICON=`, the user's AutoPlay choice and Shift-to-skip; a refused or failed launch is reported in words, never silently dropped. The CD drive's context menu shows AutoPlay.
2. Make stderr line-buffered (and stdout line-buffered when it is a terminal or the serial log) for every native program at C runtime start, in the SDK crt or a newlib patch; refresh what `make sdk` refreshes; remove the desktop's local workaround if it becomes redundant.
3. Host tests: AutoPlay with generated media (Win32 autorun, IS5 stub, missing program, Shift), crt buffering; the desktop, Win32, installer and SDK suites stay green.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-autorun` (branch `wt/fx-autorun`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-autorun-report.md` in the worktree. Leave changes uncommitted.
