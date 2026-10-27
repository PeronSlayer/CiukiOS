# fx-doswin3: DOS games in a window must run as well as in full screen (recurring T23 complaint)

- **Written:** 2026-10-26 16:47 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Evidence (owner's T23 trial of image 5bb73dba, Pentium III 1 GHz, SuperSavage, 1024x768):** private logs in the lead's checkout: /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/serial.log (serial capture of several boots) and /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/disk/LOGS/BOOT*.LOG, CIUKIST.LOG (read them, never copy them into the tree; quote only the numbers you need). Earlier trial logs for comparison: legacy/local/physical/c42f556e/disk/LOGS and 2ca3b0b6.
- **Owner, verbatim:** "DOS games better but in a window still lag." DOOM is a bit better in a window but still not playable. Wolfenstein 3-D runs perfectly in full screen and lags in a window "like all DOS games".
- **History:** this complaint has recurred for over a week. Earlier attempts: 101-dosvid, 101-dosvid2 (damage tracking, native copy, 3dfx STRETCH, direct LFB), t23-dosperf (CLI proof rules, lean PM CLI/STI, batching beside armed images, OPL3 lean ports, shadow retirement). Explain with figures why each did not close the gap, before proposing a fix.

## Scope
1. **Reproduce the trial figures.** From the [dosvm] perf/perf_cost/perf_port, vm-audio, display/presenter, [cpu] and [kernel] records, separate where the time goes per game, window versus full screen (Wolfenstein 3-D is the cleanest pair: perfect full screen, lagging window). Cover:
   - VM exits and their costs;
   - presenter copies and conversion (8 bpp palette to 32 bpp, scaling) and their frequency;
   - compositor and desktop work per frame;
   - scheduling share of the VM thread against the desktop;
   - IRQ and timer delivery.

   Reproduce the window-vs-full-screen difference in the host replay at the T23 cost model, matching the measured numbers.
2. **Fix the dominant costs** until the window path is within a small, stated factor of full screen on the replay. Candidates to evaluate:
   - present only on a game frame or vsync, never per VGA write burst;
   - convert dirty spans with the Savage 2D engine (the T23 has a working 2D BCI path, "[accel] savage bci_test result=ok"): palette expansion and stretch blit into the window's surface;
   - avoid double copies (VM frame to desktop surface to scanout);
   - give the focused game VM the scheduling share it has in full screen;
   - skip compositor recomposition of unchanged desktop areas.

   Do it within the display driver model (generic code free of family names).
3. **Evidence.**
   - Before/after replay numbers for DOOM, Wolfenstein 3-D and Warcraft II in a window at the T23 model.
   - Full-screen paths unchanged.
   - Existing DOS, display, desktop and kernel latency suites green.
   - A short list of the serial/boot-log records the lead should compare on the next T23 trial.

Report to build/codex/fx-doswin3-report.md. Leave changes uncommitted.
