# f2-83 — the top bar looks pixelated and the bottom bar looks like a flat white strip: visual quality and depth of the two bars

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-83` from main (after `06c3d1fa`).
- **Authority:** `apps/desktop/compositor.c` (bar painting, task buttons, orb), `theme.c/.h` (paint helpers, gradients, text), `apps/desktop/convert_portrait.py` (orb mark), host tests/goldens in `tests/host/desktop/`, `docs/design/desktop-shell.md`.

## Owner directive (2026-10-18, after the 9af46936 trial on the T23 at 1024×768×32)

"The top bar looks too pixelated; the bottom bar, with some design skill, make it prettier and more readable: it looks like a white strip without depth."

## Required

1. **Design research first** (cite in `docs/design/desktop-shell.md`): principles for bars/docks with depth (vertical gradients with a 1 px light top edge and a 1 px dark bottom edge, inner shadows, 8–12 % contrast steps, text with a 1 px soft shadow on dark bars), anti-aliasing of circles and rounded rectangles with coverage-based integer alpha, dithering for gradients in 16 bpp (ordered 4×4) to avoid banding, hinted anti-aliased text for 12–13 px UI fonts (FreeType already used). References: Microsoft's Windows 7 taskbar/"Aero basic" guidance and Apple's HIG on toolbars as inspiration only (no artwork), GNOME HIG on contrast.
2. **Top bar.** Smooth two-stop gradient (per theme) with dithering in 16 bpp; the orb's circle, well and highlight anti-aliased (coverage alpha at the edge, no stair-steps); the portrait scaled with the deterministic area scaler and composited with an anti-aliased circular mask; tray icons at their native 16 px without scaling artefacts; clock/date text hinted and anti-aliased with a soft shadow; nothing drawn with nearest-neighbour scaling.
3. **Bottom bar.** Depth: gradient body, 1 px highlight at the top, 1 px shadow line at the bottom, subtle inner shadow under the top edge; task buttons as rounded rectangles (3 px radius, anti-aliased) with a soft bevel, clear hover, pressed/active (darker body with a 2 px accent line) and minimized (lighter, italic-free label) states; labels truncated with an ellipsis, readable contrast ≥ 4.5:1 on both themes; the bar must not read as a flat white strip in Ciuki Blue or Platinum.
4. **Consistency.** Both bars share the same visual language (same edge treatment, same gradient family) and the Start menu and notification popups match; nothing changes in geometry (bar heights, orb 38 px, boxes) so the runner observer and hit-tests stay valid.
5. **Host coverage:** goldens re-recorded deliberately, 32 bpp and 16 bpp renders checked for banding (a test asserting no two adjacent gradient bands differ by more than one step), contrast test for labels, `make CAP= desktop`, desktop tests, gate 11/11; PNG captures at 1024×768 and 800×600 for both themes plus 2× crops of the orb and of three task buttons in `build/codex/f2-83-frames/`.
6. Report `build/codex/f2-83-report.md` with the PNG paths.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Identity rules of AGENTS.md; integer-only rendering as the compositor audit requires; no Microsoft or Apple artwork.
