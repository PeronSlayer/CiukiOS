# fx-doomfs2: DOOM in full screen loses audio and game speed under load

- **Written:** 2026-10-26 16:47 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468.
- **Evidence (owner's T23 trial of image 5bb73dba, Pentium III 1 GHz, SuperSavage, 1024x768):** private logs in the lead's checkout: /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/serial.log (serial capture of several boots) and /home/peronslayer/Desktop/CiukiOS/legacy/local/physical/5bb73dba/disk/LOGS/BOOT*.LOG, CIUKIST.LOG (read them, never copy them into the tree; quote only the numbers you need). Earlier trial logs for comparison: legacy/local/physical/c42f556e/disk/LOGS and 2ca3b0b6.
- **Owner:** "DOOM full screen still has problems with audio and game speed when I start spamming rockets."

## Scope
1. **Reproduce the trial figures** from the [vm-audio], [dosvm] perf/perf_cost, [kernel] timer and stretch, and [sched] records of the DOOM full-screen sessions. Find what breaks under heavy action:
   - SB16 DMA period and IRQ timing, underruns, mixing cost;
   - PIT 70 Hz/35 Hz delivery and lost ticks, which set the game speed;
   - CPU share of the VM against the host tasks;
   - expensive exits growing with the number of sounds.
2. **Fix it.** Under load the game keeps its tick rate and the audio has no gaps or slowdown, as on Windows 98. Each fix is backed by a host replay of the measured pattern at the T23 cost model (many simultaneous SFX).
3. **Tests.**
   - Before/after replay figures.
   - The existing DOOM audio replay (233710 traps, 24 flips, 1806 IRQs, 167359 SB16 units as the baseline counters).
   - The DOS, audio and kernel latency suites stay green.

Report to build/codex/fx-doomfs2-report.md. Leave changes uncommitted.
