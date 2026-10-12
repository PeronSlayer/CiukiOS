# Legacy 0.8 feature inventory for 0.9.0

Status: working inventory, 2026-10-12, branch `wt/legacy-recovery` from
`main` at `5ab55e1`. The owner wants the user-visible features of the 0.8
desktop back in 0.9.0. This table lists every user-visible feature of the 0.8
desktop and shell, where its code and assets live on branch `legacy-0.8`, its
state in 0.9.0, and the 0.9.0 branch that owns its recovery.

Sources: `git ls-tree -r legacy-0.8` (mainly `src/apps/*.c`, `src/com/shell*.inc`,
`src/com/ui_*.inc`, `assets/`), `CHANGELOG.md` sections "0.8 line — final
state", "0.8.3" and earlier, `docs/screenshots/0.8.3/`, and the 0.9.0 tree
(`apps/desktop`, `apps/libdeskui`, `apps/files`, `apps/ciuknote`,
`apps/tasks`, `apps/dosbox`, `src/kernel/audio`).

Status values: **present** (works in 0.9.0 with an equivalent scope),
**partial** (some of the 0.8 behaviour exists), **missing** (nothing yet).
Owners: `wt/ui-look` (toolkit look, themes, fonts, decorations, dialogs),
`wt/desktop-shell` (Start menu, taskbar, desktop icons, wallpapers, About,
Control Panel, Run, Alt+Tab), `wt/icons` (icons), `wt/clean-boot` (boot
splash, shutdown/restart), `wt/legacy-recovery` (this branch: applications and
data that nobody else owns). "Later step" marks features whose 0.9.0 home is
a later F-step (network F6, browser, DOS games F3/F5), not tonight's work.

## Desktop shell

