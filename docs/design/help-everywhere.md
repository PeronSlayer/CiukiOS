# Help everywhere (G15, 2026-10-24)

Rows HP-1 to HP-4, KB-11, SM-7 of `os-feature-inventory.md`. Windows 98
reference behaviour (all from the Windows 98 Resource Kit text and the
Microsoft documentation named below); the decisions follow it.

## Reference behaviour

- **F1** sends `WM_HELP` (`HELPINFO`, `HELPINFO_WINDOW`: control id, handle,
  help context id, pointer) to the window with the keyboard focus;
  `DefWindowProc` passes it to the parent of a child window. The program opens
  its help. Sources: MSDN `WM_HELP`, `HELPINFO`, `SetWindowContextHelpId`
  (https://learn.microsoft.com/windows/win32/shell/wm-help); Resource Kit
  keyboard chapter (`RK-keys`).
- **Shift+F1** and the "?" caption button (`WS_EX_CONTEXTHELP`,
  `DS_CONTEXTHELP`) send `WM_SYSCOMMAND` `SC_CONTEXTHELP` (0xF180): the pointer
  becomes the help arrow (`IDC_HELP`), the next click is not delivered but sends
  `WM_HELP` for the window under it; Esc cancels. The "?" replaces minimise and
  zoom. Source: MSDN `WM_SYSCOMMAND` (https://learn.microsoft.com/windows/win32/menurc/wm-syscommand).
- **Tooltips** appear after the pointer rests about 500 ms and go away after
  5 s, a click or a key (`TTM_SETDELAYTIME` defaults, Resource Kit `RK-vmm`).
- **MB_HELP** message boxes: the Help button and F1 send `WM_HELP` to the owner.
- **Start > Help** opens Windows Help itself (no submenu).
- **Help** has Contents (books and topics), Index (keywords) and Search (all
  words); Back returns to the previous topic.

## What CiukiOS does

- `apps/accessories/help*.c`: 42 topics: the welcome page and four books (Using CiukiOS, Programs,
  Troubleshooting, Reference), one topic per built-in
  program (id = program name), Contents tree, Index tab (sorted keywords,
  type-ahead), Search tab (every word, prefix match, quoted phrases, ranking
  title 8 / keyword 6 / text 1), Back (16 deep). `help topic:<id>` opens a
  topic; a file path still opens a read-only text file.
- libdeskui: `du_help_topic`, `du_help`, `DU_EV_HELP` (F1 and context help;
  `help_consumed` lets a program answer itself), `widget.tip` tooltips,
  `widget.help` What's This? text, Shift+F1 mode with the help cursor, the
  dialog "?" button (`du_dialog_help_button`). A program without a topic keeps
  F1 as a key (DOS window). The client opens Help with `DESK_LAUNCH` of `help`
  and argument `topic:<id>`; the desktop accepts exactly that (or no argument)
  from any program (`desk_help_argument`).
- Desktop: Start > Help opens Help (About CiukiOS moved to the Start menu
  root, keyboard shortcuts are a Help topic); F1 on the empty desktop opens
  the desktop topic. v3 style bit `DESK_V3_CONTEXTHELP` (16) gives the "?"
  caption button in the minimise slot (also on owned windows); a press sends
  server-to-client `DESK_V3_HELP` (empty). Theme glyph `DESK_GLYPH_HELP`,
  `DESK_TB_HELP`. Window-model row 63 (renumbered after the integrated row 62).
- Win32 user32: F1 -> `WM_HELP` through `DefWindowProc` (controls forward F1),
  parent forwarding, `GetWindowContextHelpId`/`SetWindowContextHelpId`,
  `SC_CONTEXTHELP` mode with `IDC_HELP`, click and Esc handling in `input.c`,
  `MB_HELP`, `WS_EX_CONTEXTHELP`/`DS_CONTEXTHELP` -> v3 style, `DESK_V3_HELP`
  -> `SC_CONTEXTHELP`.

## G15B delivery

- `help_hlp.c`: checked internal-file B+ trees; block LZ77; WinHelp 3.x
  `|Phrases` and 4.x `|PhrIndex`/`|PhrImage`; topic headers and paragraph
  formatting records; `|CONTEXT`, numeric `|CTXOMAP`, `|KWBTREE`/`|KWDATA`;
  topic, external-file and macro hotspots. Original font and image layout is
  reduced to text and image placeholders. Related links preserve their text.
- `help_chm.c` with the pinned libmspack CHM/LZX reader: ITSF/ITSP,
  uncompressed and MSCompressed members, reset tables/SpanInfo, #SYSTEM,
  #IVB/#STRINGS, HHC hierarchy and HHK keywords. `help_markup.c` reads text,
  links and named anchors; script/style contents are excluded.
- WinHelp navigation macros: JumpId/JI, PopupId/PI, JumpContext/JC,
  PopupContext/PC, JumpHash/JH, PopupHash/PH, Contents, Finder, Index,
  Next/Prev/BrowseNext/BrowsePrev and KLink/KL/ALink/AL. Macro strings are
  parsed completely before a page changes. The UI also handles Back/B,
  Exit and CloseWindow. Browse uses the file's sequence addresses.
- `help_navigation.c` and `help_view.c`: built-in related-topic links,
  external relative links, anchors, Back, external Contents/Index/Search,
  and long-topic parts. Limits: 16 MiB input, 8 MiB decoded text, 1 MiB per
  topic, 1,024 topics, 2,048 contents entries, 4,096 index entries and 128
  links/anchors per topic. The 60,000-byte view part keeps the existing
  textarea capacity; Next/Previous Part exposes the rest. Cached allocations
  shrink to the decoded data, rather than retaining maximum-size buffers.
- Eleven reversible question-driven troubleshooters cover unresponsive
  programs, sound, display, DOS games, network, disks, startup/shutdown,
  memory, printing, hardware conflicts and Windows game graphics. Next is
  disabled until an answer is chosen; Back, Finish and Cancel work. They
  provide guidance and never silently change settings.
- `du_set_text` supplies default tooltip and context-help strings by control
  type and common action name. Explicit program strings take precedence;
  typed text is never used as its tooltip. Modal controls use the same
  defaults and now participate in delayed tooltip tracking. Host coverage
  checks every registered control of the 18 native application cases.
- The existing `DESK_LAUNCH` opcode carries the bounded argument described in
  `apps/desktop/help_request.h`. v2 server-to-Help launches forward new
  requests to the existing trusted Help client; v3 client requests may launch
  only Help. The desktop restores/focuses its existing window, including
  during asynchronous startup, and tracks WinHelp callers for HELP_QUIT.
  No syscall, devctl, message number or public ABI record was added. The
  background loader discards superseded requests; repeated requests for an
  open file preserve its Back history.
- `user32/help.c` implements ANSI/Unicode WinHelp dispatch, context/control
  maps, keywords and macros. `hhctrl.ocx` exports HtmlHelpA/W at ordinals
  14/15, forwarded to USER. Supported HTML Help commands: display topic,
  contents, index, search tab, numeric context and close all. Windows paths
  resolve through GetFullPathName/GetFileAttributes before conversion to
  native drive paths. HtmlHelp returns a real hidden USER child proxy which
  forwards WM_CLOSE; it is not a cross-process HWND for the native viewer.
  [Microsoft HH_HELP_CONTEXT](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/htmlhelp/hh-help-context-command)
  documents replacing the topic in an open help window. The primary export
  reference is [Wine's HHCTRL specification](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/hhctrl.ocx/hhctrl.ocx.spec).

## Remaining compatibility work

HP-5 stays partial: original HLP fonts, bitmap placement, tables and context
popup geometry; arbitrary help-defined toolbar/menu macros, conditional macros,
marks and commands which execute programs; binary CHM indexes and active HTML;
HtmlHelp embedding, positioning and a true foreign viewer HWND. These need
additional rendering/API work rather than successful-return stubs. The small
navigation subset above returns an explicit unsupported-command error for
other macros. Cross-file navigation currently resets Back history; Windows 3.0
TOMap compatibility is not qualified. G16 owns the file dialogs' caption ?
buttons. Programs without a Help menu still have F1.
The Win32 bridge currently requires an existing desktop window/transport;
HWND-less programs need a broker path. CHM text uses plain text rather than a
browser's CSS, image and full Unicode rendering. Standard Help command tails
are bounded by the existing 160-byte launch argument. The SDK's `off_t` is
64-bit; both native and host decoder recipes use `SIZEOF_OFF_T=8`, with the
documented warning-only upstream portability patch. Prefix decoding is capped
at 32 MiB independently of the output allocation limit.

The owner-local Resource Kit media were read in place for host validation;
their text and binaries were not copied into this repository. Both CHMs and
TZEDIT, WINDIFF and DEPENDS HLP topic streams decode in the host harness.
ASan/UBSan assertions and frame comparisons reach their success markers;
LeakSanitizer teardown requires a lead rerun outside this sandbox, whose
ptrace restriction prevents LSan's thread inspection. This is not a green
LSan result. See `build/codex/g15b-report.md` for exact commands and results.

## G15B reference and implementation contract

Research: Microsoft [WinHelpA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-winhelpa)
defines context, keyword, macro and contents requests; a missing topic must not
silently open an unrelated page. The [Wine 10.0 WinHelp reader](https://github.com/wine-mirror/wine/blob/wine-10.0/programs/winhlp32/hlpfile.c)
provides the primary implementation reference for HLP B+ trees, block LZ77,
3.x/4.x phrase dictionaries and topic addresses. The
[libmspack CHM decoder](https://github.com/kyz/libmspack/blob/55d501976171397ccd5d5a7a1ca7da065b1d9a06/libmspack/mspack/chmd.c)
implements ITSF directories, reset tables and LZX. Reuse that pinned decoder,
with a bounded native memory adapter, rather than implementing another LZX.
[Wine HTML Help](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/hhctrl.ocx/chm.c)
documents through its implementation the #SYSTEM defaults and #IVB/#STRINGS
context aliases. HHC/HHK sitemap objects supply Name/Local entries.
The owner's read-only Resource Kit `build/rskt/wrkc27_1.htm`, “Checking for
Common Issues”, specifies selecting a troubleshooter in Help and progressively
eliminating possible causes; `wrkc09_7.htm` describes clicking ? then a control.

Decisions: external help is read-only topic text with selectable cross-links,
Contents and Index, using the existing Help window. No help macro executes
shell commands or loads native code. Navigation macros resolve through the same
checked topic lookup as links; unsupported commands return an explicit error.
Archive input, decoded topic storage, topic count and per-topic text are bounded;
invalid offsets, truncated dictionaries and cycles fail without replacing the
current page. HLP rendering preserves readable text rather than original GDI
font/bitmap layout. Troubleshooters ask questions, provide Back/Next/Cancel and
record answers only in memory. Toolkit control defaults supply help by control
type and action name, with program-specific strings taking precedence.
