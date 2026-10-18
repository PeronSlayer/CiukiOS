# Desktop shell bars and protruding Start orb (f2-82)

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

F2-82 research rechecked the three primary sources above on 2026-10-18.
EWMH sections 2.1.1, 3.9 and 5.7 distinguish reserved work area from window
stacking; Microsoft documents the launcher separately from application
windows. Decision: retain the 32-pixel work-area boundary and paint the orb
as shell chrome above windows. Notifications keep their existing expiry,
replacement and action semantics. These references inform behavior; the
owner's Vista-like geometry is drawn locally without Microsoft artwork.

Amendment 2 research rechecked the [EWMH specification](https://specifications.freedesktop.org/wm/latest-single/)
and [PNG specification](https://www.w3.org/TR/png/) on 2026-10-18 against
the existing shared geometry and pinned converter. Decision: resize through
the generated geometry and existing deterministic portrait scaler; preserve
the reserved work area, stacking and approved source pixels.

The signature control is a 38-pixel orb at (6,0), extending 6 pixels below
the 32-pixel bar. A concentric 42-pixel concave well is clipped to the bar.
Integer radial shading, a soft elliptical highlight, a thin dark outline
and a one-pixel shadow surround the unchanged, deterministic 24-pixel area
average of the approved portrait. Hover brightens the gloss; menu-open
inverts the gradient and shifts the portrait one pixel. The existing scaler
already supports 24 pixels; no additional bitmap is required.

`convert_portrait.py --geometry` generates `start-orb.h` and `start-orb.json`
from one geometry definition. The build validates both; the desktop uses
the header and runner observers use the JSON for the orb and portrait boxes.
The observer compares every fully opaque portrait cell against the pinned
converter, independent of theme/gradient. Circular hit-testing includes the
protrusion; the menu still anchors at (6,32). Orb damage includes its shadow,
and client-only, scroll and window-copy fast paths exclude that area just
as they exclude notification chrome. DOS and full-screen geometry stays
unchanged.

Only Ciuki Blue (default) and Platinum remain selectable. The removed saved
theme maps explicitly to Ciuki Blue in both settings readers and logs once
per process, including when it follows another valid theme line.

Intentional golden changes: shell bars, orb, task widths, tray popup placement,
notification placement and Control Panel's two-theme list. Classic theme
frames are removed. Frames of application controls without shell chrome
remain unchanged except for theme-list content. Host captures cover 1024x768
and 800x600; runtime QEMU evidence belongs to lead integration.

F2-82 intentionally re-records shell reference, interaction, compositor,
transient/notification and boot-frame goldens for the larger orb. App-only
and dialog-only frames retain their existing hashes. Twenty additional orb
goldens cover idle, hover, pressed, maximized-window and notification states
in both themes at both resolutions. The dedicated replay checks circular
hits above and below the bar, menu/keyboard activation, notification input
priority on a narrow screen, shadow damage, copy exclusions and exact
incremental/full-frame parity. Host qualification also retains the
no-allocation-after-first-frame and DOS transition checks.
