# fx-doomwin: DOOM in a window on the T23, sequencer plane selects and debug traps (T23 trial 6c3b7800)

- **Written:** 2026-10-27 09:50 by the lead. Implementer: Claude agent (Codex weekly quota at 77 %). Base: main 0b9381696.
- **Evidence:** owner's T23 (Pentium III-M), image 6c3b7800, which already carries fx-doswin3, fx-dospic and fx-vmclock. Private log `legacy/local/physical/6c3b7800/disk/LOGS/BOOT.LOG` in the lead's checkout, lines 3212 onwards (DOOM is `vm=1 gen=11971`); read it, never copy it. The owner: DOOM in a window still lags; Warcraft II in a window lags a little.

## Findings from the log (per perf window)

- `perf_port port=03c5` 41-76 thousand exits costing 242-426 ms; `perf_cost cause=vga_io` 259-427 ms. DOOM writes the sequencer map mask (index 2 at 0x3C4, data at 0x3C5) for each column in unchained mode; each exit costs about 5.4 µs.
- `perf_cost cause=db` up to 11,451 exits costing 438 ms (about 38 µs each) and 273 ms in another window.
- `perf_port port=0388` (OPL) 4-9 thousand exits, 26-101 ms.
- Session 1 (`BOOT1.LOG`): the BIOS VM worker `bios-input#37` takes 18-30 % of each CPU window while Quake III runs (`device_top task=bios-input`).

## Scope

1. **Plane selects without a full exit.** Research how Windows 9x's VGA VDD and DOSBox/QEMU handle sequencer index/data writes in planar/unchained modes; cite the sources in the DOS VM video design document. Give 0x3C4/0x3C5 (map mask, and the other sequencer registers DOOM writes per frame) a lean path that updates the virtual VGA state without the generic exit cost, consistent with the windowed presenter, the dirty tracking and full-screen handoff. Measure the per-write cost in the existing DOS window replay (`tests/host/dosvm_*`, `t23_*` timing models) with the T23 figures above and report before/after.
2. **Debug-trap cost.** Find what produces the `db` exits in DOOM's window run (TF single-step used for CLI/IF tracking, `cli_learned`, pushf/popf handling, or guest breakpoints) and why each costs ~38 µs; remove the avoidable ones and cut the cost of the rest. Report counts before/after in the replay.
3. **bios-input load.** Explain from the code why the BIOS VM worker runs 18-30 % of the CPU on the T23 (input IRQ reflection into the BIOS, 10 ms polling, USB legacy) and reduce it where the input path does not need firmware.
4. **Acceptance.** The DOOM and Wolfenstein 3-D window replays of `tests/host/dosvm_test.c`/`dosvm_guest.c` show the wall-to-game ratio reduced from the current figures (state them), with audio cadence and the existing DOS VM, presenter, held-IRQ and audio suites green.

Rules: work only in the worktree `/home/peronslayer/Desktop/CiukiOS-wt-fx-doomwin` (branch `wt/fx-doomwin`); never commit, merge, push, run QEMU or build images; run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to build/codex/fx-doomwin-report.md in the worktree. Leave changes uncommitted.
