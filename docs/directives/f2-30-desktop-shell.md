# Directive f2-30: desktop shell — window frames, taskbar, Ciuki menu, program launch, About

- **Step:** F2+ (desktop track towards 0.8 parity). **Contracts:**
  `f2-acceptance.md` (ring-3 desktop, crash isolation, identity through
  `apps/desktop/convert_portrait.py`, tagline `A modern Retro OS`),
  `docs/desktop.md` (protocol v1, focus/input/damage/lifecycle, lead-action
  boundaries), `posix-subset.md` (spawn, wait, signals, clock),
  `AGENTS.md` identity rules (approved portrait only, Tango icons, English
  UI copy).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-desktop-shell`, from `main` at or after `266bbf6`.
  Files: `apps/desktop/*` (compositor, input, client handling; protocol
  changes only as a versioned extension), `apps/demo/*` only for test
  clients, `sdk/` only to install `protocol.h` as `ciuki/desktop.h` (the
  lead note of f2-08, allowed here), `tests/host/desktop/*`,
  `tests/suites/f2-desktop.json` additions, `docs/desktop.md`, icons from
  `assets/icons/` per its README.

## Scope

1. **Window management.** Title bar with the client's title, close
   button (close request to the client; SIGTERM after a bounded timeout),
   focus by click, drag by title, z-order, minimise to the taskbar;
   keyboard focus follows the active window; a window losing focus
   receives releases for held keys and buttons.
2. **Taskbar.** Bottom bar with the Ciuki button (icon derived
   deterministically from the approved portrait), one button per open
   window, a clock from the POSIX clock. The Ciuki menu lists launchable
   programs (a manifest `\SYSTEM\DESKTOP\MENU.CFG` with name, path and
   icon, or a `/bin` scan: decide and record), an About entry and a
   Restart entry through the supervisor's existing reboot path (power-off
   only if the kernel exposes one; otherwise absent).
3. **Program launch** from the menu with `posix_spawn` under the
   supervisor's process-group rules; the program's windows appear when it
   connects and disappear on exit; the crash-isolation gates stay as they
   are.
4. **About window:** portrait, name, tagline, kernel build identity (use
   what the kernel already exposes to processes; otherwise the desktop's
   own build manifest) and the Tango/Ciuki notice.
5. **Determinism.** Tango icons where an icon is needed, the existing
   font; the gate-mode `desktop_screen` digests of the existing cases
   must stay stable (gate mode may keep the plain desktop; record).
6. **Tests.** Host tests for hit-testing, z-order, focus, taskbar layout
   and menu parsing; a QEMU `f2-desktop` case `desktop-shell` with a
   scripted input sequence (open the menu, launch the demo, drag, close)
   and `desktop_screen` digests.

## Acceptance by the lead

Host tests; existing `f2-desktop` cases unchanged; `desktop-shell` PASS
on `qemu-t23` and `qemu-e500`; a runner screenshot for the owner.
