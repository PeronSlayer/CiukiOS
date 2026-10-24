# DOS windows and per-program properties (G17)

Researched on 2026-10-23 against the owner's Windows 98 Resource Kit,
chapter 25, `wrkc25_3.htm` (Changing PIF Files, Changing Memory Settings,
batch files and MS-DOS mode), and Microsoft's original Windows 95 Resource
Kit text, [Setting Properties for MS-DOS-Based Applications](https://techshelps.github.io/MSDN/DNWIN95/HTML/S65BF.HTM).
The latter is a mirror of Microsoft's text. Clipboard semantics follow
[Microsoft's clipboard documentation](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard)
and [standard formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats).
Wheel semantics follow the primary upstream [CuteMouse Wheel API 1.0](https://github.com/FDOS/mouse/blob/master/wheelapi.txt).

## Expected behaviour and implementation decision

Properties belong to the executable, including programs outside the game
catalogue. Program arguments do not create a different profile. Store a
versioned, bounded private profile in `/system/settings`, with the full
normalised executable key in the record; validate it before reading or
replacing a hash-named file. Save by atomic rename. Missing profiles use the
existing defaults; malformed profiles must never become guest commands.

Provide Program, Font, Memory, Screen and Misc pages with Apply, OK and
Cancel. Command line, working directory and startup batch run on the next
launch. Close on exit, background suspension, QuickEdit, fast pasting,
termination warning, font zoom and idle sensitivity are per-program choices.
Memory offers the existing paged/locked compatibility choice; numeric memory
quotas remain automatic and are not represented by ineffective controls.
The existing VMM owns allocation, ROM emulation and protected memory.
Unsupported PIF controls remain documented gaps, rather than saved no-ops.

The DOS window owns a separate display surface. The kernel's 640×480 guest
surface stays intact; the client scales it into the area below the toolbar.
Toolbar actions are Mark, Copy, Paste, Full screen, Properties, Background
and font size. Mark/QuickEdit freezes guest execution at its existing safe
checkpoint, obtains visible text from the VGA model (not OCR), highlights a
rectangle, and copies trimmed rows separated by CR/LF. Toolbar graphics selection is
refused explicitly. Paste uses the toolkit clipboard provider and bounded
guest key events, preserves queue backpressure and releases held keys when
focus changes. Unsupported pasted characters are refused before any typing.

The CiukiOS MS-DOS-mode equivalent is the existing exclusive full-screen
display session under the VMM, started with that program's profile and batch
configuration. It does not unload the VMM or execute arbitrary CONFIG.SYS
drivers. Leaving with Alt+Enter restores the desktop; no real-mode takeover
is introduced.

The existing BACKGROUND operation must also suspend windowed guests on
focus loss, wake on focus return and apply a changed policy immediately.
Idle sensitivity changes the existing empty INT 16h poll threshold, without
changing HLT/INT 28h/INT 2Fh waits or increasing any memory limit.

Wheel events reach INT 33h: 0011h advertises Wheel API, 0003h returns and
clears the signed counter in BH, 0005h/0006h accept BX=FFFFh, and callback
mask bit 7 delivers a separate counter in BH. Positive guest counts mean
downward movement; the desktop input convention is converted at the boundary.

## Remaining PIF compatibility

Native private profiles are not binary Windows PIF import/export. Shortcut
hotkeys, Run minimised/maximised, custom CONFIG.SYS drivers, numeric
conventional/EMS/XMS/DPMI limits, per-program screen-saver permission,
exclusive mouse capture and reserved Windows shortcut keys need further
desktop/kernel interfaces. Screen restore and fast ROM/dynamic video memory
are managed by the existing VGA/VMM implementation, without user toggles.
Host tests establish source behaviour only; the lead owns integration and
runtime qualification.

## Integration with the G5 system clipboard

The merge rechecked [Microsoft Clipboard Operations](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations)
and [Standard Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats).
Both DOS interfaces use G5's desktop-owned store and its exclusive open/close
protocol; the toolbar's toolkit text provider is bound to that transport.
G17's toolbar keeps its rectangular text selection, UTF-8 conversion and
bounded ASCII paste with the persisted fast-paste option. G5's window menu
also retains graphics selection/DIB copy and Unicode-to-CP437 paced paste.
The two marking modes cancel each other when changing interface, and paste
and autorun are serialized. Menu selection coordinates and pixels pass
through G17's guest viewport mapping, keeping the toolbar intact at every
zoom. Deferred desktop messages from clipboard RPC are drained before
reading the channel again.

Main's `CIUKI_DOSVM_TEXT=18` and `CIUKI_DOSVM_MARK=19` remain fixed.
G17's `CIUKI_DOSVM_TEXT_CHUNK=22` and `CIUKI_DOSVM_POLICY=23` follow main's clipboard commands 20 and 21;
all four commands retain their payloads in the unchanged 136-byte command.
The VM checkpoint waits while either marking policy holds the guest.
The fx-15 G4 rows remain 31–35 and G17 follows at 36–40; the executable
scenario `row_dos_controls` is `WM_ROW=29` (zero-based).
