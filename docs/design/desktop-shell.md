# Desktop shell bars (f2-81)

Research checked before implementation, 2026-10-18:

- The [EWMH specification](https://specifications.freedesktop.org/wm/latest-single/)
  distinguishes the work area reserved by panels, maximization within that
  area, full-screen geometry, and task buttons that activate or minimize
  windows (sections 2.1.1, 2.7, 3.9 and 5.7). CiukiOS already reserves
  `DESK_BAR` at both edges. Keep those dimensions and existing DOS/full-screen
  transitions; move shell controls without changing the window protocol.
- Microsoft's [taskbar documentation](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar)
  describes the launcher, window buttons and notification area as distinct
  shell functions. The owner's layout places launcher and system status above
  the work area and window buttons alone below it. Both retained themes use
  left-aligned tasks. Window captions are never repeated in the top bar.
- The [Desktop Notifications specification](https://specifications.freedesktop.org/notification/latest/protocol.html)
  separates replacement, expiry, dismissal and activation. Preserve CiukiOS's
  bounded notification slot, coalescing and actions while moving its anchor
  below the top bar. This is a behavior reference, not D-Bus implementation.

The implementation previously drew tray icons and time in the bottom bar,
date and the focused title in the top bar, and limited tasks by the tray's
width. Shared geometry now keeps tray/time/date at the top right, an explicit
notification anchor between tray icons and clock, and tasks across the bottom.
Tray popups drop down; task tooltips rise from their bottom buttons. Damage
and overlay exclusion must cover both old and new popup positions.

The signature control is a 24-pixel circular orb within the existing 32-pixel
bar, with an integer radial gradient in the theme's accent and the existing
18-pixel deterministic area average of the approved Ciuki portrait. Preserve
the portrait pixels, source hash, fonts, English copy and tagline. Hover and
menu-open states change the orb face, with the existing one-pixel pressed
portrait offset. No external artwork is used.

Only Ciuki Blue (default) and Platinum remain selectable. The removed saved
theme maps explicitly to Ciuki Blue in both settings readers and logs once
per process, including when it follows another valid theme line.

Intentional golden changes: shell bars, orb, task widths, tray popup placement,
notification placement and Control Panel's two-theme list. Classic theme
frames are removed. Frames of application controls without shell chrome
remain unchanged except for theme-list content. Host captures cover 1024x768
and 800x600; runtime QEMU evidence belongs to lead integration.
