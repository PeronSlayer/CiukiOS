# Desktop applications and shortcuts — 29 September 2026

Owner requests:
- the classic shortcuts of a graphical OS (Alt+F4, Win+R for Run, and so on);
- a more precise and detailed Task Manager;
- a file manager like Explorer or Dolphin, with a right-click menu that
  creates folders and copies, cuts and pastes files;
- the legacy Windows Notepad, identical and complete, next to the DOS editor.

## Why modules

SHELL.COM was full: 60,768 bytes against its 0xEF00 arena ceiling, with the
built-in Files and Tasks inside. Those two moved out, and so did the
removable-media code. Files, Tasks and Notepad are now separate programs in
`\SYSTEM\APPS`, written in C (OpenWatcom), and SHELL.COM hosts them.

The shell is now 55,856 bytes, host included.

### Host contract

The host is `src/com/shell_apps.inc`; the C side is in `src/apps/app.h`.

- **Loading.** A module is a flat 16-bit image with a `CAPP` header. The
  shell loads it into its own zero-filled DOS block, at offset 100h.
- **Frame.** The shell draws the window frame: title from the module,
  minimize, maximize, close and a resize grip.
- **Events.** The shell calls the module's far entry with:
  - OPEN (with an argument such as a path);
  - PAINT, KEY and MOUSE (left down, move, up, right click);
  - ACTION, for the hits the module registered;
  - POLL (idle work; "busy" skips the HLT);
  - CLOSE (a module may keep its window open, e.g. unsaved text);
  - SUSPEND.
- **Services.** The module draws and asks through one far service entry:
  - rectangles, text, fixed-pitch text, bevels, buttons, hits, icons;
  - repaint, run a program, open another module, close;
  - sounds, the window list and window commands, idle time.

  Services run on the shell's own stack.
- **Before a DOS program runs** (SUSPEND), each module either:
  - saves its state (Files: the folder) and is unloaded, freeing its memory;
  - or stays resident, e.g. Notepad with unsaved text.

  Modules reopen after the program ends.

| Module | Image | Memory | Window |
| --- | --- | --- | --- |
| `FILES.APP` | 34 KB | 62 KB (+ an 8 KB block while copying) | 8 |
| `NOTEPAD.APP` | 27 KB | 36 KB (+ 60 KB text, undo and clipboard blocks) | 12 |
| `TASKS.APP` | 18 KB | 25 KB | 9 |

Build: `scripts/build_apps.sh`, called by `scripts/build_full.sh`.

## Shortcuts

The BIOS never reports the Windows key, the Menu key or Ctrl+Alt+Del. The
desktop takes them from the BIOS keyboard intercept (INT 15h AH=4Fh).
Jemm386 used to catch Ctrl+Alt+Del itself for a soft reboot, which then
never completed. `patches/jemm-ciukios-ctrl-alt-del.patch` passes the key on
like any other: in the desktop it opens Tasks, and elsewhere the BIOS resets
as before.

| Keys | Action |
| --- | --- |
| Win (alone), Ctrl+Esc | Programs (the Start menu) |
| Win+R | Run (also: `notepad [file]`, `explorer [folder]`, `taskmgr`) |
| Win+E | Files |
| Win+D, Win+M | show the desktop (minimize every window) |
| Win+Tab, Alt+Tab | next window |
| Win+F1 | About |
| Alt+F4 | close the window; on the bare desktop: shut down |
| Ctrl+Shift+Esc, Ctrl+Alt+Del | Task Manager |
| Menu key, Shift+F10 | context menu of the active application |

## Files (Explorer / Dolphin style)

- **Layout.**
  - Menus: File, Edit, View, Go, Help.
  - Toolbar: Back, Forward, Up, Cut, Copy, Paste, Delete, Properties, Views.
  - Address bar; left pane with Places (local disk, Applications, System,
    floppy, USB, CD-ROM) and a Folders tree (the disk, with the folders along
    the current path opened); status bar with the selection size and the
    free space.
- **Views.** Details (sortable Name/Size/Type/Modified columns), Large
  Icons and List.
- **Selection.** Click, Ctrl+click, Shift+click, Shift+arrows, Ctrl+A,
  Invert Selection, type to find.
- **Context menu (right click).**
  - On an item: Open, Open with Notepad, Edit (DOS Editor), Cut, Copy,
    Paste into a folder, Delete, Rename, Properties.
  - On the background: views, sorting, Refresh, Paste, New Folder, New Text
    Document, Properties.
