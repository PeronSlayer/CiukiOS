# CiukiOS 0.9.0 pre-alpha: QEMU captures

Ten 1024x768 captures taken on 2026-10-12 from the integration image
`8435a16a…` (SHA-256 `8435a16a5840987255f6573b446fce53fbedca9181fe0a8797b05379e1e3374d`,
`build/f0/ciukios.img` built from branch `wt/integ2` at commit `96027a71`).
QEMU ran the Pentium III profile under TCG (`-cpu pentium3 -accel tcg -m 512`)
with the AC'97 output silenced and a normal boot (no test request): the
frames are the production desktop, Platinum theme, Tango 0.8.90 icons and
the Inter/Inconsolata UI fonts exactly as the guest rendered them.
Screendumps were taken over QMP and converted to PNG by scripts kept in the
lead's scratch area (not committed). They replace the earlier set taken from
image `d964369f…` on the morning of the same day.

These captures are illustrations, not evidence: the QEMU campaign of this
image was not run (`docs/validation/2026-10-11-integrated-image-f0-regression.md`,
section "2026-10-12 integration images").

| File | Content |
| --- | --- |
| `01-desktop.png` | The desktop: the owner's Ciuk2 wallpaper scene in "fill" position, the Ciuki button at the left end of the top bar and the date at its right, desktop icons (Computer, Documents, DOS Box, CiukiNote, Control Panel, Trash) with soft-halo labels, the taskbar with the tray (volume, clock). The wordmark and the tagline "Classic computing, one woof at a time." are part of the wallpaper scene |
| `02-ciuki-menu.png` | The Ciuki menu open from the top-bar button: Programs, Documents, Settings, Help, Run..., Shut Down... |
| `03-programs.png` | Programs submenu: CiukNote, Files, Task Manager, DOS Box, Ciuki Demo, Lua, Control Panel, Accessories, Games, Developer |
| `04-files-computer.png` | Files in its "Computer" view: drive `C:`, Places (Computer, Programs, Documents, System, Games), the Folders tree and a title bar of the uniform 24 px style |
| `05-ciuknote.png` | CiukNote, the text editor, in front of Files; the taskbar shows both windows |
| `06-task-manager.png` | Task Manager, Applications tab (CiukNote, Files - Computer, Task Manager running), with 5 processes, 1 % CPU and 44M of 511M memory in the status bar |
| `07-control-panel-calculator.png` | Control Panel on the Display page (themes Platinum, Ciuki Blue, Classic 98; wallpaper list; Position buttons Center, Tile, Stretch, Fit and Fill with Fill selected; desktop colours) behind the Calculator accessory |
| `08-games-menu.png` | Programs > Games: "Wolfenstein 3D (shareware)" and "DOOM (shareware)", the two games pre-installed on the games disk |
| `09-wolfenstein-3d.png` | Wolfenstein 3-D 1.4 shareware in its DOS window: the program's own start-up screen (available memory, Mouse and Sound Blaster marked, "Press a key") |
| `10-doom.png` | DOOM 1.9 shareware in its DOS window during start-up: the "DOOM System Startup v1.9" messages with the DPMI zone memory and `doom1.wad` ("shareware version") while the refresh daemon initialises |

The DOS windows are V86 DOS virtual machines running the CIUKIDOS shell with
the kernel's DPMI host; the games come from `C:\GAMES\CIUKIGAM.IMG`
(`third_party/games`, shareware packages with their licence texts; the
Wolfenstein 3-D bundling clause is an open owner decision, see
`docs/build-and-run.md`). Frames 09 and 10 show start-up screens under TCG,
not game play.
