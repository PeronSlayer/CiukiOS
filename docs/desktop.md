# F2 ring-3 desktop and client protocol

## f2-34 resize research (2026-10-12)

Before implementation, the existing fixed CONFIGURE/client surfaces, frame hit
testing and toolkit layout functions were checked against Microsoft's
[WM_SIZING](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-sizing)
(eight border/corner drag directions) and
[WM_GETMINMAXINFO](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-getminmaxinfo)
(separate tracking limits and maximized geometry). Ciuki uses client dimensions
for limits and keeps the 3 px border and 24 px caption. Toolkit resize passes
preserve model/widget state; anchors grow content and move bottom bars, and a
layout hook handles application-specific geometry.

The existing `execution-abi.md` surface/channel contract and
`proc/surface.c:surface_map` enforce read-only rights on every received surface.
The lead decision of 2026-10-12 uses client allocation instead: no kernel or
syscall ABI change. The [Wayland surface specification](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface)
provides the reference for attaching client-owned buffers, applying a new size
atomically and retaining the old buffer until its release. Ciuki uses its own
CONFIGURE/DAMAGE round trip for those roles. Pending size requests supersede
unsent replacements; an already sent replacement completes before allocating
another. The desktop maps all received attachments read-only.

Merge verification (2026-10-12): the [POSIX mmap specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html)
defines `MAP_FAILED` as mapping failure. `desk_receive` retains its check before
replacing the current surface and its ordinary peer cleanup on failure. Both
ordinary DAMAGE and a valid resize DAMAGE end the launched program's
`awaiting_frame` hold; the resize path still maps, swaps, releases the old
surface and sends COMMITTED. CREATE_WINDOW alone does not release held input.

## Protocol v2.1: client-owned resize surfaces (f2-34)

HELLO negotiates major 2 (header byte 0) and minor 1 (header byte 2).
Header byte 3 stays reserved. Every later message uses that pair. v1 and v2.0
clients retain their existing lengths, zero byte 2, fixed surfaces and close
behaviour. Other v2 opcodes retain their fields and directions under v2.1.
The shared header remains the SDK's `ciuki/desktop.h`; no syscall record or
kernel mapping right changes.

| Opcode | Direction | v2.1 bytes | Extension |
| --- | --- | ---: | --- |
| CREATE_WINDOW | Client → desktop | 44+n | Existing title@16, then seven u32 fields@16+n: flags, minimum w/h, maximum w/h, step w/h; exactly one client-created surface fd |
| CONFIGURE | Desktop → client | 20 | w@4, h@8, serial@12, flags@16; zero fds |
| DAMAGE | Client → desktop | 24 | Existing rectangle@4, configure serial@20; zero fds for ordinary damage (serial 0), exactly one replacement surface fd for a resize acknowledgement (nonzero serial) |

CREATE's only capability is `DESK_RESIZABLE=1`. Fixed windows send all seven
fields as zero. Resizable limits are positive client dimensions, at most 2048,
contain the initial size and use positive steps. Tracking rounds down to a
step relative to the minimum. Swap DAMAGE must cover the entire replacement
surface and its format/stride/page allocation must pass the existing surface
validator. The window also enforces its advertised limits and steps. Malformed
messages, invalid serials, attachment types or changed versions close only the
peer, releasing its descriptors and mappings.

Initial CONFIGURE has serial 0 and `DESK_CONFIGURE_COMMITTED=1`. A resize
proposal has a new nonzero serial and flags 0. The frame follows pointer motion;
the desktop clips the old surface into the requested content well, filling
newly exposed pixels with the theme's window background. Both old and new frame
bounds (including the shadow) are damaged. There is no scaling, desktop-side
allocation or out-of-bounds read of the old surface.

The toolkit allocates a writable replacement in the same format, calls
`du_resize` (anchors then the app layout hook), renders its whole tree and
sends full DAMAGE with the serial and the attachment. The desktop maps it
read-only, atomically replaces its surface, unmaps/closes its old reference and
damages the window. CONFIGURE with the acknowledged serial, new dimensions and
COMMITTED then permits the client to unmap/close/free its old surface/back
buffer. The client never writes the previous surface after the swap send.

Only one replacement is allowed per window. A newer proposal discards an
unsent replacement before another is allocated. Once the send succeeds, new
proposals are coalesced without allocation until COMMITTED arrives; an older
proposal already in flight can complete while the newest frame geometry stays
visible. Committed serials increase and cannot exceed the latest issued serial.
Unsent desktop proposals coalesce; under queue backpressure the latest proposal
and the surface confirmation are retained separately. Client surface/back
buffer failures or peer closure take the ordinary cleanup path. Surface
allocations return from two to one after each round trip, and zero at teardown.

The 3 px frame has a 7 px exterior grab margin, eight resize directions and a
visible bottom-right grip. The pointer uses the existing arrow (no resize
cursor family exists). The title zoom button toggles the saved user frame and
the work area between the 32 px top bar and 32 px taskbar, subject to limits
and steps. Title double-click retains windowshade. Moving/resizing a zoomed
window exits zoom; fixed clients and built-in About/Run/Shut Down windows do
not zoom or resize. DOS Box keeps its fixed 640×480 client surface.

`du_window_open_resizable` advertises per-app minimums, a 2048×2048 maximum and
unit steps: Files 600×360, CiukNote 440×280, Task Manager 400×420, Control Panel
its initial page size, Help 440×280 and Image Viewer 320×240. Calculator,
Character Map, Clock/Calendar and Volume Control keep their fixed control
layouts. Layout preserves document/model state, list selection, caret/focus,
dialog entries and timers; content grows and bottom bars retain their height.