| # | 0.8 feature | What it did | legacy-0.8 location | 0.9.0 status | Owner |
|---|---|---|---|---|---|
| 1 | Top bar | "CiukiOS" title with the portrait, which opened the system menu | `src/apps/desktop.c` (top bar), `src/com/shell_gui.inc` | partial: dark bar with "CiukiOS" and tagline; no menu | wt/desktop-shell |
| 2 | Top-bar meters | Five clickable readings: VOL, NET, CPU, RAM, DISK (idle/busy) | `src/apps/desktop.c` lines ~370-400 | missing | wt/desktop-shell |
| 3 | Volume pop-up | Slider with "Volume", "Muted", "No device" from the VOL meter | `src/apps/desktop.c` (`VOL_TRACK_X`) | missing on main; Volume Control window done on wt/legacy-recovery (`/Programs/mixer`, DESK_MIXER_SET) | wt/desktop-shell (tray icon, launches `mixer`) |
| 4 | Bottom taskbar | Task buttons for open windows; date and clock box | `src/com/shell_gui_windows.inc`, `src/apps/desktop.c` | partial: taskbar, "Ciuki" button, task buttons, HH:MM:SS clock; no date | wt/desktop-shell |
| 5 | System/Start menu | Programs, Files, Run, DOS Prompt, Control Panel, Display, Sound, Network, Image Viewer, Music Player, Task Manager, About, Shut Down | `src/apps/desktop.c` menus `m_system*` | partial: "Ciuki" menu lists every /Programs ELF, About, "Restart (unavailable)" | wt/desktop-shell |
| 6 | Application Library ("Programs") | Window listing installed programs by category | `src/apps/desktop.c` (`DI_PROGRAMS`), `src/com/shell_apps.inc` | missing | wt/desktop-shell |
| 7 | Desktop icons | Computer, Programs, Recycle Bin, Control Panel, DOS Prompt, Floppy, USB drive, CD-ROM; files in `C:\DESKTOP` | `src/apps/desktop.c` `desk_items[]` | missing | wt/desktop-shell |
| 8 | Desktop context menus | Arrange by name/type, Line Up, Refresh, Paste, New Folder, New Text Document, Properties; per-icon Open/Explore/Remove/Properties | `src/apps/desktop.c` lines ~1120-1160 | missing | wt/desktop-shell |
| 9 | Icon properties | Type, location, size, attributes (Read-only/Hidden/Archive) | `src/apps/desktop.c` `pdl[]` | missing (Files has Properties) | wt/desktop-shell |
| 10 | Window menu | Restore, Minimize, Maximize, Close (Alt+F4) | `src/apps/desktop.c` `m_window` | partial: compositor close/minimize; no menu | wt/ui-look (decorations) |
| 11 | Window decorations | Platinum title bars, pinstripe rails, min/max/close boxes, title fitting | `src/com/shell_gui_draw.inc`, `shell_gui_detail.inc` | partial: flat compositor title bars | wt/ui-look |
| 12 | Window resize | Drag edges to resize | `src/com/shell_gui_resize.inc` | missing | wt/ui-look |
| 13 | Keyboard shortcuts | Win+E Files, Win+R Run, Win+D show desktop, Win+F1 About, Ctrl+Shift+Esc Task Manager, F3 Run, Ctrl+Esc Programs, F4 full-screen DOS | `src/apps/desktop.c` menu accelerators, `src/com/shell_gui_input.inc` | missing (test keys only) | wt/desktop-shell |
| 14 | Alt+Tab | Switch windows | `src/com/shell_gui_input.inc` | missing | wt/desktop-shell |
| 15 | Run dialog | Opens programs by name; aliases NOTEPAD, MSPAINT, EXPLORER, TASKMGR, CONTROL, DEVMGMT | `src/apps/run.c` | partial: Task Manager "New Task" | wt/desktop-shell |
| 16 | About CiukiOS | Welcome header (version, tagline), About and Credits tabs, dedication "In loving memory of Ciuk (Ciuki)...", open-source credits, GPLv2, Alcybercloud.it, shortcuts line, "Don't show this at startup" (saved), Application Library button | `src/apps/about.c`, `docs/screenshots/0.8.3/about.png` | partial: internal About window with dedication, kernel build, clock source, Tango credit | wt/desktop-shell |
| 17 | About at startup | Opens at boot unless suppressed (WELCOME.CFG) | `src/apps/about.c` | missing | wt/desktop-shell |
| 18 | Wallpapers | Three Ciuki photos (adaptive), three Tango tiles, solid colour; position; cached native pixels | `misc/ciukios_bg/`, `assets/wallpapers/tiles/`, `src/apps/wallp.c`, `src/com/shell_wallpaper.inc` | partial: centred portrait on solid colour | wt/desktop-shell |
| 19 | Mouse pointer schemes | Tango (Default), Classic 95, 3D Contrast; arrow/text/busy shapes | `assets/cursors/CURSORS.DAT`, `src/apps/control.c` Mouse | partial: one drawn arrow | wt/ui-look |
| 20 | System sounds | Startup sound, Info/Warning/Error/Confirm event sounds (Kenney Interface, CC0) | `assets/sounds/`, `src/com/shell_system_sounds.inc`, `src/com/ui_sound_*.inc` | missing (native audio exists, F4-01) | unowned; proposed wt/desktop-shell (playback in the shell) |
| 21 | Boot splash | Original Ciuki photograph during boot | `misc/CiukiOS_SplashScreen.png`, `src/com/boot_session.inc` | partial (VESA splash in loader) | wt/clean-boot |
| 22 | Shut Down / Restart | Shut Down dialog, power queue (`SHUTDOWN/REBOOT <s>`) | `src/apps/desktop.c` `M_SHUTDOWN`, `src/com/shell.asm` | missing ("Restart (unavailable)") | wt/clean-boot |
| 23 | Recycle Bin | Deleted items in `C:\RECYCLED`, Restore, Delete, Empty, Properties | `src/apps/recycle.c`, `src/apps/sys.c` | missing (Files deletes permanently) | wt/desktop-shell (desktop icon); the bin model is unowned |
| 24 | Recent documents | none in 0.8 (requested for 0.9.0) | - | missing on main; list done on wt/legacy-recovery (`apps/libdeskui/recent.h`, `$HOME/recent.lst`; Image Viewer, Help and CiukNote record) | Start menu Documents entry: wt/desktop-shell |

## Look and settings (Control Panel)

