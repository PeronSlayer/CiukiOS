# F2-74 removable media notification and safe removal

Research checked on 2026-10-16:
[USB Mass Storage Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
sections 3.3 and 5.3, requires completed command/data/status transactions before
issuing the next command. Reset recovery repairs transport framing; it is not
safe removal. [T10 SAT START STOP UNIT clarification](https://www.t10.org/ftp/t10/document.05/05-226r1.pdf)
distinguishes cache synchronization from the load/eject command.
[SFF-8020i](https://www.jmargolin.com/project/SFF8020i.pdf), section 10.8.25,
defines START STOP UNIT, opcode 1Bh, LoEj=1 and Start=0 for tray ejection.

The repository already has generation-qualified MEDIA_SAFE_REMOVE requests,
whole-device open-handle preflight, FAT barriers and clean unmount, and an
ATAPI eject command. Extend that interface with a read-only, generation-qualified
MEDIA_GET_BLOCKERS query listing each owning process once across every partition
of the physical USB device. Preserve the existing request layouts. Detach the
optical namespace before sending eject; a failed command leaves it unmounted
until the existing readiness service republishes it. Successful USB removal
releases the storage stage, retains enumeration and prevents automatic remount.
Device inventory retains the reason and identifies USB mass storage explicitly.

The physical a395ffa8 logs show an empty optical drive and USB mount refusal
with result -12. They contain no framebuffer capture and do not prove a popup
layout diagnosis. Host replay separately reproduces a compositor defect: the
opaque client repaint and accelerated scrolling/moving shortcuts omit active
notifications from their overlay exclusions. A client underneath can overwrite
the notice. Notification wrapping also measured four pixels wider than drawing.
Use the same text width for measurement and drawing, reserve complete text rows,
and exclude those shortcuts where they intersect the notification (see review fixes below). Refresh inventory
before posting removal success, so the disconnect update cannot replace it.

The additive blocker record contains 64 `(pid, name[32])` entries (2324 bytes),
covering the process-table limit. It is a fresh observation after EBUSY, not an
ownership token. The UI formats names and PIDs, with an explicit remaining count
if its bounded text area fills. Duplicate descriptors produce one process row;
sibling partitions are included. A descriptor undergoing final-close cleanup
can still refuse removal even if it no longer has a process-table owner.

`CIUKI_MEDIA_SAFE_REMOVED` in GET_STATE's per-letter type array is an absent
USB volume's retained successful-removal result, with its physical generation.
It is never in `removable_mask`, never an arrival and never a usable volume.
The retained result prevents a later physical unplug from producing a second,
misleading unsafe-removal notification. A surprise removal still warns and
uses the existing dirty-volume admission/repair rules on insertion.

Removal completes before its eight-second notification lifetime starts.
Inventory refresh precedes the explicit success notice. Notifications have a
close affordance, Escape and outside-click dismissal, retain click-to-open,
and use the existing 160 ms entry animation without stealing taskbar clicks.
A newer media observation can replace an earlier media warning, so a quick
reinsertion cannot leave a stale removal message visible.

The f4-media suite adds a USB write, busy refusal, safe-remove and QMP detach
case and requires safe-remove evidence after each existing optical reinsertion.
The optical surprise-removal/stale-handle checks remain. USB detach is restricted
to the generated fixture's explicitly named frontend. These declarations have
host validation only; the lead must build the canonical image and run the suite.

A failed GET_STATE observation preserves the last successful desktop snapshot;
it cannot fabricate an unplug or dismiss a drive's notification. The service
replay covers this alongside a ten-second removal request and busy-process names.

The baseline compositor was rebuilt in a small host harness and fails on the
precise client damage rectangle, producing the visibly overwritten panel in
`build/host/f2-74/before/platinum-640x480-client-damage.png`. The fixed replay
checks complete versus incremental frame parity on every turn. Its 54 pinned
frames cover three themes, two resolutions, attach entry/settle, client writes,
busy refusal, safe removal, expiry, CD insertion/ejection and surprise removal.
The approved portrait is loaded directly through the existing deterministic
converter. PNGs are in `build/host/f2-74/after/`; updated generic notification
captures are in `build/host/f2-74/transients/after/`. These are host review frames.

Host validation on this worktree: all 102 desktop tests passed using package
discovery (`-s tests/host/desktop -t tests/host`); the complete kernel host script,
15 optical host tests, five volume-resolution runner tests and the new media
runner test passed. The SDK was rebuilt with one job, and the desktop and kernel
built successfully. Kernel stack limits and the integer instruction audit passed.
All host commands ran sequentially with `nice -n 19`. Address and undefined
behavior sanitizers remained enabled; leak detection was disabled because the
sandbox does not permit LeakSanitizer's process inspection. No canonical image
was built, and no QEMU or hardware qualification was run. The gate result and
artifact hashes are recorded in `build/codex/f2-74-report.md`.

## Review fixes (2026-10-16)

Additional primary references checked before implementing the review fixes:
[BitBlt](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt)
defines separate source and destination rectangles; destination clipping does
not protect against copying an overlay from the source.
[DrawText](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawtext)
documents measuring wrapped text with the drawing width and suppressing partial
last lines. These are rendering references, not new Win32 API dependencies.
[POSIX stat](https://pubs.opengroup.org/onlinepubs/000095399/functions/stat.html)
provides file identity, size and modification metadata without reading contents.
[Media insertion/removal](https://learn.microsoft.com/en-us/windows/win32/devio/detecting-media-insertion-or-removal)
distinguishes media disappearance from arrival; CiukiOS's authoritative usable
namespace is its removable mask, even when a removal result retains a generation.

Repository decisions: absent current removable drives return Files to Computer.
Protect only rectangles intersecting the notification's shadow and entry sweep;
check both endpoints of window copies and the full scroll area. Cache drive text
by letter and generation. At media polls compare cheap volume-label and
AUTORUN.INF identity/size/modification metadata; rescan/read label contents only
on changes (or explicit F5 refresh). Selection and layout consume cached text.
Fit whole blocker entries using production text wrapping, reserving the wrapped
remaining-count row before accepting each entry. Notification bounds use the
actual display; Files uses its guaranteed minimum message width and maximum
available body height, so an automatically narrowed dialog still fits.
