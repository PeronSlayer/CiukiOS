# Live painting, CiukWeb and software OpenGL — QEMU evidence

All runs use copied FAT16 disk images in QEMU. No physical PC or GPU
acceleration is qualified. The final build command is
`CIUKIOS_VM_WINDOW=1 bash scripts/build_full.sh`; its log is
`build/tests/native-gl-final2-build.log`.

| Gate | Result and evidence |
| --- | --- |
| CiukPaint held stroke | `build/tests/native-gl-final2-paint/report.json` passed, including `live_stroke_visible=true`, the restored blue ellipse outline, palette checks, BMP save/reopen and the `stroke-held.ppm` capture before button release. |
| M4 and SB16 audio | `build/tests/native-gl-final2-vanille-audio/report.json` passed DOOM Vanille with Files open, 10,084,934 sampled PCM energy, bounded close, focus and two independent DOS VMs. |
| CiukWeb first page | `build/tests/native-gl-final2-ciukweb/report.json` passed with QEMU NE2000 and NAT. It downloaded 713 bytes containing `Example Domain` and rendered the page in a native `BROWSER.APP` window; `ciukweb-page.ppm` is the framebuffer. |
| Software OpenGL | `build/tests/native-gl-final2-opengl/report.json` passed: red/green/blue triangle pixels and Escape exit in an M4 DOS window; `opengl-triangle.ppm` is the framebuffer. |
| Cursor with DOS open | `build/tests/native-gl-final2-cursor/report.json` passed: visible pointer movement consumed in 0.063 s on the desktop and 0.122 s with a text DOS VM open on this QEMU host. |
| Cursor during DOOM gameplay | `build/tests/native-gl-final4-game-cursor/report.json` passed with DOOM level 1 visible in `cursor-with-doom.png`: 0.058 s on the desktop and 0.185 s while DOOM ran in an M4 window on this QEMU host. The gate bounds the game result to 0.5 s. |
| Icon loader and sprites | `scripts/test_ui_icons.py` passed all 36 native Tango/Ciuki sprites, 18 loader/lifetime cases and 8 reader edge cases after the palette fix. |

The image packages `C:\SYSTEM\GL\TINYGL.LIB`, `GL.H`, `CIUKGL.H`, the
TinyGL license, `C:\PROGRAMS\CiukGL\GLDEMO.EXE` and
`C:\SYSTEM\APPS\BROWSER.APP`. MicroWeb and its `WEB.COM` launcher are no
longer preinstalled. `CiukWeb` uses native desktop UI and HTML text rendering,
with mTCP `HTGET.EXE` still performing HTTP transfer. The graphics library
is a software OpenGL subset, not full OpenGL conformance or guest GPU access.

During the release profile, BIOS tick based TSC calibration in a forked DOS
VM sometimes reported about 10.5 GHz. CVSESSION rejected that value while
setting up virtual video, so COM and MZ test programs could appear to fail
before their first instruction. DPMIRUN now measures over the firmware's
timed wait, checks the result, and uses a bounded BIOS tick fallback. On this
QEMU host the new measurement was about 4.3 GHz; the text program, guest
mouse/audio and VGA window gates passed individually with the corrected
runtime in `build/tests/native-gl-tsc-debug2-{text,guestio,vga}/`.

A later profile exposed a second timing race: PCM buffer rendering could
finish an SB16 DMA block, but its virtual IRQ was not delivered until a
later device poll. `cvdev_audio_poll()` now pumps the pending IRQ in the
same service pass. DOOM Vanille's live SB16/SFX gate passes on the rebuilt
image. Adding CiukWeb's Tango icon initially requantized the shared icon
palette and changed CiukPaint colours; new icons now use the stable 0.8.0
palette. The complete Paint gate passed after that correction.

The automated VM-window profile at `build/tests/native-gl-final3-profile/`
was interrupted when the host rebooted. Its 17 completed gates have exit
code zero; there is no `SUMMARY.json`. After the host graphics configuration
was corrected, the remaining gates were run one at a time, with one QEMU
process at a time. The checked-in
[serial recovery summary](serial-summary.json), copied from
`build/tests/native-gl-serial-summary.json`,
records **26/26 passing gates** across the interrupted profile and the
separate runs. The image SHA-256 is identical in both profile directories
and the final image; this is a combined verification, not a completed single
invocation of `test_vm_window_profile.sh`.

The earlier run saw interleaved serial output inside a `[DOSVM] fork`
prefix during the second `COMMAND.COM` launch. The M4 gate now checks the
executable path in that launch; both DOOM SFX variants passed with the
corrected check in `build/tests/native-gl-final4-{vanille,original}-sfx/`.
