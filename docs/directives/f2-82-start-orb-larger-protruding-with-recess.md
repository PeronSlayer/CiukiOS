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

## Amendment (lead, 2026-10-18 21:25) — the runner's desktop observer must follow the orb

On main `df31084a` (f2-81 merged) `tests/host/desktop/test_desktop.py::test_default_desktop_wallpaper` fails with `observed["failures"] == ['start_button']` and `tests/host/test_desktop_gate.py` fails with `approved Ciuki portrait region mismatch`: `scripts/test/run.py` `observe_desktop_screen` (lines ~879–915) hardcodes the old Start button geometry (`x0 = 4+(96-size-6-53)//2+shift`, `y0 = 4+(24-size)//2+shift`, portrait 18 px beside the 53 px "CiukiOS" text) and compares the portrait cells against the pinned converter. The same observer runs in the QEMU gates (`desktop_screen` cases), so the gates would fail too.

Required in addition: the observer derives the portrait region (position and size) from one shared source with the desktop (a small JSON written by the desktop build or by `apps/desktop/convert_portrait.py` describing the orb geometry and the portrait size/offset per bar height, read by `run.py`), keeps the pinned-converter cell comparison on the orb's portrait at its new size (28 px), tolerates the orb's gradient only outside the portrait cells, and the two failing tests plus `tests/host/test_normal_boot.py` pass on the merged tree before the report is written.
