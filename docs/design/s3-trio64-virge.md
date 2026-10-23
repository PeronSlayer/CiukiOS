# S3 Trio64 and ViRGE native 2D ports (f2-118)

The two separate registry adapters are `s3trio64` (S3 Trio64) and `s3virge`
(S3 ViRGE). They retain firmware VBE modes. There is no native mode provider,
host DMA, bus mastering, reset, PLL, panel programming or destructive VRAM test.
Shared glue handles ownership, bounded waits, legacy cursors and lifetime;
each backend owns its register layout, FIFO tests and command sequences.

Primary sources and implementation decisions:

| Source | Decision and repository comparison |
| --- | --- |
| [Haiku r1beta5](https://github.com/haiku/haiku/tree/r1beta5), commit `a1f00118db8b858feb73f64ac4dc3d038b82bf9b`, [`trio64_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_draw.cpp), [`trio64.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64.h), [`trio64_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_init.cpp), [`trio64_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_mode.cpp) | Port 8514-style FILL/COPY, FIFO and idle status, scissor and write-mask setup. Half-open Ciuki widths/heights become hardware counts minus one. The donor's RGB565 exposure is preserved; other packed formats punt. Validate CR50 engine width, CR67 format, CRTC pitch and zero display start against the active framebuffer. Distinguish original Trio64 from V+ using PCI revision bit 6, including the original's 1600-pixel refusal. |
| [`virge_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_draw.cpp), [`virge.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge.h), [`virge_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_init.cpp), [`virge_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_mode.cpp), same tag | Port the separate S3D register interface, RGB565 format, GX2/mobile FIFO encoding and overlap direction on both axes. Width is minus one, height is the actual count. Disable autoexecute with NOP and program source/destination bases, pitch and clipping only after idle. Replace the donor's unbounded waits with deadline and iteration bounds. Refuse STREAMS scanout, nonzero display start and mismatched pitch/format. |
| Haiku [`s3/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/s3/driver.cpp), same tag | Match Trio `5333:8811/8814/8901`; ViRGE `5333:5631/883d/8a01/8a10/8c01/8c03`. Both use framebuffer BAR0 and the internal 64 KiB register aperture at BAR0+16 MiB, independently of the existing Savage adapter. Exclude Trio3D and Savage IDs. Reuse part A's complete live framebuffer ownership gate before accessing hardware. |
| Haiku [`trio64_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_cursor.cpp) and [`virge_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_cursor.cpp), same tag | Reserve a separate offscreen page within the 12-bit KiB cursor address range. Initialize 2048 bytes transparently (the Trio workaround), then interleave two AND bytes and two XOR bytes per 16-pixel group. Translate the legacy opaque mask to AND=`~mask`, XOR=`image & mask`; use a 64-pixel storage stride for the required 32-pixel image. Hide before replacement; verify base, position and enable registers. CR48 is the final position write. |
| X.Org [`s3v_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_accel.c), commit `211e2831dcf13c6e5847f6caf894f559d9104c3d` | The original ViRGE/VX BLT_BUG width bands are refused before any register write: 26–32 pixels at the supported 16 bpp. No transparent mono or disabled line path is imported. These are conservative refusals, not a hardware qualification of the remaining widths. |
| X.Org [`s3v_hwcurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_hwcurs.c), same commit | Cursor colours on older 16-bpp parts use native low/high bytes, repeated for double-clock pixels; GX2/MX use RGB bytes. Convert native RGB565 to RGB8 for the latter. No X.Org source is imported. |
| S3 [Trio32/Trio64 databook](https://vgamuseum.info/images/doc/s3/trio32_trio64_graphics_accelerators_mar1995.pdf), CR45 and cursor origin register reference; S3 [ViRGE databook DB019-B](https://www.bitsavers.org/components/s3/DB019-B_ViRGE_Integrated_3D_Accelerator_Aug1996.pdf), extended CRTC register reference; S3 [ViRGE/VX DB025-A](https://www.bitsavers.org/components/s3/DB025-A_ViRGE_VX_Integrated_3D_Accelerator_Jun1996.pdf), section 15.5 | Checked indexed vendor text: CR45.4 is right-side storage; CR55.4 selects Windows/X11 cursor truth tables. Enable only contiguous storage with the Windows truth table, otherwise keep the desktop's software cursor. Direct PDF retrieval was blocked or too large; register sequences are grounded in the pinned, retrieved donor files. |
| Owner's Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`, Display Driver Overview (lead's read-only `build/rskt`) | Its DIB-engine/minidriver split matches the existing `swdib.c` fallback. Unsupported formats/parameters return clean refusal; a submitted failed COPY is never blindly replayed. |

The chosen Haiku `virge_init.cpp` exposes RGB565 (and indexed 8 bpp),
although its low-level engine setup also contains a 24-bit opcode. The
additional primary-source check of X.Org
[`s3v_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_driver.c),
`S3VModeInit`, shows that packed RGB24 needs STREAMS scanout state. Both
Ciuki ports therefore bind for acceleration only in RGB565. RGB24/32 remain
software. ViRGE/VX RGB565 admits both CR67=0x40 and 0x50, preserving its
firmware clock selection. No clock or STREAMS programming is imported.

All donor downloads remain under `build/research/haiku-s3/`. Imported notices
are retained per file and in `docs/licenses/haiku-s3-r1beta5-NOTICES.txt`.
The two source contracts missing from this branch were read in the lead's
checkout: `video-driver-families.md`, including its lead decisions, and
`display-command-semantics.md`, including its lead decisions. Part A's
`accel_ops`, capability bits and userspace/SDK ABI are unchanged.

Activation first commits the existing conservative unkeyed `accel.cfg`
attempt marker outside the presenter lock. It retains any nonempty marker,
including an older driver's interrupted attempt. Activation consumes one
authorization, claims VGA ports and uncached registers, then performs setup
and saved-pixel FILL/COPY comparisons under `fbdev_accel_probe`. The marker
is cleared only after success. The small saved block is restored only after
proven idle. Setup preserves VGA register locks and index selection; MMIO
decode is enabled through CR53 if necessary and restored during safe stop.
All remaining scanout/clock/panel state is left with VBE.

Waits have a 20 ms monotonic deadline, a 4001-poll frozen-clock cap, 5 us
pacing, and presenter release at each 1 ms slice. Failed pacing, cancellation
and timeout quarantine the driver for the boot. CPU readiness is cached and
nonwaiting. Quarantine hides the cursor and disables native capabilities;
CPU access still requires idle and a proved cursor disable. Unsafe stop
retains access leases and the rejecting guard. No reset is used as a proof.
No engine command requests host data, so a bounded poll cannot leave a
command awaiting a CPU transfer. Safe stop revokes mappings before release.

The native optional operation currently exposed is the monochrome cursor.
It requires spare independently claimed VRAM and supported firmware cursor
layout. Odd negative cursor coordinates and coordinates outside [-30,2047]
are refused before changing position: the donor's negative offsets are even.
No ARGB cursor, offscreen row lease, PATTERN or WRITE is advertised. INVERT
is present in the donor, but part A has no INVERT hook or capability number;
its dispatch awaits part C/f2-101d under Amendment 1. It is not falsely
advertised or assigned an unreviewed ABI number here.

`nice -n 19 python3 -B tests/host/test_s3legacy.py` exercises independent
register decoding against production software pixels, exact command order,
both overlap axes, cursor layout/order, PCI/probe/format refusals, marker
failure, quarantine, frozen-clock and post-submission timeout, cancellation,
and the production resource glue. It also compiles all five native i686
objects and links them relocatably. The generic DM9 check automatically
uses both adapters' native ops with no software replay; cursor evidence
comes from `cursor_test`. Device Manager and Control Panel consume the
registry's display names through their existing generic records.

The H-page dispatcher previously required an optional private sweep before
admitting DM9. It now runs the generic native comparison directly for a
registered driver without a sweep. FILL and COPY evidence are required for
PASS; missing evidence, timeout and faults fail. An unsupported active format
is SKIP. Select a qualifying VBE RGB565 mode before hardware validation;
the new generic path does not change modes merely to claim acceleration.

Host evidence is recorded in `build/codex/f2-118-report.md`. ASan with its
default leak checking needs an environment where LeakSanitizer may inspect
threads; the sandbox denies that inspection. No image, QEMU or physical
hardware evidence is claimed. Both families still need real hardware DM9
comparisons before qualification.