| # | 0.8 feature | What it did | legacy-0.8 location | 0.9.0 status | Owner |
|---|---|---|---|---|---|
| 25 | Control Panel window | Applets: Display, Wallpaper, Appearance, Fonts, Sound, Mouse, Keyboard, Date and Time, System, Device Manager, Drivers, Task Manager, Settings Registry, Network; settings in `\SYSTEM\UI\DESKTOP.CFG` | `src/apps/control.c` | missing | wt/desktop-shell |
| 26 | Appearance / colour schemes | CiukiOS Classic, Ocean, Rose, Slate, Forest, Desert, Lilac, High Contrast; desktop colour; which desktop icons show | `src/apps/display.c` (`scheme names`), `src/com/ui_theme.inc` | missing (fixed DU_* colours) | wt/ui-look (schemes); icon visibility: wt/desktop-shell |
| 27 | Fonts | 11 CFN fonts (Atkinson, CiukiOS, DejaVu, Fira Sans, Inconsolata, Inter, Noto Sans/Serif, Open Sans, Roboto, Work Sans), system font choice, install new font, preview | `assets/fonts/`, `src/apps/control.c` Fonts | missing (one 5x7 glyph table) | wt/ui-look |
| 28 | Display Properties | Adapter/driver/monitor details, modes, preview, confirm, automatic rollback | `src/apps/display.c`, `display_probe.c` | missing | wt/desktop-shell (Control Panel); mode switching needs a kernel interface |
| 29 | Sound applet | Startup sound on/off, event sounds, master volume, mute | `src/apps/control.c` Sound | missing | wt/desktop-shell (applet); master volume window: wt/legacy-recovery |
| 30 | Mouse applet | Pointer speed, double-click speed with test folder, scheme | `src/apps/control.c` Mouse | missing | wt/desktop-shell |
| 31 | Keyboard applet | Repeat delay and rate | `src/apps/control.c` Keyboard | missing | wt/desktop-shell |
| 32 | Keyboard layouts | none in 0.8 (US only); requested IT and US | - | missing on main; US and Italian tables done on wt/legacy-recovery (`apps/libdeskui/keymap.h`) | setting and wiring: wt/desktop-shell; non-ASCII glyphs: wt/ui-look |
| 33 | Date and Time applet | Calendar with month names and weekday grid, time fields, automatic detection, time zone lookup | `src/apps/control.c` Date and Time | missing on main; calendar view done on wt/legacy-recovery (`/Programs/clock`); setting the clock is not possible (no RTC write call) | applet: wt/desktop-shell |
| 34 | System applet | CiukiOS identity, computer, memory, VM manager state | `src/apps/control.c` System | partial (About shows kernel build) | wt/desktop-shell |
| 35 | Device Manager | Devices by class with properties | `src/apps/devices.c` | missing | unowned (needs a kernel device-list call) |
| 36 | Drivers | Installed DOS drivers, enable/disable/remove, Add New Driver (DRIVER.INF) | `src/apps/devices.c`, `assets/drivers/` | missing (0.9 drivers are kernel-native) | retired for the native desktop; DOS drivers return with F3 |
| 37 | Settings Registry | Key/value editor (SZ, DWORD, BINARY) | `src/apps/control.c`, `src/apps/regstore.c` | missing | unowned |
| 38 | Network panel | IPv4 static/DHCP, host name, MTU, adapter details | `src/apps/control.c` Network | missing | later step (F6 network) |

## Applications

