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

* Keep Computer, Documents, and Recycle Bin in their established order; sort
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
* New Bitmap uses the existing blank BMP creator. New Shortcut creates a
  CiukiOS counted `.lnk` record with the current default Files target. The
  target/name wizard and opening arbitrary desktop `.lnk` records remain
  follow-up work; the format is not Microsoft `.lnk` and is not presented as
  compatible with it.
* System icon labels are per-desktop overrides in `desktop.cfg`; their
  canonical names remain the position keys and their actions remain unchanged.
