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

## A program that ends in its first seconds (2026-10-24)

Windows 98 keeps a DOS window titled "Finished - <name>" with the program's
last screen when the program ends and "Close on exit" is off; a program
started from the Games menu that fails at once ("not enough memory", a
missing file) otherwise vanished before its window ever showed it, so the
user saw nothing. Decision: when a launched program returns to the prompt
before the window showed its first frame, or within its first 10 s
(`DOSBOX_EARLY_EXIT_MS`, `apps/dosbox/loading.h`), the DOS Box shows the
VM's text screen, titles the window "Finished - <name>" and keeps it open even
with "Close on exit" on, with the notice "The program has ended. Read its
messages, then close this window." A program that ran longer still closes
its window on return with "Close on exit". Host replay: DOS Box scenarios 23
and 24 (return 20 ms after the command, no frame), 31 (shown for 12 s, closes)
and 32 (shown at 0.5 s, returns at 2 s, keeps the window); the end-to-end
game launch "exit" mode quits after 11 s.

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

## Window chrome (visual audit 2026-10-24)

Reference: the Windows 98 MS-DOS Prompt toolbar (font size combo, then icon
buttons Mark, Copy, Paste, Full screen, Properties, Background, Font) and its
tabbed Properties sheet (Program, Font, Memory, Screen, Misc with OK, Cancel
and Apply), as described in the same Resource Kit chapter and the Windows 95
Resource Kit page cited above; no Microsoft artwork is used.
Decision: the 28-pixel toolbar keeps its size (the guest mapping depends on
it) and is drawn with libdeskui primitives and Tango icons (`edit-select-all`,
`edit-copy`, `edit-paste`, `view-fullscreen`, `document-properties`,
`preferences-desktop-wallpaper`, `preferences-desktop-font`), with hover,
pressed and toggled states (Mark active, Background on) and an immediate
tooltip. The font size is a combo (Fit, 100%, 125%: the real choices, not the
Windows font list) whose drop list uses the existing menu. The Properties
sheet uses the toolkit's tab, edit, combo, check box and slider looks; only
fields that exist in the stored profile are shown. The toolkit widget tree is
not linked into this client: it paints with the same primitives and shares one
geometry table between painting and hit-testing (`window.h`).

## Screen usage: games start full screen (fx-dosfull, 2026-10-27)

Owner decision 2026-10-27: every DOS game starts full screen by default;
Alt+Enter switches to a window; the choice is remembered per program.

