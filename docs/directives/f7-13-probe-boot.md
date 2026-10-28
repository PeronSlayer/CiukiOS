# f7-13: the Savage boot probe must never block startup (T23 trial, image 5bb73dba)

- **Written:** 2026-10-26 16:27 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main be7852468 (f7-11 merged).
- **Evidence (owner's T23, ThinkPad SuperSavage pci 5333:8c2e, 1024x768, 15532032 bytes VRAM, Pentium III):** with BOOT.CFG `safe=0 serial=1 3d=probe` the machine never reached the desktop. The screen stayed on the loader text (last line L:SELECT_ERROR) and nothing was written to disk: no LOGS directory, no CIUKIST.LOG. With the same image and `safe=0 serial=1` it boots to the desktop. The serial capture of that boot is in the lead's checkout, legacy/local/physical/5bb73dba/serial.log (private, never commit). It shows the 2D engine probe passing, with marker_write taking 329140 us. A serial capture of a probe boot is not available yet.
- **Probable mechanism (verify it, do not assume it):** savage3d_boot_diagnostic runs from savage2d_record (the driver's .record hook, src/kernel/drivers/video/s3savage/savage2d.c:1521, s3savage_register.c:59) on the display path. The minimum probe, the full-size gate and 23 precision phases at 1024x768 run synchronously there, with CPU reference rendering, chunked readback over the bus and possibly IRQ-off or presenter-locked windows, so the display worker never presents the desktop. Durable marker writes may also wait on storage from that context.

## Scope
1. Find with file:line evidence every path the probe takes on the display worker and at boot: locks held, IRQ state, waits on storage or registry, loops and their bounds. Estimate the cost of each phase on a Pentium III with the repository's T23 cost model (CPU reference rendering of full-size scenes, readback bytes at the measured bus rates, per-phase totals at 640x480/800x600/1024x768). Record the estimate in the report.
2. Move qualification off the startup path. The desktop must start and present normally while qualification runs as a background job on a low-priority worker after the system is up. It must never hold the presenter or display locks across CPU work, and never leave IRQs off beyond the existing bounded engine windows. The engine stays in software until qualification completes. Present grants and leases are unaffected.
3. Bound everything:
   - a total wall-clock budget for the whole qualification, small enough for an interactive boot (propose one and justify it from the estimate);
   - per-phase deadlines;
   - yielding between chunks;
   - fewer or smaller samples where the precision argument allows it (keep the declared tolerances, or reduce the claims honestly);
   - a timeout or budget overrun refuses hardware admission cleanly: restore, no quarantine of a healthy engine, a record of why.
4. Make the outcome observable and durable:
   - per-phase serial and boot-log records ([3d] probe phase=... elapsed_us=...);
   - the attempt marker keeps its meaning: a probe that never finished disables the next probe until cleared;
   - document how an administrator clears it to retry (the existing mechanism or a minimal one).
   A probe that hangs the machine must never prevent the next boot from reaching the desktop.
5. Tests:
   - host tests with the fake Savage and a slow-bus and slow-CPU model reproducing a long qualification: the desktop presents while it runs; the budget expires and refuses cleanly; the marker blocks the next attempt;
   - existing render3d, Savage family, display and kernel latency suites stay green.
   Run them one at a time with nice -n 19.

Another implementer (wt/f7-12) is changing pixel-format selection and possibly adding D24/S8 to the Savage backend (savage3d.c, encode.h, gdi32). Keep your changes to probe orchestration, scheduling and budgets where possible. If you must touch the phases in savage3d.c, keep edits minimal and list them so the lead can merge both.

Report to build/codex/f7-13-report.md, with the T23 retest procedure (BOOT.CFG with 3d=probe, serial capture, the records to expect). Leave changes uncommitted.
