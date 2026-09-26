# Native windows and banked console repair

The native desktop now keeps Programs, Run, About, Display and Sound as independent windows. Opening another native window retains the previous window's position and Run edit buffer. Titles raise and drag windows; native windows can be minimized and restored through their task buttons. Alt+Tab switches windows, Alt+F4 closes the active window, and Ctrl+Tab changes the Programs category. DOS applications still execute fullscreen with exclusive DOS/video ownership, then return to the retained desktop.

The automatic video profile uses the validated monitor timing when available. When DDC/EDID is unavailable, it enumerates real banked VBE modes up to 800×600 instead of immediately choosing 640×480 VGA. Explicit 640 and SAFE recovery preferences remain available. A mode must still pass geometry, pitch, readable/writable bank and pixel-format checks before the renderer uses it.

Rendering uses a 60 KiB staging allocation, split into horizontal bands suitable for the selected pixel format. A band reaches VRAM only after its background, windows, controls and text have been composed. Dragging invalidates the union of the old and new bounds; editing invalidates the text field. Unchanged native pixels are skipped during the copy. The software mouse cursor stays visible while bands are composed and is hidden only for an intersecting band's VRAM copy. No physical-machine frame-rate claim is inferred from screenshot timings.

The original photograph, English labels and the exact tagline `A modern Retro OS` remain. Floppy, USB drive and CD-ROM icons are visible on the initial desktop and launch `MEDIA FLOPPY`, `MEDIA USB` and `MEDIA CD` respectively. Actual device access and its restrictions belong to MEDIA.COM; icons alone do not establish device support.

## DOS console

At high color depths, one vertical glyph can span multiple 64 KiB VBE banks. Repainting every glyph and every intermediate scroll state caused extreme directory-output delays. The console now records changed cell ranges while commands run and paints them in scanline order before waiting for keyboard input. Long output flushes periodically; interactive key echo remains immediate. The compatibility COMMAND.COM binary is unchanged by these GUI/console additions.

The independent HDD validator measured all 116 entries of `DIR WINDOWS`, followed by a successful ECHO, in 0.974 seconds including command typing at 1280×800. The earlier renderer exceeded 90 seconds in the reported baseline. Evidence: `build/full/hdd-regression-2026-09-06/video-after-2/`.

## Validation

`scripts/qemu_test_native_windows.py` boots an installed HDD without a CD, injects the specified shell and optional repaired kernel into a disposable copy, and sends real BIOS keyboard / relative PS/2 mouse input. It checks visible overlap, retained field pixels, focus and close, six distinct live drag positions, minimize/task restore, keyboard minimize, category changes, four cursor corners with immediate reverse, a DOS ECHO, and desktop return. Opening-frame checks allow only the new window, the old active title/focus indicator and the task buttons to change; unrelated desktop pixels must stay identical. The software cursor's exact 24×16 footprint is measured separately: it may briefly disappear during its intersecting band's VRAM copy, and must be complete after rendering. The Cirrus 24-bit capture observed this transient; it did not show damage to the desktop.

The earlier 800×600 run captured eight frames with five distinct intermediate opening states, with no unrelated pixel changes. Results and screenshots are in `build/full/ui-multiwindow-2026-09-06/windows-final/`. Additional runs cover 1024×768 indexed, automatic 1280×800 32-bit, and a Cirrus BIOS without DDC selecting 800×600 24-bit. See each result JSON and its preserved `tested-shell.com` for exact provenance; intermediate failed candidates remain in the evidence directory. These initial passes did not expose the minute-change bug described below.

The old installed kernel failed during the return to DOS after long high-resolution GUI sequences, including a control with periodic text flushing disabled. The identical shell passed with the repaired BIOS disk-register-preservation kernel. Final qualification must include that kernel, not just replace SHELL.COM on the old HDD.

The native keyboard loop uses the enhanced BIOS functions so Alt+Tab's `A5` scan code remains distinguishable after Alt has been released. The relevant primary implementation reference is [SeaBIOS keyboard handling and key table](https://github.com/coreboot/seabios/blob/master/src/kbd.c).

## Minute-change corruption found during integration

The initial integrated 800×600 run failed when returning to DOS despite earlier
window tests passing. RAM comparison found the Run window's text inside kernel
instructions. The clock poll had left ES pointing at the BIOS data area (0040)
when a changed minute triggered a scene redraw. The Run field's STOSB copy used
that segment and overwrote kernel memory. This was a time-dependent defect,
independent of pixel depth, not an AC97 failure.

The clock redraw now sets ES to the shell segment before drawing. The Run field
also saves ES and establishes its own destination segment for the string copy.
`scripts/qemu_test_gui_clock.py` starts the RTC at 12:00:35, opens Run, types real
keyboard input, and waits for the displayed minute to change without editing
guest memory or RTC state. The old shell writes 18 unexpected kernel bytes; the
new shell preserves all 38661 checked kernel code bytes at both 800×600×8 and
1280×800×32. DOS ECHO and the subsequent desktop return also pass.

Evidence under `build/full/ui-multiwindow-2026-09-06/`:

- `clock-before/results.json`: deterministic old-shell corruption.
- `clock-after-final/results.json`: repaired 800×600 minute change and DOS return.
- `clock-after-auto-final/results.json`: repaired AUTO32 minute change and DOS return.
- `integrated-fixed-800/result.json`: complete window/input/DOS test with the
  integrated kernel, final MEDIA.COM and clock fix, plus final kernel comparison.
- `integrated-fixed-auto/result.json`: the same complete test at 1280×800×32,
  including unchanged kernel code after DOS and desktop return.

The final shell is `clock-fixed-shell.com`, SHA-256
`e5d54d2e25f67fa917cba0d22f66044eb0faf2cd394f84ca94d9ed5e32aaa700`.
The integrated test also captures only startup AC97 PCM before any input or
application: 221140 PCM bytes, 1.254 seconds at 44.1 kHz stereo, RMS 3573.78,
peak 14646. `startup-only.wav` is a finalized playable WAV. Captured boot frames
match the original SPLASH.BIN photograph and show distinct progress-bar states.
