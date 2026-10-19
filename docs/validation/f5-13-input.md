# F5-13 input research and implementation decisions

Research before implementation, 2026-10-19:

- [Microsoft DirectInput SDK header](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/um/dinput.h): retained DX3/DX5/DX8 structures, interface slots, GUIDs, DIDFT flags and property identifiers. Consulted as ABI facts, no implementation imported.
- [Microsoft device formats](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416593(v=vs.85)) and [device properties](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416595(v=vs.85)): copied custom formats, object selection and pre-acquisition properties.
- [Microsoft GetDeviceData](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417894(v=vs.85)): DX3 16-byte/DX8 PE32 20-byte records, peek, null-buffer flush/count, overflow indications.
- [Microsoft SetCooperativeLevel](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417921(v=vs.85)): foreground loss and exclusive mouse pointer hiding. Reject background-exclusive keyboard/mouse per the binding contract's C13 correction.
- [Microsoft ToAscii](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-toascii): character translation and packed two-character/dead-key semantics; Num Lock is resolved by VK, not by ToAscii.
- [Microsoft keyboard layout samples](https://github.com/microsoft/Windows-driver-samples/tree/main/input/layout): separate physical scan/VK and character tables.
- [Andries Brouwer's scan-code observations](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/kbd/scancodes-1.html): set-1, E0, Pause E1 1D 45 E1 9D C5, Ctrl+Break E0 46, PrintScreen/SysRq. The repository kernel input contract already normalizes Pause to 0x200 and supplies repeat edges.
- [Upstream Italian layout](https://github.com/reactos/reactos/blob/master/dll/keyboard/kbdit/kbdit.c): OEM VK numeric cross-check only; explicitly has no dead keys. No Wine DirectInput implementation read or copied.

The online Microsoft legacy documentation and SDK header retain period ABI
structures but are not an inspected original DirectX 7/8 SDK distribution.
Period SDK archival provenance remains a qualification limit.

Repository comparison: USER has scan/VK tables and desktop keymap character
conversion, per-thread dequeued state and global asynchronous state. DirectInput
builtins are placeholders. V3 supplies original relative records separately as
RELATIVE_EVENT (73), and POINTER supports RELATIVE/HIDDEN. Ordinary MOTION is
client position; WARP never generates raw motion. Both GUID factories must share
one engine (dinput8 forwards to dinput) and subscribe through USER, without a
protocol change. Exclusive pointer state overrides ordinary capture/clip and
restores their latest requests when released.

The directive's Italian dead-key premise conflicts with `apps/desktop/keymap.c`:
US and Italian both emit spacing characters directly, including Italian Shift+ì
(`^`). Preserve the desktop's actual mapping; do not invent a different Italian
layout or compose desktop TEXT twice. The pinned dead-key composition set is
empty for these two layouts.

V3 KEY has no LED/toggle snapshot. Track received lock transitions; initial LED
state and locks changed while unfocused cannot be reconstructed. V3 currently
routes raw relative events only to the focused process with POINTER RELATIVE,
and ordinary keys/buttons to the focused client. Background nonexclusive devices
retain acquisition across focus loss, clear stale state, and accept subsequent
routed events; full background delivery requires desktop-owner work, outside
this directive. Cross-process exclusive arbitration also requires desktop/service
authority; process-local instances can be arbitrated here. These limitations
must not be presented as qualified Windows 98 behavior.

Verification and remaining evidence are in `build/codex/f5-13-report.md`.

The v3 parser requires RELATIVE with CAPTURE or CLIP. Nonexclusive mouse
readers request the ordinary screen bounds when no ordinary capture/clip exists;
this leaves pointer movement unconstrained within the screen. Exclusive readers
request CAPTURE|CLIP|RELATIVE|HIDDEN with the window's client bounds. These are
existing flags, and normal capture/clip requests are retained under the override.
Foreground loss is published to DirectInput before application activation
callbacks can inspect or reacquire the device. Background acquisition does not
require a pointer request while unfocused.

Additional inspected references: [Microsoft EnumDevices](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417804(v=vs.85))
(class DEVICE excludes keyboard/mouse), [Microsoft OemKeyScan](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-oemkeyscan)
(CP437 conversion and no CTRL+ALT result). OemKeyScan reuses the personality's
code-page converter. The f5-09 report was read from the main checkout;
f5-10A's tracked validation record was inspected (its ignored report was absent).
The f5-02 report/corpus have no input runtime fixture; f5-13 adds a supplemental
PE input fixture using the same Linux loader adapter rather than relabelling an
unrelated f5-02 structural fixture as input evidence.

Final source cross-check: [Microsoft interpreting mouse axis data](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418272(v=vs.85))
defines absolute DirectInput mouse coordinates as accumulated raw motion from
an arbitrary origin. The written f5-13 directive instead explicitly requests
absolute mode from cursor position. This implementation follows that directive:
relative mode always uses raw counts, absolute mode uses cursor coordinates
clamped to the requested range. This is a specification disagreement for the
lead, not a Windows-compatible absolute-mode claim. No architecture decision
or protocol change was silently substituted for the written requirement.

Cooperative window validation cross-check: [Microsoft GetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getparent)
returns an owner for owned top-level popups; it cannot be used as a child-window
test. SetCooperativeLevel tests WS_CHILD and process ownership instead. The host
fixture pins acceptance of an owned popup and rejection of a child.

Keyboard naming/message cross-check against Microsoft's keyboard-input overview:
Num Lock uses the extended 0x45 key-name entry, while Pause uses ordinary 0x45.
Set the Win32 extended lParam flag for Num Lock as well as E0 positions; its DIK
remains 0x45. This is a logical Windows message distinction, not a change to the
kernel's physical set-1 position. [WM_SYSKEYDOWN](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-syskeydown)
also specifies F10 system-key messages with context bit zero when Alt is absent.
Both cases are pinned in the message fixture.
