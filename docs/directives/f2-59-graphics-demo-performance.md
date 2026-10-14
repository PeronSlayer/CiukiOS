# f2-59 — Graphics Demo at 15 frames per second on the T23: the software path must run at full rate

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-59` from `main`. Host-testable; the lead measures on the T23.

## Evidence (T23 Pentium III-M 1.13 GHz, SuperSavage IX/C quarantined, 1024x768x32, image 293c05af, 2026-10-14)

The Graphics Demo's rotating cube (320x240 software rasteriser, nearest scaling to the window) shows about 15 frames per second; the owner judges the whole system slow. Desktop statistics of the same boot: full-frame present 25–32 ms, composite up to 89 ms max, `overruns` present in the 60 Hz pacing. A 320x240 textured, z-buffered cube is about 77k pixels: on this CPU a tight fixed-point rasteriser should run well above 60 fps; the rest of the cost is the scale, the client surface damage, the composition and the LFB upload.

## Task

1. Measure on the host harness with the cycle budget of a 1 GHz Pentium III (benchmarks already exist in `tests/host/desktop/`): per frame, the rasteriser, the 2x/3x scale into the client surface, the protocol damage/present round trip, the compositor's client copy and the LFB upload. Report the breakdown before and after.
2. Make the demo client cheap: render at the window's native size only when ≤ 640x480, otherwise render 320x240 and let the compositor scale with an integer-ratio fast path (row duplication with `rep movsd`); damage only the changed rectangle; pace to the display at 60 Hz without sleeping past vsync slots; no per-pixel function calls in the scale and blit loops; frame-rate counter from presented frames.
3. The desktop side: when a client surface is opaque and unscaled, present it with one row copy per line straight to the output (no chrome repaint when only the client changed); verify the `overruns` disappear in the display record on the host replay harness of f2-50.
4. Targets: cube ≥ 60 fps at 2x scale on the T23 (software), sprites page ≥ 60 fps, 2D benchmark unchanged.

## Acceptance

Desktop host suites green with the breakdown in the report; the lead measures on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-59-report.md`.
