# f2-62 — Blue is the default CiukiOS theme, refined (owner request 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-62` from `main`. Host-testable; the lead reviews the goldens and the T23.

## Request

The owner wants the existing **Blue** theme as the default theme of CiukiOS and "a bit prettier". Identity rules of `AGENTS.md` apply: the approved Ciuki portrait and Tango icons stay untouched; UI copy stays English.

## Task

1. Default theme = Blue for a fresh install (`desktop.cfg`/registry default, Control Panel shows it selected); an existing user choice is kept.
2. Refine Blue within the theme system (`apps/desktop/theme.c`, compositor): title bar gradient like Windows 98 (active/inactive), consistent 3D bevels on buttons and frames, taskbar/Start button and tray contrast, selection and menu highlight colours, dialog background, icon label shadow on the wallpaper; keep every control readable at 1024x768x32 and in 16 bpp. No new assets.
3. Re-record the compositor and desktop goldens that legitimately change (`tests/host/desktop/test_compositor_performance.py --benchmark --frames 2`, demo goldens), and keep the incremental/full redraw parity tests green. Put before/after PNGs of the desktop with a window, a menu and a dialog under `build/host/` and list their paths in the report.

## Acceptance

`tests/host/desktop` green with re-recorded goldens; the lead looks at the PNGs and the owner judges on the T23. Files: `apps/desktop/theme.c`, compositor theme tables, defaults, tests/goldens. Not `desktop.c`/wallpaper logic (f2-61).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f2-62-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
