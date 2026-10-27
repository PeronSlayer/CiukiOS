# Programs, shortcuts and media show their own icons (fx-exeicons)

Owner request 2026-10-27, directive `docs/directives/fx-exeicons.md`. Executables
and similar files that carry their own icon, or name an icon file meant for
them, show it automatically everywhere CiukiOS shows them, as Windows 98's
Explorer does. The Tango 0.8.90 family stays the system's default for every
object without an own icon, and the approved Ciuki portrait stays the
identity icon (`AGENTS.md`); nothing here redraws or replaces either.

## Research (checked 2026-10-27)

| Source | What it settles |
| --- | --- |
| Microsoft, [ExtractIconA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-extracticona) and [ExtractIconExA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-extracticonexa) | Index 0 is the first icon group; `-1` counts RT_GROUP_ICON resources (1 for an .ico); a negative index names the group whose resource ID is its absolute value; ExtractIcon returns 1 for a file that is neither a program, a library nor an icon file, NULL without icons; ExtractIconEx returns UINT_MAX for a missing file. |
| Microsoft, [SHGetFileInfoA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfoa) | SHGFI_ICON, SMALLICON, OPENICON, USEFILEATTRIBUTES, ICONLOCATION, TYPENAME and EXETYPE semantics and values (EXETYPE: `MZ`, `NE`/`PE` with the Windows version in the high word, console `PE` with 0). |
| J. Hornick, [Icons](https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)), MSDN 1995 | ICONDIR/ICONDIRENTRY, GRPICONDIR/GRPICONDIRENTRY (`nID` replaces the file offset), ICONIMAGE = BITMAPINFOHEADER with doubled height, XOR image then AND mask; Windows 95 takes the alphabetically first named group, else the lowest ID; image choice: the closest size, then the colour depth of the display. |
| Microsoft, [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) section 6.9 and [Resource file formats](https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats) | Three-level resource tree (type, name, language); named entries first, sorted, then IDs ascending; high bit = subdirectory / name string; RT_ICON/RT_GROUP_ICON; PNG images inside RT_ICON only since Windows Vista. |
| MS-DOS Encyclopedia, [Appendix K](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/appendix-k/), and this repository's `sdk/win16/ne` (`docs/design/win16-subsystem-contract.md`) | NE resource table: `rscAlignShift`, TYPEINFO (`0x8000|type`), NAMEINFO offset/length shifted by the alignment, `0x8000|id`. |
| Microsoft, [\[MS-SHLLINK\]](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/16cb4ca1-9339-4d0c-a68d-bf1d6cc0f943) | ShellLinkHeader IconIndex, HasIconLocation and the StringData order, LinkInfo LocalBasePath + CommonPathSuffix, IconEnvironmentDataBlock (0xA0000007). |
| Microsoft, [How to Customize Folders with Desktop.ini](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-customize-folders-with-desktop-ini) | `[.ShellClassInfo] IconFile`/`IconIndex` (relative paths allowed); the folder must be made a system folder (read-only or system attribute, PathMakeSystemFolder); Unicode desktop.ini. |
| Microsoft, [Autorun.inf Entries](https://learn.microsoft.com/en-us/windows/win32/shell/autorun-cmds) | `[AutoRun] icon=iconfilename[,index]` represents the drive in Explorer instead of the standard drive icon; .ico, .bmp, .exe or .dll. |
| Microsoft, [WM_GETICON](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-geticon), [WNDCLASSEXA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-wndclassexa) | A window without WM_SETICON uses its class icon; a NULL hIconSm is taken from the hIcon resource at the small size; ICON_SMALL2. |
| SM Soft, [The PIF file format in various Windows versions](https://www.smsoft.ru/en/pifdoc.htm) | Section headings (16-byte name, next, data offset, data length) after the 0x171-byte basic section; "WINDOWS VMM 4.0" data: icon file name at 0x58 (80 bytes, default `PIFMGR.DLL`), icon number at 0xA8. |
| Wine, [user32/exticon.c](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/user32/exticon.c), [user32/cursoricon.c](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/user32/cursoricon.c) (`CURSORICON_FindBestIcon`, `CURSORICON_CreateIconFromANI`), [shell32/folders.c](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/shell32/folders.c) (`getIconLocationForFolder`) | Edge cases: the first language entry, NE group/icon lookup by `0x8000|id`, an NE table without TYPEINFO has no resources, ANI `anih` flag AF_ICON and the first `icon` chunk of `LIST fram`, the size-then-depth rule, desktop.ini lookup order. |
| R. Chen, [How does Add/Remove Programs get the size and other information?](https://devblogs.microsoft.com/oldnewthing/20040709-00/?p=38493), The Old New Thing 2004 | Without DisplayIcon, Add/Remove Programs takes the Start menu shortcut whose name shares the most words with DisplayName. |

## Decisions

* **One bounded core**, `apps/libdeskui/iconx.c`: freestanding (no libc, no
  heap, integer arithmetic only), compiled into libdeskui, the Win32 USER
  facade and ciukishell. It detects ICO/CUR/ANI/BMP/PE/NE/DOS, walks PE and
  NE resources, ranks images and decodes DIBs (1/4/8/16/24/32 bpp, core and
  info headers, AND mask, XP alpha when any alpha byte is set) and resamples
  with the integer area filter of `icons.c`. Every read goes through a
  three-line read cache within a 4 MiB budget; directories are capped at 4096
  entries, icon groups at 512 images, links/PIF/INI files at 64 KiB, nesting
  of resource directories at 2 levels, shortcut chains at 4. A damaged or
  hostile file returns an error and the caller shows the type's default.
* **Image choice**: the closest size, a larger image on a tie (a downscale
  looks better than an upscale), then the deepest colour, then directory
  order; an image that does not decode (a Vista PNG, a truncated DIB) yields
  to the next candidate. Windows 95 treats all depths of 8 bpp or more as
  equal; CiukiOS draws in 32 bpp, so the deepest image is the right one.
* **Inverted pixels** (AND = 1 over a non-black XOR colour, cursors) are shown
  opaque in the inverse colour, their look over a white background, since the
  compositor has no screen-inversion blend for icons.
* **Resolution** (`fileicon.c`, `du_file_icon_resolve`): programs and
  libraries (`.exe .dll .cpl .scr .ocx .icl`) and `.ico .cur .ani` show
  themselves; `.lnk`/`.ciuki` (CIUKLNK3 and MS-SHLLINK) show IconLocation, else
  their target's icon (through further shortcuts), else the target's type;
  `.pif` shows its VMM 4.0 icon unless it is PIFMGR.DLL's (the MS-DOS
  default), else a Windows program target's icon, else the MS-DOS program
  icon; `.url` shows `[InternetShortcut] IconFile`; a read-only or system
  folder shows its desktop.ini icon; a drive root (`/`, `/mnt/<letter>`) shows
  AUTORUN.INF `icon=`, read for CD-ROM and fixed drives; floppies and
  removable media are not read for an icon (a diskette access per Computer
  view would be slow and noisy). Windows paths map as Files does (`C:\` = `/`,
  `D:\` = `/mnt/d`), `%SystemRoot%`/`%windir%` = `C:\WINDOWS`; a missing path
  component is looked up case-insensitively (ISO 9660 and FAT names).
* **DOS programs** keep the MS-DOS program icon (`type:executable`) unless a
  PIF or shortcut names another icon; `.bat` keeps its script icon.
* **Libraries**: the directive asks DLLs to show their first icon group too.
  Windows' registry gives `exefile` the DefaultIcon `%1` (its own icon) but
  `dllfile` a generic shell icon, and Wine's `wine.inf` sets `%1` for
  `exefile` only; CiukiOS follows the directive (`own_types` in
  `fileicon.c` is the one place to change it).
* **A file that was not read** (missing, unreadable, or of a type without
  an own icon) leaves the caller's existing default in place; only a file
  that was read and has no own icon (a shortcut to a document, a DOS
  program) reports its resolved default, the target's type.
* **The cache** (per process, `du_file_icon`) is keyed by path (plus
  `|index` for explicit locations) and size, stores the subject's
  modification time, and extracts again when a lookup brings a different one.
  A worker thread does every extraction in Files, the Control Panel and the
  desktop and wakes the UI when it published; until then the caller draws the
  type's icon. Pixel buffers are freed only after five idle seconds, so a
  pointer from the current paint stays valid. Scripted gate runs and host
  harnesses extract in line (deterministic frames).
* **Where**: Files (every view, the Recycle Bin, search results, the
  Computer view's drives, the Properties dialog; shortcuts get the Windows
  lower-left overlay); the desktop (desktop folder items, Win32 desktop
  shortcuts, the Start menu's Programs folders and its Win32 shortcuts, the
  Games folder's shortcuts, Quick Launch and taskbar toolbars, shortcut
  Properties, the AutoPlay dialog's medium); Add/Remove Programs (DisplayIcon, else the best Start menu
  shortcut: target inside the program's folder first, then shared words with
  DisplayName; never the uninstaller's shortcut); Win32 windows (caption,
  taskbar button, Alt+Tab) through `DESK_V3_WINDOW_ICON`: WM_SETICON, else
  the class icon, else the executable's first icon group (owner requirement;
  Microsoft documents a default icon there), 16 px from the group's own 16x16
  image; Win32 `ExtractIcon(Ex)`, `PrivateExtractIcons` (now NE, CUR, ANI and
  BMP too), `LoadIcon` (best 32x32 image plus the 16x16 one),
  `WM_SETICON`/`WM_GETICON`, `SHGetFileInfoA` (ICON, ICONLOCATION, TYPENAME,
  EXETYPE, OPENICON, USEFILEATTRIBUTES) with the Tango defaults through
  user32 `CiukiShellIcon`.
* **Unchanged by design**: the Run dialog (Windows 98's has a fixed icon and
  no per-entry icons); Task Manager's application list keeps the program icon
  of the owning process name (the process list carries no executable path and
  the window list no icons); bundled DOS games keep the Tango game icon.
