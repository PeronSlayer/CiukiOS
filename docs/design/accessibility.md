# Accessibility (G25)

Directive `docs/directives/g17-g32-os-gaps-batches-4-5.md`, item G25; inventory rows AC-1 to AC-8,
KB-17, WM-21, CP-1 of `docs/design/os-feature-inventory.md`. Written 2026-10-24, before the code.

## Reference behaviour

Primary sources (read for this item):

- *Microsoft Windows 98 Resource Kit*, appendix H, "Configuring Accessibility Features Using Control
  Panel" (`wrkapph_3.htm`), "Configuring … with the Accessibility Wizard" (`wrkapph_4.htm`), "Using the
  Accessibility Features" (`wrkapph_5.htm`: hot keys, status indicator, time-out, Magnifier); appendix G,
  "Accessibility Shortcuts" (`wrkappg_3.htm`). Extracted at `build/rskt/`, never redistributed.
- Microsoft Learn, structures that Windows itself uses for these features, with the flag values and ranges:
  [STICKYKEYS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-stickykeys),
  [FILTERKEYS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-filterkeys),
  [TOGGLEKEYS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-togglekeys),
  [MOUSEKEYS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mousekeys),
  [SOUNDSENTRY](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-soundsentrya),
  [HIGHCONTRAST](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-highcontrasta),
  [ACCESSTIMEOUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-accesstimeout),
  [SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa).

What Windows 98 does, as the sources state it:

| Feature | Behaviour |
| --- | --- |
| StickyKeys | One tap latches a modifier (Shift, Ctrl, Alt) until the next non-modifier key or mouse click; two taps lock it until a third tap. Two keys pressed together switch it off (option). Tones: low-high beep for a latch, one high beep for a lock. Hot key: Shift five times. |
| FilterKeys | SlowKeys (a key must be held for an acceptance delay), RepeatKeys (delay and rate of repetition, or no repetition), BounceKeys (a repeated press of the same key within a time is ignored). "Beep when a key is pressed or accepted". Hot key: right Shift held eight seconds; three warning beeps at four seconds, a siren at eight; at twelve seconds the first emergency setting (no repeat, no acceptance delay, repeated keystrokes ignored), at sixteen the second (no repeat, two-second acceptance delay). |
| ToggleKeys | High tone when Caps, Num or Scroll Lock turns on, low tone when it turns off. Hot key: Num Lock held (the Resource Kit table says five seconds, its procedure text and Microsoft Learn say eight). |
| MouseKeys | The numeric keypad moves the pointer: 8 keys around 5; 5 clicks, + double-clicks, Ins starts a drag, Del releases; / - * select the left, right or both buttons (both = middle button); Ctrl jumps, Shift moves one pixel; Num Lock toggles the keypad back. SlowKeys applies to these keys. Hot key: left Alt + left Shift + Num Lock. |
| SoundSentry, ShowSounds | A visual cue (title bar, window or display flash) whenever the computer makes a sound; ShowSounds is a global flag asking programs for captions. |
| High Contrast | A global flag plus a colour scheme; 12 selections in the Resource Kit. Hot key: left Alt + left Shift + Print Screen. |
| Hot keys | Each plays a rising siren for on and a falling one for off, and shows a confirmation dialog that the user can cancel. |
| Status indicator | An icon in the taskbar (or free-floating) showing which features are on, the latched Shift/Ctrl/Alt rectangles, the mouse buttons (shaded selected, filled held) and a stopwatch for FilterKeys. |
| Time-out | After the idle time (5 to 30 minutes) the features are switched off (FilterKeys, MouseKeys, ToggleKeys, StickyKeys and high contrast), optionally with a falling siren. |
| Magnifier | Up to nine times; follows the mouse, the keyboard focus and the text-editing focus; invert colours; High Contrast; the magnified area is a display only; keys: Windows+Up/Down level, Windows+Page Up invert, Windows+Page Down mouse tracking. |
| Mouse pointers | Schemes with large and inverted pointers (Mouse > Pointers). |

## CiukiOS design

### Where it lives

- **`apps/desktop/access.c`, `access.h`** — the engine: pure, integer, no I/O. One raw input event or one clock tick in;
  events to deliver, tones, state changes to persist, a confirmation to ask and the indicator state out.
  Tested alone (`tests/host/desktop/access_test.c`, 15 groups).
