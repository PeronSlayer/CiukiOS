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

## Lead decision 2026-10-12 (addendum after the first Codex report)

Codex reported that live resizing is blocked: `src/kernel/proc/surface.c` (`surface_attachment`, `surface_map`) gives every received surface read-only rights, as the F2 window contract requires ("transferred surface descriptions MUST have read-only mapping rights"), so the desktop cannot hand a client a writable replacement surface.

Decision: **client allocation**, no kernel or ABI change.

1. On a CONFIGURE with a new size, the client (toolkit path, `du_resize`) allocates a new surface of the new size and the same pixel format, lays out and repaints its tree into it, and sends that surface as the attachment of the acknowledgement the protocol already defines for CONFIGURE (the first PRESENT/DAMAGE after it, carrying the configure serial).
2. The desktop keeps compositing the old surface until that message arrives; on arrival it maps the attachment read-only, swaps it under the window atomically, drops its reference to the old surface and repaints the window plus the exposed desktop area.
3. The client never draws into the old surface after sending the swap message and releases it once the desktop's next message for that window confirms the new size (no new ABI: the existing CONFIGURE/PRESENT round trip).
4. Memory bound: at most one pending replacement per window; a CONFIGURE arriving while a replacement is pending supersedes it (the pending surface is released unsent).
5. Resize borders and corner, zoom to the work area, minimum sizes and the per-app relayout stay as specified above; the desktop never allocates on the client's behalf.

Acceptance (host): the replay harness shows a Files window resized twice with the correct frame content after each swap and the surface counts back to baseline afterwards; `deskui_test` covers a CONFIGURE superseding a pending replacement; `docs/desktop.md` records this reading of the ABI. Same constraints as above (no QEMU, no commits, no diary).
