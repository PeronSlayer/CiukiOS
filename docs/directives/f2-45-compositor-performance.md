# f2-45 — Compositor: 130 ms per full frame at 1024x768

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-45` from `main` (f9f112a0). Host-testable. Files: `apps/desktop/compositor.c`, `apps/libdeskui/draw.c`, the paint helpers of `shell.c` (icons/text only; not the tray/taskbar layout, which f6-02 edits), `tests/host/desktop/`. Do not touch `desktop.c` or `wallpaper.c` (f2-44).

## Evidence (T23, image 3299a24f, Pentium III-M 1.13 GHz, 1024x768x32)

```
[console] [display] frames=2 damage_px=1572864 … present_us=118000 composite_us=265000
[console] [display] frames=18 damage_px=5223680 … composite_us=562000
[console] [display] frames=7 damage_px=620800 … composite_us=590000
```

Compositing runs at 6–9 Mpx/s (≈ 120–180 cycles per pixel): per-pixel loops with `d->pixels[(uint32_t)y*d->width+x]` indexing (`fill`, `window`, `portrait`, `scene`), per-pixel clipping, icon and text blits per pixel, then `present_box` copies the rows again into `accel_front`. The owner sees the redraw lag on every window move and menu. The presentation side is f1-40.

## Task

1. A host benchmark in `tests/host/desktop/` that composites a representative scene (wallpaper, four windows, Start menu open, taskbar, cursor) at 1024x768 and reports cycles per pixel with `rdtsc`; run it before and after and put both numbers in the report.
2. Row-based painting: row pointers, `memcpy` for opaque spans (client surfaces, wallpaper rows from the pre-rendered background, title bars), blending only where alpha exists (shadows, cursor, translucent icons), clip computed once per box, icon/text blits by row. Damage granularity: check the per-second clock and the hover paths damage only their boxes, not the whole taskbar.
3. Check `apps/` compile flags (`-O2`, `-march=i686`? `-fno-strict-aliasing` costs?); report what the build uses and change only with evidence from the benchmark.
4. Pixel-exact output: the existing desktop golden tests must pass unchanged; add goldens for the benchmark scene.

## Acceptance

Benchmark ≥ 4x faster on the host (the owner's target on the T23: a full frame in ≤ 25 ms, measured with the existing `composite_us`); desktop host suite green.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-45-report.md`.
