# CiukiOS 0.9.0 pre-alpha: QEMU captures

Captured on 2026-10-12 from the integration image `d964369f…` (branch
`wt/integ2`, commit 98ff5ca1) with `build/lead-scratch/shots.sh`: QEMU
11.1 `pc-i440fx-9.2`, `-cpu pentium3 -accel tcg -m 512 -vga std`, silent
AC'97 backend, no test request (a normal boot), screendumps over QMP
converted to PNG. The desktop was driven with the PS/2 mouse through QMP
(Start menu → Programs → program), so the frames show the production
desktop, Platinum theme, Tango 0.8.90 icons and the Inter/Inconsolata UI
fonts exactly as the guest rendered them. The DOS window runs the CIUKIDOS
shell inside a V86 DOS VM.

| File | Content |
| --- | --- |
| `splash.png` | kernel-drawn boot splash (the original Ciuki photograph) |
| `desktop.png` | desktop with icons, top bar and taskbar |
| `start-menu.png` | Start menu with the Programs submenu |
| `files.png`, `ciuknote.png`, `task-manager.png` | the three F2 applications |
| `control-panel.png`, `calculator.png` | Control Panel and an accessory |
| `dos-window.png` | the DOS window at the CIUKIDOS prompt over the open applications |