| # | 0.8 feature | What it did | legacy-0.8 location | 0.9.0 status | Owner |
|---|---|---|---|---|---|
| 39 | Files | Explorer: places tree, details/icons, cut/copy/paste, rename, delete, properties, open with | `src/apps/files.c`, `src/apps/sys.c` | present (f2-31 `apps/files`) | - |
| 40 | Files opens images/music | Images open in the viewer, audio in the player | `src/apps/desktop.c` `ext_is(...)` | partial on main (text only); pictures open in the Image Viewer on wt/legacy-recovery; audio has no player | wt/legacy-recovery |
| 41 | CiukNote | Notepad with find/replace, go to, word wrap, fonts, time/date | `src/apps/ciuknote.c` | present (f2-32 `apps/ciuknote`) | - |
| 42 | Task Manager | Applications, Processes, Virtual Machines, Performance | `src/apps/tasks.c` | partial: Applications, Processes, Performance (f2-33); no VM tab | VM tab unowned (needs a VM list call) |
| 43 | Image Viewer | BMP/PNG/JPEG/GIF, zoom (wheel, +/-), pan, fit (F), previous/next image in the folder | `src/apps/viewer.c`, `src/apps/webimg.c`, `third_party/tjpgd` | missing on main; done on wt/legacy-recovery (`/Programs/viewer`, stb_image v2.30: PNG/JPEG/BMP/GIF and raw XRGB) | wt/legacy-recovery |
| 44 | Music Player | WAV/MP3/OGG/FLAC through MEDIAWORK, Prev/Play/Pause/Stop/Next, elapsed time, seek | `src/apps/player.c`, `src/media/`, `third_party/audio` | missing (native `/dev/audio` exists) | not done tonight; F4 media follow-up (dr_libs and stb_vorbis are in `third_party/audio`) |
| 45 | Calculator | none in 0.8 (requested as Windows 98 class accessory) | - | missing on main; done on wt/legacy-recovery (`/Programs/calc`) | wt/legacy-recovery |
| 46 | Clock / Calendar | Desktop clock and the Date and Time calendar | `src/apps/desktop.c`, `src/apps/control.c` | partial on main (taskbar clock); analog/digital clock with month calendar on wt/legacy-recovery (`/Programs/clock`) | wt/legacy-recovery |
| 47 | Help / README | Command guide (HELP), F1 shortcut lists in apps | `src/com/shell_help_data.inc`, app Help menus | partial on main (Files/CiukNote Help boxes); CiukiOS Help with 10 topics and a read-only text viewer on wt/legacy-recovery (`/Programs/help`) | wt/legacy-recovery |
| 48 | Character Map | none in 0.8 | - | missing on main; done on wt/legacy-recovery (`/Programs/charmap`, printable ASCII of the system font) | wt/legacy-recovery |
| 49 | CiukPaint | Paint program, live strokes | `src/apps/paint.c` | missing | unowned (large; later) |
| 50 | CiukWeb browser | HTML, forms, PNG/GIF/JPEG, CSS worker, BearSSL TLS 1.2, mQuickJS | `src/apps/browser.c`, `src/apps/web*.c`, `src/web/` | missing | later step (F6 network) |
| 51 | DOS Prompt window | DOS in a desktop window (forked VM) | `src/apps/dosvm.c`, `src/com/dos_window_*.inc` | present: `/Programs/dosbox` (f3-02) | - |
| 52 | DOOM / Doom-vanille / Wolf3D in a window | Launchers and window ports | `src/com/doom*_launch.asm`, `src/ports/` | partial: DOS workloads (f3-06) run in the VM; no launchers | later step (F3/F5 games) |
| 53 | Full-screen DOS | F4 switches the DOS session to full screen | `src/com/shell_vm_window.inc` | missing | later step (F3) |
| 54 | Lua / native demo | none in 0.8 | - | present: `/Programs/lua`, `/Programs/demo` | - |

## Shell commands (COMMAND.COM)

| # | 0.8 feature | What it did | legacy-0.8 location | 0.9.0 status | Owner |
|---|---|---|---|---|---|
| 55 | System commands | HELP, VER, ECHO, CLS/CLEAR, REBOOT, SHUTDOWN (power queue) | `src/com/shell.asm`, `shell_help_data.inc` | missing natively; CiukiDOS in the VM has its own shell | VER/HELP text: Help viewer (wt/legacy-recovery); power: wt/clean-boot |
| 56 | Navigation and files | CD, DIR, PWD, PATH, WHERE, TYPE, COPY, DEL, REN/MOVE, MD, RD | `src/com/shell.asm` | present in the DOS VM shell; natively through Files | - |
| 57 | EDIT | Full-screen editor with F1 shortcuts | `src/com/ciukedit.asm` | replaced by CiukNote | - |
| 58 | RUN / program launch | `<name> [args]` | `src/com/shell.asm`, `guiexec.asm` | partial: Start menu, Task Manager New Task | wt/desktop-shell (Run) |
| 59 | Network commands | NETSTART, IPCONFIG, NETCFG, PING, FTP/FTPSRV, PKTCHK | `src/com/netcfg.asm`, `ipconfig.asm`, `icmpd.asm` | missing | later step (F6) |
| 60 | Setup / installer | Graphical installer and disk setup | `src/com/setup.asm` | missing | unowned (needs a 0.9 install design) |

