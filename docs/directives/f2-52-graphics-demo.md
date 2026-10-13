# f2-52 — Graphics demo: visible proof of the 2D accelerator and a software 3D scene

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-52` from `main`. Host-testable. Owner request 2026-10-13: a 3D demo to see whether hardware acceleration works. Scope note: 3D acceleration (Direct3D/OpenGL on the Savage engine) is step F7; what exists today is the 2D engine (fills, copies, pattern, cursor). The demo must show the 2D engine honestly and provide the 3D scene in software, with the accelerator used for the blits.

## Task

1. `/Programs/gfxdemo` ("Graphics Demo" under Programs > Demo or Accessories): three pages with a live frames-per-second and milliseconds-per-frame counter: (a) **2D benchmark**: screen-size fills, 256x256 copies, pattern fills, run with the accelerator and in software, side by side, with the measured MB/s and the accelerator state read from the kernel (`driver=savage ops=… quarantined=…`); (b) **3D cube/teapot** in software (fixed-point rasteriser, flat and Gouraud shading, z-buffer, 320x240 scaled to the window, the Ciuki portrait as a texture), presented through the accelerated blit when available; (c) **Bouncing sprites** (classic 1998 demo scene) to show frame pacing.
2. A "System info" strip: display mode, bpp, accelerator name and state, present and composite times from the desktop statistics.
3. Host tests: deterministic frames (fixed time step) with goldens for each page, the benchmark harness on fake surfaces.

## Acceptance

Desktop host suites green; `make CAP= apps` builds; payload inventory and menu tests updated; `docs/desktop.md` mentions it.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-52-report.md`.
