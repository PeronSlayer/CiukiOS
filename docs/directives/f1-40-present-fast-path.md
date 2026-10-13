# f1-40 — Framebuffer presentation: 53 MB/s into a write-combined LFB

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-40` from `main` (f9f112a0). Host-testable. Touch only `src/kernel/drivers/fbdev.c`, its header and its host tests (f1-38 edits `savage2d.c` and the `accel_work` block in parallel: leave that block alone).

## Evidence (T23, 1024x768x32, MTRR WC over the whole LFB: `[display] mtrr wc base=e8000000 size=4194304 result=ok slot=1`)

```
[console] [display] frames=2 damage_px=1572864 presents=2 lfb_bytes=6291456 present_us=118000 composite_us=265000
[console] [display] frames=18 damage_px=5223680 … lfb_bytes=20894720 present_us=723000
```

A full 3 MiB frame takes 59 ms (≈ 53 MB/s). `fbdev_present_to` converts **per pixel** (`memcpy` into a local, `pack()` with six shifts reading the layout from `d` each time, a volatile 32-bit store), and `fbdev_fill_to`/`fbdev_copy_to` do the same. Reads from the LFB (uncached) cost far more than writes: any software copy whose source is the LFB must go.

## Task

1. Row fast paths: when the device is xRGB8888 (red 16/8, green 8/8, blue 0/8) copy whole clipped rows with `rep movsd` (dword aligned source and destination, pitch ≠ width*4 handled); for RGB565 convert 64 pixels into a stack buffer and `rep movsd` the row; 24 bpp keeps the generic loop. Hoist the layout out of the loops; no `volatile` per pixel (a compiler barrier per row is enough; the WC buffers drain on the next serialising event — document it with the Intel SDM reference). `fbdev_fill_to`: `rep stosd` rows. `fbdev_copy_to` must never read the LFB: check its users (`screen_copy`/`scroll_copy` in `apps/desktop/compositor.c` through the accel request path) and make the software fallback re-present from the RAM surface instead.
2. Keep the `present full` timing record and the `[console] [display]` counters; add `lfb_mb_s` (bytes/us) to the periodic record so the T23 capture shows the achieved bandwidth.
3. Host tests: fake devices of each layout; pixel-exact equality between the generic path and the fast path over random rectangles (odd clip offsets, partial rows, pitch padding, 16 bpp dithering if any was there before — none is expected).

## Acceptance

Host suites green; i686 kernel compiles. Target on the T23 (lead): full 1024x768x32 frame ≤ 20 ms (≥ 150 MB/s).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-40-report.md`.
