# Native desktop, display detection and startup sound

CiukiOS opens its native desktop in the largest usable banked VBE mode, using
higher color depth to break ties at the same resolution. The first preferred
EDID timing, when present and checksum-valid, bounds the desktop to the display.
After T23 feedback, missing/invalid EDID now selects VGA 640x480x16. The
card's maximum alone does not establish the connected panel's dimensions. Detection is
cached for the current shell process. A missing DISPLAY.CFG or `AUTO` enables
this behavior; explicit 640/800/1024 and Safe VGA settings remain available.

The renderer supports indexed 8-bit, direct 15/16/24/32-bit, and planar VGA
640x480x16 as fallback. It validates mode support, graphics/color flags, readable
and writable banks, window size, bank granularity, pitch, RGB masks and console
allocation size. It neither changes protected-mode descriptors nor hooks the
shared video/mouse services. It verifies the active mode with 4F03h and uses
the active scanline pitch from 4F06h before drawing; inconsistent geometry
restores VGA. BIOS calls preserve the caller's interrupt flags. DOS uses the same
mode, with a character buffer sized from the actual resolution. This is a
legacy BIOS/banked renderer: linear-only modes and UEFI GOP are outside its ABI.
The console is limited to 32,760 cells (64 KiB allocation).

Windows and DOS games continue to own their display while running. Automatic
selection changes the native desktop and DOS console; it preserves the installed
Windows display configuration. Explicit 640/800/1024 presets still use the
existing transactional shell/Windows setting. AUTO is saved through the same
staged file/rename path without resetting Windows settings.

The VBE fields and behavior follow the [VESA VBE 3.0 specification](https://www.cs.utexas.edu/~dahlin/Classes/439/ref/hardware/vbe3.pdf)
and the [VBE/DDC definitions published by RTEMS](https://docs.rtems.org/doxygen/5.2/vbe3_8h_source.html).

## Interface

The English desktop uses a slate workspace, midnight title bars, ivory content,
graphite text, turquoise focus accents and warm folder icons. Desktop and setup
share `src/com/ui_theme.inc` and the existing 13px regular/bold font. The stepped
C mark remains the signature; the only presentation line is “A modern Retro OS”.
The original Ciuki boot photo is retained.

The application library expands on larger screens and adds a Places & settings
sidebar, keyboard shortcuts, item types, distinct Windows/Costa/Display/Run
icons and the current resolution/color depth. Its compact layout still fits
640x480. A CiukiOS menu opens Programs, Files, Run, DOS, Display, Sound, About and
Power. Display and Sound are native graphical panels with actual actions; the
sound panel reads the saved on/off preference. The installer uses the same
palette, chrome and mark, retaining its existing Quick/Full/Install operations.

Settings commands report unsuccessful exits in a graphical error dialog;
unsupported 800/1024 VBE presets are rejected before writing configuration.
Startup-sound preference writes check creation, the byte count and close status.

Tab finds the currently focused control in each dialog's own hit list. Run
coalesces buffered editing keys before repainting, while keeping insertion,
Delete, Home/End and its horizontal viewport.

## Cursor

The old absolute driver coordinates can wrap below zero and saturate at the
opposite edge. Desktop and installer now consume signed relative motion into
private 32-bit coordinates, clamp before narrowing, and synchronize their
position with INT 33h. The full saved cursor rectangle stays inside the screen
(x <= width-24, y <= height-16). The cursor can reverse immediately at any edge.
Private cursor storage covers four-byte pixels and preserves pixels at bank
boundaries. Shared driver code and other applications' mouse behavior are not
modified; client state is saved and restored around each GUI session.

## Audio

The melody plays once before desktop video/mouse initialization. Startup enters
the selected graphics mode once, without first clearing a separate VBE console. The original
1.28-second synthesized composition is retained with more PCM headroom used
(peak approximately 14,646/32,767 in QEMU; no clipping). AC-link and analogue
power-up waits now use BIOS time deadlines, with an amplifier-settling interval.
DMA halt alone is no longer accepted as playback completion: the last-buffer
completion flag must also be present. Error cleanup stops DMA and releases its
buffers. Automatic boot stays silent when PCM is unavailable or already busy;
the speaker motif remains an explicit SOUND TEST diagnostic only. Automatic
boot does not send raw EC commands, and the player restores incoming PIC masks.
Firmware waits also have finite I/O-paced iteration budgets if BIOS ticks stop.
OFF suppresses playback. See the [T23 follow-up](t23-boot-recovery-2026-09-06.md).

The previously validated VSBHDA, HDPMI, Windows launcher and game launchers are
unchanged. Physical T23 sound and monitor behavior still require a hardware
boot; QEMU cannot establish that its analogue speaker output is audible.

## Validation artifacts

Build and acceptance logs, screenshots, audio and payload hashes are under
`build/full/ui-detail-2026-09-06/`. The final release manifest records which logs
passed and the ISO/burn outcome. Earlier failed logs are retained as debugging
evidence; only the final manifest identifies release acceptance.

`qemu_test_desktop_capabilities.py` uses a disposable BIOS filtering fixture to
exercise real emulated modes at 15,16,24,8 bits and VGA recovery, plus EDID and
missing-EDID behavior. The fixture is not shipped. Native desktop tests check
all four edges and immediate reversal, moving/resizing windows, Run editing,
DOS transitions and manual display profiles. Audio tests measure nonconstant
PCM for AC97 and (in that earlier release) PC speaker. The T23 follow-up now
requires silence for the automatic PC-speaker fallback and disabled sound.

Earlier ISO, superseded by the T23 follow-up: `9735edc51ab7220a9e0a5c0082cee60c011a69c13d565b96821216bb12436a94` (149356544 bytes).
CD-RW writing completed on `/dev/sr0`, exit status 0, with ejection requested.
No separate optical preflight or payload readback was performed.
VS Code scope `app-code-111849.scope` remains capped at 8,000,000,000 bytes.
