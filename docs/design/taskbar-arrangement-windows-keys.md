# G4: taskbar menu, window arrangement and Windows-key shortcuts

- **Written:** 2026-10-23 for directive G4 (`docs/directives/g1-g6-os-gaps-batch-1.md`),
  inventory rows WM-11, WM-12, WM-13, KB-5, KB-8, KB-9, KB-12, KB-13 and SM-9
  (`docs/design/os-feature-inventory.md`). The behaviour below was written down
  from the sources before the code; the rendering rows are in
  `docs/design/desktop-rendering-architecture.md` (fx-15, rows 31-35).

## Sources

| Key | Source |
| --- | --- |
| RK-G2 | *Microsoft Windows 98 Resource Kit*, Appendix G, "General Keyboard-only Commands" (`wrkappg_2.htm`): Alt+Esc, Alt+Space, Alt+F4, Alt+F6 ("switch to next window in application"), Alt+Tab, Ctrl+Esc, "Ctrl+Esc, Esc: move focus on taskbar so you can use Tab and then Shift+F10 for context menu, or use Tab and arrow key to change tasks, or use Tab to go to desktop", Alt+M ("when focus is on taskbar or desktop, minimize all windows and move focus to desktop"), Alt+S ("when no windows are open and no items are selected on desktop, display Start menu"), Shift+F10, F10 |
| RK-G4 | same book, "Microsoft Natural Keyboard Keys" (`wrkappg_4.htm`): Application key or Shift+F10 (pop-up menu for the selected object), Windows key (Start menu), Win+F1 (Help), Win+Tab (cycle through taskbar buttons), Win+E (Explorer), Win+F (Find files or folders), Win+Ctrl+F (Find computer), Win+M (Minimize All), Shift+Win+M (Undo Minimize All), Win+R (Run), Win+Break (System Properties) |
| MS-casc | [CascadeWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-cascadewindows): a top-level window with `WS_EX_TOPMOST` or `WS_EX_TOOLWINDOW` is not arranged; `MDITILE_SKIPDISABLED`; `MDITILE_ZORDER` ("arranges the windows in Z order"; otherwise the order of `lpKids`, the Z order preserved); "causes all maximized windows to be restored"; the return value is the number of windows arranged |
| MS-tile | [TileWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tilewindows): `MDITILE_HORIZONTAL`/`MDITILE_VERTICAL`, the same exclusions, maximised windows restored |
| MS-icon | [ArrangeIconicWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-arrangeiconicwindows): arranges the minimised child windows of a parent (also the desktop); returns the height of one row of icons |
| MS-sys | [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand) and the window menu (Restore, Move, Size, Minimize, Maximize, Close; the system menu opens from the caption icon, a double click on the icon closes) |
| MS-menu | [Menus: mnemonics](https://learn.microsoft.com/en-us/windows/win32/menurc/about-menus): a menu item's access key is the letter after `&`; Windows 98 draws it underlined; a letter with a single match runs the item, with several it moves the selection |

The Windows 98 behaviours quoted are the checked ones; where the books do not
fix a detail (the cascade size and offset, the tile grid) the choice is written
in "Decisions" below.

## Behaviour implemented

### Taskbar menu (WM-12)

A right click on the empty part of the taskbar (the bottom bar between and
after the task buttons) or on the empty part of the top bar (not the Start
orb, not a tray icon: those keep their own menus) opens the taskbar menu at
the pointer, above the bar when it would leave the screen:

`Cascade Windows`, `Tile Windows Horizontally`, `Tile Windows Vertically`,
`Minimize All Windows`, separator, `Properties`. After an arrangement the menu
changes as in Windows 98: after Minimize All the item reads `Undo Minimize All`
(replacing Minimize All Windows); after a cascade or a tiling `Undo Cascade` or
`Undo Tile` follows Minimize All Windows. The arrangement items are disabled
when no window can be arranged. `Properties` opens the Control Panel until the
taskbar properties page exists (G20 owns Taskbar Properties; the Toolbars
submenu is G20's too). A right click on a task button is still the window's own
menu (Restore, Move, Size, Minimize, Maximize, Close).

### Arrangement (WM-12, WM-13)

Eligible windows (`desk_arrangeable`): they have a task button, are not
minimised, owned, built-in dialogs (Run, About, Shut Down ...) or closing; a
window with an owned window open keeps its place (the owned window stays
centred on it); a Win32 popup or tool window or a topmost window is left
alone (MS-casc). Nothing is arranged while a shutdown is in progress. Work
area: between the two bars; every result is clamped into it by `desk_move`
and `desk_resize` (size limits and steps of the program apply). A window that
cannot be resized keeps its size and takes the cell's corner. Maximised and
shaded windows are restored first (MS-casc, MS-tile).

Decisions where the sources are silent:

* **Cascade:** every window at three quarters of the work area; offset by one
  caption height (24 px) down and right; the sequence starts over at the top
  left when the next offset would leave the work area. The stacking is not
  changed (MS-casc: "preserves the Z-Order"), so the window in front stays in
  front and ends at the lower right; the window at the back starts at the top
  left.
* **Tile:** the window in front takes the first cell. Up to three windows
  form one strip (Horizontally: stacked one above the other; Vertically: side
  by side); more fill a grid of ceil(sqrt(n)) by ceil(n/ceil(sqrt(n))) cells,
  the larger count on the side of the tiling direction; the windows of a short
  last line share its full width or height. Cells touch; no gaps.
* **Undo:** one record, replaced by the next arrangement. It stores, per
  window id, the frame, the restore frame, and the minimised, maximised and
  shaded states, and undoes only the windows that still exist. It does not
  disappear when the user changes a window by hand.

### Minimize All and Show Desktop (WM-13)

`desk_minimize_all` (Win+M, Alt+M on the desktop or taskbar, the menu item)
sends every window with a task button that is not owned to the taskbar at
once, without the caption animation, together with the native dialogs it owns;
nobody is active afterwards. `desk_arrange_undo` (Shift+Win+M, the menu item)
brings them back and re-activates the window that was active.
`desk_show_desktop` (Win+D) minimises everything, and when nothing is left to
minimise brings the windows back if the last arrangement was Minimize All. The
Quick Launch "Show Desktop" button is G20's (Quick Launch does not exist
yet); it will call `desk_show_desktop`.

### Keys (KB-5, KB-8, KB-9, KB-12, KB-13)

* **Windows key (either):** a lone press and release opens or closes Start; any
  chord does not. Chords: `D` Show Desktop, `M` / `Shift+M` Minimize All / Undo,
  `E` Computer (Files), `R` Run, `F` Find, `Ctrl+F` Find Computer, `Tab` /
  `Shift+Tab` move the taskbar's keyboard focus, `Break` System Properties
  (`--panel=system`), `F1` Help (the Help program, else the shortcut list). The
  keys of a chord are not delivered to the program, repeats and releases
  included. Other Win+key combinations still reach the program.
* **Find (G7 integration):** Win+F and F3 on the desktop call the shared
  `desk_shell_find` entry point to launch G7's Files criteria with `find:`.
  Win+Ctrl+F keeps the separate Find Computer placeholder.
* **Taskbar keyboard focus (KB-13):** Win+Tab starts at the button after the
  active window; Ctrl+Esc opens Start and Esc closes it leaving the focus on
  the Start button. Tab and Shift+Tab walk Start and the task buttons and leave
  to the desktop past either end, Left/Right/Home/End also move, Enter or Space
  presses the button (restores a minimised window, minimises the active one, as
  a click does), Menu key or Shift+F10 opens its menu, Esc gives the focus back
  to the window that had it. A click or any window taking the focus ends it. The
  taskbar being active, no window is. A focus cue (the theme's focus ring, the lit
  Start orb) shows the button.
* **Alt+M** (desktop or taskbar in focus) minimises all windows; **Alt+S** with no
  window open and nothing selected opens Start.
* **Alt+F6** brings the next window of the same program (same process or
  same launched application) to the front; windows cycle by stacking.
  Ctrl+F4/Ctrl+F6 and Alt+Hyphen need MDI (WM-15), which is outside this
  directive.
* **Alt+Tab / Alt+Shift+Tab / Alt+Esc / Alt+F4 / Alt+Space** were already
  complete (KB-1 to KB-3, WM-11) and are covered by `taskbar_test.c`: Alt+F4 on
  the desktop opens Shut Down.
* **Application key / Shift+F10 on the desktop (KB-8):** the context menu of
  the selected icon, else the desktop's menu. In the taskbar focus: the menu of
  the button. Windows' own widgets keep handling the key themselves (libdeskui).

### Menu access keys (SM-9, WM-11)

Fixed shell menus carry Windows 98 access keys (Start: Programs, Documents,
Settings, Help, Run, Sh*u*t Down; the window menu: Restore, Move, Size,
Mi*n*imize, Ma*x*imize, Close; the desktop, icon, taskbar and right-drag menus
likewise); the letter is underlined. A letter with one match runs the item
(a submenu opens), with several moves the selection; menus without access keys
(Programs and the lists built from installed software) jump to the next entry
starting with the letter.

### Caption icon (WM-11)

In the themes whose caption draws an icon (Ciuki Blue; Platinum has none) a click
on the icon opens the window menu under it; a second click within the double-click
time closes the window; the click that only dismisses the open menu does not open
it again.

### Win32 (WM-12)

`CascadeWindows`, `TileWindows` and `ArrangeIconicWindows` are exported by the
original USER (`sdk/win32/user32/arrange.c`) with the semantics of MS-casc,
MS-tile and MS-icon and the same geometry as the taskbar menu. They move the
windows of the calling process only (other processes' windows are not
reachable); desktop minimised windows live on the taskbar, so
`ArrangeIconicWindows(desktop)` only reports the row height.

### Win32 maximised state and track sizes (WM-7, WM-8)

`DESK_V3_PLACE` and `DESK_CONFIGURE` carry a state word: 0 normal, 1 minimised,
2 hidden, and now **3 maximised**. `ShowWindow(SW_MAXIMIZE)` sends 3 and the
desktop zooms the window to the work area (it remembers the previous frame);
a zoom started by the desktop (title button, double click, taskbar menu) is
reported with state 3, so the program sees `IsZoomed` and `WM_SIZE` with
`SIZE_MAXIMIZED`; restoring reports `SIZE_RESTORED`. Minimising or hiding a
maximised window keeps the maximised state (Windows does); only state 0
restores the window to its previous place. The new `DESK_V3_LIMITS`
(client to desktop, 16 bytes: minimum and maximum track size of the client
area, 0 = default) carries what `WM_GETMINMAXINFO` returned in
`ptMinTrackSize` / `ptMaxTrackSize` (outer sizes; user32 converts them with the
3 px border and 24 px caption); user32 asks the window on every placement and
sends the packet only when the values changed. The desktop keeps them in
`v3_limits`, applies them to sizing and to the maximised window, and keeps them
across placements. A malformed packet (minimum above maximum, more than 2048)
does not parse, as for every other v3 packet. `ptMaxSize` and `ptMaxPosition`
are not honoured.

## Keyboard hardware paths

The Windows keys reach the desktop as set-1 positions `0x15b` (left) and
`0x15c` (right), the Application key as `0x15d`, Pause/Break as `0x200`, from
every producer: PS/2 set 2 (`E0 1F`, `E0 27`, `E0 2F`, the 8-byte Pause
sequence; `src/kernel/drivers/i8042.c`), the firmware set-1 decoder (`E0 5B`,
`E0 5C`, `E0 5D`; `src/kernel/vm/fwinput.c`) and USB HID usages `0xE3`, `0xE7`,
`0x65`, `0x48` (`src/kernel/usb/usb_hid.c`). The tests of those three paths
assert the codes.

## Not delivered (gaps)

See `build/codex/g4-report.md`.

### fx-28 fixed-size windows

Direct Display Properties launches now register a fixed-size native window.
Native arrangements already move such windows without resizing. Win32
`CascadeWindows` and `TileWindows` now add SWP_NOSIZE without WS_THICKFRAME;
sizeable test fixtures explicitly request a sizing frame. Added mixed fixed and
sizeable regression coverage and an appended fx-15 rendering row. The Control
Panel browser opens Display in a separate fixed-size applet through DESK_LAUNCH,
retaining its own sizing style. Monitor Settings launches Power Management by the
same path. The shell authorises only authenticated Control Panel clients and the
exact display/power arguments; other programs and panel arguments are refused.
