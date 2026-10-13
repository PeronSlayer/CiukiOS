# f2-50 — Desktop rendering architecture: from "it stutters" to a smooth 1024x768 on a Pentium III

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f2-50` from `main`. Host-testable (benchmarks and goldens); the lead measures on the T23. Owner directive 2026-10-13: the system still stutters ("scatta"); study a better rendering system.

## Evidence (T23 1024x768x32, image e86305de)

Full-frame costs after f1-40/f2-45/f2-48: presentation 23 ms (134 MB/s), composition 37–58 ms. Small updates: `px/frame=9004 present/frame=354 us composite/frame=427 us`. Window drags recomposite 22 % of the frame. The owner perceives stutter on window moves, menus and app opening. The architecture today: every client paints into its surface; the desktop composites the whole damaged area into one RAM frame (`d->pixels`), copies it into `accel_front` when an accelerator exists, and the kernel presenter converts rows into the LFB; the shell paints icons, text and chrome per damage rectangle; frame pacing by `desk_frame_due`.

## Task

1. **Measure stutter, not throughput**: a host harness replaying a recorded interaction (window drag, Start menu open/close, app launch with first frame) that reports per-frame latency from input to present and the distribution (p50/p95/max) with the painter breakdown of f2-48; the same metric exposed in the `[console] [display]` record (`frame_p95_us`, `frame_max_us`) for the T23.
2. **Design** (`docs/design/desktop-rendering-architecture.md`, researched against Windows 98 GDI/DDraw practice, BeOS app_server and X11 compositing of the period): per-window backing stores so a drag is a blit of cached pixels, not a repaint; chrome (captions, borders, shadows) cached per window state; opaque-region tracking so hidden pixels are never painted; damage coalescing per frame with a 60 Hz pacing budget and a drop policy when a frame exceeds it; the kernel presenter fed directly from the composite buffer (no `accel_front` copy) and accelerated blits/fills used for window moves when the engine is ready; dirty-row lists instead of rectangles for text updates. State what stays and what changes, with the expected cost per operation on a 1 GHz Pentium III.
3. **Implement the design** behind the existing protocol (clients unchanged), pixel-exact with the current goldens except where the design document declares an intentional change (recorded with new goldens).
4. Targets on host (scaled) and T23: window drag of a 400x300 window ≤ 8 ms per frame, Start menu open ≤ 15 ms, full-frame redraw ≤ 30 ms, p95 input-to-present ≤ 33 ms.

## Acceptance

Desktop host suites green; the new harness report in the directive report; the lead measures on the T23 with the new record.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-50-report.md`.