- **Operations.**
  - New folder and new text document, then rename in place.
  - Copy, cut and paste, of folders too: a background job with progress and
    Cancel, replace prompt (Yes, Yes to All, No, Cancel), date and
    attributes kept.
  - Delete with confirmation, recursive for folders. There is no Recycle
    Bin: the dialog says "permanently".
  - Properties: type, location, size, contents, attributes (editable).
  - Drag and drop: selected items onto a folder, a place or a tree folder
    move there; with Ctrl they are copied, and from removable media they
    are imported. The pointer shows "Move n item(s)" or "Copy n item(s)".
- **Opening files.** Text files open in Notepad; `.COM`/`.EXE` run from
  their own folder.
- **Keys.** Enter, Backspace, Alt+Left/Right/Up, F2, F5, Del, Alt+Enter,
  Alt+D or F4 (address bar), Ctrl+C/X/V.
- **Removable media.** Through MEDIA.DRV, read-only: browse, preview text,
  and copy files, then paste them into a folder on C: (an import).
- **Limits.** DOS 8.3 names, and one mounted drive (the kernel's).

## Notepad (classic Windows Notepad)

- **File.** New, Open, Save, Save As (with the Windows file dialog, a type
  filter and a replace prompt), Page Setup, Print, Exit. Unsaved changes
  give "The text in the X file has changed. Do you want to save the
  changes?"
- **Edit.** Undo (one level, toggling as in Windows), Cut, Copy, Paste,
  Delete, Find, Find Next (F3), Replace (Find Next, Replace, Replace All,
  Match case), Go To (off with Word Wrap, as in XP), Select All, Time/Date
  (F5).
- **Format.** Word Wrap, Font.
- **View.** Status Bar (Ln, Col).
- **Help.** Help Topics, About.
- **Also as in Windows.**
  - Right-click menu, shortcuts, F10 and Alt menus.
  - Double-click selects a word.
  - A file whose first line is `.LOG` gets the time and date appended on
    each opening.
  - The text limit is 61,440 bytes; Windows 9x stopped at 64 KB.
- **Printing.** To LPT1 through the BIOS (INT 17h): plain text with
  margins, header and footer (`&f` file, `&p` page, `&d` date, `&t` time).
  A missing printer gives a message.
- **Font.** The desktop has one bitmap font, shown in fixed pitch. The Font
  dialog offers Regular or Bold; other faces and sizes are not available.

## Task Manager

- **Applications.** The desktop windows and their state, with End Task,
  Switch To and New Task (Run).
- **Processes.** The DOS memory arena by owner: image name, PID (PSP),
  memory, blocks. Sortable.
- **Virtual Machines.** Each VM of the VM manager, from the new CVSESSION
  operation VMM_LIST (48h):
  - its state and its DOS window session with devices (video, keyboard,
    mouse, sound);
  - which VM has the focus.

  Give Focus and End VM (VMM_KILL) act on it.
- **Performance.**
  - CPU usage with a 60-second history. It is the share of time the desktop
    did not spend waiting in HLT, measured per BIOS tick, so DOS VMs count.
  - Conventional memory, the DOS arena, free XMS and EMS, the disk, and the
    VM manager's switches.
- **View.** Update speed (High, Normal, Low, Paused) and Refresh (F5).

## Fixed on the way

- **Kernel date and time.**
  - INT 21h AH=2Ah returned a fixed date, 2026-08-30.
  - AH=2Ch used only the low word of the BIOS tick count, so the hour
    wrapped.

  Both now read the RTC, and the weekday comes from CMOS register 6. The
  kernel is 43,257 bytes, 7 under its ceiling.
- **Module memory.** DOS does not clear allocated memory. A module loaded
  where another had been saw that module's bytes in its static data, and
  crashed. The host now zero-fills every module block.

## Gates

- **`scripts/qemu_test_desktop_apps.py`.** Shortcuts, Files operations
  (including a drag and drop and a click in the Folders tree), Notepad
  editing, Tasks tabs and End Task. After shutdown the disk is checked with
  an independent FAT parser: copy, move, drag-and-drop move, recursive
  delete, the saved text, the `.LOG` stamp, and today's date from
  Time/Date.
- **`scripts/qemu_test_native_media.py`.** Floppy, USB disk and CD-ROM
  browsing, preview and a byte-exact import, plus an ejected floppy.
- **`scripts/qemu_test_wallpaper_import.py`.** Wallpaper tiles copied with
  Files, then applied.

The first two run in `scripts/test_vm_window_profile.sh`; the media gate
runs when its fixtures are built. Modules log their state on COM1
(`[FILES]`, `[NOTEPAD]`, `[TASKS]`, `[DESKTOP] WINDOW nn OPEN/CLOSE`).
QEMU evidence only.
