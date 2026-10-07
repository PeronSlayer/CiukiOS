# Unified desktop properties

CiukiOS routes the desktop background's **Properties** command, Control Panel's
Display, Wallpaper, and Appearance entries to one **Display Properties** window.
This follows Windows 95's consolidated Display Properties sheet: it grouped
desktop background, window appearance, and supported display resolution/color
settings so users could adjust them from one place. CiukiOS has no screen-saver
page, so its pages are Screen, Adapter, Monitor, Advanced, Background, and
Appearance. The four existing display-information pages keep their indexes so
display diagnostics and mode-selection tests remain stable.

## Background page

The page reads `\SYSTEM\UI\WALLS.DAT`, a bounded CWC1 catalog with an 8-byte
header and up to 99 fixed 44-byte records. Index zero is the built-in solid
background; catalog entries follow in file order. A selected item and one of
five positioning styles remain pending until Apply or OK. Cancel discards those
pending choices. The displayed style names and meanings are Fill (crop to cover),
Fit (preserve the whole image and aspect ratio), Stretch (resize to the target
rectangle), Center (original size, centered), and Tile (repeat the image).

The shell may discover newly copied `WALLnn.CWP` files without rewriting the
packaged catalog. The page therefore asks the shell for its current wallpaper
count when it opens and on F5, then adds filename labels for numbered entries
after the catalog's listed records. F5 preserves the pending wallpaper choice,
style, and live appearance preview. Catalog access uses DOS INT 21h/AH=3Dh in
read-only mode; this matches the documented open-file service and avoids
changing the catalog during discovery.

These modes align with Microsoft's wallpaper-position definitions. `Fit`
preserves aspect ratio and may leave letterbox space, whereas `Fill` crops to
avoid it. CiukiOS uses a single desktop surface, so multi-monitor `Span` is not
offered. The shell retains catalog, persistence, cache, and rendering ownership;
the Display app reads only the small catalog and calls the wallpaper service to
commit a selection.

## Appearance page

The existing eight color schemes, desktop-color swatches, preview, and eight
desktop-icon visibility controls are hosted by Display Properties. Scheme
selection previews the palette live. Apply or OK saves the palette and icon bits
through the existing `cfg_save()` path to `\SYSTEM\UI\DESKTOP.CFG`; Cancel
restores the last saved palette. A successful Apply becomes the baseline for a
subsequent Cancel.

## Wallpaper service

`app_wallpaper()` provides the current selection, generation, file kind/name,
and style. `app_wallpaper_apply(index, style)` commits a selected catalog index
and style. The packed `app_wallpaper_info` remains 20 bytes; its last byte stores
the style. Style values are `WP_FILL=0`, `WP_FIT=1`, `WP_STRETCH=2`,
`WP_CENTER=3`, and `WP_TILE=4`. The service rejects invalid indexes/styles and
reports success only after persistence succeeds.

## References and decision

- Microsoft, *Introducing Microsoft Windows 95*, section on consolidated Display
  Properties: https://www.bitsavers.org/pdf/microsoft/windows_95/Introducing_Microsoft_Windows_95_1995.pdf
- Microsoft Learn, [Property Windows](https://learn.microsoft.com/en-us/windows/win32/uxguide/win-property-win): Properties opens a property sheet; pages should be task-based and use specific concise labels.
- Microsoft Learn, [DESKTOP_WALLPAPER_POSITION](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-desktop_wallpaper_position): Center, Tile, Stretch, Fit, and Fill definitions; Fit preserves the full image and aspect ratio, while Fill crops to cover.
- Ralf Brown's Interrupt List, [INT 21h/AH=3Dh Open File](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html): read-only open access mode used to inspect the wallpaper catalog.

The implementation decision is one shared Display Properties host, a delayed
commit for wallpaper selection/style, reversible live palette preview, and the
existing configuration files and rollback behavior. The shell remains the
wallpaper engine; the C app owns the unified property-window controls.

## Control routing

The first runtime gate exposed a real collision: module control 50 (Fill) was
encoded as 150, but Shell dispatched only module actions 100..149. Action 150
opened the old Display panel instead. Control routing must use the owning
application window, as [WM_COMMAND control notifications](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-command)
do, rather than treating a control ID as a global command. CiukiOS forwards
100..248 only when the active window belongs to a loaded module. Legacy windows
retain their own action ranges; 249/250 retain command/redraw meanings. CAPP
control IDs are therefore 0..148. Catalog rows use bounded visible-row IDs
100..108, independently of a catalog containing up to 99 entries.
