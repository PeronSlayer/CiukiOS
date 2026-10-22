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

## F2-98b-fix1: chain-4 comparison and present cadence

Research checked 2026-10-23 before the correction. The
[QEMU 10.1 VGA memory paths](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c)
select the chain-4 plane from the low two address bits and distinguish
doubleword display addressing from host memory access. Its backing layout
differs from Ciuki's separate plane arrays. In this repository,
`cvga_store_chain4_span`, `cvga_chain4_dirty` and `cvp_chain4_row32` agree:
successive displayed bytes within one chain-4 plane are four array positions
apart, with 16-bit wrap. Mode X/Y uses consecutive array positions. The new
shared `video_planar_changed` comparison incorrectly used the Mode X/Y
address calculation for both. A store at mode 13h offset 100 changes plane 0
at 100, outside the comparison's incorrect first-row span 0..79.

Keep the existing
[F3-41 cadence contract](../directives/f3-41-bounded-kernel-stretches-presenter-compositor-present-cadence.md)
(required item 2 and review amendment item 3): sample changing mode 13h at
half the programmed refresh, with the existing fast-flip exception. F3-45
preserved that test; [F3-52](f3-52-window-wait.md) preserved its fractional
deadlines through WAIT/PRESENT. The
[DOOM source](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/doomdef.h)
defines 35 game tics per second; the half-refresh cap is Ciuki's explicit
policy, not a general VGA or Windows requirement. Microsoft's
[update-region contract](https://learn.microsoft.com/en-us/windows/win32/gdi/the-update-region)
supports retaining precise damage rather than manufacturing unchanged
updates. The owner's read-only Resource Kit `wrkc28_8.htm`, Graphics Device
Interface / Graphics Subsystem, was also checked; it describes the DIB engine
and display minidrivers, not a DOS presentation-frequency mandate.

Correct the cache's chain-4 stride and wrap while retaining the contiguous
Mode X/Y comparison. Keep the 35/30 assertions, row callback convention and
`video_rows` bookkeeping. Add incremental scanout coverage against the scalar
renderer, including all four planes, panning, split screens, address wrap,
and unchanged stores. Host commands, results and qualification limits are
recorded in `build/codex/f2-98b-fix1-report.md`.

The cadence fixtures keep their original 35/30 and 70 expected counts.
They now give flipped pages distinct pixels, and the 60 Hz deferral fixture
makes a visible store alongside its timing change. Blank-page flips and
timing-only changes cannot require a surface update under F2-98b's unchanged
frame rule. This changes the test inputs, not the cadence policy or counts.

The pending-DAC-observation assertion in `test_bounded_present` retains its
one-present expectation and checks the completed sample count, acknowledged
display counter and retained-renderer golden. The cache must retain the
DAC mask from the sample's captured display generation, not the live mask
at the end: a change across a yield can occur after row admission, leaving
rows unconverted. Retaining the initial mask ensures that the pending
display change forces those rows through conversion at the next due sample.
Separate black-page flips now explicitly check zero surface rows and zero
scanline damage while retaining the 70 Hz fast-flip sampling policy.

The complete ASan/UBSan DOS VM host executable passed 65,186,617 checks:
35/30 presents for the 1,000-tick changing-pixel cases, 70 changing-page
flips after warm-up, 384 incremental chain-4 goldens, and all existing
video/wait tests. LeakSanitizer then exited because ptrace is restricted in
the sandbox; assertions passed, but leak checking requires the lead's
unrestricted run. No QEMU or image build was performed.
