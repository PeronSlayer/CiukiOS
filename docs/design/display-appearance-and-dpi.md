# Display appearance and DPI (G23)

Research and implementation record for the G23 rows DS-2, DS-5, DS-6, DS-9,
DS-10, FT-4 and FT-5 in `os-feature-inventory.md`.

## Windows 98 behavior

The Windows 98 Resource Kit describes Display Properties as the place to
change background, on-screen fonts, colors and color palette. Its accessibility
appendix says Appearance schemes are previewed and then customized by selecting
an item and its font; scalable elements include window titles, scroll bars,
borders, menus and other standard elements. The same Control Panel family
offers large fonts and screen effects. CiukiOS follows this model with a live
sample and saved user choices. Its identity and supplied Ciuki themes remain
unchanged; user schemes are additions.

Microsoft's Win32 `SystemParametersInfo` documentation treats settings such as
font smoothing, full-window dragging, icon metrics and wallpaper as system
parameters. CiukiOS keeps these preferences in the desktop-owned
`desktop.cfg`, which is already the shared source read by the shell and native
UI clients. A changed appearance must invalidate the complete desktop because
the palette, metrics and font rasterization can affect every visible window.

Refresh rate and monitor identification are hardware facts, not user-created
values. Microsoft documents refresh rate as the driver's `dmDisplayFrequency`
mode member, and EDID as monitor-supplied identification/capability data read
over DDC. CiukiOS's VBE mode ABI currently carries dimensions, depth, pitch and
current state only; it has no refresh-frequency or monitor/EDID record. This
item must not label a fabricated monitor name or frequency as detected. Until a
reviewed display ABI and firmware acquisition contract exists, these fields
remain read-only/unavailable.

## Sources

- Owner's Windows 98 Resource Kit, `wrkc23_2.htm` (Display Control Panel
  description), `wrkapph_3.htm` (“Scalable User Interface Elements”), and
  `wrkc24_3.htm` (display device configuration), read from the local
  read-only extraction at `/home/peronslayer/Desktop/CiukiOS/build/rskt/`.
- Microsoft, [SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa): system-wide parameters include font smoothing, drag-full-windows and desktop pattern/wallpaper.
- Microsoft, [DEVMODEA](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-devmodea): `dmDisplayFrequency` is the vertical refresh rate, and enumeration may report 0/1 for the hardware default.
- Microsoft, [Monitor Class Function Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/monitor-class-function-driver): monitor EDID is retrieved through the monitor device stack and DDC.
- Microsoft, [ChangeDisplaySettingsA](https://learn.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-changedisplaysettingsa): a mode change uses only values returned by mode enumeration and can request refresh frequency when supported.
- Microsoft, [NONCLIENTMETRICSA](https://learn.microsoft.com/en-us/windows/desktop/api/winuser/ns-winuser-nonclientmetricsa): non-client fonts and dimensions are a coherent scalable metrics set.

## CiukiOS implementation decisions

`desktop.cfg` is the persistence contract. The new background pattern and RGB
colour choices use the existing durable display preview and settings reload
path; the theme preview also uses the existing theme path. These screen changes
cause a full repaint (fx-15 row 42). The current VBE mode list contains only
mode id, dimensions, depth, pitch and linear/current flags (`abi.h`,
`ciuki_display_mode`); refresh frequency and EDID are absent. The current bitmap
font assets have fixed raster sizes and
most native controls use fixed pixel geometry; a real Large Fonts/DPI mode
therefore requires a coherent scaling pass across fonts, controls, icons and
hit targets. The implementation must not expose a percentage control that
only enlarges glyphs while leaving the interaction geometry unchanged.

## G23 status

Updated by G23 implementation. The inventory row remains partial wherever the
hardware ABI or coherent DPI scaling work described above is not delivered.
