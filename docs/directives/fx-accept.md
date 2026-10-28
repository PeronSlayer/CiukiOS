# fx-accept: automated acceptance of the owner's trial list on QEMU qemu-t23

- **Written:** 2026-10-27 11:55 by the lead. Implementer: Claude agent. Base: main 80f0bc34c or later.
- **Owner decision 2026-10-27:** every hardware image first passes an automated acceptance run of the owner's trial list on the QEMU `qemu-t23` profile; a failing item blocks the image. This lifts the 2026-10-19 QEMU suspension for this purpose only (one QEMU at a time, `MemoryMax=1500M`, `MemorySwapMax=0`, never during image builds).
- **Why:** the T23 trial of image 6c3b7800 failed on paths that were green on host: a refused CD (`malformed_iso9660`), the Quake III installer hang, DOS full screen refused, Duke Nukem 3D stalled after the refusal, a Safe Mode display preference reapplied after restart, Restart that did not restart.

## Scope

1. **Suite `tests/suites/acc-t23.json`** run by `scripts/test/run.py acc-t23` on the canonical image with copy-on-write overlays, as the test architecture requires (`docs/design/test-architecture.md`). Reuse existing probes and suite cases where they exist (f3 workloads for DOOM/Wolfenstein 3-D, f5 installers/titles, f6 browser, f4 media, f9 CD smoke, sweep/reset handling); write new guest probes or runner stimuli only for what is missing. Each case states its pass predicate from serial/disk records, never from screenshots alone.
2. **Cases (the owner's list, in this order):**
   1. Boot to the desktop in the normal mode at 1024x768 with no error notification.
   2. Quake III Arena demo: run the VISE installer from the desktop folder to completion (installer exits, files and shortcuts present, no not-responding state), then start the game from the Start menu to its first presented frame and quit.
   3. DOS games from the Games menu: DOOM, Wolfenstein 3-D, Warcraft II demo, Duke Nukem 3D shareware and Quake: each starts in a window, reaches gameplay or its demo loop, enters full screen and returns to the window, then exits. Record per game the full-screen result and the window performance counters the T23 logs use (`[dosvm] perf`, `perf_port`, `perf_cost`).
   4. CD: the runner inserts, through QMP, generated ISO images (no commercial content): a standard one, one with volume sequence 0, one with Joliet; each mounts as D:, lists its files, and AutoPlay reacts; eject unmounts. An empty drive is present as D: in My Computer like Windows 98 (record the current behaviour if it is not yet implemented, as a known failure with its directive).
   5. NetSurf opens local copies of the owner's two sites through the existing HTTPS fixture (`scripts/test/https_fixture.py`; snapshots in `build/downloads/ns-sites-2026-10-26`, never committed) and records layout/fetch errors.
   6. Safe Mode boot, then a normal boot: the normal boot keeps the user's display mode and shows no error.
   7. Restart from Start > Shut Down: a `[power] reboot requested` record, a clean storage shutdown and the next boot.
   8. Shut down: power off through ACPI S5 on QEMU.
   9. When fx-is5boot is merged: an InstallShield 5 setup with a generated test archive starts through the native bootstrap (the real Half-Life CD stays local-only).
3. **One command for the lead:** `scripts/test/acceptance.sh` (or a `make` target) that runs the whole suite sequentially under the memory caps, keeps only failing overlays, and writes a one-page summary per case (PASS/FAIL, key figures, log excerpt path) to `build/test-runs/acc-t23/summary.txt`. Total wall time target: under 45 minutes.
4. **Host tests** for the runner logic, predicates and ISO generators (`tests/host`), green with the existing runner tests.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-accept` (branch `wt/fx-accept`); never commit, merge or push; never run QEMU or build images (the lead runs the suite and reports back what fails); run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-accept-report.md` in the worktree. Leave changes uncommitted.
