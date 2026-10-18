# f2-82 — Start orb: larger, protruding below the top bar like Windows Vista's, sitting in a recessed well

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-82` from main (after `df31084a`).
- **Authority:** `apps/desktop/compositor.c` (`desk_start_button`, bar painting, damage, shell chrome above windows), `shell.c` (hit-testing, menu anchor), `theme.c` (orb colours per theme), `desktop.h`, host tests/goldens in `tests/host/desktop/`, `docs/design/desktop-shell.md`.

## Owner decision (2026-10-18, after seeing the f2-81 captures)

"Make it bigger, protruding like Windows Vista's, with that recess." The f2-81 orb is a flat 24 px disc inside the 32 px bar.

## Required

1. **Geometry.** Orb diameter 44 px on the 32 px top bar: centred vertically on the bar's bottom edge region so that it protrudes about 10 px below the bar over the work area (like Vista's orb over the taskbar edge), left margin 6 px. The protruding part is shell chrome: painted above every window (including maximized ones), included in damage and in the compositor's exclusion like the notification overlay; the work area and maximized-window geometry stay at the bar's bottom edge (windows may be overlapped by the orb's protrusion only).
2. **Recess ("incavo").** Behind the orb the bar shows a circular well slightly larger than the orb (diameter 48 px): a concave dish with an inner shadow at the top, a lighter rim at the bottom, drawn in the bar's colours per theme; the dish is clipped to the bar (the protruding part of the orb has no dish behind it, only the desktop).
3. **Sphere look.** Radial gradient in the theme accent (darker rim, lighter centre), a glossy highlight on the upper half (soft ellipse, white at low alpha), a thin dark outline, a 1 px soft drop shadow on the protruding part; the Ciuki portrait centred inside at 28 px from the pinned deterministic conversion of `assets/brand/ciuki-logo.png` (no redraw, no regeneration). Hover: brighter highlight; pressed/menu open: inverted gradient. All integer arithmetic as the existing renderer; no new fonts or assets beyond the pinned portrait sizes already available (add a 28 px conversion to the deterministic pipeline if missing).
4. **Behaviour.** Hit-testing on the full circle including the protrusion; keyboard activation unchanged; the Start menu anchors under the bar at the orb's left edge; the stall observer, screen observer and scripted pointer paths use the new box; the DOS window and full-screen paths are unaffected.
5. **Host coverage.** Render goldens for both themes at 1024×768 and 800×600 (orb idle/hover/pressed, a maximized window under the protrusion, notification overlap), hit-test tests, `make CAP= desktop`, desktop tests, gate 11/11; PNG captures of the orb in `build/codex/f2-82-frames/` (whole desktop and a 2× crop of the orb corner).
6. Report `build/codex/f2-82-report.md` with the PNG paths.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Identity rules of AGENTS.md (exact approved portrait, deterministic conversion only). No Microsoft artwork: the shape and placement are the reference, the drawing is ours.
