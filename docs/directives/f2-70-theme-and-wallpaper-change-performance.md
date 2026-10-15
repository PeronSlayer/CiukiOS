# f2-70 — Changing theme or wallpaper collapses the frame rate and degrades the desktop (T23, image a4f67ac1)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-70` from `main`.

## Evidence

Owner: the desktop runs acceptably (software path, the Savage engine is still quarantined on this image: `bci_test result=failed error=-5` in the serial fragments of boot 1, so f1-65 did not fix the fill on hardware; see f1-67), but changing the theme or the wallpaper makes the frame rate collapse and the desktop degrade from then on. Earlier T23 logs: wallpaper scaling 14.5 s (image 66843edc), `[desktop] stall stage=shell worker=wallpaper/settings no_frame_ms=8260`; f0-20 reduced the sampler from 10.5 to 4.1 ms per row block. `[console] [display]` records show `lfb_bytes` up to 27 MB per 2 s window after large damage.

## Task

1. Reproduce on host with the replay harness: theme change and wallpaper change on a 1024×768 desktop with three windows open; measure frames, presents, lfb bytes and the worker's time before and after.
2. Wallpaper: decode and scale once per mode into a cached surface (on disk under `SYSTEM/cache` and in memory), never on the interaction path; present the change as one full frame; the settings worker must not hold the desktop (no `no_frame_ms` above 100).
3. Theme change: recolour cached chrome assets once, invalidate goldens properly, repaint once; no per-frame re-rasterisation of text (FreeType glyph cache must survive a theme change).
4. Find why the degradation persists after the change (a leaked damage region, a cursor path left in software, a worker still running, the scanout flag) and fix it; add a replay assertion that frame cost returns to the baseline within one second.

## Acceptance

Desktop suite and gate green with the new replay; PNGs under `build/host/f2-70/`; the owner judges on the T23.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f2-70-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
