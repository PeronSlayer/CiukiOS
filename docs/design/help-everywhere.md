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

- `apps/accessories/help*.c`: 37 topics: the welcome page and four books (Using CiukiOS, Programs,
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
  `DESK_TB_HELP`. Window-model row 31.
- Win32 user32: F1 -> `WM_HELP` through `DefWindowProc` (controls forward F1),
  parent forwarding, `GetWindowContextHelpId`/`SetWindowContextHelpId`,
  `SC_CONTEXTHELP` mode with `IDC_HELP`, click and Esc handling in `input.c`,
  `MB_HELP`, `WS_EX_CONTEXTHELP`/`DS_CONTEXTHELP` -> v3 style, `DESK_V3_HELP`
  -> `SC_CONTEXTHELP`.

## Not done

WinHelp/HTML Help engines (HP-5), interactive troubleshooter wizards (the
Troubleshooting book is step lists), Help menu entries in programs that have
no Help menu (F1 works everywhere), a single Help window reused across
requests, topic cross-links, tooltips on every toolbar of every program (Files
toolbar and Help have them), "?" buttons on the toolkit's file dialogs (G16
replaces them).
