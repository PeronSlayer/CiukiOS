# f2-48 — Compositor and desktop I/O, second round after the T23 measurements

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-48` from `main`. Host-testable. Files: `apps/desktop/compositor.c`, `apps/libdeskui/draw.c`, `apps/desktop/wallpaper_io.c`, `apps/desktop/render_wallpaper.c`, their tests.

## Evidence (T23, image 24c98799 with f2-45 and f1-40 merged, 1024x768x32)

```
[console] [display] frames=2 damage_px=786648 presents=2 lfb_bytes=3146592 present_us=47000 composite_us=116000
[console] [display] frames=27 damage_px=794234 presents=29 lfb_bytes=3176936 present_us=32000 composite_us=51000
[console] [desktop] stage=first_input_handled us=253000
[console] [desktop] stage=wallpaper_file_read us=3579000 cache_hit=1 … stage=wallpaper_applied us=3761000
```

Presentation of a full frame now costs 23.5 ms (134 MB/s, was 59 ms); composition 58 ms per full frame (was 130 ms), still above the 25 ms target. The wallpaper worker reads the 3 MiB cache with 4 KiB requests (3.4 s) although f1-39 raised native reads to 64 KiB.

## Task

1. Wallpaper worker and cache writer: 64 KiB requests (`read`/`pread` now accept them), measured in the `[desktop] stage=` records; target ≤ 0.5 s for the 3 MiB cache on the T23.
2. Compositor: profile the remaining 58 ms with the benchmark of f2-45 on the host, extended with a cycle breakdown per painter (background rows, windows, captions, shadows, icons, text, cursor, `present_box` copy to `accel_front`); eliminate the largest items: full-frame background rows should be one memcpy per row from the pre-rendered frame, opaque client rows one memcpy, shadows only on their strips, no per-pixel work on opaque areas, the `accel_front` copy only when an accelerator is active. Pixel-exact against the f2-45 goldens.
3. Damage: a window drag must repaint only the union of the old and new frames; the Start menu open/close only its box; verify in the harness that a 400x300 window drag composites ≤ 0.25 full frames.

## Acceptance

Benchmark ≥ 2x faster than the f2-45 state on the host with the breakdown in the report; desktop host suite green. Target on the T23 (lead): `composite_us` ≤ 25 ms per full frame.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-48-report.md`.
