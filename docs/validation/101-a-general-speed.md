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


Review follow-up: media observer recovery and complete readiness coverage

The review directive adds two requirements: retry a failed snapshot after its
watch descriptor closes, and observe changes that do not mount/unmount a drive.
Research checked before these fixes:

- Microsoft [Detecting Media Insertion or Removal](https://learn.microsoft.com/en-us/windows/win32/devio/detecting-media-insertion-or-removal),
  [DBT_DEVICEARRIVAL](https://learn.microsoft.com/en-us/windows/win32/devio/dbt-devicearrival)
  and [DBT_DEVICEREMOVECOMPLETE](https://learn.microsoft.com/en-us/windows/win32/devio/dbt-deviceremovecomplete)
  describe media events independently of an application's periodic refresh.
  These current Win32 pages support the notification semantics; their current
  minimum-version tables are not evidence of Windows 98 implementation internals.
- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  sections 10.6 and 10.8.26.1, describes medium-change unit attention and TEST
  UNIT READY. The existing ATAPI driver already turns these into media generations;
  an audio-only CD deliberately has no ISO namespace mount.
- Microsoft's [USB Device not recognized](https://learn.microsoft.com/en-us/previous-versions/troubleshoot/windows-server/usb-device-not-recognized-error)
  describes a warning for a malfunctioning attached device. This later-Windows
  reference supports the existing warning, not an NT device-manager architecture.
- Microsoft's [Using Timers](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-timers)
  retains explicit timer lifetimes. Keep the existing two-second retry for an
  unavailable observation, with readiness suppressing it only while a live watch
  exists.

Repository comparison: media_run closes its descriptor on any state, volume or
CD query error; media_poll must clear the previous readiness confirmation when
collecting that failure. The fallback predicate must additionally require a live
worker descriptor. Optical service already compares observed generation/result;
notify media observers after publishing that transition, including audio-only
arrival/removal. USB enumeration already increments a bounded, one-warning-per-
quarantined-device counter; notify after incrementing it. Both reuse the existing
media serial and poll wake, without modifying drive generations or public ABI.
Unchanged optical state and already quarantined ports must emit no extra wakes.
The serial is captured before copying state so a concurrent later transition
remains readable. Namespace-only publication continues to use the same serial.


Historical corroboration checked during follow-up validation: Windows 98
Resource Kit, Chapter 12 `wrkc12_1.htm` lists AutoPlay among the multimedia
features; Chapter 24 `wrkc24_4.htm`, “CD-ROM performance problems occur when
AutoPlay is enabled”, identifies AutoPlay as enabled by default. The owner's
extraction was read in place. These passages support preserving the existing
AutoPlay behavior while fixing readiness; they do not specify a notification
implementation or justify silently disabling audio-CD observation.
