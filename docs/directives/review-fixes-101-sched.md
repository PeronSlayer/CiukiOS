# Review fixes for wt/101-sched

- **Written:** 2026-10-25 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Apply every finding of the review below with a regression test for each (the test must fail before the fix), rerun the suites that cover the changed files, and report.

**Verdict: merge after fixes.** Reviewed `338ae94ff` (`HEAD`). Two concrete defects:

1. **[P2] A failed media query permanently disables automatic refresh.**  
   At [desktop.c:2217](/home/peronslayer/Desktop/CiukiOS-wt-101-sched/apps/desktop/desktop.c:2217), scans are suppressed once `media_events==1`. But a later snapshot failure closes the worker descriptor at line 1519 without resetting that state. `media_watch_fd()` then returns `-1`, while every two-second fallback request is discarded. Subsequent arrivals cannot recover the observer.

   **Minimal fix:** when collecting an unsuccessful snapshot, reset watch confirmation and permit the timed retry. Require a live descriptor before suppressing fallback scans. Test: successful watch → query failure → descriptor closes → timed retry reopens it.

2. **[P2] Namespace readiness does not cover all the observations whose polling was removed.**  
   The same [desktop.c:2217](/home/peronslayer/Desktop/CiukiOS-wt-101-sched/apps/desktop/desktop.c:2217) guard assumes every relevant change produces readiness. However, [media_dev.c:22](/home/peronslayer/Desktop/CiukiOS-wt-101-sched/src/kernel/drivers/media_dev.c:22) advances the serial only for namespace changes:
   
   - Audio-only CD insertion/removal changes CD state without mounting a volume; see [optical.c:48](/home/peronslayer/Desktop/CiukiOS-wt-101-sched/src/kernel/drivers/optical.c:48). AutoPlay can miss it indefinitely.
   - USB enumeration failures increment the unrecognized-device counter without notifying this observer; see [usb_core.c:924](/home/peronslayer/Desktop/CiukiOS-wt-101-sched/src/kernel/usb/usb_core.c:924). The desktop warning is consequently missed.

   **Minimal fix:** publish media readiness for those transitions before eliminating their polling, or retain targeted fallback observations. Add tests that change CD/error state while leaving the namespace unchanged.

The security audit found **no I/O-grant leakage**: NDIS and VGA grants invalidate the deny cache; malformed allowlists fail closed; switches clear outgoing grants before installing incoming permissions. Exit, kill, watchdog, fault, and VM86/fullscreen paths preserve that invariant. VGA revocation conservatively leaves the cache invalidated.

No additional blocker found in allocator bounds/alignment/accounting, statistics throttling, or IRQ-off credit accounting. Firmware attribution still relies on the existing bounded-phase heuristic. The logger yields after releasing RAM/VFS locks; retained drain ownership serializes batches and preserves protected-record order.

Existing allocator, record, serial, and bootlog test binaries passed with leak detection disabled, matching the filesystem runner’s sanitizer policy. Fresh compilation was unavailable in the read-only sandbox; these are reruns of existing binaries.