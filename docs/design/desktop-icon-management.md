# Desktop icon management (G19)

Windows 98 reference and CiukiOS implementation contract, 2026-10-24.

## Reference behavior and repository comparison

The Windows 98 Resource Kit, *Microsoft Windows 98 Resource Kit*, Appendix G
(`build/rskt/wrkappg_1.htm`) documents `Ctrl+A` as Select All and `F2` as
Rename; its desktop keyboard section in chapter 6 (`build/rskt/wrkapph_5.htm`)
documents keyboard selection, Enter to open, and Shift+F10 for a context menu.
The shell's existing `shell.c` already implements Ctrl+A, arrows, Enter, F2
for files, and context menus. G19 adds Home/End and type-ahead to that same
desktop selection model.

The Windows 98 shell exposes Arrange Icons by Name, Type, Size, and Date,
Auto Arrange, and Line Up Icons. CiukiOS already persists free icon coordinates
in `desktop.cfg`; its current Arrange action only clears coordinates, while
Line Up snaps existing icons to cells. G19 sorts the projected desktop folder
entries by those four keys, preserves fixed system icon order, makes automatic
placement deterministic and persisted, and keeps manual placement available
when automatic arrangement is off.

The Resource Kit, chapter 6 (`build/rskt/wrkc06_4.htm`), describes choosing a
wallpaper image file. Microsoft's shell wallpaper API contract describes a
full path as the wallpaper input (`IActiveDesktop::SetWallpaper`,
https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-iactivedesktop-setwallpaper).
CiukiOS already accepts an absolute PNG/JPEG/BMP path in `desktop.cfg` and
Control Panel owns the settings file and applies changes through the desktop
settings protocol. G19 adds a Browse path picker there and routes Set as
Wallpaper from Paint, Image Viewer, and NetSurf through the same settings
owner/protocol instead of writing shell state from clients.

## Decisions

* Keep My Computer, My Documents, and Recycle Bin in their established order; sort
  only user entries. Name and type compare case-insensitively, ties use the
  original entry name, size and date use the FAT metadata already in the Files
  model.
* Auto Arrange uses the existing icon grid, fills top-to-bottom then left-to-
  right, persists its mode and generated positions, and is disabled by a manual
  drag. Line Up only snaps current positions to the grid.
* Type-ahead selects the next case-insensitive prefix match, wrapping once;
  Home/End focus the first/last icon. Ctrl+A remains select-all.
* Wallpaper paths are absolute and must name a readable supported image.
  Applying a wallpaper uses the existing Control Panel settings writer and
  reload message so cache publication remains owned by the desktop.
  `DESK_LAUNCH` remains denied to ordinary clients; the desktop grants the
  narrowly validated Control Panel wallpaper request only to Paint, Image
  Viewer, and NetSurf, with an absolute PNG/JPEG/BMP path. Paint and Viewer
  use the existing v2 launch message; NetSurf uses the bounded
  `DESK_V3_LAUNCH` request because its transport has no v2 channel. The v3
  opcode is accepted only for this Control Panel wallpaper action.
* New Bitmap uses the existing blank BMP creator. New Shortcut opens a
  two-page Create Shortcut wizard (command line, then name). Browse opens a
  filesystem picker with Computer/drives, folders, Up and keyboard selection;
  Cancel creates nothing, including when Finish was queued but not serviced.
  Finish exclusively creates a counted CiukiOS link.
  This format remains distinct from Microsoft MS-SHLLINK.
* System icon labels are per-desktop overrides in `desktop.cfg`; their
  canonical names remain the position keys and their actions remain unchanged.

## G19 continuation contract