## Summary

60 rows. State on `main` (5ab55e1): 6 present (including EDIT replaced by
CiukNote), 17 partial, 37 missing.
With `wt/legacy-recovery` merged, 10 rows gain an implementation (Volume
Control window, recent documents, keyboard layout data, calendar, Files
opening pictures, Image Viewer, Calculator, Clock, Help, Character Map).

The most visible gaps and their owners:

- Look: colour schemes (8 in 0.8), fonts (11 CFN fonts), decorations,
  window resize, pointer schemes - `wt/ui-look`.
- Shell: About with the dedication, Credits tab and startup option; Control
  Panel and its applets; desktop icons; wallpapers; top-bar meters; Run;
  Alt+Tab and the 0.8 shortcuts; tray volume icon - `wt/desktop-shell`.
- Boot splash and Shut Down/Restart - `wt/clean-boot`.
- Icons for the new accessories (Tango names in `apps/desktop/catalog.h`) -
  `wt/icons`.
- Recovered on `wt/legacy-recovery`: Image Viewer, Calculator, Clock and
  Calendar, CiukiOS Help, Volume Control window, Character Map, recent
  documents list, US/Italian layout tables, Files opening pictures.
- Still unowned: Music Player, system sounds playback, Recycle Bin model,
  Device Manager, Settings Registry, Task Manager VM tab, CiukPaint, Setup.
  Later F-steps: network panel and commands, CiukWeb, game launchers,
  full-screen DOS.

## Interfaces added on wt/legacy-recovery

- `apps/desktop/catalog.h`: launcher catalogue of the accessories (name,
  label, Tango icon name, file argument). `desk_program_graphical`,
  `desk_program_label` and `desk_catalog_takes_file` use it; the icons
  branch maps the Tango names (`accessories-calculator`,
  `accessories-character-map`, `office-calendar`, `help-browser`,
  `audio-volume-high`, `image-x-generic`) through `du_icon(name, size)`.
- Protocol v2 `DESK_MIXER_SET` (client to desktop: serial, left, right,
  mute; answer `DESK_TASK_RESULT`). Only `/Programs/mixer` launched by the desktop
  may send it; the desktop applies it with the `CIUKI_MIXER_FD` control handle
  the supervisor delegates (F4 contract, "Volume and persistence"). The
  handle is not inheritable, so the mixer window cannot hold it itself. A
  tray volume control in the shell can call the same `mixer_apply` path.
- `struct du_ui.paint`: optional application drawing after the widgets,
  under menus and dialogs (image, clock face, glyph grid).
- `apps/libdeskui/recent.h`: recent documents in `$HOME/recent.lst`, one
  absolute path per line, most recent first, at most 15.
- `apps/libdeskui/keymap.h`: US and Italian layout tables
  (`du_keymap(layout, scancode, SHIFT|ALTGR|CAPS)` to a code point,
  `du_utf8_encode`). The compositor glyph table is ASCII only, so accented
  letters need glyphs from `wt/ui-look` before the Italian layout is useful
  in text fields.
- Files opens pictures in `/Programs/viewer` through `DESK_LAUNCH` with the path
  as its argument, as it already does for CiukNote; the desktop passes the
  argument to every catalogue program with `takes_file` set.

Sources for the behaviour recovered: legacy-0.8 `src/apps/viewer.c`,
`control.c` (Date and Time calendar, Sound), `desktop.c` (menus, file
types), `about.c` (dedication text); Microsoft keyboard layouts KBDUS and
KBDIT142 (<https://kbdlayout.info/kbdus>, <https://kbdlayout.info/kbdit142>,
checked 2026-10-12); the Windows 98 standard calculator conventions (immediate
execution, trailing decimal point, Ctrl+L/R/M/P memory keys) from the
accessory's documented behaviour, not re-verified online tonight; stb_image
v2.30 (<https://github.com/nothings/stb>).