- **`apps/desktop/access_desktop.c`** — the desktop glue: `desk_input` hands every event to the filters first
  (`desk_access_filter`, weak: programs that link `input.c` without it see the raw stream); the filter delivers what
  remains through `desk_input_unfiltered`. It owns the tray indicator, the confirmation window, SoundSentry/ShowSounds,
  high contrast re-theming, the pointer scheme reload and the Magnifier state.
- **`desktop.c`** calls `desk_access_tick` every service turn and adds `desk_access_wake` to the loop's deadlines
  (pause reason `access`), so SlowKeys, the hot-key holds, MouseKeys and the time-out wake the loop exactly when due.
- **`settings.c`** stores everything as `access_*` lines of `desktop.cfg`; `access.h` keeps the record and the
  parser inputs header-only so Setup, which links `settings.c`, needs no engine.
- **`apps/control/access.inc`** — the Accessibility Options applet (Control Panel, Input category).
- **`sdk/win32/user32/system.c`** — the SPI records and `SM_SHOWSOUNDS`.
- **`theme.c`** — four high-contrast schemes, not listed among the selectable themes.
- **`compositor.c`**, **`assets/cursors`** — pointer schemes, overlay boxes and the Magnifier panel.

The records are the Win32 structures' flag words (`SKF_*`, `FKF_*`, `TKF_*`, `MKF_*`, `SSF_*`, `HCF_*`, `ATF_*`), so
`SystemParametersInfo` is a field copy and the Control Panel, the desktop and the Win32 layer agree on meaning.

### Event path

```
kernel input -> desk_drain_input -> desk_input
   -> desk_access_filter: hot keys -> FilterKeys -> MouseKeys -> ToggleKeys -> StickyKeys
        -> desk_input_unfiltered (shell shortcuts, focus routing)
```

Hot keys see the raw stream first so they work with FilterKeys on; MouseKeys keys go through FilterKeys (the
Resource Kit says SlowKeys applies to them); StickyKeys sees only what survived, so a latched modifier reaches
the program as a real Shift/Ctrl/Alt down, and text derives from it as it does from a real key
(`apps/libdeskui/widgets.c`: "text derives from KEY events plus Shift/Caps"). MouseKeys emits motion and button events
with source `DESK_ACCESS_SOURCE`, which skips the Control Panel pointer-speed scaling (a step is an exact pixel).

### Exact behaviour implemented

StickyKeys (Shift, Ctrl and Alt as in the Resource Kit; the Windows keys pass through so the Start menu still opens on their release): a latch emits the modifier's key-down once; the physical release is swallowed; the next non-modifier
key-down passes and then the latched modifiers are released; a mouse-button release also releases them. A second tap locks
(when "press twice to lock" is on), a third unlocks. A combination ends with a modifier release; if
`SKF_TWOKEYSOFF` the feature switches off then and every held modifier is released. Five Shift presses (either Shift,
consecutive: any other key press resets the count) switch it; the fifth press is consumed. Tones only with
`SKF_AUDIBLEFEEDBACK`.

FilterKeys: with a non-zero bounce time, BounceKeys alone (a press within the time after that key's release is ignored,
with its repeats and release); otherwise SlowKeys when the acceptance delay is non-zero (the press is held back and
delivered by the tick when the key has been down long enough; a shorter press never happened), and RepeatKeys on the
hardware repeat events (value 2): dropped until the repeat delay, then at most one per repeat rate; `FKF_NOREPEAT`
(a CiukiOS flag, `0x1000`, not a Win32 bit) drops them all. Hardware typematic cannot be made faster than the
keyboard produces it; the filter only slows it. The emergency levels set bounce 500 ms plus `NOREPEAT`, then
acceptance 2 s plus `NOREPEAT`, as the Resource Kit describes, and are saved.

ToggleKeys: the desktop counts Caps, Num and Scroll Lock presses from "off" (there is no LED query; PS/2 LED
control is G12), so the tones follow the desktop's count.

