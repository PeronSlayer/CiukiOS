# Display modes supported by the DOS-window presenter

The VBE 3.0 mode table defines mode `101h` as 640×480 with 256 colors; its
ModeInfoBlock describes 8 bits per pixel. It defines the direct-color memory
model separately and specifies the component masks used to interpret those
pixels ([VBE Core Functions Standard 3.0, pp. 19 and 36–38](https://www.cs.utexas.edu/~dahlin/Classes/439/ref/hardware/vbe3.pdf)).
SeaBIOS's QEMU Standard VGA mode table confirms the relevant firmware modes:
`110h` 640×480×15, `111h` ×16, `112h` ×24, and `142h` ×32
([upstream SeaBIOS source](https://qemu.googlesource.com/seabios/+/refs/tags/rel-1.8.0/vgasrc/bochsvga.c#33)).

The DOS-window path sends VBE scanout through `dosvm.c::band_present` and the
CVSESSION retained presenter. `band_present` refuses a one-byte target, and
`vga_presenter.c::valid_format` supports target pixels of 2, 3, or 4 bytes.
Consequently a valid indexed 8-bit desktop mode can make a DOS window fall back
to its blank client fill even while the guest continues running. The desktop's
existing `ui_mode_switch` ends and recreates the compositor band and framebuffer
binding; the retained presenter also invalidates on a target-format change, so
the evidence points to a format-support mismatch rather than stale pitch/cache
metadata.

Decision: DISPLAY.APP offers only direct-color VBE modes with 15, 16, 24, or
32 bits per pixel and validated RGB masks/access. This lets its existing
geometry/depth sort choose 640×480×15 (`110h`) ahead of indexed `101h` when the
user chooses the smallest mode. The separate VGA 16-color recovery profile
remains available through its existing explicit path; it is not treated as a
VBE direct-color framebuffer mode.

The production-path host fixture models indexed `101h` and direct-color
640×480 modes. It confirms that `101h` is omitted and the sorted first 640×480
candidate is supported direct-color 15-bit mode `110h` with banked access in
V86. On 2026-10-07, `python3 scripts/test_display_probe.py --output
build/tests/display-probe-640-direct` passed, and OpenWatcom compiled both
`display.c` and `display_probe.c` with warnings treated as errors.

Runtime qualification passed on 2026-10-07 with QEMU Standard VGA. The test
started at 1024×768, previewed and kept banked direct-color mode `110h`
(640×480×15), then launched the shipped `DOOM.COM -warp 1 1` twice. Both runs
responded to gameplay input (148,056 changed client pixels), exposed and
navigated the full six-row menu, accepted `y` at the quit prompt, returned to
the desktop, and closed the DOS window. The recorded VBE readback confirms
mode `0110h`, 640×480 geometry, 1280-byte pitch, and `path=BANK`; the result is
in `build/full/t23-vbe-fix/final-display-doom-std2/report.json` and its
`DISPLAY.LOG`. This validates the QEMU Standard VGA path only; the separate
Cirrus run is recorded below. Neither run is a physical-hardware result.

The QEMU Cirrus run used the same 1024×768→640×480 preview and kept mode
`110h`. Its readback reports 640×480, 1280-byte pitch, and `path=BANK`; the
1024×768 starting mode was `118h`. Both `DOOM.COM -warp 1 1` launches changed
the client image in response to gameplay input (140,329 and 33,628 pixels),
navigated menu rows 0–5, accepted `y` at the quit prompt, and returned to the
desktop. Details are in
`build/full/t23-vbe-fix/final-display-doom-cirrus/report.json` and its
`DISPLAY.LOG`. Together these runs qualify the QEMU Standard VGA and Cirrus
profiles; they do not establish behavior on physical adapters.

The preceding combined run stopped at the host harness's cursor assertion
before launching Doom. Its screenshot used exact RGB555-expanded cursor
colors, which the harness now recognizes without loosening its sprite-mask
check. That was a harness failure, not a DOS-window or Doom runtime failure.
