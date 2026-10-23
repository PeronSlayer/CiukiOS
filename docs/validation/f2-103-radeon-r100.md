# Radeon R100/RV100 MMIO port: host validation

The implementation follows the lead's f2-103 directive and the family
contract in the lead checkout (the latter was absent from this worktree's
part A base). This is host evidence; runtime and physical qualification
remain with the lead.

## Primary sources and implementation decisions

- [Linux v6.12 `drivers/video/fbdev/aty/radeon_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeon_accel.c),
  `radeonfb_prim_fillrect`, `radeonfb_prim_copyarea`, `radeonfb_engine_init`:
  port the MMIO format, pitch/offset packing, solid ROP and overlap direction
  sequences. Keep VBE scanout. Do not import PLL/reset, CP, DMA or GART.
  The donor's `imageblit` is CPU software, so it supplies no native WRITE bit.
- [Linux v6.12 `radeon_base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeon_base.c),
  `_radeon_fifo_wait`, `_radeon_engine_idle`, `radeon_engine_flush`, PCI table;
  [register reference `include/video/radeon.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/radeon.h)
  and [private header `radeonfb.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeonfb.h):
  FIFO-empty, GUI-inactive and destination-cache-flush completion are separate
  proofs. Replace log-and-continue timeouts with a deadline, an iteration cap,
  1 ms presenter slices and quarantine. Omit queued `WAIT_UNTIL`: synchronous
  CPU polling establishes the required ordering without a hardware wait packet.
  Validate the chip-local framebuffer base on every draw; a changed base cannot
  silently redirect writes. Discrete R100/RV100/RV200 IDs are explicit;
  integrated RS100/RS200, later generations and secondary functions stay off.
- [X.Org ATI tag `xf86-video-ati-6.14.6`, `src/radeon_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_cursor.c)
  and [register reference `radeon_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_reg.h):
  cursor replacement hides first, uses a separately reserved VRAM allocation,
  drains CPU writes and updates position under the cursor lock. Its ARGB
  payload is not compatible with the emulator's monochrome implementation.
- [QEMU v9.2.0 `hw/display/ati.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/ati.c),
  `ati_mm_read`, `ati_mm_write`, `ati_cursor_define`, `ati_vga_realize`, and
  [`ati_2d.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/ati_2d.c):
  the RV100 identity is `1002:5159`; FIFO status is 64 and cache/MMIO fields
  outside its implemented subset read as zero. The fake transport uses that
  subset, including launch registers and packed pitch decoding. Its cursor is
  64x64 with eight AND bytes then eight XOR bytes per row. Negative-position
  cropping is explicitly incomplete upstream; cleanly refuse negative moves
  before changing cursor state. Ciuki's legacy cursor uses coverage plus a
  foreground selector, so uncovered pixels become AND=1/XOR=0.
- Windows 98 Resource Kit, chapter 24, `wrkc24_3.htm`, “Display Driver
  Overview”, read from the owner's read-only extraction: the DIB engine draws
  unaccelerated operations. Keep that dispatch in the existing generic layer;
  the driver does not implement hidden CPU drawing or replay failed COPY.

Optional pattern/upload and the newer part C commands are not claimed by the
Linux F/C/Y donor. They remain software operations; XA optional engine work
and extended ABI allocation remain f2-106/f2-101d scope. No public ABI or SDK
header is changed. Device Manager and Control Panel consume the registry's
display name, `ATI Radeon R100/RV100`, through the existing generic query.

The legacy nonempty `accel.cfg` attempt record remains a global fail-closed
gate until part B introduces keyed markers. This driver never clears a
retained record or lifts same-boot quarantine. CPU safety is cached separately
from quarantine, and an unsuccessful idle/cursor-disable proof retains the
rejecting ops and resource leases.

The hardware page previously required the optional family sweep before it
would reach DM9. It now invokes the existing generic comparison directly
when that hook is absent. The existing family-sweep path remains available.
Cursor storage and registers use reusable private UC windows, revoked by the
registry's idle proof before release. They do not call the scanout-only
framebuffer mapper and do not change the active framebuffer's cache policy.

`probe` also refuses a firmware-enabled PCI bus master. The driver never
changes PCI command bits; a CP that becomes enabled or a chip-local base that
changes after activation latches an ownership failure and cannot authorize CPU
fallback on a momentary idle observation. Integrated RS parts need a separate
UMA ownership qualification and are not implied by the pack's R100 label.

## Verification

All host suites ran sequentially at `nice -n 19`:

| Command following `nice -n 19` | Result |
| --- | --- |
| `python3 -B tests/host/test_radeon2d.py` | PASS, 2 tests: fake registers + production registry/presenter; three canonical i686 objects; `nm`, 10 unique external definitions |
| `python3 -B tests/host/test_display_driver_model.py` | PASS, 7 tests; ABI oracle, 1,703 target/native checks |
| `python3 -B tests/host/test_display_driver_names.py` | PASS, 2 tests |
| `python3 -B tests/host/test_hardware_check.py` | PASS, 8 tests, including the no-sweep DM9 path and existing family sweep |
| `python3 -B tests/host/test_presenter_display.py` | PASS, 4 tests |
| `python3 -B tests/host/test_display_modes.py DisplayTests.test_kernel_switch DisplayTests.test_checkpoint_queue` | PASS, 2 tests |
| `python3 -B tests/host/test_credits.py` | PASS, 8 tests |

The unchanged Savage register/presenter fixture also passes with its repository
ASan/UBSan command and existing sanitizer settings. Radeon tests cover RGB555,
RGB565, RGB888 and XRGB8888; all four copy directions; saved-pixel restoration;
native-only comparison failure; FIFO, GUI and cache timeouts; a frozen clock;
1 ms presenter releases with console/mode exclusion; cursor plane padding,
colours, bounds, readback and failed disable; unsafe stop/guard retention;
PCI/lease/RAM-alias refusals; marker I/O failures and retained markers.

`nice -n 19 env CIUKI_DISPLAY_ASAN=1 python3 -B tests/host/test_radeon2d.py`
executes the assertions with zero failures but exits unsuccessfully when
LeakSanitizer cannot use ptrace in the sandbox. Leak detection was not disabled.
This is an outstanding environment check, not an ASan pass. QEMU, images and
physical hardware were not run. Exact logs, commands and handoff items are in
`build/codex/f2-103-report.md`.