MouseKeys: active when the feature is on and Num Lock matches `MKF_REPLACENUMBERS` (default: keypad moves the pointer
when Num Lock is off, the Microsoft Learn default). The Num Lock press of the hot key is consumed, so the lock state
is not disturbed. A direction key moves one pixel at once, nothing for 250 ms, then accelerates from max/10 (at least
8 px/s) to `iMaxSpeed` pixels per second over `iTimeToMaxSpeed`; diagonals are scaled by 181/256; with
`MKF_MODIFIERS` Ctrl multiplies by `iCtrlSpeed` and Shift gives 6 px/s (single pixels with a pause). The speed unit is
ours: Windows 98 documents "maximum speed" without a unit; pixels per second with a default of 80 gives a slow, precise
pointer, 360 crosses a 1024-pixel screen in three seconds, Ctrl x8 jumps.

High contrast: the flag and the scheme name are part of `desktop.cfg` (`access_contrast=<flags>,<scheme>`, written
right after `theme=`); the theme in force is `cfg_effective_theme()`: the scheme while on, otherwise `theme`. The
desktop, the Control Panel and every libdeskui program follow the file's first 4 KiB (`desk_settings_theme`), so all
re-theme within two seconds. The four schemes (`theme.c`): High Contrast Black (white on black, cyan selection),
High Contrast White, #1 (yellow on black, blue selection), #2 (green on black, yellow selection). The palette follows
the Windows 10 high-contrast colours (the Resource Kit names no values); the contrast ratios are checked in
`access_settings_test.c` (text >= 7:1, selection and captions >= 4.5:1, disabled text >= 3:1; the measured minimum
body ratio is 15.3:1). They are flat (no gradients, radius 0, no shadow, same frame geometry as every theme).

Time-out: idle time is the last input of any kind (key, button, motion, wheel); at the deadline the five
features switch off, with a falling siren if `ATF_ONOFFFEEDBACK`. The changes are persisted (the features stay off).

Confirmation: a hot key that switches a feature **on** and has `*_CONFIRM` opens the message window "Do you want to use X?";
Yes keeps it, No, Esc or closing it switches it off again. (Windows 98's OK/Cancel/Settings becomes Yes/No plus a
sentence pointing at Control Panel; the shell's one question window already exists.)

Tones: synthesised (`access_tone_render`, sine with 2 ms fades; sirens 600 to 1800 Hz) by the sound worker through the
same mixer as the system sounds; they play whatever the sound scheme says (they are accessibility cues) but never over a
mute or a running DOS VM (`permitted()`).

SoundSentry and ShowSounds: the desktop's alert sounds (error, information, question, End Task, device
connect/disconnect, AutoPlay, Recycle Bin emptied, start-up, shut-down, ready) trigger a 250 ms inverted flash of the
active title bar, the active window or the whole screen (a shell overlay painted last, cut from screen copies like the
other overlays) and, with ShowSounds, a caption in the notification area. Window and menu feedback sounds are interface
clicks, not alerts, and do not flash. Full-screen DOS programs own the display and the input; their sounds and keys are
not seen by the desktop (gap).

Status indicator: tray slot after the power icon, shown while StickyKeys, FilterKeys or MouseKeys is on (and
"Show status indicator on screen"): three key rectangles (filled latched, solid locked), a mouse with the selected
button shaded and the held one filled, a stopwatch. A click opens the applet; the right button the tray settings menu.

Pointer schemes (`access_general=...,<pointer>`): Ciuki Standard, Large, Inverted, Large Inverted. Large is the same
SVG drawings rasterised at the pre-fx-13 scale (1.0 instead of 0.7 around the hotspot, outline 0.8 of its drawn width; the arrow
grows from 13x19 to 18x26 opaque pixels), generated by `scripts/cursors/build_ciuki.py` into `cursors.h`
(`--check` reproduces it). Every presenter and the hardware pointer are limited to a 32x32 raster, so the scheme stops at
that size. Inverted swaps black and white inside the mask at run time. The set is the owner's Ciuki cursor set; no
other artwork is involved.

Magnifier (`access_magnifier=<on>,<level>,<tracking>,<invert>,<dock>,<percent>`): a panel across the full width at the top
(below the top bar) or bottom (above the taskbar), 10 to 50 % of the screen high, showing 1/level of the screen
around the tracked point at an integer level 1 to 9, nearest neighbour, optionally inverted, with the pointer
magnified too. It is not an active area. Tracking follows the pointer, the focused window (centre), or whichever
moved last; "text editing" has no caret protocol to follow and behaves as the focus (gap). The panel never covers the
pointer or its own source: it jumps to the other edge when the pointer (or the source) comes near it. It is a shell
overlay in `scene()`'s sense: the compositor paints it as the last pass after the software pointer
(`magnifier_compose`), and `desk_scene_reconcile` damages it when the source moves, when damage falls in the source or the
panel, or when the pointer shape changes. Screen copies and client fast paths never cross it
(`desk_access_overlay`). Windows+Up/Down change the level, Windows+Page Up inverts, Windows+Page Down toggles mouse
tracking (only while the Magnifier is on); Start > Programs > Accessories > Magnifier and the applet turn it on and off.

