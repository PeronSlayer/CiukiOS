# F2-98b foreign-surface update discipline

Research checked 2026-10-22, before implementation. Host evidence only;
hardware qualification and the canonical image remain with the lead.

## Sources and implementation decisions

* Microsoft's [update-region description](https://learn.microsoft.com/en-us/windows/win32/gdi/the-update-region)
  and [WM_PAINT contract](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-paint)
  specify invalidation accumulation and painting after queued input. Keep
  the existing v1/v2 DAMAGE and v3 DAMAGE/PRESENT rectangle encodings;
  discard portions covered by opaque higher windows before composition.
  A covered synchronous PRESENT must still complete without a screen write.
  The [clipping-region contract](https://learn.microsoft.com/en-us/windows/win32/gdi/clipping-regions)
  explicitly supports intersection and exclusion; use opaque window and
  shell-overlay bodies as exclusions, keeping their dependent shadows.
* [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex)
  describes clipping a translated copy and invalidating its exposure. Copy
  only pixels visible at both source and destination, in overlap-safe order;
  reconstruct the remainder from the retained surfaces.
* The owner's Windows 98 Resource Kit, chapter 28, `wrkc28_8.htm`,
  Graphics Device Interface / Graphics Subsystem, was read from
  `/home/peronslayer/Desktop/CiukiOS/build/rskt/` (read-only). It describes
  GDI, the 32-bit DIB engine and display minidrivers, including S3. This
  supports damage/clip/BitBlt discipline, not a claim that Windows 98 used
  a retained-surface compositor. No private source text is copied here.
* The earlier lead decision pins X11 banded regions to pixman 0.46.4 in
  `third_party/pixman/`. The upstream
  [region implementation](https://gitlab.freedesktop.org/pixman/pixman/-/raw/master/pixman/pixman-region.c)
  and [DAMAGE protocol](https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/raw/master/damageproto.txt)
  links were attempted but unavailable through the web reader; the pinned
  local implementation remains the reference for region arithmetic.

Repository audit: Win32 GDI already sends each damage rectangle; v3 GL
already has rectangle payloads and deferred completion. NetSurf instead
collapses damage to a covering box. DOS already waits on a VM producer
event and phase-preserving VGA deadlines, and the indexed renderer retains
plane bytes; its public damage rounds destination rows to eight-line bands.
Preserve legacy bands and add an opt-in exact row bitmap without changing
the command size. New clients retry the legacy request on EINVAL.

The report in `build/codex/f2-98b-report.md` records the per-event audit,
verification, model assumptions and remaining qualification.

## Implemented contract

Client DAMAGE is clipped to its displayed content, the screen, opaque higher
windows and opaque shell overlays. A full-surface producer may still use the
original rectangle protocol. An identical opaque backing is recognized
against the completed RAM scene where geometry and overlays permit an exact
comparison; XRGB padding bits are ignored. GL PRESENT completes immediately
when no visible update remains, and otherwise only after a completed frame.

Moves use a region visible at both endpoints. Copy rectangles are traversed
by vertical band in movement order and horizontally in movement order,
preserving overlapping sources. Higher windows' shadows, shell shadows,
notification entry sweeps and changed sources in the same event batch are
excluded. Horizontal and vertical scroll candidates up to 64 pixels are
proved against every copied pixel, clipped to the actual damage, and use
the same copy traversal. Failed proofs, unavailable hardware and exhausted
region storage retain the repaint fallback. No new panic path is added.

DOS opts into `CIUKI_DOSVM_PRESENT_SCANLINES` through the formerly reserved
PRESENT_BANDS word at offset 84. The response overlays the command tail
(offset 72) with 15 little-endian u32 words: bit y is destination scanline y,
0..479. The record stays 136 bytes; flags=0 retains bands and the damage
rectangle. The app retries flags=0 once on EINVAL from an older kernel.
Chain-4 and Mode X/Y share the exact previous-plane-byte comparison; the
destination surface comparison also avoids unchanged text/VBE row writes.
Forced restoration writes the entire frame. No second VM image is retained:
new persistent storage is 60 bitmap bytes per VM, and the comparison stack
buffer is 256 bytes. VM memory limits are unchanged.

WAIT_PRESENT remains a producer wait using the existing phase-preserving
VGA deadline. Its maximum timeout bounds input service, not presentation
frequency. A launch-bound DOS client's delivered frame bypasses the shell's
30/60 Hz quantization; the host test checks 70.09 Hz notifications.

NetSurf uses the already pinned pixman region implementation to retain
separate invalid rectangles. Within them only changed pixel runs are copied
to the shared surface and sent, coalescing equal adjacent row runs. A refused
send retains its copied-but-unsent rectangle. Region allocation failure uses
the existing covering-box recovery rather than dropping damage. Resize and
close release region storage. The build fingerprints pixman and installs its
license notice. Toolkit/native application and pointer ownership/raster
implementations are unchanged.

Menu highlight changes invalidate the previous/new row (including Platinum's
extra edge pixel); title changes invalidate the visible caption and that
window's task button; clock/tray width changes invalidate the previous/new
tray extent. Error notifications no longer add a redundant full top bar.

Budgets are workload-specific host models, not physical-machine evidence.
The report states their rates, included costs and exclusions. Arbitrary full
frames cannot satisfy a budget below the measured LFB transfer lower bound;
the lead must qualify actual applications on the canonical image.