Microsoft [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
describes links to files, folders and drives, their arguments and working
folder, and that deleting a link leaves its target intact. Internet shortcuts
are separate in Windows; CiukiOS also accepts HTTP, HTTPS, FTP and mailto
addresses (and other syntactically valid URL schemes) in its counted link
format and opens them with NetSurf. Activation
re-reads the link, resolves bounded chains, dispatches native programs through
the program launcher, Windows/DOS programs through the existing compatible
launcher, documents through the catalogue association, and folders/drives
through Files. Invalid, missing or cyclic targets produce a recoverable error.

Microsoft's [Shell Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)
and [Raymond Chen's drag rules](https://devblogs.microsoft.com/oldnewthing/20041112-00/?p=37323)
specify same-drive move, cross-drive copy, Ctrl copy, Shift move and Ctrl+Shift
link, with the right-drag menu allowing an explicit choice. Folder icons use
these rules including link creation; dropping on a program launches it with
the dragged paths and never removes the sources. Existing application-window
WM_DROPFILES handling remains the destination for a running application.

System icons keep their stable identities even when renamed: My Computer opens
Files and its Properties opens System; My Documents opens its folder and its
Properties opens the folder property sheet; Recycle Bin opens the bin and
owns Empty/Properties; Network Neighborhood opens the available Network tools and its Properties opens Network. Sharing discovery itself
belongs to the network work item. Refresh rescans Desktop and installed links
and repaints without reloading settings over unsaved view changes. Show
Desktop icons controls painting and hit testing while retaining the files.

The [SystemParametersInfo icon parameters](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow)
define spacing as icon-cell dimensions and a distinct caption font. Appearance
owns those settings and Apply publishes them through the existing settings
path. Position keys include the screen width and height; returning to a
resolution restores its own coordinates. The legacy unqualified keys remain
readable for migration. The local Resource Kit chapter 6 (`wrkc06_2.htm`,
`wrkc06_3.htm`, `wrkc06_4.htm`) establishes the desktop/Display ownership;
Appendix G/H supplies the desktop keyboard actions.


## Appearance and resolution profiles

The Resource Kit's *Scalable User Interface Elements* section
(`build/rskt/wrkapph_3.htm`, lines 34–52) places item and font customization
in Display Properties → Appearance. Together with Microsoft's
[icon metrics and caption-font API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow),
this establishes separate horizontal/vertical cell dimensions and a caption
font, rather than changing the icon bitmap or the shared window-title font.

Appearance → Desktop icon appearance edits cell spacing (60–256 pixels on
each axis) and selects a caption font from the renderer's seven existing
raster font variants, with a live sample. Defaults remain 96 × 94 pixels and
Inter 13. The installed TrueType preview remains accessible through Back to
installed font previews; arbitrary installed-face sizing remains part of the
separate G23 appearance work. Apply previews, OK keeps, and Cancel restores
these fields through the existing Display transaction. Show Desktop icons
continues to use Background's existing toggle and the same transaction.

`cfg_icon_position_at` and `cfg_icon_move_at` use keys of the form
`icon.WIDTHxHEIGHT.HEXNAME=x,y`. The old unqualified key is a migration fallback
for a name without a position at the selected resolution. Explicit Arrange
clears only the selected resolution and records its layout marker, which
prevents old migration coordinates from reappearing there. Rename changes
the name in every resolution profile and legacy position; deletion removes
all profiles for that name. Other profiles and similarly suffixed names are
preserved.

Profiles share the existing 16 KiB `extra` settings budget with other unknown
keys and system-icon labels, inside the 32 KiB settings-file limit. Position
writes, profile resets and multi-profile renames check the resulting size
before editing. A full budget refuses the operation while preserving old
positions; profiles are never evicted to make room. This bound predates the
resolution profiles and does not promise one saved record for every possible
4096-entry desktop. Capacity-refusal host assertions verify preservation.


## Desktop patterns

The Windows 98 Resource Kit's *Configuring Display Appearance* section
(`build/rskt/wrkc24_3.htm`, lines 438–440) identifies Display as the owner of
the desktop background pattern. Microsoft's
[IActiveDesktop::SetPattern](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-iactivedesktop-setpattern)
describes rows of eight on/off pixels, and
[SystemParametersInfo desktop parameters](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow)
distinguishes the desktop pattern from the wallpaper picture. CiukiOS uses
original deterministic geometric 8 × 8 masks (Bricks, Buttons, Checkerboard,
Circles and Tiles) alongside None. These masks contain no extracted vendor
artwork. The persisted `wallpaper_pattern` names are independent of picture,
placement and desktop colour.

The pattern repeats in screen coordinates using black foreground pixels and
the selected desktop colour as its background. Layering follows the separate
background/picture model: the pattern is painted first, then opaque wallpaper
pixels cover it. None shows the pattern alone; Center and Fit expose it in
uncovered margins, and transparent picture pixels reveal it. Full-cover
procedural scenes and opaque tiled/stretched pictures cover it. This explicit
compositing rule extends the classic monochrome pattern mechanism to CiukiOS's
ARGB image renderer. Background's Pattern chooser uses the existing Display
Apply/OK/Cancel transaction; preview requests snapshot the selected pattern so
an old worker result cannot overwrite a newer choice.

## Continuation integration notes

The file-model projection retains its established index offset of three;
Network Neighborhood follows that projection, while installed Windows desktop
links follow it with their own source-path position keys. All four system
labels have stable identity keys independent of the displayed name. The
approved Ciuki portrait and Tango icon family are retained; links add Tango's
existing symbolic-link overlay.

Link records are bounded to 4 KiB and target resolution to eight links.
Activation reads current bytes; drag hover caches a bounded resolution for its
current icon, invalidates it when the icon projection changes, and re-reads at
program drop. These small explicit reads, Browse enumeration and link creation
use the folder VFS synchronously; bulk move/copy/delete and final executable
classification/launch remain on their existing workers. This leaves removable
media latency in those small explicit operations unqualified on hardware.

Appearance reload compares spacing and font as visual state before rebuilding
the icon layer. Resolution rename preflights a settings copy before renaming
the file, so a full position budget cannot silently lose other resolutions.
Refresh preserves remaining file selections and source identity while rescanning.
