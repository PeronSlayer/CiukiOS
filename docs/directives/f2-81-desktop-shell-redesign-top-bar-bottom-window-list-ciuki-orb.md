# f2-81 — desktop shell redesign: top bar with Start orb, tray, notifications and clock; bottom bar with open windows only; no window title in the bar; Classic 98 theme removed

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-81` from main (after `7ef68c1d`).
- **Authority:** `apps/desktop/` (`compositor.c` bar geometry, `shell.c`, `theme.c/.h`, `desktop.h` `DESK_BAR`), `apps/control` theme list, `assets/brand/ciuki-logo.png` (approved portrait, deterministic conversion only), host tests `tests/host/desktop/` (goldens, gate), `docs/design/desktop*.md`.

## Owner decision (2026-10-18, verbatim intent)

- The taskbar's tray, notifications and clock/date move to the **top bar**, with the Start button; the **bottom bar** shows only the open windows.
- The active window's title is shown only in the window's own title bar, never in the top bar.
- The Start button loses the "CiukiOS" text and becomes an orb in the style of Windows Vista's, but with the **CiukiOS logo** (the approved Ciuki portrait) inside.
- The "Classic 98" theme is removed: it looks too much like Windows; CiukiOS must have its own identity. "Ciuki Blue" stays the default; "Platinum" stays.

## Required

1. Geometry: top bar (`DESK_BAR`) = Start orb at the left, then free space (no title), tray icons, notification anchor and clock/date at the right; bottom bar of the same height = task buttons only (open windows, active highlighted, minimized state), centred or left-aligned per the theme. Start menu drops from the orb. Notifications anchor under the top bar. All hit-testing, damage and the stall observer follow the new boxes; the DOS window and full-screen code use the bar heights through the existing constants/functions, not literals.
2. Start orb: circular button of bar height minus margins, soft radial gradient in the theme accent, the Ciuki portrait rendered from `assets/brand/ciuki-logo.png` by a deterministic downscale (the build's existing icon pipeline; no redraw, no regeneration), hover/pressed states; keyboard activation unchanged.
3. Themes: delete "Classic 98" and the classic caption-button style (`DESK_CONTROLS_CLASSIC`) everywhere (theme table, Control Panel list, goldens); a saved preference naming the removed theme falls back to "Ciuki Blue" and is logged once.
4. Window title: remove the bar title rendering; the window's title bar remains the only place.
5. Host coverage: render tests and goldens updated for the new layout (document the intentional changes), desktop 10x/10x tests, gate 11/11, `make CAP= desktop`, `make CAP= apps`; capture PNGs of the new desktop at 1024×768 and 800×600 in the report (host renderer).
6. Report `build/codex/f2-81-report.md` with the PNG paths.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Identity rules of AGENTS.md: the exact approved portrait, deterministic conversion only, tagline unchanged, UI copy in English. No Microsoft/Apple artwork.
