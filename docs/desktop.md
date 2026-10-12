# F2 ring-3 desktop and client protocol

## System icons: the Tango store (2026-10-12)

The owner asked for a complete copyright-free icon set as the default and only
style. Tango Icon Theme 0.8.90 (Public Domain, `assets/icons/`) is that set:
the desktop, libdeskui and its applications now draw every icon from one file,
`/system/icons/tango.cis`, instead of compiled-in sprites. Format, conversion,
size and pinned hashes: [`assets/icons/README.md`](../assets/icons/README.md).

- **Pipeline.** `apps/desktop/convert_icons.py` (standard library only) builds
  the store from the pinned archive: all 645 PNG members (16, 22, 32 px; 48 px
  derived by an integer area filter), 246 names, 726,504 bytes. `make desktop`
  writes `build/apps/desktop/tango.cis` and refuses a SHA-256 other than the one
  in `apps/desktop/tango-store.json` (which also records the archive and every
  member hash); the desktop manifest records the store under `icons`, and
  `scripts/build_image.py` installs it in `/system/icons/` with `TANGO.TXT`
  (upstream COPYING), `AUTHORS.TXT` and `CREDITS.TXT`.
- **API** (`apps/libdeskui/deskui.h`, `icons.c`):
  `const struct du_icon *du_icon(const char *tango_name, unsigned size)`
  returns `{size, argb}` (straight-alpha AARRGGBB) for a Tango name
  (`"folder"` or `"places/folder"`) at 16, 22, 32 or 48 px, or NULL.
  `du_icon_argb()` returns the pixel pointer only, `du_icon_draw(canvas, x, y,
  icon, mode)` alpha-blends it with the existing normal/selected/ghost/disabled
  modes, `du_icon_for(object, size)` looks up a CiukiOS object.
  `du_icon_paths()` selects other files before the first lookup (host tests use
  `-DDU_ICON_STORE_PATH`/`-DDU_ICON_PORTRAIT_PATH`), `du_icon_count()`,
  `du_icon_store_name()` and `du_icon_notice()` list the store and its licence
  notice. The reader keeps only the 24 KB index, decodes an icon on its first
  use (QOI chunk stream or area filter, FNV-1a checked) and caches it for the
  process lifetime; it is integer-only (the build's opcode audit covers it) and
  not thread-safe. A missing store gives NULL icons, never a failure.
  The former sprite blitter `du_icon(canvas, x, y, argb, size, mode)` is renamed
  `du_icon_blit` (same behaviour); branches that still call the old name fail to
  compile and need that one-word change.
- **Identity.** `DU_ICON_CIUKI` (`"ciuki"`) is the approved portrait conversion
  (`/system/assets/ciuki-portrait.xrgb`, pinned by `convert_portrait.py`)
  reduced with the same area filter (identical to Files' box filter at 16 and
  32 px). Computer, About, the Start (Ciuki) button and `device:computer` map
  to it; the compositor keeps drawing its own nearest-neighbour portrait for the
  Ciuki button and About, Files and CiukNote keep their portrait callbacks.
- **Map** (`apps/libdeskui/icon_map.c`, 353 entries, keys `<kind>:<object>`;
  Tango names follow the [Icon Naming Specification](https://specifications.freedesktop.org/icon-naming-spec/latest/)
  as of Tango 0.8.90, and only names present in the archive are used):
  programs (`files` system-file-manager, `ciuknote` accessories-text-editor,
  `tasks` utilities-system-monitor, `dosbox` utilities-terminal, `demo`
  preferences-desktop-peripherals, `hello` face-smile, `lua`
  applications-development, `libc_smoke` applications-system, `desktop`
  user-desktop, others application-x-executable); Start menu entries (settings,
  control panel, run, find, help, about, shut down, restart, log out, lock and
  categories); toolbar and menu actions (navigation, edit, document, view,
  formatting, media); places (computer, home, desktop, drives, removable,
  floppy, optical, network, trash); file types by extension (folders, text,
  configuration, HTML, scripts and sources, DOS `.COM`/`.EXE`/`.BAT`, images,
  audio, video, archives, fonts, disk images, office documents, certificates,
  unknown); status (volume levels, battery, network, updates, busy); message
  box icons (Tango 0.8.90 has no dialog-question; help-browser is used, as
  before); devices; settings items; emblems; window-manager buttons for a
  toolkit that wants them. `du_icon_name()` falls back to a kind's `*` entry;
  `du_icon_file(name, directory, executable)` picks folder, then a native
  executable's `/bin` program icon, then the extension, then unknown.
- **Users.** Files (toolbar, address bar, places, tree, list/icon views,
  properties), the open/save dialog (file-type icons), message boxes, Task
  Manager (Applications rows show the owning process's program icon), and the
  desktop's Start menu and taskbar buttons (through the `struct desktop` hook
  `icon`, set in `desktop.c`; host harnesses that leave it NULL draw no menu
  icons). `apps/desktop/shell_icons.h` is no longer included anywhere.
- **Tests.** `tests/host/desktop/test_icons.py` pins the store and decoded
  pixels, cross-checks the PNG decoder with Pillow, round-trips the Python and
  C readers against the archive, resolves every map entry at four sizes,
  rejects corrupt stores, checks the licence notice and renders the 32 px
  contact sheet `build/host/icons/contact-sheet-32.png`. Re-pinned host frames
  (Files, open dialog, Task Manager) were reviewed as images on 2026-10-12.
## Desktop shell (wt/desktop-shell, 2026-10-12)

The shell turns the f2-30 launcher into a Mac OS 9 Platinum x Windows 98/ME
style desktop. It lives in `apps/desktop/shell.c` (Start menu, taskbar
buttons and tray, desktop icons, built-in windows, shortcuts),
`settings.c` (desktop.cfg), `wallpaper.c` + `image.c` (wallpapers),
`keymap.c` (keyboard layouts) and the OS side in `desktop.c`
(`shell_service`: settings file, wallpaper files, mixer, power, Run). The
shell draws with the libdeskui primitives (`du_fill`, `du_edge`, `du_text`,
`du_icon`) on the compositor's frame, so the toolkit's colours and metrics
apply; `desk_theme_apply()` (weak, in shell.c) is the single place to call the
toolkit's `du_theme_*` once it exists. shell.c does no file or kernel I/O.
The non-shell compositor path (crash-isolation gate, `plain` digest) is
unchanged; compositor.c references the shell through weak symbols so the demo
and the libdeskui programs, which reuse compositor.c, link without it.

- **Start menu** (the Ciuki button at the left end of the top bar: the
  approved portrait and "CiukiOS" on a rounded gradient face, pressed in while
  the menu is open; the menu drops below the bar, submenus open to the
  right): Programs (CiukNote,
  Files, Task Manager, DOS Box, Ciuki Demo, Lua, Control Panel, then the
  Accessories and Games submenus, then a Developer submenu with Hello,
  libc_smoke and any unknown `/bin` program),
  Documents (recent files, My Documents, Clear Recent Documents), Settings
  (Control Panel and its pages, Task Manager), Help (Keyboard Shortcuts, About
  CiukiOS), Run..., Shut Down.... Submenus open on hover or click; the
  keyboard works (arrows, Enter, Esc). A vertical banner shows the portrait
  and "CiukiOS". Menus are 30 px (top) and 22 px rows.
- **Taskbar:** task buttons (icon, title, pressed state for the active
  window; right click: Restore, Minimize, Close) from x 4, at most 160 px
  each, and a tray with a volume icon and the clock (24/12 h, optional
  seconds; the date as a tooltip after 0.6 s). The top bar keeps the identity
  colours and shows the Ciuki button, the focused window's title (or a
  notice) and the date.
- **Volume:** the supervisor delegates the writable `/dev/mixer` handle to the
  desktop (`CIUKI_MIXER_FD`, F4 contract). The tray popup (slider, Mute)
  applies `MIXER_SET_MASTER` through it; without the handle the popup shows
  the public read-only state, without `/dev/mixer` "No audio device".
- **Desktop icons:** Computer (Files at `/`; the icon carries the Ciuki
  portrait), Documents (Files at `/home`), DOS Box, CiukNote, Control Panel and
  Trash (a placeholder: nothing in CiukiOS keeps deleted files yet). Click
  selects, a double click (desktop.cfg `double_click_ms`) or Enter opens, right
  click opens Open/Properties; the background menu has Arrange Icons, Refresh,
  Display Settings... and Properties.
- **Built-in windows** (internal clients, framed by the compositor): About
  CiukiOS (tabs General, System from call 70's system record and `uname`,
  Credits), Run (a `/bin` program and up to six arguments, double quotes
  group), Shut Down CiukiOS (Restart, Shut Down, Cancel) and message boxes.
- **Shortcuts:** Win (tap) or Ctrl+Esc opens the Start menu; Alt+Tab and
  Shift+Alt+Tab switch windows with an overlay; Alt+F4 sends the v2
  CLOSE_REQUEST to the active window (the Shut Down window when none is
  active). Alt is held back until the next key, so a program never sees an Alt
  press for a chord the desktop consumed; a lone Alt tap still reaches it.
- **Power:** Shut Down calls `ciuki_power(action)` (SDK `ciuki/power.h`,
  kernel call 73; 1 reboot, 2 power off) after saving desktop.cfg. The SDK
  definition is weak; with ENOSYS the desktop says that the kernel has no
  power interface yet.
- **Runtime gate scenarios** (F12/F11/F10/F9) keep the built-in defaults, never
  read or write desktop.cfg and never touch the mixer; the f2-desktop scripts
  reach the programs through Start, Programs and the program row.

### /system/settings/desktop.cfg

Plain `key=value` lines, `#` comments; unknown keys are kept verbatim (for
example a future theme engine's own keys). Invalid values keep their defaults
and are counted in a top-bar notice. The desktop writes the file through
`desktop.new` + rename; the Control Panel merges its keys into the current
file before writing it and then asks the desktop to apply it.

| Key | Values | Default |
| --- | --- | --- |
| `version` | 1 | 1 |
| `theme` | `Platinum`, `Ciuki Blue`, `Classic 98` | `Platinum` |
| `wallpaper` | `ciuki` (the approved portrait on the desktop colour, exactly where the f2 compositor draws it), `none`, `builtin:<id>` (`dawn`, `pinstripe`, `deepblue`, `twilight`, `linen`, `blueprint`), or an absolute path to a PNG, JPEG or BMP | `ciuki` |
| `wallpaper_mode` | `center`, `stretch`, `tile`, `fit` | `center` |
| `desktop_color` | six hex digits RRGGBB | `375564` |
| `clock_24h` | 0/1 | 1 |
| `clock_seconds` | 0/1 | 0 |
| `desktop_icons` | 0/1 | 1 |
| `mouse_speed` | 1..10 (5 = one pixel per count; 2/8 .. 20/8) | 5 |
| `double_click_ms` | 200..900 | 500 |
| `keyboard` | `us`, `it` | `us` |
| `recent` | absolute path; repeated, newest first, at most 8 | none |

### Wallpapers

The built-in wallpapers are original CiukiOS designs computed at the screen's
resolution by integer code (soft platinum/blue gradients with ordered dither,
pinstripes, a linen weave, a blueprint grid); no image file is shipped for
them and nothing third-party is drawn. Image files are decoded with the
vendored stb_image 2.30 (`third_party/stb`, MIT or Public Domain; notice
installed as `/system/licenses/stb_image.txt`): PNG, baseline and progressive
JPEG, and uncompressed BMP (4/8/16/24/32 bit; stb_image reads neither 1-bit nor RLE BMP). Images above 4096 x 4096,
16 Mpixel or 24 MiB are refused. `centre`, `stretch` (area average down,
bilinear up) and `fit` use the area between the bars; `tile` starts at the
top-left corner. The image contains an empty `/system/wallpapers`: the
Control Panel lists the PNG/JPEG/BMP files the owner copies there.
**The Windows ME bitmaps of the 0.8 line (`third_party/win_bg`, local and
git-ignored, Microsoft copyright) are never added to the repository or to the
image;** the desktop only shows them if the owner copies them into
`/system/wallpapers` himself (they are 8/4-bit tiles; choose Tile).

### Control Panel (`/bin/control`)

A libdeskui program (500 x 420) with tabs Display (theme list with preview,
wallpaper list with preview, Center/Tile/Stretch/Fit, six desktop colours,
desktop icons on/off), Mouse (pointer speed and double-click speed, applied
by the desktop), Keyboard (US or Italian), Sounds (master volume and mute
through the desktop's mixer handle), Date & Time (the clock and the 24 h and
seconds options; setting the clock is not offered because the ABI has no
clock-setting call) and System (portrait, version, build, memory). Every
change is saved and applied at once. `--panel=display|mouse|keyboard|sounds|
datetime|system` selects the first tab (the Settings menu uses it).

v2 opcode for it, client to desktop, version 2 only:

| Opcode | Payload bytes | Fields |
| --- | ---: | --- |
| SETTINGS=21 | 24 | u32 serial@4, request@8 (1 reload desktop.cfg: the other fields zero; 2 master volume), left@12, right@16 (0..100), mute@20 (0/1) |

The desktop answers TASK_RESULT (request 21): 0, EPERM for any client it did
not launch as `/bin/control`, ENODEV for request 2 without the mixer handle.

### Keyboard layouts

`keymap.c` maps set-1 scan codes for US and Italian (the KBDIT engraving:
`ò@ à# è[ +]`, AltGr+E = euro). The desktop's own text field (Run) uses the
selected layout at once, and every program the desktop launches receives
`CIUKI_KEYMAP=us|it`. libdeskui's `du_key_char` still uses its US table:
wiring it to `ciuki_keymap_char` (and UTF-8 insertion of non-ASCII
characters) is left to the toolkit owner.

### Icons

Each item names its Tango 0.8.90 icon; `desk_shell_icon(name, size)` maps the
names to the sprites converted today (area-averaged to 16/20/22 px when only a
larger one exists) until `du_icon(name, size)` lands. Identity icons (About,
the Computer badge, the Start banner) are area-averaged conversions of the
approved portrait, whose `#375564` surround is made transparent.

## f2-33 Task Manager and the v2 task extension (2026-10-11)

`/bin/tasks` is the CiukiOS Task Manager (menu label "Task Manager", a
400x420 window): tabs Applications, Processes and
Performance (the Virtual Machines tab arrives with F3, f3-02), menus File
(New Task (Run...), Exit Task Manager), View (Refresh Now F5; Update Speed
High 0.5 s, Normal 1 s, Low 4 s, Paused) and Help, and a status bar
(processes, CPU usage, memory). Kernel data comes from call 70 and the
termination request from call 71 (docs/design/execution-abi.md, f2-33
amendment); window data comes from the desktop through the extension below.
Research and decisions are in docs/design/f2-acceptance.md, section F2-33.

- **Applications:** columns Task and Status (Running / Not Responding from
  the desktop's heartbeat), sorted by title, PID and window id. End Task
  (Alt+E or Delete) sends END_TASK, the desktop's close path: CLOSE_REQUEST, SIGTERM
  after 2 s for its own children. Switch To (Alt+S, Enter, double-click) restores and
  focuses. New Task (Alt+N) asks the desktop to launch an admitted `/bin`
  program (name or `/bin/name`), so graphical programs get their channel and
  appear here; the launched program's row is selected once it appears.
- **Processes:** Image Name, PID, PPID, Threads, Mem Usage (resident pages x
  4 KiB), CPU (percent of the last interval), sortable by header click
  (again to reverse), plus "System Idle Process" (PID 0). End Process
  (Alt+E or Delete, with confirmation) refuses PID 0/1 and the desktop, routes the
  desktop's group through END_TASK by PID and everything else through
  call 71 with SIGKILL and the delegated authority (EPERM shows "Access is
  denied").
- **Performance:** CPU usage meter, a 60-second history graph (one point per
  second), Physical Memory (K) total/available/used, Kernel Memory (K) heap
  in use/peak/pool, Totals (processes, threads, up time).

The v2 task extension follows f2-30's rules: version 2 only, a v1 client or
a wrong direction is a protocol error (that peer is disconnected), payload
tails are zero and integers little-endian. Requests from a client that the
desktop did not launch as `/bin/tasks` receive TASK_RESULT with EPERM; no
other client gains any right.

| Opcode | Direction | Payload bytes | Fields |
| --- | --- | ---: | --- |
| WINDOWS_QUERY=15 | Client → desktop | 8 | u32 serial@4 |
| WINDOW_INFO=16 | Desktop → client | 32+n | u32 serial@4, index@8, count@12 (0..16), window_id@16, pid@20, flags@24 (1 unresponsive, 2 minimized, 4 focused, 8 internal, 16 closing), n@28; title@32, 0..63 printable bytes. count=0 is one message with every field zero. |
| END_TASK=17 | Client → desktop | 16 | u32 serial@4, window_id@8, pid@12; not both zero. window_id selects a window (pid, when nonzero, must match); window_id=0 ends a desktop child by PID: its windows get close requests, a windowless child gets SIGTERM. |
| SWITCH_TO=18 | Client → desktop | 12 | u32 serial@4, window_id@8 (nonzero) |
| LAUNCH=19 | Client → desktop | 12+n, or 16+n+a with an argument (f2-32) | u32 serial@4, n@8 (1..63); name@12, visible ASCII without spaces, matched against the `/bin` menu list. Optional argument after the name: u32 a@12+n (1..160), then a bytes (32..255 except 127), one `argv[1]` for the program; a message of any other length is a protocol error. |
| TASK_RESULT=20 | Desktop → client | 16 | u32 serial@4, request@8 (15, 17, 18 or 19), i32 result@12: 0, the launched PID, or -errno (EPERM, ESRCH, ENOENT, EAGAIN when the reply queue cannot hold a whole list, or the spawn error) |

Window ids are assigned by the desktop at window creation (never 0) and are
not reused while the counter does not wrap. The desktop never sends a partial
window list. The desktop launches `/bin/tasks` like Files (a graphical menu program:
`--channel-fd=3`, `CIUKI_DISPLAY`), plus the authority at fd 4 and
`--authority-fd=4` (absent when PID 1 installed none);
other programs never receive it.

Runtime test scenario (`tasks-qemu-t23`): at the crash-isolation probe's
interaction ARM, F9 selects the scenario (F12 is the shell case, F11 the
Files case, F10 the CiukNote case);
the desktop tracks stages 1-7 (Tasks launched from the menu, its window, demo
launched through LAUNCH, its window, END_TASK on that window, demo reaped
with status 0, Tasks' private checkpoint ping `0xf2330007` plus the desktop's
own call-70 check) and reports `keys` only at stage 7. `--test=tasks` makes
Tasks place its frame at (8, 40), use the High speed and send the checkpoint
when the ended PID has left the process list and call 71, with the delegated
descriptor, refuses that PID (ESRCH), PID 1 and the desktop (EPERM); nothing
is printed on the console. The taskbar notice shows `tasks stage N` for the lead's screenshot.

Toolkit additions on the f2-31 libdeskui (9239aca), additive; the pinned
toolkit and Files frames of `tests/host/desktop/test_deskui.py` are
unchanged: `DU_TABS` (a tab control whose box spans the 20-pixel strip,
`DU_TAB_H`, and the raised page; labels are `items[i].text[0]`, `active` is
the selected tab; click, Left/Right/Home/End when focused and, from anywhere,
Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn/PgUp emit DU_EV_SELECT; `du_tabs_hit`),
`DU_GRAPH` (`samples`/`sample_count`, 0..100, oldest first, `total` = capacity;
black field, green grid and line, integer Bresenham), `DU_GROUP` (etched
frame with its label), `DU_COLUMNS` raised from 4 to 8 for the six Processes
columns (list drawing iterates `column_count`, so Files is unaffected), Alt
plus a visible enabled button's `&` letter presses it (after the menu
mnemonics, as in the dialogs), `du_window_queue` for arbitrary v2 requests
and a `message` hook on `du_window` receiving the v2 messages the glue does
not consume (WINDOW_INFO, TASK_RESULT). Task Manager uses the toolkit's own
list columns, sort indicator, embedded scrollbars, radio menu items,
message/input dialogs, timers and `du_run`.

## f2-32 CiukNote (2026-10-11)

`/bin/ciuknote` is a Notepad-class editor on libdeskui (menu label "CiukNote",
Tango text-document icon in the menu and the taskbar): `apps/ciuknote/app.c` commands,
dialogs and find bar, `doc.c` file and text rules, `vfs.c` the POSIX boundary, `main.c`
the window glue, `apps/libdeskui/textarea.{h,c}` the multi-line widget (`DU_TEXTAREA`).
Window 640x440 on displays of 1000x700 and more (600x380 from 800x600, else 560x320;
`CIUKI_DISPLAY`), menus File, Edit, Format, View and Help with accelerators, a right-click
menu, a find/replace bar and a status bar (`Ln, Col`). Printing to LPT1 stays out (no
parallel port driver); Page Setup and Print are absent. Acceptance decisions:
docs/design/f2-acceptance.md, section f2-32.

Research (2026-10-11): Microsoft's
[Notepad help](https://support.microsoft.com/en-us/topic/how-to-use-notepad-to-create-a-log-file-dd228763-76de-a7a7-952b-d5ae203c4e12)
describes the `.LOG` feature (first line `.LOG`; the date and time are appended at the end
each time the file is opened); the 0.8 CiukNote description in the legacy archive
(`docs/desktop-apps-2026-09-29.md`) fixes the scope and the 61,440-byte limit;
[Unicode 15 chapter 3, Table 3-7](https://www.unicode.org/versions/Unicode15.0.0/ch03.pdf)
defines the UTF-8 sequences the buffer accepts as one character (overlong forms, surrogates
and truncated sequences are single invalid bytes); POSIX
[rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html) and
[fsync](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fsync.html) justify the
save path; the QEMU case follows the
[QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
conventions of the other desktop cases. These were checked against `apps/libdeskui`, the
`protocol.h`/`desktop.c` shell code and the kernel's `i8042.c` text path.

- **Text model.** One flat UTF-8 buffer with `\n` line breaks, 61,440 bytes (a line break
  counts as one byte). Every offset lies on a character boundary; a character is one
  6x13 cell of the 5x7 glyph font, tabs advance to the next multiple of 8 counted from the
  visual line start, and non-ASCII or control characters are drawn as a hollow
  placeholder box and saved unchanged (a lone CR and NUL bytes survive). Undo has one
  level: it swaps with a snapshot, so a second Undo redoes; consecutive typing,
  backspaces or forward deletes form one step and any caret movement ends it. Word wrap
  breaks after the last blank that fits and hard-breaks longer words; End on a
  soft-wrapped line keeps the caret at the end of that line. Go To is disabled while Word
  Wrap is on. Find and Replace fold ASCII case only and never match inside a character;
  Replace All is one Undo step and, like Find, does not wrap around (`Cannot find "..."`).
- **Keyboard text.** The widget takes characters from `du_key_char` (the kernel's
  unshifted TEXT is ignored and Shift and Caps Lock are applied by the toolkit), so
  capitals and punctuation are correct on the native keyboard path.
- **Files.** CRLF pairs read from a file become `\n`; the style of the first line break is
  remembered and written back (new documents use CRLF, as Windows 9x Notepad). A file
  larger than twice the limit, or longer than the limit after conversion, is refused
  (`File too large`). Save writes `<name>.cn~`, `fsync`s, then `rename`s over the target
  and removes the temporary on any error, so a failed save keeps the old file. Protocol v2
  has no title update: the creation title is `CiukNote`, or `CiukNote - <name>` when
  started with a path; the status bar shows the document name.
- **Open and Save As** are the toolkit's file dialogs with a lister that shows folders and
  `*.txt` files; a pattern typed in the name box (`*.*`, `*.bin`) is a filter for the next
  dialog, a folder name navigates, the default extension `.txt` is appended on Save and
  tried on Open, and Save over an existing file asks `<name> already exists. Do you want
  to replace it?`.
- **Clipboard.** The editor keeps its own 61,440-byte clipboard (the toolkit's is 1,040
  bytes) and mirrors the first line into the toolkit clipboard, so text copied in an edit
  box or dialog can be pasted into the document and the reverse.
- **Prompts** use the 0.8 text: `The text in the <name> file has changed.` / `Do you want
  to save the changes?` (Yes/No/Cancel) on New, Open, Exit and a window close request.
- **About** shows the approved Ciuki portrait: the application reads
  `/system/assets/ciuki-portrait.xrgb` (the desktop's deterministic conversion) and reduces
  it with the box filter Files uses to the 32x32 dialog icon.
- **Command line.** `ciuknote [--channel-fd=N] [--test=ciuknote] [path]`; a missing path
  offers to create the file.

**Opening from Files.** Files cannot spawn a window: only the desktop owns the channel and
the display grants of a client. A Files that the desktop launched therefore sends LAUNCH
(protocol v2) with the name `ciuknote` and the file's absolute path as the single
argument. The desktop honours exactly that: the requester must be a client it launched as
`/bin/files` (`program == 1`, kernel-stamped PID), the name must be `ciuknote`, and the
argument must start with `/`; every other request with an argument, from any client
including the Task Manager, is answered `-EPERM`, and the Task Manager keeps its
argument-less LAUNCH. The program is started as `ciuknote --channel-fd=3 <path>` and the
answer arrives as TASK_RESULT (the PID, or -errno), which Files shows as `Cannot start
CiukNote:` when negative. Files refuses an over-long path (`ENAMETOOLONG`, limit 160
bytes) or one with control characters (`EINVAL`) itself, because the wire rules would
disconnect it. **`apps/desktop/protocol.h` changed (`DESK_ARG_MAX`, `desk_packet.argument`),
and the SDK installs it as `ciuki/desktop.h`: run `make sdk` before `make desktop`.**

**Toolkit changes** (all additive on the f2-31 libdeskui; its 9 toolkit frames and Files' 23
frames and the Task Manager's frames stay byte-identical): `DU_TEXTAREA` (appended to
`enum du_kind`) and `du_widget.data`; the text area takes focus, keys, text, pointer,
drag, double-click and scrollbar events from `widgets.c` and posts `DU_EV_CHANGE` on edits
and `DU_EV_CONTEXT` on the right button; Tab and Shift+Tab leave an edit but a focused
text area keeps Tab for the document. **Fix:** the toolkit treated button 3 as the right
button; the kernel maps `INPUT_BTN` index+1 (left 1, right 2, middle 3), so context menus
now fire on button 2 (`widgets.c`, `list.c`; the toolkit and Files host tests were updated
accordingly). The 0.8-era additions of the first f2-32 version that the rewritten toolkit
makes redundant were dropped: menu check marks and indentation (`DU_ITEM_CHECKED`),
shifted TEXT translation (`du_key_char`), Caps Lock tracking and message-dialog focus.

**Scenario keys.** The crash-isolation gate selects a desktop scenario at run time by one
function key pressed at the interaction ARM, before the shell exists; the key's release
is the scenario's second counted edge and later uses of the key belong to the programs.
F12: shell (stage machine, `demo`). F9: Task Manager (stage machine, `tasks`).
F11: Files and F10: CiukNote are *app scenarios* sharing one mechanism (`app_test` in
`desktop.c`): the first program started from the Ciuki menu with that name is launched with
`--test=<its name>`, the taskbar clock is fixed at 12:00:00, the program sends checkpoint
PINGs with serial base+n (Files `0xf2310000`, 5 checkpoints; CiukNote `0xf2320000`, 3: ready,
saved, opened), each presented frame is reported once as ` files=1 checkpoint=N` or
` note=1 checkpoint=N` and hashed by the controller, which opens the write gate for these
cases only (`case=files-write-gate` or `case=note-write-gate`, expected 0) and allows
40 s; a clean exit (status 0) after the last checkpoint completes the scenario, and only
then do the A key edges count, so `keys=4` (two scenario edges, two A edges) cannot be
reached by an app that did not run to the end. After teardown the controller flushes
storage (`files-durable` or `note-durable`).

**Scripted input and program start (2026-10-12).** In the gate scenarios the desktop reads its input
grant into a FIFO (512 events). While a graphical program it launched has not sent its first DAMAGE
(`awaiting_frame`, at most 30 s), the scripted input stays there; it is then replayed with its recorded
spacing (kernel event timestamps), shifted by the wait, so a script no longer depends on how long a
program takes to start. Files and CiukNote move to their `--test` frame before their first frame, so the
held clicks land on the moved window. Motivation: on image `2fdd515a` Files was not ready when
`files-qemu-t23` clicked File at (27,70); the click raised the survivor demo's window (20,52) behind it,
the typed keys went to the survivor, the shell's Alt+F4 closed it (`survivor_exit:0`) and `keys=0`.
The shell scenario counts its A edges whenever they follow F12 (still published only at stage 6): its
last step is a mouse click, and the 8042 hands out a pending keyboard byte before queued aux bytes
(QEMU `hw/input/pckbd.c` `kbd_update_irq`), so on the `qemu-e500` firmware input path the A edges
reached the desktop before the close click (image `639bcca5`: stage 6, `keys=0`). Task Manager and the
app scenarios keep the arrival rule (their last steps are keyboard input). Host replay models:
`late-start` (programs ready 8 s after spawn) and `--i8042-stall-*` (`app_replay.c`).

In `--test=ciuknote` mode the window moves to (24,48), Time/Date and `.LOG` use the fixed
time 2026-10-11 09:41 UTC (`9:41 AM 10/11/2026`) and the application prints one
`CiukNote checkpoint=N event=ready|saved|opened path=... text=... digest=XXXXXXXX` line on
stdout after the frame that follows each event; the digest is FNV-1a 32 of the 640x440
surface bytes (little-endian words), which contains no clock, so the host reproduces it
(`tests/host/desktop/test_ciuknote.py`).

**QEMU case `ciuknote-qemu-t23`** (`tests/suites/f2-desktop.json`; the selector and
predicates of `files-qemu-t23` with `note-*` records and three checkpoints): F10 down/up;
the cursor is clamped to the top-left corner (two (-381, -381) moves); click the
Ciuki button in the top bar at (40, 16), Programs at (40, 50) and the `ciuknote` row of
the Programs submenu (`tests/host/desktop/shell_menu.py` models the geometry and
regenerates the scripted cases); wait 3.6 s; type `Hello
CiukNote`, Enter, `Ciuki says woof.`, Enter, F5; Ctrl+S, type `/system/tests/note.txt`,
Enter (checkpoint 2); Ctrl+N, Ctrl+O, type `note.txt`, Enter (checkpoint 3); File > Exit
by its mnemonics, Alt+F then X (the clean document closes and CiukNote exits 0; neither
CiukNote nor the desktop binds Alt+F4); A down/up. Several key taps share one QMP batch
(at most 12 set-2 bytes: QEMU's PS/2 queue drops beyond 16 bytes per batch). 121 batches,
nominal pacing 10.3 s; at the 0.33 s per 100 ms batch measured in the first QEMU run of
`files-qemu-t23` about 36 s. After QEMU stops the host checker digests `::/system/tests/note.txt`:
bytes `Hello CiukNote\r\nCiuki says woof.\r\n9:41 AM 10/11/2026`, SHA-256
`8295f4d0977502fc61022209d665ff588382b0bd1564e5e38736f022e57f807d`, plus `fsck.fat -n`
exit 0. The three `pixel_digest` fields hold the placeholder `PIN-FROM-FIRST-RUN`, as
Files' (the lead pins them from the first run). The desktop-shell cases and the Files case
address `demo` and `files`, whose distance from the bar does not change when a program that
sorts before them is added; `tasks` (added by f2-33) does move them.

**Host replay of the app cases** (`tests/host/desktop/test_desktop.py`,
`AppCaseReplayTests`; harness `app_replay.c`, `replay_desktop.c`, `replay_os.h`,
`replay_base.h`): the exact batches of `files-qemu-t23`, `tasks-qemu-t23` and
`ciuknote-qemu-t23` go through a model of QEMU's PS/2 devices (16-byte queues per
batch, five 3-byte mouse packets, deltas clamped to +-127 per packet, button state
sampled at the batch's sync, one sync per QMP command: QEMU `hw/input/ps2.c`
`ps2_queue_noirq`, `ps2_mouse_send_packet`, `ps2_mouse_sync` and `ui/input.c`
`qmp_input_send_event`, https://gitlab.com/qemu-project/qemu, checked 2026-10-11) and of
the kernel's i8042 decoder (one REL event per axis,
X before Y, button edges after the packet's motion) into the production desktop in gate
mode at the probe's interaction stage, which launches the production Files, CiukNote,
Task Manager and demo as threads of a deterministic virtual kernel (channels, surfaces,
spawn mappings, waitpid/kill, calls 70/71, the probe's report checks; the Files and
CiukNote file systems are the fake image tree of `fake_vfs.c`). Each case runs at the
declared pacing and at the measured QEMU pacing with 2.5 s program start-up; the test
requires every checkpoint in order, `keys` = 4, the gate counters, no PS/2 drop or
carry-over and the probe's window. Two consequences for scripts: a diagonal move passes
through (x1, y0), so moves inside menus go vertically first (the first Files script crossed
the Go title on its way to View > List and opened Go); and the desktop routes a click to a
window it has just focused without first sending that client the pointer position, so a
first click into an unfocused client is hit-tested at a stale position (the Task Manager
case refocuses with the taskbar button).

Host evidence (no QEMU, image build or hardware): `python3 -B -m unittest discover -s
tests/host/desktop` (textarea model, layout, pointer and rendering with 3 pinned frames;
the application against a fake file system with 20 pinned 640x440 screens and a
25,000-step random input run; the replay of the committed QMP key script, which checks the
saved bytes and the three checkpoint digests; case and arithmetic checks; the launcher
test with the LAUNCH argument wire rules, the Files-only CiukNote launch and the CiukNote
scenario; Files' report of a refused LAUNCH). Not done: printing, a Help viewer beyond a
message box, a dedicated text-editor icon (the generic text-document icon is used).

## Files

Files is the CiukiOS file manager (`/bin/files`), started from the Ciuki menu
("Files", with the Tango file-manager icon). Its window follows the Windows 98
Explorer layout in the CiukiOS colours.

- **Menus.** File (Open, Open with CiukNote, New Folder, New Text Document,
  Delete, Rename, Properties, Close), Edit (Cut, Copy, Paste, Select All,
  Invert Selection), View (Large Icons, List, Details, Sort by Name/Size/Type/
  Modified, Refresh), Go (Back, Forward, Up One Level, Address Bar, CiukiOS,
  Applications, System) and Help (Help Topics, About Files). Alt or F10 opens
  the menu bar; underlined letters work with Alt and inside open menus.
- **Toolbar.** Back, Forward, Up, Cut, Copy, Paste, Delete, Properties and
  Views (click to cycle views; the arrow lists them).
- **Address bar.** Type a path such as `/system/assets` and press Enter; Esc
  restores the current folder. Letter case does not matter on FAT volumes.
- **Left pane.** Places lists CiukiOS (`/`), Applications (`/bin`), System
  (`/system`) and every volume mounted under `/mnt`. Folders shows the tree,
  opened along the current folder; [+] and [-] expand and collapse it.
- **Views.** Details (Name, Size, Type, Modified; click a header to sort, again
  to reverse; drag a header edge to resize), Large Icons and List. Folders
  always come first.
- **Selection.** Click, Ctrl+click (toggle), Shift+click (range),
  Shift+arrows, Ctrl+arrows and Ctrl+Space, Ctrl+A, Invert Selection, and
  typing the first letters of a name; repeating one letter cycles through the
  names that start with it.
- **Context menus.** Right-click (or Shift+F10 / the Menu key) on an item for
  Open, Open with CiukNote, Cut, Copy, Paste (into a folder), Delete, Rename,
  Properties; on the background for the views, sorting, Refresh, Paste, New
  Folder, New Text Document and Properties.
- **Operations.** New Folder and New Text Document create "New Folder",
  "New Folder (2)", ... and open the name for editing. Rename in place with F2
  (Enter keeps, Esc cancels). Cut/Copy and Paste copy or move files and
  folders in the background with a progress window and Cancel; an existing
  name asks Yes, Yes to All, No or Cancel; copying into the same folder makes
  "Copy of NAME". Delete asks for confirmation and deletes permanently (there
  is no Recycle Bin yet). Properties shows type, location, size, contents,
  modification time and the read-only attribute.
- **Opening.** Enter or double-click opens folders; text documents and scripts
  open in `/bin/ciuknote` when it is installed; programs run directly.
- **Keys.** Enter, Backspace (up one level, as in Windows 98), Alt+Left/Right/
  Up, F2, F4 or Alt+D (address bar), F5, Del, Alt+Enter, Ctrl+C/X/V, Ctrl+A,
  Alt+F4.
- **Limits of this release.** Copies cannot keep dates and attributes (the F2
  system interface has no setter; moves on one volume keep them). Programs
  started from Files have no desktop window, except CiukNote, which the desktop
  starts on Files' request (f2-32, LAUNCH with a path). Drag and drop, removable
  media and the Recycle Bin come later.

## libdeskui

`apps/libdeskui/` is the static widget toolkit shared by desktop programs
(MIT, built with the SDK's `ciuki-cc` into `build/apps/desktop/libdeskui.a`).

- **Window glue** (`client.c`): protocol v2 handshake, surface and private back
  buffer, damage limited to changed pixels, PING/PONG, CLOSE_REQUEST with an
  application callback, focus loss and RESYNC releases, and `du_run`, the
  event loop with timers (no poll/select: nonblocking turns and short sleeps).
- **Drawing** (`draw.c`): clipped integer primitives, Windows 98 edges, the
  compositor's glyphs, Tango icons with selected/ghost/disabled modes
  (`du_icon_blit`, `du_icon_draw`; icons come from the store, `icons.c`).
- **Widgets** (`widgets.c`, `list.c`, `menu.c`, `dialog.c`): label, push
  button, toolbar button (with split arrow), single-line edit (selection,
  clipboard, UTF-8), list view (Details with sortable, resizable columns,
  Large Icons, List), tree view, scrollbars, status bar with panels, splitter,
  progress bar, pane headers and separators; menu bar with drop-downs,
  mnemonics and accelerators; context menus; modal message box, input box,
  open/save dialog and progress dialog with Cancel. Tab moves the focus.
- **Clipboard and timers.** A per-process text clipboard
  (`du_clipboard_set/get`) and monotonic timers (`du_timer_start/stop`).
- Applications receive `du_event`s (activate, select, open, sort, context,
  change, commit, cancel, key, tick, dialog, focus) through one callback.

Host tests: `python3 -B -m unittest discover -s tests/host/desktop`. Rendered
frames are pinned by SHA-256 in `tests/host/desktop/test_deskui.py`; a visual
change must be reviewed and re-pinned.

## f3-02 additions (2026-10-11)

`/bin/dosbox` (apps/dosbox) is an ordinary protocol-v2 client that owns one
DOS virtual machine through syscall 72 (execution-abi.md, F3-02 amendment);
the kernel renders the VM into the client's 640x480 surface and the desktop
composites it unchanged. The menu lists it from `/bin` like any program and
passes it `--channel-fd=3`. `desktop --launch=<name>` starts one `/bin` menu
program at shell start through the menu-click path; the `f3:vm-window` probe
uses it. Menu click offsets for the scripted cases follow the formula in
docs/design/f2-acceptance.md, section F2-33. Details: docs/design/f3-acceptance.md, section f3-02.

## Games (owner decision 2026-10-12)

Programs > Games (present with `/bin/dosbox`) lists the pre-installed
shareware games of `apps/desktop/catalog.h` (`desk_games`): "Wolfenstein 3D
(shareware)" and "DOOM (shareware)", Tango `applications-games`. Each starts
`/bin/dosbox --name=<game> <command>`; dosbox types the command at the DOS
prompt of its window (`apps/dosbox/autorun.h`: `\DIR\PROGRAM` becomes
`CD \DIR` and `PROGRAM`) once DOS is ready and the window has the focus. The
window VM boots the bundled games disk `C:\GAMES\CIUKIGAM.IMG`
(docs/build-and-run.md, "Pre-installed shareware games"). The Games row
follows the program rows in the top-aligned Programs submenu, so the
scripted cases' rows do not move (`tests/host/desktop/shell_menu.py` models
it; `--regenerate` leaves `f2-desktop.json` and `zz-ui.json` unchanged).

## f2-30 decisions (2026-10-11)

Research precedes the shell implementation. The [POSIX spawn specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html)
inherits eligible descriptors; this repository's `ciuki_spawn` admits explicit
fd mappings and the POSIX subset expressly excludes `posix_spawn`. The shell
uses `ciuki_spawn`, rather than adding a conflicting SDK wrapper. Ordinary menu
children inherit the desktop's supervisor-created process group (flags=0), so
the existing group-local `kill` can send SIGTERM after a two-second close grace.
The crash gate retains NEW_GROUP and its original transport and pacing.

Choose a sorted `/bin` scan, bounded to 16 regular i386 ELF candidates, excluding
`desktop` itself. Validate printable, bounded basenames and an ELF32 little-endian
ET_EXEC/i386 prefix before admitting a menu entry; the kernel loader remains the
authority on executable validity. Children receive only stdio and an explicit
channel at fd 3; graphical clients use `--channel-fd=3`. Non-graphical programs
receive no graphical option and can exit without creating a window. No image
manifest or extra payload is needed. The menu uses the generic Tango executable
icon, an About entry with the exact Ciuki portrait, and a disabled Restart row.

`proc/supervisor.c` and the public syscall registry expose no reboot request to
userspace. `core/reboot.c` is kernel-only. Wiring Restart requires a lead-owned
supervisor interface outside this directive's allowed files; the row is disabled
and power-off is absent. No privileged operation or invented syscall is used.

The [Wayland input specification](https://wayland.freedesktop.org/docs/html/apa.html)
describes focus enter/leave and surface-local pointer coordinates; the existing
Ciuki input model already uses local coordinates and synthetic releases. Keep
those rules for click focus, title dragging, minimisation and menu focus. The
close request is a negotiated protocol-v2 extension; v1 clients receive their
existing CLOSED event, with the same bounded grace before termination of an
owned child. No v1 wire layout changes.
HELLO chooses version 1 or 2 and binds that version for the channel's lifetime;
every later message uses it. Version 2 adds `CLOSE_REQUEST=14`, desktop to
client, four header bytes and zero fds. The client acknowledges with CLOSE and
exits. Mixing versions or sending this opcode in v1 disconnects that peer.
The exact shared `protocol.h` is installed as `ciuki/desktop.h` by the SDK build.

Use [POSIX clocks](https://pubs.opengroup.org/onlinepubs/000095399/functions/clock_getres.html):
MONOTONIC for deadlines and REALTIME in UTC for the taskbar; label the build-seed
fallback in About, using the kernel's existing `uname` release/version/source.
About also shows `Version <uname version>` (the CiukiOS version string of
`config/version.json`, drawn only when `uname` supplied one) under the dedication.
Keep the existing font. Taskbar and About portrait sizes sample the existing
pinned conversion deterministically; generic icons come from the pinned Tango
0.8.90 archive with its existing notices, and remain separate from Ciuki's asset.

Existing `--gate` boots retain the plain compositor. The new shell case uses
the existing crash-isolation selector and an F12 input at its interaction ARM
to enable shell test mode; no selector, kernel probe or runner API is added.
The script opens the menu, launches Demo, drags its title and closes it, then
sends the existing A/motion/button stimulus. The desktop publishes the A edges
to the controller only after the observed menu/launch/drag/close/reap sequence.
The shell case requires `keys=4`, counting both F12 edges and both A edges only
after completion; the controller sees zero qualifying key edges before then.
Omission or failure therefore cannot pass the shell interaction predicate.
Paced zero-delta input batches provide waits within the existing 1–100 ms limit.
The [QMP input API](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
supplies ordered key, relative-motion and button edges. Existing cases and their
portrait/cursor checks remain unchanged. QEMU and owner screenshots remain
lead-owned evidence.

### f2-30 host/build handoff

Nine desktop host tests pass, with ASan/UBSan on the production modules and
launcher. Coverage adds menu/ELF admission, stable sorting/capacity, taskbar
layout/restore, hidden-window hit-testing, focus releases, About lifecycle,
incremental/full shell redraw equivalence, close-request backpressure and
timeout after EOF, launcher rollback/group/fd mapping, and shell milestones.
The plain 640x480 portrait/clock/cursor fixture was independently compared with
the pre-change compositor: both raw XRGB arrays have SHA-256
`6755f9a40f4b0ee69c1253145fe53ebd75f5f1948c2429c70da1a90923f82426`.
The unchanged approved portrait conversion retains its original pinned hash.

Kernel, SDK, Lua and desktop builds pass. The compositor integer audit passes;
the image passes loader/geometry/fsck and all 55 payload read-back checks.
Image SHA-256:
`26967a98c9c25950ead6ae1a6efae5d0b4dc87473a2feb31b14e9638c5ca2621`.
This is host/static evidence only. Runtime footprint, QEMU timing/input/digests,
the two shell cases and the runner screenshot remain unqualified until the lead
runs them. The 11 pre-existing suite case objects remain byte-equivalent as JSON
values; two cases are appended.

Lead action: `tests/host/test_desktop_gate.py:test_all_suite_cases_and_predicates`
hard-codes 11 cases and treats every case outside the five normal cases as a
stand-in. Its case count, native-case classification and action expectations
need to include the two shell cases. That file is outside f2-30's allowed
`tests/host/desktop/*` scope; the implementer leaves its assertions unchanged.
The Italian development diary and QEMU/owner evidence remain lead-owned.

The following sections preserve the f2-08 handoff and its historical evidence;
f2-30 supersedes its ordinary launch, close-button and SDK-header boundaries.
Neither handoff claims QEMU or hardware acceptance.
All authored application code is within `apps/desktop/` and `apps/demo/`;
kernel and SDK files are unchanged. The lead owns integration, the development
diary and `server=desktop` evidence for the image it actually runs.

## Build and payloads

```sh
bash scripts/build_sdk.sh --archive /path/to/newlib-4.5.0.20241231.tar.gz --jobs 2
python3 -B apps/desktop/build_desktop.py
python3 -B -m unittest discover -s tests/host/desktop -v
```

`make desktop` uses the established capped recipe; the direct commands above
are the directive's implementer exception. The SDK prerequisite in this
worktree completed in 18.113 seconds, including its mandatory host checks.
All scratch/compiler files remain inside the worktree, never in host `/tmp`.

The recipe compiles both programs through `ciuki-cc`, validates current SDK
provenance and runs the SDK ELF checker: static i386 ELF32, separate RX/R/RW
LOADs, nonexecutable GNU_STACK, no undefined symbols or dynamic/TLS segments,
no SSE/MMX. It separately audits the compositor object against the production
instruction classifier, rejecting x87 too. The compositor contains only
integer arithmetic. The libc may use the SDK's qualified x87 instructions.

`build/apps/desktop/manifest.json` records commands, source/SDK hashes, ELF
sizes/LOAD memory, conversion parameters and hashes. `scripts/build_image.py`
validates these before image creation and adds `/bin/desktop`, `/bin/demo`,
and `/system/assets/ciuki-portrait.xrgb` (since 2026-10-12 also
`/system/icons/tango.cis` and its three notices). The image manifest includes each
payload's size and SHA-256 plus desktop/portrait provenance, and existing T1
mtools read-back applies to all three. `make build-full` orders kernel, Lua,
desktop, then image. No full image build was run by this implementer.

## Ciuki identity and deterministic conversion

The source is the exact approved `assets/brand/ciuki-logo.png`, RGBA8,
noninterlaced, 1254x1254. Source SHA-256:

```text
17210ac9cfe07c54d6b0069a1502a036d52cb6340715ad47bdd54fa4dd386620
```

`apps/desktop/convert_portrait.py` uses Python's standard library only, checks
that source hash, PNG signature/chunk CRCs/layout, and reverses PNG filters.
For each output coordinate `(x,y)` it selects source
`(floor(x*1254/256),floor(y*1254/256))`, preserving the complete square and pose.
Each RGB channel is flattened onto the desktop colour `#375564` with
`(channel*alpha + background*(255-alpha) + 127)//255`. The output is a raw
256x256 array, 262144 bytes, little-endian B,G,R,0 (XRGB8888). Array SHA-256:

```text
1c63236be6380187c5fde01a9e91a69cadd581a68910eaf0055762a4c3e96839
```

Both hashes are pinned in the conversion script, verified by host tests, and
included in build/image manifests. There is no redraw, palette replacement,
artwork substitution or source edit. The boot photograph is untouched.
The portrait remains a project asset separate from Tango's Public Domain icons
and from the application's MIT code. F2 adds no replacement system icon family.

The compositor centres the converted portrait against a solid blue-grey
background. Its quiet dark top bar displays `CiukiOS`, the exact English
tagline `A modern Retro OS`, and MONOTONIC uptime as HH:MM:SS (hours modulo 100).
Window frames use a cream close button, a dark focused title and a muted
unfocused title. `Not responding` is the five-second heartbeat warning.
Original 5x7 bitmap glyphs render English UI and printable ASCII titles; the
demo renders incoming ASCII text, while the protocol retains Unicode scalars.

## Protocol version 1

`apps/desktop/protocol.h` is shared by both programs. SDK-overlay edits are
outside f2-08; the lead should install this exact header as `ciuki/desktop.h`
under a later directive. The authoritative syscall records remain `ciuki/abi.h`.

Every message is a `ciuki_message`. Payload header bytes: version=1 at 0,
opcode at 1, zero at 2 and 3. All integers below are little-endian; offsets
start at payload byte zero. Reserved fields, unused fd slots and payload tail
must be zero. The kernel stamps `sender_pid`; the desktop binds each endpoint
to its first sender and rejects a different PID. One channel has one client
and at most one live window; 16 channels are supported. A message in the wrong
direction, unsupported version/opcode, bad length/count, invalid title or
geometry, duplicate HELLO/CREATE, or operation before its lifecycle stage
closes only that client's endpoint/window. Every received attachment is closed
unless successfully retained as the window surface.

| Opcode | Direction | Payload bytes | Fields and fd count |
| --- | --- | ---: | --- |
| HELLO=1 | Client → desktop | 4 | Must be first; 0 fds |
| CREATE_WINDOW=2 | Client → desktop | 16+n | u32 w@4,h@8,n@12; n printable ASCII title bytes@16, 1–63; exactly 1 surface fd |
| DAMAGE=3 | Client → desktop | 20 | i32 x@4,y@8; u32 w@12,h@16 in surface coordinates; 0 fds |
| MOVE=4 | Client → desktop | 12 | i32 frame x@4,y@8, each -2048..2048, clamped to display/work area; 0 fds |
| CLOSE=5 | Client → desktop | 4 | Close this window/channel; 0 fds |
| PING=6 | Both | 8 | u32 serial@4; 0 fds |
| CONFIGURE=7 | Desktop → client | 12 | u32 actual w@4,h@8; 0 fds |
| KEY=8 | Desktop → client | 44 | Encoded 40-byte input event@4, type KEY/TEXT/RESYNC; 0 fds |
| MOTION=9 | Desktop → client | 44 | Event@4, type MOTION; value/value2 are client-relative absolute x/y; code=0; 0 fds |
| BUTTON=10 | Desktop → client | 44 | Event@4, type BUTTON; code=1/2/3, value=down/up; 0 fds |
| FOCUS=11 | Desktop → client | 8 | u32 focused@4, 0/1; 0 fds |
| PONG=12 | Both | 8 | Echo serial@4; 0 fds |
| CLOSED=13 | Desktop → client | 4 | Best-effort notification before endpoint closure; 0 fds |

CREATE geometry must equal immutable surface info: size=24, format=1, each
dimension 1–2048, stride=w*4, exact page-rounded allocation<=16 MiB. The desktop
maps attachments with PROT_READ only. DAMAGE accepts empty rectangles but must
remain within that surface; widened/clipped frame/damage arithmetic prevents
signed endpoint overflow. The output surface equals the qualified display size.

Input-event encoding at payload byte 4 follows the ABI's offsets, including
the complete u64 monotonic timestamp. KEY retains public set-1 A=0x1e, E0
positions and Pause=0x200. MOTION replaces the kernel's relative deltas with
client-local cursor coordinates, which may be negative/outside when focus
remains on a window. BUTTON retains the ABI's code/value semantics. Source,
sequence, generation and lost_count remain available. There is no resize
operation in version 1; CONFIGURE confirms the accepted dimensions.

The desktop initiates PING after one second and marks a window unresponsive
when the outstanding serial has no matching PONG for five seconds. A later
matching PONG restores its title. An unresponsive window remains until its
channel closes or an explicit CLOSE/close-button action. IPC reads and sends
are bounded per turn. A stalled peer's 64-entry outbound queue cannot block
the desktop: excess transient replies/events may be discarded; CONFIGURE and
FOCUS are retained as pending state, and a KEY/RESYNC with lost_count=1 repairs
input after the backlog drains. Clients must clear remembered input on this
RESYNC as well as on kernel RESYNC. SIGPIPE is ignored: EPIPE performs ordinary
client cleanup, as confirmed against `proc/channel.c:channel_send`.

## Focus, input, damage and lifecycle

Left-click on client content selects/raises that window. Left-click on title
begins a drag; button-up or RESYNC ends it. The close button drops that window.
Keys/text/motion go to focus; buttons pressed in content retain their recipient
until release or focus change. Focus loss synthesizes releases for remembered
keys/buttons. RESYNC releases remembered keys/buttons, records the kernel's
current button bitmap and sends the reset event; stale drag state is cleared.
With no clients, input and the software cursor/clock continue working.

Damage is clipped to display bounds, merges touching/overlapping rectangles
transitively and keeps at most 32 regions. On saturation it uses their bounding
union. Each region rebuilds background/portrait/bar, windows in stacking order
and cursor, then presents exactly that region with equal source/destination
coordinates. A failed present retains unpresented damage for retry. Frame
movement/focus/closure marks the old/new affected frames; cursor motion marks
the old/new cursor bounds. Client surfaces remain read-only and immutable in
geometry even when their owners race pixel writes.

Peer closure is drained before EOF. EOF, malformed input and EPIPE unmap the
client's complete allocation, close its surface fd and channel, clear pending
messages and repaint. Another focused window is selected. The desktop never
exits because a client failed. Process group isolation and grant noninheritance
are enforced by the kernel/supervisor, not invented by the compositor.

## Bootstrap and f2-09 wiring

Normal launch is `/bin/desktop`; current supervisor stdio uses fd 0–2 and
`grants_install` chooses the next free slots, so display/input default to 3/4.
The supervisor must pass `--display-fd=N --input-fd=N` if it reserves other
descriptors first. Repeated `--channel=N` registers explicitly inherited
endpoints. No ordinary desktop invocation spawns programs.

Only a kernel/controller-selected `--test=crash-isolation` permits repeated
`--demo=none` or `--demo=<fault-kind>`, and/or `--victim=<fault-kind>` with
`--cycles=1..100`. For example the lead can launch:

```text
desktop --test=crash-isolation --demo=none --victim=bad-pointer --cycles=100
```

The desktop creates each test pair, explicitly maps the child's endpoint to
fd 3, spawns `/bin/demo` with CIUKI_SPAWN_NEW_GROUP, and closes its own duplicate
of the child's endpoint. Sequential victims are reaped with waitpid and a
120 ms minimum interval before the next spawn. The survivor stays alive and
exchanges PING/PONG turns. Cycle output is plain application output with PID,
raw wait status, present/input/message/reply counters. The lead must capture
it through the controller, never treat it as CIUKI_TEST records, and measure
actual post-fault ticks, resource ledgers, pixel digest and external interaction.
This argument convention is available for f2-09; the kernel does not yet wire
it or spawn the actual desktop during selected probe boots.

| Demo fault | Operation after sharing and queuing PING | Expected outcome |
| --- | --- | --- |
| bad-pointer | Invalid channel-receive buffer then unmapped store | EFAULT/EAGAIN boundary followed by SIGSEGV |
| closed-peer | Create temporary pair, close peer, send; then close desktop endpoint | EPIPE (SIGPIPE ignored), EBADF on stale endpoint, exit 70 |
| forged-fd | Attach fd 63 to message | EBADF; exit 70 |
| grant-fd | Try display_info/present/input_read on own channel; attach nonsurface endpoint | EBADF; no grant acquired; exit 70 |
| handler-fault | SIGUSR1 handler stores to unmapped address | Handler-fault policy terminates process |

Every fault path waits for CONFIGURE and leaves a desktop-channel PING reply
unconsumed. A negative check with the wrong result exits 71, so the lead must
distinguish it from expected exit 70 or signal termination.

## Contract boundaries requiring lead action

1. The directive asks for blocking input with a timeout, but call 61 and the
   SDK take only `(fd,events,capacity)`. Poll/select and interval timers are
   excluded. This implementation uses O_NONBLOCK input plus interruptible
   10 ms nanosleep; an idle keyboard cannot stall channels or five-second
   liveness checks. The lead must accept this interpretation or extend the
   contract/API in a separate directive.
2. There is no named channel service, listener or endpoint-transfer operation.
   Clients need explicit launch inheritance. The existing supervisor installs
   grants but supplies no client channel, and skips desktop bootstrap on test
   boots. f2-09 must wire the selected argument/endpoint convention and actual
   desktop probe. No kernel change was made here.
3. Grants cannot be inherited or attached. `grant-fd` tests descriptor-kind
   forgery/rejection from an ordinary client; it cannot manufacture a genuine
   installed grant or claim a valid-grant EACCES case. That case remains in
   the kernel/supervisor test harness, as required by the ABI.
4. Guest acceptance still requires desktop/survivor PID and CR3 identity,
   at least 100 replies and 100 timer ticks after *every* fault, actual redraw
   plus post-fault key and mouse/button consumption, 100 cycles with kernel
   ledgers, pixel digest and desktop_restarts=0 on all three profiles. The
   120 ms user delay and host counts are not substitutes for those measurements.
5. On ENODEV from display_info the desktop reports console fallback and exits;
   the lead must run the contracted stand-in isolation as the additional
   no-LFB/safe-mode case. Runtime heap/stack commitment and memory ledgers also
   remain guest measurements. No hardware memory limit was changed.

## Research and implementation decisions

Research preceded implementation, then was checked against this repository:

- [W3C PNG specification](https://www.w3.org/TR/png/), scanline filters and
  non-premultiplied alpha: use a pinned RGBA8 decoder and explicitly rounded
  integer flattening; preserve encoded RGB samples and the approved composition.
- [Upstream Wayland surface protocol](https://wayland.freedesktop.org/docs/html/apa.html),
  surface-local damage and compositor ownership: use surface coordinates for
  DAMAGE and one private output surface. Ciuki's fixed message/grant ABI is
  authoritative; no Wayland transport, object model or buffer-commit promise
  is imported. Since Ciuki permits racing client drawing, the compositor
  promises memory safety and immutable geometry, not tear-free frames.
- `execution-abi.md`'s input/channel/grant sections and actual
  `proc/grants.c:grants_input_read`, `proc/channel.c:channel_send`,
  `proc/supervisor.c:supervisor_spawn` establish the timed-read gap, SIGPIPE
  behavior and bootstrap convention described above. The SDK wrappers and
  `apps/lua/build_lua.py` provide the existing typed-call/provenance pipeline.

The framebuffer masks, device decoding, process teardown and POSIX wrappers
were consumed as contracted interfaces, never reimplemented or changed.

## Host evidence

`tests/host/desktop/` compiles the production protocol/client/input/compositor
modules natively against a fake surface/channel layer with ASan and UBSan.
LeakSanitizer alone is disabled because it cannot run under this sandbox's
ptrace boundary; explicit fd/mapping counts return to baseline on every cycle.
Tests cover every opcode and direction, malformed lengths/counts/tails/version/
geometry, signed clipping extremes, transitive damage merges and saturation,
frame layout, focus/raise/drag/close, set-1 A/E0/Pause, TEXT and RESYNC releases,
empty desktop, backpressure, SIGPIPE/EPIPE, five-second unresponsive/recovery,
100 victim cycles with 100 survivor replies each, cleanup, incremental/full
compositor equivalence, z-order, guard words and retrying only damaged regions.
Python tests check repeat conversion/hash determinism, corrupt/source rejection,
ELF/SDK/source/output provenance and the production image payload inventory.

The final `ciuki-cc` build and inspections completed in 0.761 seconds. A repeat
build accepted the same source/output hashes without recompilation. These ELF
sizes include debug information, which remains in the image payloads:

| Payload | File bytes | Text | Data | BSS | Page-rounded LOAD bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| /bin/desktop | 505828 | 74716 | 1840 | 1636 | 86016 |
| /bin/demo | 396804 | 46872 | 1840 | 1636 | 53248 |
| /system/assets/ciuki-portrait.xrgb | 262144 | — | — | — | — |

Desktop ELF SHA-256:
`67cabe89ea3ffd21e64ca7b21acc7728a8acae28eaefab69f5f8bf1cfa3e3eab`.
Demo ELF SHA-256:
`4d13d90164cef45de30525a6f1dda3cf3fa9f0c2ab824400fde9c60ae05ca1ec`.
The three payloads total 1164776 bytes. No runtime heap/stack figure is claimed.

`python3 -B -m unittest discover -s tests/host/desktop -v` produced:

```text
PASS protocol: every opcode/direction, lengths, fds, versions, geometry, events
PASS damage/frame: clipping, signed extremes, transitive merge, saturation, coverage
PASS focus/input: set-1 A/E0/Pause, TEXT, routing, RESYNC releases, drag/close, empty desktop
PASS lifecycle: SIGPIPE/EPIPE isolation, backpressure, 5s heartbeat/recovery, 100 victim cycles, 100 replies/cycle, fd/map cleanup
PASS compositor: incremental/full equivalence, z-order, clipping, cursor, present retry/damage bounds
PASS desktop host model (guest qualification not_run)
T1 PASS: 3 payloads read back with matching sizes/SHA-256; required directories
PASS real FAT read-back: desktop, demo, portrait sizes/SHA-256
PASS portrait: pinned source/array SHA-256, repeat determinism, XRGB bytes, corrupt rejection
PASS desktop/demo ELF, SDK/source/output provenance, payload size/SHA-256 inventory
PASS stale/tampered portrait rejected before image payload acceptance
Ran 5 tests in 3.513s
OK
```

The separate existing payload regression command
`python3 -B -m unittest discover -s tests/host -p test_image_payloads.py -v`
reported `Ran 5 tests in 0.049s; OK (skipped=1)`. Its complete Lua inventory
case was skipped because this worktree has no built Lua application; its four
other tests passed. The desktop FAT fixture is a small host fixture, not a
canonical image build. Native visual inspection at 640x480 confirmed the
portrait, tagline, monotonic clock and cursor using the production compositor;
preview/scratch files were deleted afterward.

Files delivered: `apps/desktop/{desktop.c,desktop.h,client.c,protocol.c,protocol.h,
input.c,compositor.c,assets.c,convert_portrait.py,build_desktop.py,README.md}`,
`apps/demo/{demo.c,README.md}`, `tests/host/desktop/{__init__.py,fake.c,fake.h,
desktop_test.c,test_desktop.py}`, `scripts/build_image.py`, `Makefile`, and this
guide. The SDK was built as requested without changing SDK source. The
bootstrap/probe/SDK-header boundaries above remain lead-owned.

No QEMU, systemd scope, full image build, commit, push or Git command is part of
this implementer's evidence.