### Settings and the Control Panel

`access_sticky=<hex>`, `access_filter=<hex>,<wait>,<delay>,<repeat>,<bounce>`, `access_toggle=<hex>`,
`access_mouse=<hex>,<max>,<time>,<ctrl>`, `access_sentry=<hex>,<effect>`, `access_contrast=<hex>,<scheme>`,
`access_timeout=<hex>,<seconds>`, `access_general=<showsounds>,<status>,<pointer>`, `access_magnifier=...`.
Invalid values are rejected line by line (counted by `cfg_parse`, the default stays), including the Win32 ranges
(MouseKeys 10..360 and 1000..5000 ms, filter times <= 20 s, time-out 1 to 60 minutes).

Defaults are the Windows defaults with the hot keys enabled: StickyKeys 0x1fe (510), FilterKeys 0x7e (126,
1000/1000/500/0), ToggleKeys 0x3e (62), MouseKeys 0x7e (126; modifiers on), high contrast 0x7e, 5-minute time-out
(off), status indicator on.

The applet has the Windows 98 tabs (Keyboard, Sound, Display, Mouse, General) and a Settings page with OK and Cancel for each
feature, built from the same toolkit controls. It saves on every change (live feedback, as Mouse and Sounds do),
Cancel restores what the page was opened with, and only the fields it changed are written (`ac_merge`), so a hot key
used meanwhile is not undone; it re-reads the file once a second and shows the desktop's state.

### Win32

`SystemParametersInfo` gets and sets STICKYKEYS, FILTERKEYS, TOGGLEKEYS, MOUSEKEYS, SOUNDSENTRY, HIGHCONTRAST and
ACCESSTIMEOUT with the Microsoft validation (cbSize and uiParam, the BounceKeys exclusion, 20 s, MouseKeys ranges, no
custom SoundSentry effect, reserved fields), and `GetSystemMetrics(SM_SHOWSOUNDS)` follows `SPI_SETSHOWSOUNDS`. Like every
other SPI value they are per process (`sdk/win32/user32/system.c` header comment): a Win32 program cannot yet change the
system, and the desktop's settings are not visible to it. Bridging both ways is a field copy once G12's persistence
channel exists. SerialKeys (SPI 62/63) is not provided.

### Screen-changing events (fx-15 window model)

Rows 31 to 34 of `docs/design/desktop-rendering-architecture.md`; every one is the test row `accessibility`
(`window_model_test.c` `row_access`, five presenters).

## Decisions for the lead

1. **Hot keys are on by default**, as in Windows. Five Shift presses (or Alt+Shift+Print Screen) in a windowed DOS
   program will open a dialog. Change `access_defaults` to switch them off.
2. **ToggleKeys hold time**: five seconds (the Resource Kit hot-key table and the inventory), not eight (Microsoft
   Learn and the Resource Kit's own procedure).
3. **MouseKeys speed unit** is pixels per second (above); Windows 98 does not define it.
4. **High-contrast palette** follows Windows 10 values and the four scheme names `High Contrast Black/White/#1/#2`;
   the Resource Kit's 12 selections need large-font variants that depend on G23 (DPI).
5. **Pointer schemes** are offered in Accessibility Options > Display; the Mouse page (G12) should list the same four
   (`access_general` pointer index) when it lands.
6. **Hot-key confirmation** is Yes/No, not Windows 98's OK/Cancel/Settings.

## Not done (listed in the report)

Accessibility Wizard; scalable interface elements (large fonts, DPI: G23); SerialKeys; "follow text editing" (no caret
channel); extra-large pointers; animated pointers; Win32 SPI persistence and a Win32 view of the desktop's settings;
filtering of full-screen DOS sessions; free-floating status indicator window; per-user settings; QEMU and T23
evidence (not run, by the rules of this task).
