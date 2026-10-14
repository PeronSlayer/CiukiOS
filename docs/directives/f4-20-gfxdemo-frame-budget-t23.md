# f4-20 — Graphics demo on the T23: cube at 25 fps, tiles at 40 fps — find and remove the frame budget

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-20` from `main`. Host-testable (`tests/host/desktop/test_compositor_performance.py --benchmark`, `compositor_benchmark.c`); the lead measures on the T23.

## Evidence (T23 Pentium III-M 1.13 GHz, 1024x768x32 LFB with write-combining MTRR, Savage quarantined so everything is software, image fb3147dc)

Owner: cube steady at 25 fps, tiles up to 40 fps, "not bad but we can do much better; keep tuning". Disk log while the demo ran: `[console] [display] frames=13 damage_px=788830 presents=15 lfb_bytes=3155320 present_us=30000 composite_us=35000 input_us=15000 input_lag_us=18000 interval_us=2000000 frame_p95_us=67000` → ≈2.3 ms present + ≈2.7 ms composite per frame: the compositor is not the limit at 40 ms/frame. `apps/gfxdemo/main.c:39-45` produces one frame per `DESK_GFX_PRESENTED` (`next_frame_ns`, `completed_ns`), so each frame costs a round trip app → compositor → present → ack plus the app's own render; `apps/desktop/client.c:381-386` computes the next slot. f2-59 already optimised the rasteriser (span rasteriser, integer enlargement).

## Task

1. **Instrument**: per frame in gfxdemo `render_us`, `submit_us`, `wait_us` (time from submit to PRESENTED), window size; in the compositor the slot policy actually applied (`next_frame_ns` relative to `now_ns`, refresh assumption) and per-frame `convert_us`/`copy_us` for the demo window. Publish them in the demo's FPS overlay and in the `[display]` record.
2. **Find the 40 ms**: candidates are the slot policy (a fixed 40 ms or 25 Hz cadence? a wait for the next 2 s `[display]` interval?), two copies of the window (app buffer → surface → LFB), a vsync wait, the software cube at the window's size (what size, how many pixels per frame, Gouraud/texture cost on a P3: estimate with the host benchmark scaled by clock), the `du_window` present path blocking on the compositor loop. Use the host benchmark and the records to attribute the time; do not guess from QEMU numbers.
3. **Fix** what is in our control: the demo renders the next frame while the previous presents (double buffering, one frame of latency), slot policy at the panel refresh (60 Hz) when the client is fast, no redundant copies. Target on the T23: cube and tiles ≥ 60 fps at the demo's default window size, or a measured breakdown of the remaining cost and the plan.
4. Keep the compositor goldens valid; re-record only with a justified visual change.

## Acceptance

Desktop host suites green (`tests/host/desktop`, 96 tests), benchmark numbers before/after in the report and in `docs/validation/`; the lead measures on the T23. Files: `apps/gfxdemo/*`, `apps/desktop/client.c`/`compositor.c` (frame slot and present path), `tests/host/desktop`. Not the kernel.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-20-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
