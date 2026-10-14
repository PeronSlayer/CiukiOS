# f0-20 — Whole-system stalls on the T23: USB workers idle for 9 s, desktop without frames for 2-6 s, wallpaper scaling 14.5 s, desktop start 7 s

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-20` from `main`. Host-testable; the lead runs the desktop/USB QEMU cases and the T23.

## Evidence (T23, image 66843edc, verbose boot, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/66843edc/disk-log/BOOT.LOG.clean`)

- `[usb] worker controller=usb-2474 tick=16042 scans=1 elapsed_ms=9164 scans_per_second=0.109`, `usb-2478 ... elapsed_ms=6230`, `usb-2482 ... elapsed_ms=1737` (lines 592-594): all three controller workers (`P_DEVICE` kernel tasks) made one scan in 6-9 s; the receiver's keyboard and mouse live on one of them.
- Desktop: `stage=entry us=0`, `settings_read us=7032000` (7 s), `wallpaper_decoded us=9364000`, `wallpaper_scaled us=23847000` (14.5 s to scale one wallpaper), `[desktop] stall stage=shell worker=wallpaper/settings no_frame_ms=2012 input_pending=1` and more stalls up to `no_frame_ms=6101` (lines 559-579; the f2-61 observer works). Files launch `click→first_frame 3297000 us`, `dma_ms=262`.
- Savage init this time took 2.6 s (`us=2601390`), no `[work] budget exceeded`.
- Candidates: the kernel is non-preemptible in ring 0 and these paths run long without yielding: the verbose console painting every record on the framebuffer after the desktop owns the screen (full-frame scroll repaint per line), f1-59's synchronous disk-log drain (`bootlog_sync`) called from the USB bind and blocking other workers, ELF load copy/zero loops (f2-60's 4 MiB arena is zeroed at spawn), the wallpaper worker's decode/scale syscalls, f2-61's trylock retry in the main loop, the registry save through a 4 MiB arena. Nothing in the log attributes the time.

## Task

1. **Attribution record**: every 2 s in verbose mode the kernel emits `[cpu] window_ms=2000 idle=<permille> top=<task:permille,...>` (five tasks by CPU delta, kernel threads named) and `[kernel] long_paths console_repaint_ms=<n> bootlog_drain_ms=<n> elf_load_ms=<n> max_single_ms=<n> site=<symbol>` (time spent in the known non-preemptible paths, maximum single stretch and its site). The desktop stall record of f2-61 triggers an immediate emission.
2. **Fix what the audit already shows**: no console framebuffer painting once the desktop owns the display (records go to serial and disk only); `bootlog_sync` must never block another controller's worker or input (drain on the storage task, bounded; the bind waits with a deadline); the wallpaper scaler must not run inside a long syscall; ELF BSS zeroing yields per 64 KiB.
3. Measure before/after on the host scheduler replay (the f3-17 harness with P_DEVICE workers and a P_NORMAL desktop) and keep the attribution cheap (no records when nothing exceeds 20 ms).

## Acceptance

Kernel host suite, USB and desktop suites green; the lead runs `desktop-shell`, `f1-usb` and `vm-window` on QEMU, then the T23: USB worker scans at 4/s throughout, no desktop stall records in normal use, wallpaper scaled within 1 s, desktop `settings_read` within 1 s of entry; otherwise the attribution record names the hog. Files: `src/kernel/core/*` (task accounting, console, bootlog), `src/kernel/usb/usb_core.c` worker, `apps/desktop` wallpaper path if needed. Coordinate with f1-62 (bind budget in `usb_storage.c`) and f1-63 (mount.c): do not edit those files.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f0-20-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
