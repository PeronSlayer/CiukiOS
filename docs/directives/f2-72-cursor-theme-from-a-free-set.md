# f2-72 — Cursor set from a free theme (owner request 2026-10-15)

- **Model/effort:** `gpt-6-luna`, `medium`. **Worktree:** `../CiukiOS-wt-f2-72` from `main`. Sources already on disk: `build/downloads/dmz-cursor-theme/dmz-cursor-theme-0.4.5/` (DMZ-White and DMZ-Black, PNGs at 24/32/48 px with `.in` hot-spot files; theme licence CC BY-SA 3.0, Novell) and `build/downloads/hackneyed/hackneyed-x11-cursors-master/` (MIT, Richard Ferreira; SVG sources, classic black-with-white-outline style).

## Request

The owner does not like the hand-drawn set of f2-67 and asks for a good-looking, coherent, freely licensed set. The lead presents the two candidates; the owner chooses.

## Task

1. Produce, for both themes, a contact sheet PNG (the thirteen desktop shapes plus the six paint shapes mapped to the theme's nearest shapes, 32 px, with hot spots marked) under `build/host/f2-72/`, and a short licence note for each (attribution text, share-alike consequence for DMZ, MIT for Hackneyed).
2. Build the conversion tool `scripts/cursors/import_theme.py` (PNG or rendered SVG at 32 px → `assets/cursors/cursors.h` masks and hot spots, deterministic, with the theme's licence file copied under `assets/cursors/<theme>/`), so that switching theme is one command.
3. Generate `assets/cursors/cursors.h` from the theme the lead names in the report's follow-up (default DMZ-White until the owner decides); keep the animated busy cursor as a frame sequence if the theme provides it.

## Acceptance

Desktop suite green with re-recorded cursor goldens; contact sheets for the owner.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f2-72-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
