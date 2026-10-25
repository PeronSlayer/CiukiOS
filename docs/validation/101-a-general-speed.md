# 101-a general speed: implementation decisions and host evidence

This work follows [101-a](../directives/101-a-general-speed.md). Hardware
qualification and integration remain with the lead.

Research checked before implementation:

- Microsoft's [update-region documentation](https://learn.microsoft.com/en-us/windows/win32/gdi/invalidating-and-validating-the-update-region)
  accumulates invalidated rectangles; an unchanged snapshot must not cause a
  repaint. The existing desktop compositor already follows this rule.
- Microsoft's [timer documentation](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-timers)
  gives timers explicit lifetimes. Use the existing media readiness descriptor
  after its first successful poll, retaining fallback checks on unsupported
  backends. Do not rescan unchanged media on the network timer.
- Windows 98 Resource Kit, Chapter 27, `wrkc27_3.htm`, “To check whether a
  specific driver is stalling the computer”, selects Logged (Bootlog.txt).
  The owner's extracted text was read in place. Keep startup/error evidence
  in ordinary boots; make repetitive performance windows a logged-boot feature.
  A small periodic row still rewrites an aligned 32 KiB log block. Suppress
  recognized performance rows before every output sink except in logged/test
  boots or an admitted CPU/watchdog window; keep ordinary errors, configuration
  changes and unknown producers. Desktop engine/loop publications are also
  limited to one per minute on ordinary boots. Test physical writes through
  the real formatter, RAM logger and VFS boundary, not just formatted bytes.
- Intel's [Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  Vol. 3, I/O permission bitmap, LDTR and debug-register context: cache only
  a known all-denied bitmap. Invalidate that cache whenever a grant is
  installed, including VGA. The existing LDT and debug-register loaders
  already skip unchanged disabled state.
- The upstream [hardware latency detector](https://www.kernel.org/doc/html/v5.17/trace/hwlat_detector.html)
  distinguishes interrupted no-work TSC probes from actual software work.
  CiukiOS already accepts long bounded-phase gaps for CPU budget accounting;
  use the same evidence for critical-section statistics. Preserve raw wall
  duration and all unmeasured/unbounded time. A phase label alone is not
  proof that an uninstrumented `gap` was firmware.

The implementation also checks the physical allocator at the recorded
`desktop#47` stretch site. Skip occupied bitmap words while retaining the
existing first-fit result, zone order, alignment, accounting and 32-page
IRQ-off reserve/rollback bound. This changes no memory limit or reservation.

Exact commands, map resolution, replay figures, limitations and integration
items are recorded in `build/codex/101-sched-report.md` in the implementer's
worktree. Host replay figures are operation counts or deterministic model
time, not measurements of the rebuilt image on the laptop.
