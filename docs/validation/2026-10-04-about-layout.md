# About window layout review — 2026-10-04

## Research

- Microsoft, [Tabs (Win32 UX Guide)](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-tabs): tabs present related information on separate labeled pages; use short labels that clearly describe each page and keep horizontal tabs to a single row. The guide also cautions against using tabs where one page can comfortably contain the information.
- Microsoft, [Keyboard interactions](https://learn.microsoft.com/en-us/windows/apps/design/input/keyboard-interactions): keyboard users need a clear, predictable focus order and arrow-key movement within related controls.
- GOV.UK Design System, [Tabs](https://design-system.service.gov.uk/components/tabs/): tabs hide page content, so labels should make the hidden content clear.

## Repository check and decision

The existing About window placed the Ciuki dedication between technical credits and license details in a single sequence of eleven rows. At the compact 640x480 profile that made the dedication read like another technical credit. The existing Display app already demonstrates compact, single-row page tabs with `ui_bevel`, `ui_hit`, and arrow-key page navigation.

The content forms two related peer pages, so the About window now uses clearly labeled **About** and **Credits** tabs. The About page retains the approved `ICON_ABOUT` header and the full memorial wording. Credits groups the system components above the copyright and license notices. The startup preference remains persistent and reachable, and keyboard focus, Enter, Space, arrow keys, and the `1`/`2` page shortcuts are handled in the app.

The full-image app builder accepts modules up to 0xFE00 bytes and 0x1000 paragraphs (`scripts/build_apps.sh`). This change adds only small static page labels and paint/event branches; the About source stays within that existing design budget without adding resources or dependencies.

## Executed layout check

The desktop reduces the actual About height at 640×480, below its requested
440 pixels. The first two layouts still crowded the Credits footer in real
captures; the final layout combines graphics/build on one row and puts the last
license row at client offset 256, above the status line and checkbox.

Both pages were visually inspected in a verified 640×480 QEMU capture. The
startup checkbox was unchecked, the app closed and reopened, and the unchecked
value persisted. Enter activates Continue by default. The complete About CAPP
uses 10,880 bytes of memory; the exact size is recorded by the full builder.
No portrait or icon changes were made. See the
[final screenshots and results](2026-10-04-post-release-fixes/README.md).
