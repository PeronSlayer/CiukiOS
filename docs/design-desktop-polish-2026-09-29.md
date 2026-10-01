# Design: desktop polish, Control Panel, Recycle Bin, fonts — 29 September 2026

Status: in progress. Owner requests, before phase 2 M4:
- **Files:** a Refresh button, and integration with the whole system.
- **Desktop:** customizable, with folders on it and a Recycle Bin (with its
  management).
- **Context menus:** everywhere, according to the context.
- **Visual polish:** hover on menu items, spacing and wording.
- **Fonts:** 7-10 free fonts from the internet, system font management, and
  adding new fonts.
- **Independent windows:** every window that opens (e.g. a file's
  Properties) is a window of its own that can be managed on its own.
- **Control Panel:** as in Windows, to change every system option, manage
  devices and install new drivers (roadmap phase 3).

## 1. Windows of their own (host v2)

Today a module has one window, and its dialogs are drawn inside it. The
host (`src/com/shell_apps.inc`) changes as follows.

- **Window ids.** 0-11 stay the shell's. 12-14 are the modules' main windows:
  - 8 Files, 9 Tasks, 12 CiukNote;
  - 13 Control Panel, 14 Devices;
  - 15-31 are a pool of windows that modules open at run time.
- **Opening and closing.** A module opens a window with WIN_OPEN (size,
  title in its memory, flags: dialog frame, modal) and closes it with
  WIN_CLOSE. The shell frames, moves, stacks and closes these windows like
  any other.
- **Events name their window.** `HOST.window` names the window of every
  PAINT, KEY, MOUSE and CLOSE event.
- **Dialogs become windows.** Toolkit dialogs, message boxes and file
  dialogs are such windows. A modal one blocks its owner (a click there
  raises the dialog); Properties is modeless.
- **Closing a module's main window** closes its other windows and unloads
  it.

## 2. Desktop module (DESKTOP.APP)

The desktop surface becomes a module, as Explorer is in Windows 95. The
shell keeps the top bar, the taskbar and the wallpaper.

- **Icons.** Programs, Files, DOS, Display, Control Panel, Recycle Bin and
  the removable drives (each can be hidden), plus the contents of
  `C:\DESKTOP`.
- **Using icons.** Select, double-click, Enter, F2 rename, Del (to the
  Recycle Bin), and drag to arrange or drop onto a folder or the Recycle
  Bin. Positions are saved.
- **Context menus, according to the context:**
  - desktop background: arrange, refresh, paste, new folder or text
    document, properties;
  - an icon: open, cut, copy, delete, rename, properties;
  - the Recycle Bin: open, empty;
  - the taskbar: Task Manager, show the desktop, Control Panel;
  - a window's title bar: restore, minimize, maximize, close.

  Menus are drawn in an overlay above every window.
- **Hover.** The shell sends pointer moves without a button (MOUSE_HOVER)
  to the window under the pointer, or to the overlay. Menus highlight the
  item under the pointer.

## 3. Recycle Bin

- **Storage.** `C:\RECYCLED` holds the deleted items, renamed `Dnnnn.ext`,
  and versioned `INFO2.DAT` records with the long original path, date and size.
- **Delete.** In Files and on the desktop, Delete moves items to the
  Recycle Bin; Shift+Delete deletes them permanently.
- **Managing it.** It opens in Files as a special folder: Restore, Delete,
  Empty Recycle Bin, Properties.
- **Clipboard.** The file clipboard is shared by Files and the desktop
  through `\SYSTEM\UI\CLIPBRD.DAT`.

## 4. Control Panel (CONTROL.APP, DEVICES.APP)

A window of applet icons; each applet opens its own window.

| Applet | Options |
| --- | --- |
| Display | resolution and colour depth, wallpaper |
| Appearance | colour schemes, the desktop icons |
| Fonts | installed fonts with a preview, the system font, Install New Font |
| Sound | event sounds, AC'97 master and PCM volume |
| Mouse | pointer speed, double-click speed, button swap |
| Keyboard | repeat delay and rate |
| Date and Time | date, time, calendar (RTC) |
| System | version, CPU, memory, VM manager |
| Device Manager | the devices by class (PCI bus, input, video, audio, disks, ports) with properties; hardware scan (HWDETECT) |
| Drivers | installed drivers (enable, disable, remove), Add New Driver from a package |

**Driver packages.** A folder with `DRIVER.INF`:
- `Name`, `Class`, `Version`, `Files`, `Load` (the command) and optional
  `Hardware` (PCI ids);
- installing copies the files to `\DRIVERS\<class>\<name>` and records the
  driver in `\DRIVERS\DRIVERS.CFG`;
- the new `\DRIVERS\LOADDRV.COM` loads the enabled drivers at boot, before
  the desktop.

## 5. Fonts

- **Downloads.** 7-10 free fonts (OFL, Apache or Bitstream Vera licences),
  downloaded with their licence texts into `assets/fonts`, with a SHA-256
  manifest.
- **Conversion.** `scripts/build_fonts.py` renders each font in the
  desktop's bitmap format (95 glyphs, regular and bold, 16-pixel cells) as
  `\SYSTEM\FONTS\*.CFN`, and converts any TTF/OTF for users.
- **System font.** It is set in the Fonts applet and read from
  `\SYSTEM\UI\FONT.CFG` at startup; the shell loads it into its font area.
- **Other fonts.** CiukNote's font dialog lists the installed fonts and uses
  its choice through an alternate font slot.
- **Install New Font.** Copies a `.CFN` into `\SYSTEM\FONTS`.

## 6. Polish

Spacing, alignment and wording are revised in every window (checked on
screenshots).

## Gates

- The desktop-apps gate grows: independent windows, desktop icons and
  context menus, Recycle Bin (delete, restore, empty), Control Panel applets
  (a setting that persists), a driver package installed and loaded at the
  next boot, and a font installed and used.
- The complete profile must pass.

## Status on 30 September 2026

The desktop, context menus, Control Panel, fonts, Recycle Bin and independent
Properties windows are implemented. The title buttons use theme-drawn
minimize, maximize, restore and close symbols. CiukNote and CiukPaint are
separate modules. Long file names work in desktop and Files operations; the
Files tree sidebar still uses short aliases. The QEMU desktop applications,
desktop polish, long-name and CiukPaint gates have passed on this work tree.
The driver pack's QEMU gate passes for NE2000, PCnet and RTL8139, no-match
boot, and hung/failing driver recovery. E1000 remains disabled after its
QEMU driver reported no PCI BIOS. Each DOS window now has its own VM; the
complete 0.8.0 VM-window profile passes 26/26 gates. No physical hardware
qualification is implied.
