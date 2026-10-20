# CiukiOS 0.9.2 pre-alpha: QEMU captures

Eleven 1024x768 captures taken on 2026-10-20 from the canonical image
`116ced5d…` (SHA-256 `116ced5d7c549a2fbb77c0e16282aa06e8de23b78623175bc2774d01e290a624`,
`build/f0/ciukios.img` built by `make build-full` from `main` at commit
`cc81cbb6`, clean tree, version 0.9.2 pre-alpha). QEMU ran the Pentium III
profile under TCG (`-cpu pentium3 -accel tcg -m 512`, AC'97 with a silent
audio backend) with a normal boot (no test request): the frames are the
production desktop exactly as the guest rendered it, Ciuki Blue theme,
Tango 0.8.90 icons, the owner's Ciuk2 wallpaper scene in "Fill" position.
Screendumps were taken over QMP and converted to PNG by scripts kept in the
lead's scratch area (not committed).

These captures are illustrations, not evidence: QEMU gates are suspended for
the hardware trial cycle (dev diary 2026-10-19-03). Making them exposed two
regressions of the integrated `main`, both fixed before this set was taken
(dev diary 2026-10-20-02): native applications could not open their windows
and the desktop could not load its wallpaper. A third one is still open: on
this image a DOS game launched from Programs > Games stays at
"Loading DOOM... Preparing..." (the DOS virtual machine is never created), so
this set has no game frames.

| File | Content |
| --- | --- |
| `01-desktop.png` | The desktop: top bar with the Ciuki orb at the left end, tray (network, volume, notifications) and the clock with the date at the right; desktop icons Computer, Documents, Trash and Pictures; the empty window bar at the bottom. The wordmark and the tagline "Classic computing, one woof at a time." are part of the wallpaper scene |
| `02-ciuki-menu.png` | The Ciuki menu (Programs, Documents, Settings, Help, Run..., Shut Down...) with the Programs submenu (CiukNote, Files, Task Manager, DOS Box, Ciuki Demo, Lua, Control Panel, Music Player, Image Viewer, CiukPaint, Terminal, Accessories, Games, Developer) and the Accessories submenu (Calculator, Graphics Demo, Character Map, Clock, CiukiOS Help, Volume Control, Registry Editor, Image Viewer) |
| `03-about.png` | About CiukiOS: the approved Ciuki portrait, "Dedicated to Ciuki", version 0.9.2 pre-alpha, build `cc81cbb66792`, RTC clock, 1024 x 768 in 32-bit colour |
| `04-files.png` | Files in the Computer view: drive `C:`, Places (Computer, Programs, Documents, System, Games, Control Panel, Desktop), the Folders tree |
| `05-display-properties.png` | Control Panel > Appearance > Display: the Background page with the wallpaper list (Ciuki Blue, Twilight, Linen, Blueprint, Ciuk1-3), the live preview, the five positions and the desktop colours |
| `06-ciuknote.png` | CiukNote, the text editor, with two lines typed during the capture |
| `07-task-manager.png` | Task Manager, Applications tab, with the process count, CPU usage and memory in the status bar |
| `08-music-player.png` | Music Player, the MP3 player, with an empty playlist |
| `09-ciukpaint.png` | CiukPaint, the bitmap editor, with its tool box and palette |
| `10-terminal.png` | Terminal: the CiukiOS command shell after `help` (DIR, COPY, MOVE, TYPE, START, NET, AUDIO, TASKS, LUA and the other commands) |
| `11-games-menu.png` | Programs > Games: Wolfenstein 3D, DOOM and Quake, the shareware releases pre-installed on the games disk |
