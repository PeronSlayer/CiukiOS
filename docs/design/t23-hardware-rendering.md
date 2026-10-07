# ThinkPad T23 display and startup behavior

## Hardware and firmware constraints

The reported machine identifies its display adapter as S3 SuperSavage/IXC,
PCI `5333:8C2E`. T23 configurations vary; the IBM PSREF lists XGA and higher
resolution panel variants, so the exact machine type/model (MTM) is needed
before treating 1024×768 as a universal panel limit. The archived [IBM
Personal Systems Reference (PSREF)](https://www.infania.net/misc/psref/tawbook.pdf)
and IBM's [T23 service and troubleshooting guide](https://ftpmirror.infania.net/sites/pccbbs/mobiles_pdf/t23tsguiden.pdf)
are hardware references. IBM's troubleshooting procedure specifically calls
for checking resolution, color depth, adapter, and monitor type when output is
distorted.

The [VESA VBE 3.0 specification](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)
defines the mode descriptor fields and call contracts used here: 4F01 reports
window A attributes, size, segment, granularity, pixel format, and separate
banked/linear layout fields; 4F02 selects a mode and uses BX bit 14 to request
linear access; 4F03 reports the current mode and access model; 4F05 selects a
bank in granularity units; 4F06 BL=01 reports bytes per scanline and maximum
scanlines. A successful 4F02 call alone is not proof of the active mode or
framebuffer path; read back 4F03 and validate the active pitch/layout.

The upstream [S3VBEFIX README](https://github.com/wbcbz7/S3VBEFIX/blob/master/README.TXT)
documents `/L0`, `/L1`, and `/L2` as normal, disabled, and forced LFB handling;
`/S0`, `/S1`, and `/S2` control display-start retrace waiting; `/B` enables
the banked-mode booster, which the README explicitly says does not work on
Savage cards. These are different controls. A successful `S3VBEFIX` load does
not prove that an application selected LFB, that a mode set succeeded, or
that banked rendering is accelerated.

## Physical evidence

The read-only acquisition at
`build/full/t23-vbe-fix/physical-logs-20261007-1844/` is disk sequence 56,
device serial `H551120730`, acquired 2026-10-07 18:44:30 UTC; its
`manifest.json` records hashes and sizes. `DRIVERS/ACTIVE.CFG` selects
`VIDEO=VBE`, `VIDEO_PCI=5333:8C2E`, `AUDIO=ICH_AC97`,
`AUDIO_PCI=8086:2485`, and `AUDIO_FALLBACK=SB_NATIVE`.
`DRIVERS/LOADDRV.LOG` reports `S3VBEFIX OK PCI 5333:8C2E IRQ 0x0B`.
It does not record S3VBEFIX command-line flags. `SYSTEM/VIDEO/DISPLAY.CFG`
contains `1024`; that is the saved geometry profile, not a mode ID, active
color depth, pitch, or access-model readback. `SYSTEM/BOOT.LOG` is empty and
there is no `SYSTEM/VIDEO/DISPLAY.LOG` in this acquisition. `SYSTEM/DOSVM.LOG`
contains VM lifecycle records, not VBE mode details. The available logs
therefore do not establish which mode, depth, palette, or access path produced
the photographed desktop. S3VBEFIX's successful load alone is not evidence
that the desktop is full color or that its startup audio played.

## Boot capture format and interpretation

`SYSTEM/VIDEO/DISPLAY.LOG` is a fixed 928-byte `CVB1` record written after the
first completed desktop paint for a mode: a 32-byte header, the active 256-byte
VBE ModeInfo block, a 512-byte controller block, and a 128-byte EDID block.
The header stores `CVB1` at 0; width and height at 4 and 6; the UI VBE selector
and displayed BPP at 8 and 9; `vc_lfb` and transport flags at 10 and 11; mode
ID and pitch at 12 and 14; failure stage and start-attempt count at 16 and 17;
failed mode and AX at the failure boundary at 18 and 20; session-bound and GPU markers at
22 and 23; completed session row-transfer count at 24; and 4F03 readback AX/BX
at 28 and 30. Multibyte fields are little-endian. The exact offsets and meaning
are implemented by `scripts/inspect_boot_hardware.py --video PATH [--output
JSON]`.

The UI VBE selector distinguishes an active VBE desktop from the VGA fallback.
When it is zero the UI uses VGA mode 12h (16 colors at four-bit depth), and
the retained ModeInfo bytes describe a prior/inactive VBE mode. When it is
nonzero, 4F03 AX/BX is checked against the logged mode and `vc_lfb` header
state. The descriptor checks geometry, supported direct-color depths,
non-overlapping RGB masks, usable bank window, and linear framebuffer access.
The header pitch is the active pitch. The ModeInfo `BytesPerScanLine` field at
offset 16 may have been normalized for active LFB use, so the parser labels it
as the ModeInfo pitch field rather than claiming it is the original banked
pitch. A VBE 2.0 controller may leave the VBE 3 linear pitch field zero.
EDID preferred dimensions are used only when the header, version, checksum,
preferred-timing feature bit, nonzero first detailed-timing clock, and minimum
640x480 dimensions are valid. Its preferred timing need not equal the current
mode.

Transport flag bit 0 means the renderer entered the bank-window path after
mapping validation; bit 1 means it entered the local-LFB path after range
validation. The renderer sets these bits before copying pixel bytes, so they
identify path use without measuring completed bytes. A nonzero session row
count records protected-session row transfers that returned successfully.
Together, these fields establish which renderer paths were used for this
capture, while the descriptor fields explain what firmware advertised.
They do not establish physical S3 performance or certify the adapter-specific
BIOS behavior; that still requires a fresh physical boot and captured record.
Likewise, `S3VBEFIX OK PCI ...` in `LOADDRV.LOG` establishes that the installer
reported a successful load for the PCI device, but its missing command-line
flags and lack of a runtime hook measurement say nothing about which VBE path
the desktop used. `video_hook_evidence` records can show whether the INT 10h
vector changed across driver installation, which is evidence of hook
installation; they do not show that the hook changed a particular BIOS call
or measure its effect. The BIOS mode-selection/readback path remains distinct
from a hook-specific runtime measurement. QEMU results validate parser and
startup logic only under the emulated firmware; they are not physical-hardware
claims.

Failure stages are 1 (descriptor/probe), 2 (console allocation), 3 (local
LFB preparation), 4 (banked layout/mode set), 5 (4F03 mode/access readback),
and 6 (active pitch/layout validation). AX is the value at that boundary,
which can be a DOS allocation error or a validated mode value rather than a
BIOS return status. A retained failure followed by a successful startup mode
documents a retry. The one-byte attempt counter wraps to zero at 256 attempts.

## Startup mode policy

The full build defaults to `AUTO`. It follows valid preferred EDID dimensions;
without those monitor data it bounds automatic selection to 1024×768. At
startup, try the resolved profile first, then rank remaining direct-color
candidates within that automatic monitor bound by area and then depth.
Continue if mode set or active-mode readback fails. Use the existing VGA safe path only after direct-color
candidates are exhausted. Explicit interactive display requests retain their
requested geometry and existing preview/rollback behavior; startup retry must
not rewrite an explicit profile or silently change a user-selected mode.

Record enough boot-time diagnostics to identify the actual VBE mode ID, BPP,
banked and linear pitch/masks, WinA descriptor, selected access path, 4F02
result, and 4F03/4F06 readback. Diagnostics should make clear when a mode is
rejected because its descriptor is unsupported versus when firmware refuses
the mode set. This is needed to distinguish indexed-color/palette behavior
from wallpaper quantization and to measure bank-switch cost on the physical
S3 adapter.

## Regression approach

`scripts/test_vbe_auto_bounds.py` assembles
`scripts/fixtures/vbe_auto_cpu.asm`, which exports the production AUTO chooser,
startup retry, and explicit `vc_begin` entry. It runs those machine
instructions in the existing Unicorn `Video` harness, with scripted VBE
descriptors and BIOS call results. This isolates candidate ranking, retry,
and failure behavior without QEMU or a full image build. The separate
`scripts/qemu_test_desktop_capabilities.py --case depths` remains available
for guest integration checks with a NASM TSR filtering real BIOS 4F01
descriptors in a private HDD copy.

On 2026-10-07,
`uv run --with unicorn python3 scripts/test_vbe_auto_bounds.py --output
build/tests/vbe-auto-startup-cpu-20261007` passed 19 checks against assembled
production instructions. The fixture supplied 15/16/24/32-bpp direct-color
and indexed 8-bpp descriptors, failures at 4F02, 4F03, and DOS allocation,
and absent/valid/invalid EDID data. It verified largest-area then highest
depth ranking, retry without repeating a failed candidate, restored mode-list
exclusion markers, bounded all-fail carry, exact explicit-mode failure, and
the no-EDID 1024×768 bound, and malformed flagged IDs without altering the
firmware ROM list. Only a fresh physical T23 boot and captured mode
diagnostics can qualify its S3 BIOS behavior.

## Current V86 access limitation

The desktop currently uses banked VBE in V86. The local LFB implementation
temporarily changes CR0 and cannot be used from a V86 client. On supported
QEMU adapters a verified banked/LFB alias permits CVSESSION row transfers;
that allowlist does not include S3, and no such alias is assumed for it.
CVSESSION can map an actual firmware-selected LFB, but enabling that route
requires distinct V86 state and protected transfers for fills, pointer I/O,
and recovery after a lost binding. Setting the existing `vc_lfb` flag or
adding an unchecked S3 alias would be incorrect. The present startup and
logging changes do not implement an S3 blitter or claim accelerated rendering
on the physical adapter. New physical mode and transport captures are needed
before deciding which additional driver path the hardware requires.

## Driver INT10 hook evidence test

`scripts/test_video_driver_hook.py` assembles the whole production
`src/com/loaddrv.asm` with NASM and invokes `video_hook_evidence` plus its
actual caller `PUSHF`/`POPF` wrapper at offsets parsed from the listing. The
Unicorn fixture supplies the pre-EXEC IVT copy, current INT10 vector, and
driver class. On 2026-10-07 it passed four checks: VIDEO unchanged pointers
are reported as `unchanged`, changed pointers as `hooked`, non-VIDEO classes
leave detail untouched, and the caller wrapper preserves registers, ES, and
flags. The helper's direct entry also preserves general and segment registers;
the wrapper restores flags because the helper itself does not save EFLAGS.
The artifact manifest and assembled code/listing are in
`build/tests/video-driver-hook-20261007/` (`loaddrv.com` is 3,495 bytes,
SHA-256 `80c7d36b38257638d2fdbaaf085e7cbf3c7cb1b00a8e0be37cfbcef0c0206d0c`).