Research. The owner's Windows 98 Resource Kit, chapter 25 (`wrkc25_3.htm`,
"Running Games"): "If you run a game that uses graphics modes, and Windows 98
fails to run it in a full screen, press ALT+ENTER. To run the game in a full
screen every time you start it, ... click the Screen tab, and then in the Usage
area, click Full-screen"; most popular games are listed in Apps.inf, whose
`[PIF95]` sections apply per-program settings without user configuration
(Table 25.2: `win` "Run in a window" disabled means full screen, `aen` the
ALT+ENTER key). Microsoft's Windows 95 Resource Kit text
([Setting Properties for MS-DOS-Based Applications](https://techshelps.github.io/MSDN/DNWIN95/HTML/S65BF.HTM))
describes the Screen tab's Usage: a window, "a full-screen window", or a
window sized by the graphics mode. The PIF "WINDOWS 386 3.0" record keeps
full screen as an option flag (`0008 full screen` in Wine's
[winevdm.c](https://goma.googlesource.com/wine/+/stable/programs/winevdm/winevdm.c)
`pif386rec_t`). Windows 95 and later remember the mode a DOS program was in
when it exited and reopen it in that mode (Indiana University Knowledge Base,
"In Windows, how do I switch the MS-DOS command prompt between windowed and
full-screen mode?", kb.iu.edu/d/ahgx, archived). The DOS prompt itself starts
in a window (Dosprmpt.pif), as the owner's decision keeps it.

Decision (`apps/dosbox/program.h`, `dosbox.c`, `window.h`):

* The profile's `screen` field is the Screen tab's Usage, now four values:
  Window (0), Full screen (1, the whole session from its first frame),
  Exclusive full screen (2, also paused in the background, the G17 MS-DOS mode
  equivalent) and **Full screen for graphics** (3), the default of every
  program: its text screens stay in the window and its first graphics mode
  switches to full screen. The DOS prompt (`@PROMPT`) defaults to Window, and
  a text-only program never switches. Profiles move to `DFP2`; a `DFP1`
  record had no usage of its own (Window was the only default), so a
  program's Window becomes the new default on load; explicit 1 and 2 are kept.
* Graphics comes from the kernel: STATUS sets `CIUKI_DOSVM_GRAPHICS` (256) for
  an active VBE mode, a BIOS graphics mode in the BDA (0049h other than 0-3
  and 7), or a windowed VGA model programmed into a graphics layout without
  INT 10h. The DOS Box enters through the existing `CIUKI_DOSVM_FULLSCREEN`
  path (`dosvm_fullscreen.c`: VGA transport, BIOS VBE state on panels from
  fx-biosvm2, native SuperSavage where proven) once the launched program runs,
  the window has the focus and no Properties, Mark or paste is active.
* Alt+Enter stays a kernel input path. STATUS sets
  `CIUKI_DOSVM_FULLSCREEN_HOTKEY` (512) when the last full-screen entry or
  return was the user's Alt+Enter; the DOS Box then stores the usage the user
  chose (`dosbox_usage_after`: back to the window is Window; full screen from
  Window is graphics full screen in a graphics mode, else the whole session).
  The toolbar's Full screen and the window menu's are remembered the same way.
  Only the usage is stored (`dosbox_profile_usage_save`): a program without a
  stored profile gets an empty command line, which keeps the command its
  launcher types (Games entry arguments, shortcuts).
* A refusal (QEMU's standard VGA readback, f3-29; a quarantined BIOS video
  family) keeps the game in its window with no dialog and no notice, writes one
  `dosbox: fullscreen auto ... result=refused ... continuation=windowed`
  record beside the kernel's one `fullscreen_refused` record, is not retried
  in that session and changes no setting. A busy display or a focus race is
  retried after 1 s. A return the user did not ask for (a VBE mode change,
  Alt+Tab, another window taking the focus) enters again when the window has
  the focus, 1 s after the return at the earliest (Alt+Tab's focus change
  reaches the desktop after the return) and at most 16 times per session; a failed session (watchdog,
  display error, a full game-data notice) is not entered again by itself.
  When the program ends and the window stays ("Finished - <name>"), the DOS
  Box leaves full screen so its last screen is readable.
* Files' Properties for an MS-DOS program (MZ without a PE header, or a
  non-empty .COM/.BAT, as `desk_w32_identify` decides) has a Screen page with
  the Usage choices, stored in the same profile under the key of the command
  the desktop types for that path (`dosbox_native_key`, checked against
  `desk_w32_dos_command`). The DOS Box's own sheet names the field Usage.
* The DOS Box's stderr is line buffered (as the desktop's since fx-acc1), so
  each record is one `[console]` line.

Overlap with sec-k05/sec-user-05: the usage lives in the existing DOS Box
profile (`DOSBOX_SETTINGS_DIR`, `dos-program-<hash>.cfg`), so it moves into the
user profile with the other DOS settings; Files reaches it only through
`program.h`, so moving `DOSBOX_SETTINGS_DIR`/`dosbox_profile_path` moves both
consumers. The kernel change touches `dosvm_client.c` (STATUS flags) and
`struct dosvm` (`fullscreen_hotkey`), next to sec-k05's own edits there.

Evidence (host): kernel STATUS flags (`dosvm_fullscreen_test.inc`
`test_fullscreen_dosfull`); the production DOS Box over the production kernel
boundary (`tests/host/dos/game_launch_*.c`: `frame`/`exit` refuse full screen
once and continue in the window, `fullscreen` enters at mode 13h, the user's
Alt+Enter returns and is remembered as Window, frames reach the window, no
second entry); the DOS Box replay scenarios 33-37 (automatic entry, remembered
Alt+Enter, refusal, mode-change re-entry and watchdog stop, busy retry);
profile defaults, migration and the usage-only save
(`dosbox_properties_test.c`); the Files Screen page (`files_properties_test.c`,
frame `g6-dos-screen`); and the acceptance case `acc-dos-games`, whose steps
accept either outcome: full screen at the first graphics mode on a display
that accepts it, or the window after QEMU's refusal.
