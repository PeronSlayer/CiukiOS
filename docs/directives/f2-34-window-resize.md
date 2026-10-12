# Directive f2-34: window resizing in the desktop protocol and toolkit

- **Step:** F2+ (desktop). **Contracts:** `docs/desktop.md` (window protocol
  v2, surfaces, damage), f2-30/f2-31 sections of `docs/design/f2-acceptance.md`,
  `docs/design/execution-abi.md` (surfaces), `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-34` from `wt/integ2`. Files: `apps/desktop/protocol.h/.c`,
  `client.c`, `input.c`, `compositor.c`, `shell.c` (frame hit testing),
  `apps/libdeskui/client.c`, `deskui.h`, `widgets.c`, the three applications
  (`apps/files`, `apps/ciuknote`, `apps/tasks`) and the accessories only for
  relayout on a new size, `tests/host/desktop/*`, `docs/desktop.md`.

## Problem

Client surfaces have a fixed size in protocol v2: `DESK_CREATE_WINDOW` carries
the size, `DESK_CONFIGURE` confirms it, and the frame painter of the new look
draws no resize handles. Users expect to resize windows by dragging the
borders and corners and to zoom to the work area; the "zoom" title button
currently collapses (windowshade).

## Scope

1. **Protocol.** Add a client-to-desktop capability flag at `CREATE_WINDOW`
   (resizable, with min/max size and an optional size step) and a
   desktop-to-client `DESK_CONFIGURE` with a new size and a new surface: the
   desktop allocates the new surface, sends it with the CONFIGURE (fd passing
   as at creation), the client repaints into it and acknowledges with its next
   `PRESENT`/damage carrying a configure serial; until the acknowledgement the
   desktop keeps compositing the old surface scaled or clipped (choose and
   document; no flicker, no stale pixels beyond one frame). Old clients without
   the flag keep today's fixed size. Version the protocol as `docs/desktop.md`
   prescribes (minor bump, both versions parsed, `tests/host/desktop` protocol
   tests extended for every new opcode/direction/length).
2. **Window manager.** Resize borders (3 px frame plus a 6–8 px invisible
   grab margin) and the bottom-right corner; pointer feedback through the
   existing cursor shapes if any, otherwise none; live resize at the pointer
   rate with damage limited to the frame and the new/old surface union; the
   zoom title button toggles between the user size and the work area (keep
   windowshade on double-click); keyboard Alt+F8-style resize is out of scope.
   Internal windows (About, Run, Shut Down) stay fixed.
3. **Toolkit and apps.** `du_ui` gets a layout pass on size change (anchors or
   a simple flex: list views and text areas grow, toolbars and status bars keep
   their height); Files, CiukNote, Task Manager, Control Panel and the
   accessories declare resizable where it makes sense (Calculator fixed) and
   relayout; the DOS window stays fixed at 640x480 (the VM frame size).
4. **Gate behaviour.** The F2 gate scenarios and the three app scripts
   (`tests/host/desktop/shell_menu.py`, `tests/suites/f2-desktop.json`) must
   keep passing in the host replay; set only invalidated digests to
   `PIN-FROM-FIRST-RUN`.

## Acceptance by the lead

`make CAP= sdk`, `make CAP= desktop`, `tests/host/desktop` (48+ tests, the
replays included), `tests/host/test_desktop_gate.py` and `test_normal_boot.py`
PASS; a host render (compositor capture) of Files at two sizes; QEMU
`f2-desktop` cases unchanged; the lead checks resizing by hand on QEMU.
