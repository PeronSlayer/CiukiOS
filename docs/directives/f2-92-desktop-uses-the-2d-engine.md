# f2-92 — The desktop must actually draw with the 2D engine (today nothing uses it)

- **Phase/model:** F2/F1, Claude agent (Opus). **Worktree:** `wt/f2-92` from `main`.
- Evidence: T23 trial of image 5a10eef4… (2026-10-22 ~13:05–13:12), serial copy `build/lead-scratch/t23-5a10eef4-serial.txt` (5,904 lines). Records: `[accel] savage bci_test result=ok … pixels=16`, `caps=3`, `[accel] summary accelerated=1` — and yet the owner sees no difference, because no code path draws with the engine: `src/kernel/drivers/savage2d.c` exposes start, self-test and sweeps only, and `apps/desktop` composes every frame in software straight into the LFB. The "2D acceleration: YES" line is therefore misleading.

## Required

1. **Immediately honest indicator:** until the engine is used, the splash/notice line reads `2D engine: OK (drawing: software)` / `2D engine: NO (<reason>)`; when the engine is used for drawing it reads `2D acceleration: YES`; the summary record gains `used=0|1`.
2. **Kernel accel service:** a bounded command interface (ioctl on the display device, or the desktop's existing display grant) with solid fill, screen-to-screen copy and clipped host-to-screen image write through the BCI, with completion waits bounded (1 ms slices, never under a lock the desktop holds), fallback to software on any error with the engine quarantined and the indicator flipped; programming from the X.org savage driver and the Savage4/SuperSavage manual, cited (never invented register programming).
3. **Desktop use:** window move/raise (screen-to-screen copies of unchanged regions), solid fills (desktop colour, window faces) and the scanout of unchanged window surfaces through the engine; software path kept for everything else; a per-frame record `[desktop] accel fills=… copies=… writes=… fallback=…` every 2 s.
4. Host tests on the register model: the three operations, clipping, the bounded wait, the fallback; the desktop golden frames unchanged with the engine model; measurement on the model of a window move at 1024×768×32 (software vs engine).

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f2-92-report.md`.

## Amendment 1 (lead, 2026-10-22 15:15, decision on the delivery)

Accepted; the directive's premise was half wrong: syscall 77 already offered
fill and copy and the compositor called them, but on the T23 the engine
became ready after the desktop's first frame, the desktop never loaded the
hardware cursor and window copies require it, waits gave up after ~1.5 ms
and a drawing failure quarantined silently. Reserved: `CIUKI_ACCEL_WRITE`,
`CIUKI_ACCEL_OFFSCREEN`, the `used=` field. The engine's own rate on the T23
and the host-to-screen write on the real chip are confirmed by the trial.
