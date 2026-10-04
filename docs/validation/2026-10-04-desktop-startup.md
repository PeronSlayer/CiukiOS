# Desktop startup and window titles, 2026-10-04

## Primary UI guidance and decision

Microsoft's title-bar guidance says the title identifies the app, must leave
caption controls visible, and may be truncated with an ellipsis when space is
limited. Its Win32 window documentation likewise describes the title as the
application name or the window's purpose. Apply that guidance to CiukiOS's
custom VGA/VBE frame: show the actual module/window name, clip it to the title
region before the minimize/maximize/close controls, and keep a visible focus
difference between active and inactive windows. Do not let a long caption
overwrite its controls.

The first-run About page is an OS welcome screen, not a second app library.
Show it at startup by default and expose a plainly labelled “Show at startup”
checkbox. The checkbox stores a user preference independently from the fixed
72-byte `DESKTOP.CFG` format. The agreed persistence contract is
`\SYSTEM\UI\WELCOME.CFG`: exactly one ASCII byte `1` (show) or `0` (skip);
missing, unreadable or invalid content defaults to `1`. About and shell startup
must use the same file and interpretation.

The About module uses the existing `ICON_ABOUT` runtime icon, a deterministic
conversion of the owner's approved Ciuki portrait. It preserves the English
tagline `A modern Retro OS` and identifies release 0.8.3. The project splash
and Tango icon family remain unchanged.

## References

* [Microsoft title bar design](https://learn.microsoft.com/en-us/windows/apps/design/basics/titlebar-design) — identify the app, keep caption controls visible, and truncate long titles with an ellipsis.
* [Microsoft Win32 About Windows](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-windows) — a title typically names the application or the window's purpose.
* [Microsoft guidelines for app settings](https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings) — user-customizable preferences belong in a settings surface.

## Validation

The final runtime recognized the virtual display as “QEMU VirtIO GPU”, the
driver as “CiukiOS VirtIO GPU 2D”, and the monitor as “QEMU Monitor”. The
800x600 desktop, 640x480x16 preview and post-reset About page at 640x480 are
available from the run artifacts:

- [Display Properties at 800x600](../screenshots/0.8.3/display.png)
- [640x480x16 preview and confirmation dialog](2026-10-04-release-runtime/display-16bit-preview.png)
- [About page at startup](../screenshots/0.8.3/about.png)
- [About reopened at 640x480 after system reset](2026-10-04-release-runtime/about-640.png)

The 640x480x16 preview was cancelled and returned to 800x600; the 640x480x32
preview timed out and also returned to 800x600. A 32-bit mode was then
accepted. After changing “Show this page at startup” to off, the runtime
performed a system reset: the About page did not open automatically, the saved
640x480 profile remained active, and F1 reopened About at that resolution.
The final harness passed both `display_profile_persisted` and
`about_preference_persisted`.

The final run passed; its complete results are in
[`final-runtime/results.json`](2026-10-04-release-runtime/results.json).
