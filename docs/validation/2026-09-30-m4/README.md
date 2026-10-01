# M4 integration evidence — 30 September 2026

All observations here are from QEMU. They do not qualify physical PCs.
QEMU runs outside CiukiOS as the test machine. DOS code inside CiukiOS runs
in x86 virtual 8086 or DPMI execution contexts managed by Jemm386 and
CVSESSION; no QEMU binary runs inside the guest OS.

## Implemented path

`DOSVM.APP` starts each DOS-window program through `VMFORK.COM` and
`DPMIRUN.COM /V`. CVSESSION gives each guest a private DOS arena and session.
The desktop samples that session's video into compositor bands. DOSVM polls
video damage and queues the guest client rectangle, while the shell's module
host schedules a paint without expanding it to the full window. Closing a
window kills only its VM. The former synchronous shell DOS-window manager is
not linked into `SHELL.COM`. The final image contains a 57,568-byte
`SHELL.COM` (60,928-byte image ceiling), a 42,511-byte `CIUKIDOS.SYS`
(43,264-byte ceiling), and a 4,772-byte `DOSVM.APP` (10,928 bytes including
its module memory allocation).

The desktop reads mouse input through its resident INT 33h driver. Its event
loop now handles input before polling continuous guest video damage, so a
busy protected-mode game cannot starve clicks. HDPMI's IRQ bridge routes
keyboard and mouse IRQs through the session monitor.

## Final 0.8.0 result

`build/tests/vm-window-profile-080-final-20260930/SUMMARY.json` records
**26/26 passing gates** on the final 0.8.0 QEMU image. It includes the
rewritten behavioral DOS-window tests: five VGA modes and resize, COM/MZ
text and nested EXEC, INT 33h mouse and SB16/OPL sound, a protected-mode
DPMI probe, Task Manager Give Focus and End VM, two independent windows,
original DOOM and doom-vanille with and without sound effects, plus DOS
memory, desktop and native-media regression gates. The profile also includes
1,079 peripheral-model assertions. The final build log is
`build/tests/m4-080-release-build.log`.
At the time of that profile, `build/full/ciukios-full.img` had SHA-256
`d943917b7728cb2f0bedeb05e57f8157204098cd619e2ec8c5350a071b696ed5`.
The subsequent TestGames, registry and PE status build is recorded in
[`../2026-09-30-post-m4/README.md`](../2026-09-30-post-m4/README.md).

M4's specified QEMU scope is complete. These runs do not qualify physical
hardware or imply every DOS program works.

## Development runs and debugging history

- `build/tests/m4-gate-precise-damage-3-20260930/report.json`: focused M4
  gate passed on a development image. It captured DOOM gameplay with Files
  open, bounded close, Run response after release, two text VMs, and an
  independent close. Its gate did not yet click between the two text VMs.
- `build/tests/vm-window-profile-m4-final-20260930/SUMMARY.json`: 16 of 25
  profile lanes passed. Eight older DOS-window harnesses still expect the
  removed in-shell manager. The HDPMI lifetime lane stopped in its second
  BIOS wait on that build. The IRQ bridge now passes errors through to HDPMI,
  and the same lifetime command passed in isolation with the rebuilt host:
  `build/tests/m4-lifetime-irq-pass-20260930/`. The harnesses were later
  replaced with behavioral gates, and the final profile above passes.
- The strengthened `scripts/qemu_test_m4.py` adds click focus between two
  VMs and measures desktop Run latency from the press. Both original DOOM
  and doom-vanille passed every check on the latest build:
  `build/tests/m4-original-edges-20260930/report.json` (0.74 s Run
  response) and `build/tests/m4-doomvan-edges-20260930/report.json`
  (0.73 s). Each run includes `doom-with-files.png`, `two-text-vms.png`
  and `first-vm-survives.png`.
- `build/tests/m4-guestio-pass-20260930/report.json`: a real DOS program
  in an M4 window received mouse movement, press and release through INT 33h,
  completed SB16 DMA and OPL output, and yielded PCM energy 48,084,453 in
  QEMU's WAV capture. Explicit desktop `DEV_MOUSE` events now reach an
  inherited DOS mouse driver even when its PS/2 stream command was sent
  before the VM fork. The peripheral-model unit gate passed 1,079 assertions,
  including a test of that inherited stream state, in
  `build/tests/m4-unit-peripherals-stream-inherit-20260930/`.
- `build/tests/m4-original-mouse-final-20260930/report.json`: the focused
  original DOOM and two-window M4 gate passed again after that mouse fix.
- `build/tests/m4-doomvan-mouse-final-20260930/report.json`: the same focused
  M4 gate passed with doom-vanille after the mouse fix.
- `build/tests/m4-doomvan-sfx-fire-20260930/report.json`: doom-vanille with
  `-nomusic` produced PCM after the gate fired the player's weapon. This
  isolates Sound Blaster effects from music; the sampled energy was
  9,945,974. A previous measurement was silent because the probe never
  triggered an effect. The same gate now drives input before checking audio.
- The final 0.8.0 image feature gates also passed: CiukPaint BMP
  save/reopen/pixel checks at `build/tests/ciukpaint-080-final-20260930/`,
  long-name create/copy/move/recycle/restore and a clean FAT16 check at
  `build/tests/long-names-080-final-20260930/`, and the virtual NIC,
  no-match and watchdog driver-pack gate at
  `build/tests/driver-pack-080-final-20260930/`.
- The desktop-polish gate at
  `build/tests/desktop-polish-080-final-20260930/` passed desktop/context
  menus, independent Properties, Recycle Bin empty and restore, settings,
  font install, CiukNote, and a driver package that loads on the next boot.
- `scripts/test_ui_icons.py` passed all 35 native icons, their transparency,
  palette, planar map and loader lifetime checks on the same checkout.
- `build/tests/m4-text-bios-20260930/report.json`: COM and MZ BIOS text
  programs both passed DOS file roundtrips, nested EXEC, live file I/O and
  bounded ESC exit in their own M4 windows. The screenshots show the text
  surface; the revised gate also asserts its blue background and white text.
- `build/tests/m4-dpmi-port-20260930/report.json`: the protected-mode
  DPMIPORT probe passed in an M4 window and returned to the desktop.
- `build/tests/m4-vga-precise-size-20260930/report.json`: a deterministic
  VGA probe passed five distinct modes, minimize/restore and resize. After
  returning the window to its exact original dimensions, only 96 pointer
  pixels differed; the vacated area was repainted. Earlier attempts showed
  apparent full-frame differences because QEMU's software-pointer position
  had restored the window two pixels too wide. The gate now checks the exact
  resulting frame, and it passes.
- `build/tests/m4-fullscreen-check-20260930/serial.txt`: with the shipped
  resident manager, F4 enters the full-screen DOS prompt and does not return
  `SHELL.COM` to the loader. The profile's manual image removes
  `VMSTART.COM`, which is not this M4 boot configuration.

The original profile baseline before M4 was 22/25; its three known failures
were `doom-vanille`, `dos-window-text` and `vga-window`. The 16/25 figure
above cannot be compared as a simple regression count because several
legacy harnesses addressed symbols that M4 removed. Their behavior is
covered by the passing 26/26 final profile.