Host evidence: `deskui_test` exercises unsent supersession, EAGAIN, deferred
in-flight proposals, read-only transferred descriptors, EOF and allocation/map
failures. `desktop_test` checks v2.1 wire rejection rules, borders/corners,
tracking steps, zoom restore, windowshade, fixed clients and backlog recovery.
`test_window_resize.py` replays six Files swaps through the production client
and desktop, at 600×380 and 760×520 in both themes. It also exercises zoom to the full work area and restore across windowshade.
It compares every content pixel with the toolkit frame, incremental redraw with
a complete composition, and surface/descriptor/mapping counts with their baseline.
`CIUKI_RESIZE_FRAMES` optionally saves the six host compositor PNGs. Existing
QMP app/gate scenarios remain unchanged; QEMU evidence belongs to the lead.
Owner decision 2026-10-12 (f2-35): native applications live in `/Programs`,
with `PATH=/Programs` and `HOME=/Documents`. The Documents shortcut and recent
list use `/Documents` and `/Documents/recent.lst`. The canonical FAT32 image
preserves the display names `Programs`, `Documents`, `System` and `Games`;
runtime `/system` paths and `C:\GAMES\CIUKIGAM.IMG` retain case-insensitive
lookup. `/tmp`, synthetic `/dev` and `/mnt`, and the drive mapping are unchanged.
See [the POSIX namespace contract](design/posix-subset.md#posix-paths-over-the-drive-vfs).

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
- **Map** (`apps/libdeskui/icon_map.c`, 361 entries, keys `<kind>:<object>`;
  Tango names follow the [Icon Naming Specification](https://specifications.freedesktop.org/icon-naming-spec/latest/)
  as of Tango 0.8.90, and only names present in the archive are used):
  programs (`files` system-file-manager, `ciuknote` accessories-text-editor,
  `tasks` utilities-system-monitor, `dosbox` utilities-terminal, `demo`
  preferences-desktop-peripherals, `hello` face-smile, `lua`
  text-x-script, `libc_smoke` applications-system, `desktop`
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
  executable's `/Programs` program icon, then the extension, then unknown.
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
  approved portrait inside a circular accent orb, pressed in while
  the menu is open; the menu drops below the bar, submenus open to the
  right): Programs (CiukNote,
  Files, NetSurf, Task Manager, DOS Box, Ciuki Demo, Lua, Control Panel, then the
  Accessories and Games submenus, then a Developer submenu with Hello,
  libc_smoke and any unknown `/Programs` program),
  Documents (recent files, My Documents, Clear Recent Documents), Settings
  (Control Panel and its pages, Task Manager), Help (Keyboard Shortcuts, About
  CiukiOS), Run..., Shut Down.... The menu opens under its button, the groups carry folder icons and each program its own icon (2026-10-25). Submenus open on hover or click; the
  keyboard works (arrows, Enter, Esc). A vertical banner shows the portrait
  and "CiukiOS". Menus are 30 px (top) and 22 px rows, all with 16 px icons.
- **Bars:** the top bar holds the Ciuki orb, free space, system tray icons,
  notification anchor and clock/date (24/12 h, optional seconds, full date
  tooltip). The bottom bar contains only left-aligned task buttons from x 4,
  at most 160 px each, including minimized windows. The active window is
  highlighted; right click opens its system menu. Window captions appear in
  their own frames and as task labels, never in the top bar. Tray popups and
  notifications open below the top bar. See [shell design](design/desktop-shell.md).
- **Network status:** the tray network icon is left of the speaker. It shows
  link-up, link-down or no-adapter state; its tooltip identifies the cable or
  adapter state. Clicking it opens Control Panel → Connections → Network;
  if the Control Panel is absent, a small status popup remains available.
  The desktop refreshes this read-only snapshot every two seconds.
  Address configuration and socket activity are provided by the kernel's
  lwIP-backed network service, not by the tray.
  Network lists the detected adapters. Adapter Properties shows driver,
  PCI/MAC, IRQ/I/O/MMIO, cable and supported media options; its checkbox enables
  or disables the interface. TCP/IP Properties selects automatic DHCP or a
  static address, mask and gateway, up to three DNS servers, host and domain.
  Apply saves the settings for that PCI/MAC and applies them immediately.
  Renew and Release control a DHCP lease. Status shows effective addresses,
  lease time, packets and errors; Refresh updates it. Test resolves the entered
  name through the system resolver and connects to the gateway on TCP port 80,
  showing success, refusal or timeout separately. A refused port can still
  belong to a reachable gateway; the test does not certify Internet access.
- **Volume:** the supervisor delegates the writable `/dev/mixer` handle to the
  desktop (`CIUKI_MIXER_FD`, F4 contract). The tray popup (slider, Mute)
  applies `MIXER_SET_MASTER` through it; without the handle the popup shows
  the public read-only state, without `/dev/mixer` "No audio device".
  Opening the popup also sends `MIXER_REFRESH_MASTER` on the delegated
  handle (f1-100): on a ThinkPad the kernel reads the EC once, so the popup
  shows the hardware volume keys at once; the kernel also reads the EC every
  2 s (f2-96, `BOOT.CFG` `ecpoll=0` turns that poll off).
- **Desktop icons** (desktop as a folder since f2-42,
  [design](design/desktop-folder.md)): the fixed icons Computer (Files at
  Computer; the icon carries the Ciuki portrait), Documents (Files at
  `C:\Documents`) and Recycle Bin (Files at the Recycle Bin, [design](design/recycle-bin.md)), then the
  entries of `C:\Documents\Desktop`. Click selects, a double click
  (desktop.cfg `double_click_ms`) or Enter opens; right click opens Open and
  Properties, plus Open With, Cut, Copy, Rename and Delete for folder
  entries; system icon labels can be changed in place with F2. The background
  menu has Arrange icons by Name, Type, Size or Date, Auto Arrange, Line up,
  Refresh, New (Folder, Text file, Bitmap image, Shortcut), Paste and
  Properties. Desktop keyboard navigation supports arrows, Home, End,
  type-ahead and Ctrl+A. New Shortcut currently writes a CiukiOS `.lnk` record
  targeting Files; a target picker and opening arbitrary desktop link records
  remain unimplemented.
- **Built-in windows** (internal clients, framed by the compositor): About
  CiukiOS (540 x 420; tabs General, System from call 70's system record and `uname`,
  Credits, see [About credits](#about-ciukios-credits-f2-97)), Run (a `/Programs` program and up to six arguments, double quotes
  group), Shut Down CiukiOS (radio-button choices for Restart and Shut Down since 2026-10-25, with OK and Cancel) and message boxes.
- **Shortcuts:** Win (tap) or Ctrl+Esc opens the Start menu; Alt+Tab and
  Shift+Alt+Tab switch windows with an overlay; Alt+F4 sends the v2
  CLOSE_REQUEST to the active window (the Shut Down window when none is
  active). Alt is held back until the next key, so a program never sees an Alt
  press for a chord the desktop consumed; a lone Alt tap still reaches it.
- **Shut Down and Restart:** confirmation saves desktop.cfg and opens a
  built-in "Shutting down..." window using the ordinary frame painter.
  Every client receives the same close request as Alt+F4, including DOS
  windows; their process exit lets the kernel tear down the VM. Clients get
  three seconds to exit, concurrently, then survivors are terminated through
  the existing call 71 path. IPC, painting and child reaping continue; normal
  input and launches are blocked. Escape or Cancel returns to the desktop;
  programs already closed are not relaunched.
  Settings streams are closed by the save operation; the delegated mixer
  descriptor and any regular-file/directory descriptors (including redirected
  logs) are closed before `ciuki_power` (call 73; 1 reboot, 2 power off).
  EBUSY is retried every 250 ms for five seconds. Persistent EBUSY shows
  "Some programs are still running:" and the remaining client names, with
  Cancel and Force. Force terminates remaining targets and retries once after
  allowing 250 ms for teardown. If no client is still known,
  the message identifies an unknown storage holder rather than inventing a
  program name. Other failures retain the power error message with errno text,
  including the existing ENOSYS explanation. Cancel/error restores normal
  input; after the delegated mixer descriptor has been closed, public mixer
  reads remain available but control requires a new desktop session (only
  PID 1 can delegate that capability).
  Runtime gates and the crash-isolation probe never enter this sequence.

  Research (f2-38): [POSIX monotonic clocks](https://pubs.opengroup.org/onlinepubs/000095399/functions/clock_getres.html)
  provide elapsed-time deadlines unaffected by wall-clock changes;
  [WNOHANG](https://pubs.opengroup.org/onlinepubs/9699919799/functions/wait.html)
  allows reaping without blocking the compositor;
  [close](https://pubs.opengroup.org/onlinepubs/009604499/functions/close.html)
  releases descriptors; and [SDL's close event](https://wiki.libsdl.org/SDL2/SDL_WindowEventID)
  is a request for application cleanup. Repository checks: `desk_request_close`
  already queues CLOSE_REQUEST/CLOSED and `desk_child_exited` releases clients;
  `storage_shutdown` refuses referenced VFS nodes before detaching volumes.
  Decision: reuse that protocol and monotonic event-loop deadlines, leaving
  the kernel's storage refusal and VM lifecycle unchanged.
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
| `theme` | `Platinum`, `Ciuki Blue` | `Ciuki Blue` |
| `wallpaper` | `ciuki` (the approved portrait on the desktop colour, exactly where the f2 compositor draws it), `none`, `builtin:<id>` (`dawn`, `pinstripe`, `deepblue`, `twilight`, `linen`, `blueprint`), or an absolute path to a PNG, JPEG or BMP | `/system/wallpapers/Ciuk2.png` |
| `wallpaper_mode` | `center`, `stretch`, `tile`, `fit`, `fill` | `fill` |
| `desktop_color` | six hex digits RRGGBB | `375564` |
| `clock_24h` | 0/1 | 1 |
| `clock_seconds` | 0/1 | 0 |
| `desktop_icons` | 0/1 | 1 |
| `icon_auto_arrange` | 0/1 | 0 |
| `icon_sort` | 0 (Name), 1 (Type), 2 (Size), 3 (Date) | 0 |
| `mouse_speed` | 1..10 (5 = one pixel per count; 2/8 .. 20/8) | 5 |
| `double_click_ms` | 200..900 | 500 |
| `keyboard` | `us`, `it` | `us` |
| `display_mode` | confirmed VBE mode id, decimal 1..65535; 0 leaves the boot mode | 0 |
| `recent` | absolute path; repeated, newest first, at most 8 | none |

### Wallpapers

The built-in wallpapers are original CiukiOS designs computed at the screen's
resolution by integer code (soft platinum/blue gradients with ordered dither,
pinstripes, a linen weave, a blueprint grid); no image file is shipped for
them and nothing third-party is drawn. Image files are decoded with the
vendored stb_image 2.30 (`third_party/stb`, MIT or Public Domain; notice
installed as `/system/licenses/stb_image.txt`): PNG, baseline and progressive
JPEG, and uncompressed BMP (4/8/16/24/32 bit; stb_image reads neither 1-bit nor RLE BMP). Images above 4096 x 4096,
16 Mpixel or 24 MiB are refused. `center` (1:1, centred, cropped or banded),
`stretch` (non-uniform to the area; area average down, bilinear up), `fit`
(uniform scale inside the area, bands in the desktop colour) and `fill`
(uniform scale max(area width / image width, area height / image height), so
the image covers the area, with the overflow cropped equally on both sides;
same resampling as `stretch`) use the area between the bars (1024 x 704 at
1024 x 768); `tile` starts at the top-left corner of the screen. The mode
names keep their order and values (`fill` was appended), so files written
before `fill` existed still parse. The image installs the owner's three
adaptive Ciuki scenes (`assets/wallpapers/ciuki`, 1448 x 1086 PNG, the 0.8-line
artwork extended with scenery so that a centred, aspect-preserving fill keeps
the dog and the lettering) in `/system/wallpapers`; `Ciuk2.png` with `fill` is
the default (owner decision 2026-10-12). At 1024 x 768 the 4:3 scene scales by
1024/1448 to 1024 x 768 and loses 32 rows (45 source rows) at the top and at
the bottom of the work area, nothing at the sides. The Control Panel lists
them with any PNG/JPEG/BMP files the owner copies there; "Ciuki portrait" is
the approved portrait on the desktop colour.

The wallpaper worker reads matching raw frames from `/System/cache/wallpaper`
and keeps a bounded cache of prepared frames, decoded source and placement
layer in memory. Runtime requests never write raw disk frames or their LRU
journal: FAT's full-file COW on each short write can otherwise delay publication
and block the shell's directory reads. Cache generation remains available to
explicit synchronous tooling. See [F2-73 publication validation](validation/f2-73-wallpaper-publication.md).

**The Windows ME bitmaps of the 0.8 line (`third_party/win_bg`, local and
git-ignored, Microsoft copyright) are never added to the repository or to the
image;** the desktop only shows them if the owner copies them into
`/system/wallpapers` himself (they are 8/4-bit tiles; choose Tile).

### Control Panel (`/Programs/control`)

f2-37 display research (2026-10-12):
[Microsoft ChangeDisplaySettingsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexw)
distinguishes testing a mode from persisting it. CiukiOS therefore requests a
temporary mode through the desktop and writes `display_mode` only after the
user confirms a successful switch. The 15-second confirmation interval is
the f2-37 requirement; it uses libdeskui's monotonic timer, not the UTC clock.
[VESA VBE 3.0, controller and mode information](https://pdos.csail.mit.edu/6.828/2012/readings/hardware/vbe3.pdf)
defines the BCD version, bytes per scanline and video memory units; the panel
displays the normalized f1-23 record and offers only its linear 16/24/32-bit
modes. It does not invent refresh rates or EDID information.
[Upstream Linux PCI IDs](https://kernel.googlesource.com/pub/scm/linux/kernel/git/ralf/linux/+/linux-2.5.53/drivers/pci/pci.ids)
identifies the SuperSavage IX/C (`5333:8c2e/8c2f`), Rage Mobility M/P
(`1002:4c4d/4c52`) and contemporary Intel, NVIDIA, S3/VIA and 3dfx entries.
Adapter names come from `/System/hwids/pci-ids.json` through the existing
`hwid_name` callback; a database match does not imply a native driver. Unknown device pairs remain numeric. Repository comparison: the
existing panel exposed wallpaper/theme only, with immediate settings reload;
f1-23 owns the actual mode switch and compositor relayout.
[The PCI ID project's upstream database](https://raw.githubusercontent.com/pciutils/pciids/master/pci.ids)
also confirms VIA CLE266 (`1106:3122`), KM400/KN400/P4M800 (`1106:7205`),
and Intel 865G (`8086:2572`) and 915G (`8086:2582`) for the later baseline.

Since f2-75 (2026-10-16) the Control Panel opens on a two-pane home view of
seven categories and nine applets taken from one shared catalog
(`apps/desktop/settings_catalog.h`, also used by the Start Settings menu and
the tray); Display Properties has the Background, Appearance and Settings
tabs, and System has General, Hardware check, Logs and Drivers
([interaction audit](design/desktop-interaction-audit.md)). The tab layout
below is the f2-37 record of 2026-10-12; `--panel=` keeps accepting its
names as aliases.

Display Properties Background includes Browse for an absolute PNG, JPEG or
BMP path. Paint, Image Viewer and NetSurf also offer Set as Wallpaper; these
requests go through Control Panel's settings writer and the desktop settings
reload path.

The f2-37 layout: a libdeskui program (560 x 420) with tabs Display (theme list with preview,
wallpaper list with preview, Center/Tile/Stretch/Fit/Fill, six desktop colours,
desktop icons on/off, Screen area and filtered Colours lists), Mouse (pointer speed and double-click speed, applied
by the desktop), Keyboard (US or Italian), Sounds (event assignments and Preview,
Default/No sounds schemes, separate UI sounds volume, master volume and mute
through the desktop's mixer handle), Date & Time (the clock and the 24 h and
seconds options; setting the clock is not offered because the ABI has no
clock-setting call), System (portrait, version, build, memory and current
display), and Adapter (current geometry/depth/pitch, VBE version/video memory,
PCI adapter identity, VBE strings, monitor placeholder and Refresh).
`--panel=display|mouse|keyboard|sounds|datetime|system|adapter` selects the
first tab (the Settings menu uses it).

Network status is shown in the system tray. The icon distinguishes an active
link, no cable and no detected adapter; its popup reports the driver, MAC,
link and driver state. The read-only status view reports link state, while
lwIP handles DHCP address setup and DNS for native socket clients.

The **Programs** applet is Add/Remove Programs. Its sorted list combines
Win32 installer registrations from HKLM and HKCU, bundled DOS games, and
user-added DOS and native programs. It shows name, version, publisher and
size; system components are marked protected. Remove asks for confirmation,
runs a registered Win32 uninstaller or removes the selected DOS/native
program and its launch records, and refreshes the menus without a reboot.
Bundled game removal warns that restoring it requires reinstalling the system.
Install opens the Files picker for removable media or CD setup programs and
launches them through the existing DOS or Win32 path; CiukiOS does not add a
separate installer engine.

System event sounds use the pinned Kenney CC0 pack. Startup and Desktop ready,
shutdown, menu open/command, window open/close, minimise/restore, error,
information/question dialogs, volume steps and removable device connect/disconnect
have individual assignments. AutoPlay has an assignment and a client event hook
(the AutoPlay window itself is described under "AutoPlay for removable media and
CDs" below). Empty recycle remains the existing placeholder until recycling is
implemented. The initial removable-media snapshot
is silent; later arrivals, removals and generation replacements produce events.

`/System/sounds/scheme.cfg` supplies the default WAV basenames. Existing
`/system/settings/sounds.cfg` preferences are read as a migration overlay. With
F2-47, typed registry values override both: `Desktop\Sounds` has boolean `Enabled`
and integer `Volume` (0–100), and `Desktop\Sounds\<event>` has string `File`
(an empty string disables that event). The Sounds page commits all its preferences
in one registry branch merge, preserving other Desktop keys, then asks the service
to reload. Choosing Default restores the pack's mappings while retaining UI volume;
No sounds disables automatic playback. Preview still works in No sounds, subject
to UI volume, master mute and VM priority. Custom assignments can be edited in
Registry Editor and take effect on service reload or desktop restart.

The sound worker owns file access, mixer queries, WAV mixing and nonblocking audio
writes. Input/compositing only enqueue bounded commands. UI gain scales the two
voices before saturation and never changes a game's stream gain. Master mute and
active/queued DOS VM audio suppress UI sounds; a VM taking over also cancels current
UI playback. Suppressed events are discarded. The existing mixer diagnostics flags
expose VM activity without changing record size. Preview observes the same policy;
the separate hardware test retains its fixed 1 kHz diagnostic tone.

Dragging the tray volume slider submits each changed step, and Up/Down in its popup
change the master by one percent. Extended keyboard Volume Up/Down and Mute keys
are also handled. Volume feedback follows successful asynchronous mixer completion;
volume bursts are throttled on enqueue and playback to at most one tick per 60 ms.
See [F4-12 research and decisions](design/f4-ui-sounds.md).

Wallpaper and other preferences still save immediately. Resolution and colour
depth stay staged until Apply. A successful desktop reply and matching query
open “Keep these settings?” with a 15-second countdown; Revert is the default
and Escape choice. Keep merges only `display_mode` into the latest settings
file, preserving recent documents and unknown keys, then requests a reload.
Revert, timeout, save failure and panel close request the previous active mode
through the desktop relay. Main persists each successfully presented mode
(see the integration note below). There is a separate 15-second
reply timeout before confirmation and during rollback; missed rollback replies
are reported, never described as a successful restore. Apply is disabled when
the query is unavailable or the active mode cannot be identified for rollback.
The two scrollable mode lists live below the wallpaper controls; the monitor
preview and five Position buttons retain their geometry. libdeskui now has 64
widget slots to accommodate the extra controls and seventh tab.

**f1-23 integration:** the SDK overlay's `<ciuki/display.h>` now imports the
canonical records from `<ciuki/abi.h>` (12-byte mode, 612-byte query info).
The older 48-byte call-67 record is `struct ciuki_framebuffer_info`; no host
ABI renaming shim or provisional desktop bridge remains. The SDK supplies
call-74 query/set and `display_start` installs the real desktop callback,
including pending-frame tracking, counter polling, relayout and startup replay.
SETTINGS retains main's uint16 wire validation (including zero, which the kernel
rejects if unlisted), reserved-zero fields, permission check and shutdown EBUSY
guard. The relay calls the callback once and returns its result and serial.

Integration research rechecked the [VESA VBE 2.0 Core Functions standard,
sections 4.3–4.5](https://www.phatcode.net/res/221/files/vbe20.pdf): controller
information supplies the mode list, strings and VRAM in 64 KiB units; mode
information supplies geometry/pitch; setting an LFB uses bit 14. The existing
f1-23 kernel owns that firmware operation. The Control Panel consumes its
queried table and authenticated desktop relay; it adds no firmware calls.
Main's desktop persists a mode after its first successful presented frame,
including a preview or rollback. The panel's Keep/Revert timer remains, but
strict persistence only after Keep needs a separate lead decision about that
existing service contract.

v2 opcode for it, client to desktop, version 2 only:

| Opcode | Payload bytes | Fields |
| --- | ---: | --- |
| SETTINGS=21 | 24 | u32 serial@4, request@8: 1 reload (all three values zero); 2 master volume (left@12/right@16 0..100, mute@20 0/1); 3 temporary display mode (id@12 1..65535, @16/@20 reserved zero) |

The desktop answers TASK_RESULT (request 21): 0, EPERM for any client it did
not launch as `/Programs/control`, ENODEV for request 2 without the mixer handle,
ENOSYS for request 3 without a display callback, or the mode-set error (for
example EBUSY while a DOS guest holds the display). The panel correlates display
replies with the latest request serial; older replies cannot confirm a mode.
The desktop callback applies temporary requests without setting `want_save`
or `want_reload`. The confirmed key follows the existing settings reload path.

Host coverage: `DesktopTests.test_control_display_modes` exercises the synthetic
query table, duplicate resolution filtering, supported colour depths, control
layout, acknowledgement/confirmation/revert, Escape and delayed timer ticks,
query/queue/save failures, config merge and wire validation, PCI table lookups,
Adapter rendering/Refresh and the System summary under ASan/UBSan.
`test_shell_launcher` verifies the production desktop request permissions,
result serial, mode callback, EBUSY/ENOSYS and lack of premature persistence.
`CIUKI_WALL_FRAMES=build/f2-37` exports the host review PNGs; they are not guest
or hardware qualification.

### Keyboard layouts

`keymap.c` maps set-1 scan codes for US and Italian (the KBDIT engraving:
`ò@ à# è[ +]`, AltGr+E = euro). The desktop's own text field (Run) uses the
selected layout at once, and every program the desktop launches receives
`CIUKI_KEYMAP=us|it`. libdeskui's `du_key_char` still uses its US table:
wiring it to `ciuki_keymap_char` (and UTF-8 insertion of non-ASCII
characters) is left to the toolkit owner.

### Icons

Each item names its Tango 0.8.90 icon; `desk_shell_icon(name, size)` returns
it from the Tango store (`/system/icons/tango.cis`, through the desktop's
icon hook) at a size the store holds. One size per place (owner feedback
2026-10-12: one fixed, coherent icon set): every menu (the Start menu too)
16 px, taskbar buttons, title bars and the tray 16 px, built-in windows,
message boxes and Alt+Tab 32 px, desktop icons 48 px (Tango 0.8.90 has no
48 px PNGs: the store's 48 px entries are the area filter of the 32 px
artwork), Files' toolbar 22 px. Tango draws some icons differently at 16 px
and at 22/32 px (`preferences-desktop` is a window grid at 16 px and a panel
at 32 px); keeping one size per place keeps each place consistent. The shell's
program table, Start menu rows, desktop icons and `catalog.h` use the same
Tango names as `icon_map.c` (`program:`, `menu:`, `settings:`, `place:`), so a
program has one icon in the Start menu, Files' `/Programs` and Task Manager
(`test_icons.py` compares them); two rows of one menu never share an icon
(`shell_launch_test.c`): Run... is `go-jump`, Settings `preferences-system`
(Control Panel keeps `preferences-desktop`), Ciuki Demo
`preferences-desktop-peripherals`, each game `input-gaming` (the Games folder
keeps `applications-games`), Clock `appointment-new` (Date & Time keeps
`office-calendar`). Identity icons (About, the Computer badge, the Start
banner) are area-averaged conversions of the approved portrait, whose
`#375564` surround is made transparent.

Own icons (fx-exeicons, 2026-10-27; [file icons](design/file-icons.md)): a
program or library, an icon file, a shortcut (its IconLocation or its
target's icon), a PIF, an Internet shortcut, a read-only or system folder
with a desktop.ini and a CD with an AUTORUN.INF `icon=` show their own icon
wherever the shell shows them; the Tango icon above stays their type's
default. libdeskui extracts them on a worker (`du_file_icon`, the bounded
`iconx` core); the shell repaints its icons, open menus, AutoPlay and the
taskbar when the worker publishes. Win32 windows send their icon
(`DESK_V3_WINDOW_ICON`: WM_SETICON, the class icon, else the executable's)
for the caption, the taskbar button and Alt+Tab.

Desktop icon labels (Windows Vista/7 style, readable on any wallpaper): white
Inter 13 (`DESK_FONT_LABEL`), up to two centred lines broken by width in a
96 x 94 cell, over a soft dark halo: the glyph coverage blurred twice by the
binomial filter 1 4 6 4 1 (radius 4), raised 3x, capped at 82 % and moved
1 px down, blended in black. The halo is integer-only, computed once per
label and theme text mode and cached (`label_halo`), and stays inside the
cell, so a partial repaint is exact. The selected label sits in a rounded
(radius 4), translucent box: a light tint of the theme's selection colour at
about 50 % with a lighter outline, the wallpaper showing through.
`DesktopTests.test_look_reference_frames` renders the icons over Ciuk2 in
every theme (`look_capture PORTRAIT OUTDIR WALLPAPER`) and checks the text,
the halo, the translucent selection and that nothing is drawn outside the
cells.

Research (2026-10-12): Microsoft's Windows 7 UX guide,
[Icons](https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-icons):
application and Control Panel icons come at 16, 32 and 48 px, dialog icons at
32 and 48 px, 16 px icons are drawn straight-on and simplified ("simply
scaling down ... does not work"), which is why Tango's 16 px artwork differs
from its 32 px artwork; the Vista desktop list view draws its labels as
"shadow text on transparent backgrounds"
([`LVS_EX_TRANSPARENTSHADOWTEXT`](https://learn.microsoft.com/en-us/windows/win32/controls/extended-list-view-styles)),
the model of the halo above.

## About CiukiOS credits (f2-97)

The Credits tab of the About window (`apps/desktop/shell.c`, `credits_page`) is a
scrollable page, replacing the former nine-line text:

- **Header (fixed).** The approved Ciuki portrait, unchanged, area-sampled to 56 px in a
  sunken frame; `CiukiOS` in the large face; the tagline `Classic computing, one woof at a
  time.`; `Dedicated to Ciuki.  Version <human version>` (the version string of
  `config/version.json`, which the kernel reports through `uname`, rendered as `1.0.0 Beta 1`).
  General shows the human name and full `+build.<build id>.g<short commit>` details.
- **List (scrolling).** One line per component with three columns: name, version or pin,
  licence. Seven groups in this order: System, DOS, Windows compatibility, Network and
  browser, Media, Tools, Icons; group names are bold in the theme's selection colour. The
  rows come from `apps/desktop/credits.inc`, which mirrors [`docs/credits.md`](credits.md);
  `tests/host/test_credits.py` fails when the two differ or when a version, licence or
  notice path drifts from the pins in `config/`, `third_party/*/UPSTREAM.json` and the notice
  files.
- **Scrolling.** Up, Down, Page Up, Page Down, Home and End; the mouse wheel (three rows
  per notch, as the toolkit's list views); a click in the thin scroll bar jumps to that
  position. Leaving the tab resets the scroll. Left, Right and Tab still change tab, Esc and
  Enter close the window.
- **Licences.** The `Licences` button (or the L key) asks the desktop to start Files on
  `/system/licenses`, the folder of the notices installed in the image.
- **Tests.** `tests/host/desktop/test_about.py` renders the page with the production
  compositor (modes `about-credits`, `-end`, `-pgdn`, `-wheel` of `compositor_capture.c`,
  driven through `desk_input` key and wheel events) and pins the four frames by SHA-256; it
  also checks that the identity header does not move while the list scrolls. The portrait and
  the Tango icons are not touched.

The wheel reaches the window through `desk_shell_wheel` (`input.c`, `CIUKI_INPUT_WHEEL`):
only a focused About window on its Credits tab consumes it, every other window still gets
the event.

## Compositor, 2D engine and open defects (status of 2026-10-22)

- **2D engine (f2-92, f1-97).** A universal registry selects native drivers for copies, fills and writes
  through the 2D engine (`CIUKI_ACCEL_WRITE`, `CIUKI_ACCEL_OFFSCREEN`, self-test at the first use,
  off-screen cache of unchanged surfaces). The indicator reads `2D engine: OK (drawing:
  software)` until the engine has drawn, then `2D acceleration: YES`; `accel=0` in `BOOT.CFG`
  disables native 2D drivers. The synchronized software reference engine handles visible
  targets; family status is in [the display model](design/display-driver-model.md). The record is `[desktop] accel fills= copies= writes= fallback= frames=
  lfb_bytes=`.
- **What the T23 measured (image `a4d9454e`, dev diary 2026-10-22-07).** The engine is used
  (`copies=88` in 2 s) but the compositor still composes about 40 full frames per second with
  only the music player open (two desktop threads take 40-50 % of the CPU; idle 8-20 %), and
  `[desktop] stall stage=idle ... no_frame_ms=524-676 input_pending=1` shows the desktop
  sleeping with input queued. The hardware cursor is loaded while the software cursor is
  still drawn (two cursors), and the ThinkPad volume keys no longer reach the mixer because
  the EC poll is off (f1-100).
- **Decision (owner, 2026-10-22, dev diary 2026-10-22-08).** The compositor is rebuilt on the
  Windows 98/X11 model: clipped, region-driven updates and never a full-frame composition,
  reusing the X11 region algorithms (xorg `miregion.c` / pixman `pixman-region32.c`) and
  pixman's compositing under the MIT licence (directive f2-95, amendment 1). Frame rate
  capped at 30 fps for continuous small damage, wake on input, budget under 10 % CPU at rest
  on the T23 model. f2-96 removes the duplicate cursor and polls the EC every 2 s; f1-101
  raises the priority of audio producers. These are open until the next image passes on the T23.

## f2-33 Task Manager and the v2 task extension (2026-10-11)

`/Programs/tasks` is the CiukiOS Task Manager (menu label "Task Manager", a
400x420 window): tabs Applications, Processes and
Performance (the Virtual Machines tab arrives with F3, f3-02), menus File
(New Task (Run...), Exit Task Manager), View (Refresh Now F5; Update Speed
High 0.5 s, Normal 1 s, Low 4 s, Paused) and Help, and a status bar
(processes, CPU usage, memory). Kernel data comes from call 70 and the
termination request from call 71 (docs/design/execution-abi.md, f2-33
amendment); window data comes from the desktop through the extension below.
Research and decisions are in docs/design/f2-acceptance.md, section F2-33.

The system pagefile is `C:\System\PAGEFILE.SYS`. Its allocated extents
persist across boots so startup does not recreate the full backing file; page
contents are treated as disposable and are initialized before use.

- **Applications:** columns Task and Status (Running / Not Responding from
  the desktop's heartbeat), sorted by title, PID and window id. End Task
  (Alt+E or Delete) sends END_TASK, the desktop's close path: CLOSE_REQUEST
  (the program may ask to save; no deadline while it answers PINGs); a window
  already closing or not responding gets "This program is not responding",
  whose End Task terminates it (G3, `docs/design/desktop-shell.md`). File >
  Shut Down opens the desktop's Shut Down dialog. Switch To (Alt+S, Enter, double-click) restores and
  focuses. New Task (Alt+N) asks the desktop to launch an admitted `/Programs`
  program (name or `/Programs/name`), so graphical programs get their channel and
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
desktop did not launch as `/Programs/tasks` receive TASK_RESULT with EPERM; no
other client gains any right.

| Opcode | Direction | Payload bytes | Fields |
| --- | --- | ---: | --- |
| WINDOWS_QUERY=15 | Client → desktop | 8 | u32 serial@4 |
| WINDOW_INFO=16 | Desktop → client | 32+n | u32 serial@4, index@8, count@12 (0..16), window_id@16, pid@20, flags@24 (1 unresponsive, 2 minimized, 4 focused, 8 internal, 16 closing), n@28; title@32, 0..63 printable bytes. count=0 is one message with every field zero. |
| END_TASK=17 | Client → desktop | 16 | u32 serial@4, window_id@8, pid@12; not both zero. window_id selects a window (pid, when nonzero, must match); window_id=0 ends a desktop child by PID: its windows get close requests, a windowless child gets SIGTERM. |
| SWITCH_TO=18 | Client → desktop | 12 | u32 serial@4, window_id@8 (nonzero) |
| LAUNCH=19 | Client → desktop | 12+n, or 16+n+a with an argument (f2-32) | u32 serial@4, n@8 (1..63); name@12, visible ASCII without spaces, matched against the `/Programs` menu list. Optional argument after the name: u32 a@12+n (1..160), then a bytes (32..255 except 127), one `argv[1]` for the program; a message of any other length is a protocol error. |
| TASK_RESULT=20 | Desktop → client | 16 | u32 serial@4, request@8 (15, 17, 18 or 19), i32 result@12: 0, the launched PID, or -errno (EPERM, ESRCH, ENOENT, EAGAIN when the reply queue cannot hold a whole list, or the spawn error) |

Window ids are assigned by the desktop at window creation (never 0) and are
not reused while the counter does not wrap. The desktop never sends a partial
window list. The desktop launches `/Programs/tasks` like Files (a graphical menu program:
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

`/Programs/ciuknote` is a Notepad-class editor on libdeskui (menu label "CiukNote",
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
`/Programs/files` (`program == 1`, kernel-stamped PID), the name must be `ciuknote`, and the
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
`--test=<its name>`, the top-bar clock is fixed at 12:00:00, the program sends checkpoint
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
Ciuki button in the top bar at (16, 16), Programs at (40, 50) and the `ciuknote` row of
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

Files is the CiukiOS file manager (`/Programs/files`), started from the Ciuki menu
("Files", with the Tango file-manager icon). Its window follows the Windows 98
Explorer layout in the CiukiOS colours.

- **Menus.** File (Open, Open with CiukNote, New Folder, New Text Document,
  Delete, Rename, Properties, Close), Edit (Cut, Copy, Paste, Select All,
  Invert Selection), View (Large Icons, List, Details, Sort by Name/Size/Type/
  Modified, Refresh), Go (Back, Forward, Up One Level, Address Bar, Computer,
  Programs, Documents, System, Games) and Help (Help Topics, About Files). Alt or F10 opens
  the menu bar; underlined letters work with Alt and inside open menus.
- **Toolbar.** Back, Forward, Up, Cut, Copy, Paste, Delete, Properties and
  Views (click to cycle views; the arrow lists them).
- **Address bar.** Shows drive-letter paths such as `C:\Programs` and
  `D:\Games`. Type either a drive-letter path or an absolute POSIX path
  such as `/system/assets` and press Enter; Esc restores the current location.
  Letter case does not matter on FAT volumes.
- **Left pane.** Places lists Computer, Programs (`/Programs`), Documents
  (`/Documents`), System (`/system`) and Games (`/Games`, when present).
  Folders starts at Computer, then C: and the other mounted drives, followed
  by their folders; [+] and [-] expand and collapse branches.
- **Computer.** The top location lists C: (the boot volume `/`) and D:..Z:
  for directory entries `/mnt/d`..`/mnt/z`. Up from any drive root returns
  to Computer. Drives open like folders; filesystem operations and Properties
  are disabled at Computer. In `C:\`, only `dev`, `mnt` and `tmp` are hidden,
  in both the list and the tree. The mount directory is presented as Computer.
  Programs continue to use the POSIX VFS paths.
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
  open in `/Programs/ciuknote` when it is installed; programs run directly.
- **Keys.** Enter, Backspace (up one level, as in Windows 98), Alt+Left/Right/
  Up, F2, F4 or Alt+D (address bar), F5, Del, Alt+Enter, Ctrl+C/X/V, Ctrl+A,
  Alt+F4.
- **Limits of this release.** Copies cannot keep dates and attributes (the F2
  system interface has no setter; moves on one volume keep them). Programs
  started from Files have no desktop window, except CiukNote, which the desktop
  starts on Files' request (f2-32, LAUNCH with a path). Drag and drop, removable
  media and the Recycle Bin come later.

The path presentation follows [Microsoft’s path naming documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file):
a drive root includes the backslash, and folder separators are backslashes.
Drive-relative forms such as `C:Programs` are rejected; bare `C:` opens its
root. Absolute slash paths retain their POSIX meaning, consistent with
[POSIX pathname resolution](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap04.html#tag_04_16).
Conversion preserves component case and normalises separators. Files resolves
the stored spelling through its existing FAT-aware canonicaliser. The VFS
metadata currently has no optical-volume type, so Computer uses the catalog’s
generic Tango drive icon; no optical type is inferred from names.

Files updates its title (`Files - C:\Programs`, or `Files - Computer`) as
locations change, following the window-caption behaviour described by
[Microsoft’s SetWindowText API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowtexta).
The existing protocol had only a creation title, so the additive v2
`SET_TITLE` request (opcode 23) carries a 32-bit length and 1..63 printable
ASCII bytes after the 4-byte header. It takes no descriptors or target ID
and changes only the sending client’s existing window. The desktop damages
the frame and taskbar; libdeskui queues the request. Files retries when its
queue is full and respects the existing 63-character caption limit.

Integration check (2026-10-12): the Computer view and the renamed ordinary
folders remain in place with protocol v2.1 resizing. Files opens a resizable
window (minimum client size 600x360), retains its drive-letter caption and
runs `files_layout` through the toolkit's `du_resize` layout callback. The
path rules above were rechecked against Microsoft's naming documentation;
case-preserving FAT lookup stays behind the display conversion, and Computer
remains a UI location rather than a VFS directory.

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

`/Programs/dosbox` (apps/dosbox) is an ordinary protocol-v2 client that owns one
DOS virtual machine through syscall 72 (execution-abi.md, F3-02 amendment);
the kernel renders the VM into the client's 640x480 surface and the desktop
composites it unchanged. The menu lists it from `/Programs` like any program and
passes it `--channel-fd=3`. `desktop --launch=<name>` starts one `/Programs` menu
program at shell start through the menu-click path; the `f3:vm-window` probe
uses it. Menu click offsets for the scripted cases follow the formula in
docs/design/f2-acceptance.md, section F2-33. Details: docs/design/f3-acceptance.md, section f3-02.

## Games (owner decision 2026-10-12)

Programs > Games (present with `/Programs/dosbox`) lists the pre-installed
shareware games of `apps/desktop/catalog.h` (`desk_games`): "Wolfenstein 3D
(shareware)" and "DOOM (shareware)", Tango `input-gaming` (the Games folder is `applications-games`). Each starts
`/Programs/dosbox --name=<game> <command>`; dosbox types the command at the DOS
prompt of its window (`apps/dosbox/autorun.h`: `\DIR\PROGRAM` becomes
`CD \DIR` and `PROGRAM`) once DOS is ready and the window has the focus. The
window VM boots the bundled games disk `C:\GAMES\CIUKIGAM.IMG`
(docs/build-and-run.md, "Pre-installed shareware games"). The Games row
follows the program rows in the top-aligned Programs submenu, so the
scripted cases' rows do not move (`tests/host/desktop/shell_menu.py` models
it; `--regenerate` leaves `f2-desktop.json` unchanged; the scratch `zz-ui.json`
copy of those cases is no longer tracked).

## NetSurf browser (F6)

The browser payload is installed as `/Programs/netsurf`. It is listed as
**NetSurf** ("Web browser") in the Programs menu, with the Tango
`internet-web-browser` icon in the menu, on its window and task button, and in
Files and Task Manager. Its program record (`apps/desktop/catalog.h`
`desk_applications`) grants the desktop window channel, so every launch route
(Programs menu, desktop shortcut, Run, Terminal, ShellExecute and the
GATE_BROWSER test route) starts `netsurf --channel-fd=3`; a launch without
that argument still uses the channel the desktop maps at fd 3. Its desktop window
provides a URL bar, navigation controls, status line and framebuffer content
area. It renders
local `file:` pages and `data:` URLs, including the supported HTML/CSS, GIF and
BMP content. Bookmarks and history live in the user's NetSurf profile under
`C:\Documents\NetSurf`; local directory browsing uses the Files picker.
HTTP and HTTPS use the native network service, with redirects, cookies and
gzip responses. HTTPS requires a valid RTC-set system clock dated 2026 or
later and ready kernel entropy. A recent build date does not qualify as an
RTC clock. TLS 1.2 and the pinned CA store are used; certificate exceptions
and revocation checking are unsupported.

JavaScript is off by default on the target since 2026-10-25: running
example.com's script made the Duktape engine read a null object, a fault still
open (`docs/design/netsurf-port-plan.md`). It is switched on from NetSurf's
options. Ctrl+L focuses the address without typing an `l` into it, and images
over one megapixel are shown through the scaled decoder.

"Secure connections are not ready yet" means the kernel returned EAGAIN.
The page estimates the remaining part of the first boot minute, without
promising readiness; after that minute it says the estimate has elapsed.
Reload retries entropy acquisition automatically: on any browser error
page the Reload button fetches the site address shown in the URL bar again,
without a new history entry and never replaying a form submission. The
browser stays responsive while waiting and never substitutes a weak seed.
An unavailable-entropy page indicates another entropy error. The clock page
asks you to set the RTC; certificate pages identify an untrusted or
malformed certificate, a hostname mismatch, or expired/not-yet-valid dates.
Protocol errors can mean the server does not support TLS 1.2. DNS, offline,
refused/timed-out connection and truncated-response pages describe separate
network failures. These pages and the "not ready" page end with the network
state (f6-22): no adapter, adapter stopped, no link (check the cable), no
address yet (waiting for a DHCP server), or the address with the outcome of
the browser's last name lookup (DNS ok, DNS lookups failing, DNS not tried
yet). Each browser exit appends one line, with a UTC time, to
`C:\SYSTEM\LOGS\NETSURF.LOG`: the exit reason, layout timings and the
network state (`network=no-address`, `dns-ok`, ...); the file starts over
after 64 KiB. The kernel's `[net] dhcp`, `[net] resolver` and `[net] stack`
lines in `BOOT.LOG` show the lease, the first DNS answer and the adapter's
frame counters.

## f2-30 decisions (2026-10-11)

Research precedes the shell implementation. The [POSIX spawn specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html)
inherits eligible descriptors; this repository's `ciuki_spawn` admits explicit
fd mappings and the POSIX subset expressly excludes `posix_spawn`. The shell
uses `ciuki_spawn`, rather than adding a conflicting SDK wrapper. Ordinary menu
children inherit the desktop's supervisor-created process group (flags=0), so
the existing group-local `kill` can send SIGTERM after a two-second close grace.
The crash gate retains NEW_GROUP and its original transport and pacing.

Choose a sorted `/Programs` scan, bounded to 16 regular i386 ELF candidates, excluding
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
MONOTONIC for deadlines and REALTIME in UTC for the top-bar clock; label the build-seed
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
validates these before image creation and adds `/Programs/desktop`, `/Programs/demo`,
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
tagline `Classic computing, one woof at a time.`, and MONOTONIC uptime as HH:MM:SS (hours modulo 100).
Window frames use a cream close button, a dark focused title and a muted
unfocused title. `Not responding` is the five-second heartbeat warning.
Pre-rasterised FreeType UI atlases (`apps/desktop/ui_fonts.h`, F2-63) render English UI
and titles; the demo renders incoming ASCII text with the monospace atlas, while the
protocol retains Unicode scalars.

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

Normal launch is `/Programs/desktop`; current supervisor stdio uses fd 0–2 and
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
fd 3, spawns `/Programs/demo` with CIUKI_SPAWN_NEW_GROUP, and closes its own duplicate
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

The names and measurements below record the earlier build, before the
2026-10-12 namespace amendment; current builds use `/Programs`.

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

## Graphics Demo (f2-52)

Programs > Accessories > **Graphics Demo** launches `/Programs/gfxdemo`.
Its three tabs show a 2D benchmark, an OpenGL cube and OpenGL tiles, drawn
by the software OpenGL subset of `sdk/libgl` (TinyGL) since f7-01
([opengl-software](design/opengl-software.md)); the f2-52 scenes were a
textured software cube with Flat/Gouraud shading and a bouncing Ciuki sprite
scene. Each page displays
client FPS and ms/frame. Pause freezes scene motion. The window resizes and
scales its 320×240 scene with nearest-neighbour sampling. The texture is the
same approved `/system/assets/ciuki-portrait.xrgb` conversion as the desktop.

Win32 programs can load `opengl32.dll` for the implemented OpenGL 1.1 software
subset. WGL contexts present through the existing desktop/GDI paths; this is
CPU rendering, not hardware acceleration. The advertised extension list
contains only implemented extensions. Direct3D through wined3d is planned
after the 1.0 Beta.

Run benchmark briefly fills the display, restores it after each sample, and
compares screen-size fills, 256×256 copies and 8×8 pattern fills per column.
Each result accumulates four timed batches (at least 8 ms, at most 64 operations
each) to measure fast copies with Ciuki's 1 ms clock; ms/batch is the average
batch duration. Accelerator measurements time synchronous scanout operations;
software measurements include drawing in RAM and CPU upload. Decimal MB/s
counts destination bytes in the active display bpp, once for a copy. These
columns measure different presentation paths, so the software result is not
a pure VRAM fill/copy measurement. Setup and desktop restoration are excluded.
Missing capability, quarantine, failed uploads, stopped clocks or a missing
kernel operation increment show **Unavailable**, never fabricated hardware
numbers. The user must focus the window while the benchmark runs.

The System info strip reports display dimensions/bpp, `driver`, completed
`ops`, `quarantined`, and average desktop present/composite time from its
existing two-second samples. Surface uploads use the CPU. When supported,
the compositor presents repeated non-solid rows of an enlarged software scene
with actual 2D screen copies, recovering from the RAM composite on failure.
This does not demonstrate hardware 3D or vsync; 3D acceleration remains F7.

The display grant remains exclusive to the desktop. Only its launch-bound
Graphics Demo endpoint may send `DESK_GFX_QUERY` (8 bytes, serial),
`DESK_GFX_BENCH` (12 bytes, serial and fixed index 0–5) or request the existing
system statistics. Replies are `DESK_GFX_INFO` (64 bytes, display and kernel
snapshot) and `DESK_GFX_RESULT` (40 bytes, result, bytes, ns and completed ops).
Samples are limited to one per 100 ms, with no client coordinates or register
access. The SDK adds `ciuki_display_accel_info(display, generation, &info)`
using the optional output of the existing grant-authorized QUERY syscall;
capability-only callers remain compatible. See [graphics-demo.md](design/graphics-demo.md)
for the research and measurement decisions. Host frame goldens and fake-surface
coverage are in `tests/host/desktop/test_gfxdemo.py`; no guest qualification is
claimed from them.

## Display mode service (f1-23)

The SDK's `<ciuki/display.h>` exposes `ciuki_display_query(info)` to any process
and `ciuki_display_set(mode_id)` to the supervisor's desktop. Initialize
`info.version=CIUKI_DISPLAY_VERSION` and `info.size=sizeof(info)`; the version-1
record is 612 bytes and contains current width/height/depth/pitch, VRAM bytes,
VBE BCD version, copied OEM/vendor/product strings, PCI adapter identity and
up to 32 LFB modes. Query/set return 0 or **negative errno directly**. The
existing syscall-67 framebuffer function and its 48-byte wire layout remain;
its C record is now `struct ciuki_framebuffer_info`.

Control Panel must ask the desktop to switch through its existing authenticated
v2 settings channel: `DESK_SETTINGS_DISPLAY` (=3), 24-byte `DESK_SETTINGS`,
serial at byte 4, request at 8, uint16 mode id encoded as u32 at 12, zeros at
16/20. Only the Control Panel launched by this desktop is accepted. The desktop
returns `DESK_TASK_RESULT` with request `DESK_SETTINGS` and 0 or negative errno.
Send this request instead of calling set from the applet process or writing an
unconfirmed mode into the configuration file. The UI belongs to f2-37.

Each desktop frame polls the readable u32 change counter on its display
fd (`read(fd,&counter,4)`). A committed mode or restore changes it. The desktop
queries the active mode, allocates/maps a replacement XRGB8888 output surface,
releases its previous mapping/fd, clamps windows and the pointer to the new work
area, sends CONFIGURE to resizable clients and lays out bars/icons again.
Client minimum/step constraints remain binding; an oversized fixed/minimum-size
client is positioned with its title reachable and clipped by the compositor.
The wallpaper cache is rebuilt for the new geometry and a full screen is
presented. If output allocation fails, the old surface remains private and
the kernel's independent 10 s frame watchdog restores the preceding mode.

`/system/settings/desktop.cfg` gains `display_mode=<decimal VBE id>`; zero
keeps the loader default. At startup the desktop tries it once only if it is
still listed. Settings reloads preserve the confirmed mode; runtime changes
use the settings-channel request above. The desktop saves the selected id only
after a complete successful frame in that mode, and saves a restored mode after
its recovery frame. The loader never reads this setting, so the next boot
starts with its ordinary boot-mode selection before applying a qualified saved
preference.

The ABI, transaction/firmware ownership, source research and qualification
matrix are specified in the [f1-23 acceptance amendment](design/f1-acceptance.md#amendment-f1-23--display-inventory-and-runtime-vbe-modes-2026-10-12).
Host coverage: `tests/host/test_display_modes.py`, `display_test.c`, the firmware
worker cases in `v86_test.c`, bootinfo cases in `kernel_lib_test.c`, and RGB565
reference fixtures in `fbdev_test.c`. Real QEMU/T23/E500 mode changes remain
lead-owned qualification.


## AutoPlay for removable media and CDs (F2-51)

The desktop's one-second media polling also queries `MEDIA_GET_VOLUME` for
D: through Z:. A new mount generation opens an internal AutoPlay window with
the volume label and letter. Multiple arrivals are queued in letter order;
eject, surprise removal or replacement cancels both the dialog and any
queued confirmation for the old generation. Failed observations preserve
the previous snapshot and block confirmation until the media can be checked. The desktop refreshes the snapshot before executing
an accepted choice. Media already present at desktop startup are arrivals.

The actions are **Open folder to view files**, **Play music** for WAV, MP3,
OGG or FLAC at the root or in a case-insensitive Music folder, **View pictures**
for PNG, JPEG, BMP, GIF or XRGB at the root or in Pictures, **Run program**
when declared by AUTORUN.INF, and **Do nothing**. When AUTORUN.INF names a
program the medium can start, **Run program** is selected by default
(fx-autorun, below). Music Player receives the
containing directory and builds its playlist through its existing directory
loader; Image Viewer opens the first matching picture (path order).
The Music Player's existing directory loader includes subdirectories.
The UI uses the current shell theme and beveled controls. Up/Down/Tab selects
an enabled action, Space toggles the always checkbox, Enter accepts, Escape
or Cancel dismisses. Closing the window does not remember a choice.

AUTORUN.INF is read only from the medium's root (case-insensitive name), with
a 4096-byte bound and at most 2048 entries per directory inspected. The
case-insensitive `[autorun]` section recognizes `open`, `icon`, and `label`;
other sections and keys are ignored. Labels display up to 32 printable ASCII
characters. Program paths stay on that medium: parent traversal, explicit
drive names, control characters and commands DOS Box cannot type are rejected.
DOS executable paths with spaces are disabled because its typist cannot quote
the program token (quoted paths without spaces are supported).
[Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
defines the extended signature offset in the MZ stub. DOS COM/BAT and MZ
EXE entries are offered, including small DOS headers described by the
[Microsoft MS-DOS Encyclopedia](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/section2/).
Windows programs (PE, and 16-bit Windows NE such as InstallShield 5 setup
stubs) start through the Win32 personality since fx-autorun (below); LE/LX
and OS/2 NE executables are MS-DOS programs. A missing, unreadable or
unsupported entry stays visible and disabled with its reason ("Run
MISSING.EXE (not found)"). Explicit DOS commands such as
`D:\SETUP\INSTALL.COM /S` type three lines: `D:`, `CD \SETUP`, then
`INSTALL.COM /S`, setting the drive and working directory before execution.

**Windows CD autoruns (fx-autorun, 2026-10-27).** Research:
[Microsoft, Autorun.inf Entries](https://learn.microsoft.com/en-us/windows/win32/shell/autorun-cmds)
(`open=[exepath\]exefile [param1 [param2] ...]`, a bare name in the drive's
root, a path relative to the root otherwise; `icon=` represents the drive;
without `shell=` the drive's default menu item is "AutoPlay", which launches
the `open` program, and the default command also runs on double-click; the
XP SP2 `action` entry "always appears as the first option in the Autoplay
dialog and is selected by default"),
[Microsoft, Enabling and Disabling AutoRun](https://learn.microsoft.com/en-us/windows/win32/shell/autoplay-reg)
("Users can manually suppress AutoRun by holding down the SHIFT key when they
insert the CD-ROM"; the drive's shortcut menu includes an **AutoPlay**
command; "To run AutoRun manually, either right-click the drive icon and
select AutoPlay from the shortcut menu or double-click the drive icon"),
[Microsoft, Creating an AutoRun-Enabled Application](https://learn.microsoft.com/en-us/windows/win32/shell/autoplay-works)
and the Windows 98 Resource Kit (App. G, `wrkappg_1.htm`: "To bypass Autorun
when inserting a compact disc: Press SHIFT while inserting the compact
disc"; Ch. 17 and 24: AutoPlay is enabled by default and shows the disc's
own banner program). On Windows 98 AutoRun starts `open=` at once; CiukiOS
keeps the owner's chooser (f2-51) and makes that program its first, selected
choice, so Enter starts it and **Always do this** remembers it.

- **Classification** (`apps/desktop/exe_kind.h`, the rule of
  [Wine 10.0](https://gitlab.winehq.org/wine/wine/-/tree/wine-10.0)
  `server/mapping.c` `get_image_params`, field offsets from `include/winnt.h`
  `IMAGE_OS2_HEADER`): "PE\0\0" at e_lfanew is Win32; "NE" with target OS 2
  (Windows, NE+0x36) is 16-bit Windows; NE for OS/2/BOSS, LE/LX and plain MZ
  are MS-DOS programs (Wine's `CreateProcessInternalW` hands all of them to
  its VDM). AutoPlay reads the header at e_lfanew also beyond its 4 KiB
  sniff, up to 64 KiB into the file.
- **Windows program**: launched with a record (target on the disc, the
  `open=` arguments, current directory = the disc root) through the desktop's
  launch worker, the same path as shortcuts; an NE setup stub reaches the
  kernel's InstallShield 5 admission there
  ([installshield5-bootstrap.md](design/installshield5-bootstrap.md)). While
  the worker runs, further arrivals wait; its result is recorded as
  `[desktop] autoplay run kind=win32|win16 result=N`. **Always do this** is
  saved only after the program started. A refused image (`-ENOEXEC`) is
  explained by the Win32 personality's own dialog ("is not a valid Win32
  application", "is a 16-bit Windows program ..."); any other failure opens
  "AutoPlay: NAME could not be started. REASON (error N)". MS-DOS programs
  keep the typed DOS Box route described above.
- **Shift**: Shift held when the new medium is observed, or when its content
  probe completes, skips AutoPlay for that insertion
  (`[desktop] autoplay skipped drive=D reason=shift`); the drive is mounted
  and usable.
- **ICON=**: the dialog and the drive show the disc's icon (fx-exeicons).
- **My Computer** (Files): a drive whose AUTORUN.INF names a startable
  program has **AutoPlay** as the first, bold item of its shortcut menu, and
  double-click/Enter on the drive runs it, as on Windows; **Open** still
  browses the disc, and the folder tree and address bar never run it. The
  label cache (`ap_drive_info`, read on the Files scan worker when the
  medium or its AUTORUN.INF changes) carries the launch record; the verb
  sends it to the desktop as an ordinary shortcut launch.
- **Desktop launch routing**: Start > Run, Files and My Computer, desktop
  shortcuts, the Startup folder and AutoPlay share `desk_w32_identify`: PE
  and 16-bit Windows programs go to the Win32 personality, MZ DOS programs,
  `.com` and `.bat` to the DOS Box. The Win32 launcher names the image in
  Windows form (`D:\SETUP.EXE`, `C:\Program Files\...`) in argv[0] and the
  command line, as Explorer does, so GetModuleFileName, the IS5 admission's
  sibling lookup and the bootstrap's `-f`/`-x1` operands use the disc's drive
  whatever the current drive is.

Host coverage: `tests/host/desktop/autoplay_test.c` (PE and IS5 NE autoruns,
header past the sniff, deferred arrivals and saved choice, refusal and
failure wording, missing program, Shift at arrival and at probe end, LE and
OS/2 NE as DOS, the drive verb record), `files_ui_test.c`
(`autoplay_verb_tests`), `win32_adapter_test.c` (routing of PE, NE IS5 stub,
other NE, LE/LX, MZ, .com, .bat; Windows names), `win32_launcher_test.c`
(argv[0] and command line), `kernel_core_test.c` (the Win16 refusal record)
and `v3_test.c` (its dialog text).

**DOS Box window (2026-10-25):** `exit` at the DOS prompt closes the window;
a program that ends within 10 seconds leaves its text on screen with the title
"Finished - <name>"; Alt+F4 closes the window without hanging the system.

**Current runtime boundary:** DOS Box still supplies only its private disk
image to the guest (`src/kernel/vm/dosvm_disk.c`). This change implements the
launch request and command typing, but does not expose host removable drives
to the DOS guest. Running a DOS program directly from that medium needs that
VM drive integration; host launch tests do not establish guest execution.

There is no ICO decoder in the current image or Tango store readers. The
`icon` entry is parsed but does not replace the standard Tango drive icon.
Files uses the AUTORUN label when available, otherwise the mount-time volume
label, while keeping the original drive name for navigation. This is the
label-only fallback permitted by the directive.

**Always do this for this device** saves `Action` (integer 0..4) and `Label`
in `Desktop\AutoPlay\<volume id>` through libreg. FAT IDs combine the
nonzero BPB volume serial and volume size. ISO IDs fingerprint the PVD volume
and volume-set identifiers, volume space size and creation/modification dates.
These identify a filesystem volume, not a USB port or physical device; cloned
volumes can share choices. An unavailable ID disables remembering. An
unavailable remembered action falls back to the dialog; launch or persistence
errors are reported. Control Panel's **AutoPlay** page lists the remembered
volume/action pairs and **Ask every time** removes the selected choice.
Registry operations reopen their root, so edits in Registry Editor and Control
Panel are observed on the next arrival. libreg's existing concurrent-writer
limits apply.

The optional media sound callback selects `device_connect` and
`device_disconnect` from the installed system sound keys. The current F4-06
sound set contains neither event (F4-12 is absent here), so insertion/removal
is silent until those events become available. AutoPlay adds no sound assets.

Research and implementation decisions:
[Microsoft AUTORUN.INF entry reference](https://learn.microsoft.com/en-us/windows/win32/shell/autorun-cmds)
defines open paths/parameters, icon metadata and label truncation. CiukiOS
implements the owner's requested chooser and per-volume remembering rather
than adopting all version-dependent Windows AutoRun policies.
The [Microsoft FAT specification (original white paper mirror)](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
defines BPB serial/label fields at 39/43 for FAT12/16 and 67/71 for FAT32,
qualified by the extended boot signature 0x29. These fields are copied during
the existing validated mount; the label is the BPB label.
[ECMA-119, section 8.4](https://www.ecma-international.org/wp-content/uploads/ECMA-119_4th_edition_june_2019.pdf)
defines the primary volume descriptor fields used for the CD label and
fingerprint. The fingerprint is a CiukiOS choice, not an ECMA volume UUID.
Neither query introduces disk I/O in the desktop event loop beyond the bounded
content scan. The original USB removal query and ABI remain unchanged.

Host coverage: `tests/host/desktop/test_autoplay.py` runs the production shell
against fake mounted files and real libreg storage under ASan/UBSan, covering
music, CD autorun, pictures, saved choices across insertions/letters, Windows
autoruns (fx-autorun), eject during confirmation, replacement and queued media,
parser bounds, unavailable saved actions, launch failures and optional sound
notifications. The Control Panel desktop harness also checks listing, resetting,
empty state and registry-read errors. Kernel file tests verify the new copied
query, metadata, version/size/reserved/pointer validation and stale CD handling.
No QEMU or hardware evidence is claimed here.

## DOS full screen (2026-10-14, F3-20)

A DOS window's taskbar context menu offers **Full screen — Alt+Enter**. The
kernel also recognizes Alt+Enter before guest input, in a window and in full
screen. Alt+Tab returns to the desktop. The default keeps DOS running in its
window; `/Programs/dosbox --pause-background` instead pauses it until focus or
full-screen entry resumes it. The window and taskbar entry survive throughout.
Its retained surface says “Running full screen — Alt+Enter to return”. A refused
entry reports its reason temporarily, while the DOS session remains available.

Since fx-dosfull (owner decision 2026-10-27) a DOS game starts full screen by
itself at its first graphics mode, as Windows 98 does; its text screens and the
DOS prompt stay in the window. Alt+Enter, the toolbar's Full screen and the
menu entry are remembered per program as the Screen tab's Usage (DOS Box
Properties, or Files > Properties of the program). A refusal (QEMU's standard
VGA) keeps the game in its window without a message and changes no setting.
See `docs/design/dos-program-properties.md`.

Presentation/acceleration return EBUSY while the display grant is suspended;
the desktop keeps servicing its channels and input. This intentional pause does
not trigger the desktop frame watchdog. Return increments the existing display
change counter, remaps the confirmed framebuffer generation and forces a full
repaint through the existing display notification path. Guest VBE mode setting
remains unavailable. The kernel's independent watchdog handles VM death,
owner death and a stationary guest instruction pointer for 15 seconds; the last
condition excludes cooperative idle/suspension and is a recovery heuristic, not a general proof that a game has hung.

The `vm-fullscreen-qemu-t23` and `vm-fullscreen-cirrus-qemu-t23` rows run the bundled DOOM demo, uses physical
Alt+Enter input for entry and return, then freezes the returned VM at its normal
worker checkpoint for a second state-transfer round trip. The desktop's runtime
`--test=vm-fullscreen` option fixes its clock for this observation. The runner
requires changing full-screen demo frames and exact equality of both entire return screendumps, with no masks or ignored
pixels, as well as mode and cleanup records. This row has not been run by the
implementation agent; standard VGA, Cirrus and the T23 remain lead validation.

F2-74 fixes removable-media notification repainting over client windows and
adds explicit close dismissal. The tray's removable-media icon accepts either
mouse button; its menu and Files show the volume name with the drive letter.
Busy removal queries `MEDIA_GET_BLOCKERS` and shows program names and PIDs.
Safe USB removal retains an absent-volume result so the later unplug does not
warn again. See [the implementation and validation record](validation/f2-74-removable-media.md)
for the additive query, replay captures and pending guest/hardware qualification.

## First boot (f9-05)

After a Setup CD installation, `C:\SYSTEM\SETUP.STATE` carries `phase=installed`
and `firstboot=pending`. On a normal boot the desktop (`apps/desktop/firstboot.c`,
`firstboot_native.c`) runs a finishing wizard before the ordinary session; the
placement is inside the desktop so it uses the grants the desktop already holds
and needs no new supervisor authority. Without the file, or with
`firstboot=done`, nothing runs and the canonical image boots as before.

Steps run in order and each is bounded: `accel`, `sound` (10 s), `network`
(10 s, observes the network service snapshot; never blocks), `games`
(registration by catalogue identity, idempotent), `entropy` (5 s, one-byte
`getentropy`; EAGAIN means not ready), `clock`, `summary`. Each result is one
`step=<name> result=<ok|skipped|failed>` line appended to `SETUP.STATE` with
`fsync`; a restart resumes at the first step without a result. Completion
appends `firstboot=done`; the file is never deleted. The kernel log gets
`[firstboot] step=<name> result=<r> ms=<n>`.

The accelerator message only reports what the kernel did during this boot:
"Hardware acceleration is on" means the display driver registry reports
acceleration and the attempt marker `C:\system\settings\accel.cfg` is empty.
"Software rendering is in use" means the driver reported none, quarantined the
engine, or the marker was left non-empty by an earlier hang. Control Panel >
Display offers "Retry acceleration after restart", which only empties that
marker. The wizard decides nothing per machine model.

Records: `C:\SYSTEM\SETUP.LOG`, `C:\SYSTEM\HARDWARE.TXT`, `C:\SYSTEM\SETUP.STATE`
and `C:\SYSTEM\BUILD.TXT` (source commit, kernel ELF SHA-256, build manifest id,
build date; the image SHA-256 is on the release page, since an image cannot
contain its own hash). The Setup CD rescue page lists disks, views the hardware
report (which quotes `BUILD.TXT`), and exports logs and the report to
`\CIUKIOS-RESCUE-<date>\` on the first USB FAT volume; it never writes to an
ATA disk.

## System Tools: ScanDisk and System Configuration (G14, 2026-10-24)

Programs ▸ Accessories ▸ System Tools has **ScanDisk** (`/Programs/scandisk`)
and **System Configuration** (`/Programs/msconfig`); the design and the
Windows 98 sources are in
[startup-recovery](design/startup-recovery.md).

- ScanDisk lists the drives, Standard or Thorough, "Automatically fix
  errors", "Save lost fragments as files" and "Copy cross-linked files";
  Start checks one drive at a time and shows ScanDisk Results (also in
  `C:\SYSTEM\LOGS\SCANDISK.LOG`). A fix a drive in use needs is scheduled for
  the next start. Files ▸ drive Properties ▸ Tools ▸ Check Now opens it for
  that drive.
- System Configuration: General (Normal, Diagnostic = the Startup menu once,
  Selective = seven driver classes), Advanced (`BOOT.CFG`: Enable Startup
  menu, Disable ScanDisk after bad shutdown, ScanDisk without asking, serial
  log), Automatic Skip Driver (classes skipped after two failed starts, with
  Enable) and Tools (Registry Checker backups and Back Up Now; System File
  Checker scan, Update and Restore). General and Advanced take effect at the
  next start.
- At start the desktop shows what happened: Safe mode (corner labels and a
  notice), a previous start that did not complete, driver classes the
  Automatic Skip Driver agent skipped, ScanDisk results, and a registry root
  Registry Checker restored; it makes the day's registry backup.
