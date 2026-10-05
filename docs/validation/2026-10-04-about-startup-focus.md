# About startup preference and title focus — 2026-10-04

## Research and decision

Microsoft's [WM_NCACTIVATE reference](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-ncactivate) describes updating title-bar appearance when activation changes. Its [nonclient-area guidance](https://learn.microsoft.com/en-us/windows/win32/gdi/nonclient-area) likewise calls for repainting the title bar as a window becomes active or inactive. CiukiOS draws its own title bars, so the matching rule is to invalidate and redraw the window frame whenever keyboard focus changes.

The stale gray title came from the mouse focus path: clicking the desktop marks `app_desktop_focus`, then clicking the already-active application window skipped `ui_windows_raise` because the window ID had not changed. The desktop-focus flag therefore remained set and the title renderer continued choosing the inactive color. The click path now raises an already-active window when the desktop had focus; the existing raise routine clears that flag and invalidates the window frame. Left and right clicks share the same behavior.

The About checkbox now says **Don't show this at startup**. An absent or invalid preference defaults to showing About. `WELCOME.CFG` keeps its existing byte meaning (`1` = show, `0` = hide), with the UI state inverted when loading and saving for compatibility. The owner-requested image preference reset to `1` is handled by the root integration.

## Manual check for the full build

The full Linux VM run in
`build/tests/desktop-web-audio-2026-10-04/integrated-rgb/results.json` verified
the default startup opening, inverse-checkbox persistence (`WELCOME.CFG=0`),
suppression after a real reboot, and re-enabling the preference. Actual SDL
captures showed the title returning to active RGB `(52,73,121)` after a
desktop click and a click in the same About window. Its later browser step
failed separately because numeric IPv4 hosts were sent to DNS; that defect
was corrected and the browser was rerun independently.

Open About and confirm the checkbox is unchecked. Check it, close About, reopen it, and confirm it remains checked; verify the next startup suppresses the page. Uncheck it, then verify the next startup opens About. For title focus, click About, click the desktop, then click the same About window body and title bar; the title should return to its active color. Repeat by right-clicking the same window after giving focus to the desktop.
