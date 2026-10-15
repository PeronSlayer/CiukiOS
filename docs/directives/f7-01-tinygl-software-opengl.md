# f7-01 — A working OpenGL for native programs: TinyGL port with real frame-rate numbers (owner request 2026-10-15)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f7-01` from `main`. Source on disk: `build/downloads/tinygl/tinygl-main/` (TinyGL, C-Chads fork of Fabrice Bellard's library; zlib-style licence requiring an acknowledgment in the product and its documentation; ~OpenGL 1.1 subset, software rasteriser, no floating-point dependency on an FPU beyond what the i686 build provides).

## Request

The owner's 3D test (`gfxdemo`: cube and tiles) has disappointed on every image; he asks for an OpenGL that works and gives concrete frame-rate figures. Hardware 3D on the Savage belongs to step F7 later; this directive delivers the software OpenGL layer now.

## Task

1. Port TinyGL as `sdk/libgl` (static library for native programs, built by `scripts/build_sdk.py`/`sdk/*` like FreeType in f2-63): `gl.h`, `glu`-subset as needed, a `CiukiOS` zbuffer backend that renders into a client surface (XRGB8888) presented through libdeskui; no OS-specific code in the library tree, acknowledgment and licence under `sdk/libgl/README.md` and `licenses/`.
2. Port `gfxdemo` to it: the cube (textured and lit) and the tiles scene through OpenGL calls, with an on-screen and logged frame counter (`[gfxdemo] fps=… frame_us=… scene=…`, one record per second) and a fixed-time benchmark mode (`--benchmark 10` prints the average and p95 over ten seconds and exits) so that numbers are comparable between images and machines.
3. Host test: render the two scenes offscreen on the host build and compare against golden digests; measure the host frame time as a sanity bound. Target on the T23 (1 GHz, software path): cube 640×480 above 60 fps, tiles above 40 fps; report what the host achieves.
4. `docs/design/opengl-software.md`: the subset provided, the backend, the numbers, and the path to hardware acceleration in F7.

## Acceptance

SDK build, host tests and the desktop suite green; QEMU `f4-demo` (or the existing gfxdemo case) green with the fps record; the owner reads the fps on the T23. Files: `sdk/libgl/*`, `apps/gfxdemo/*`, `scripts/build_sdk.py`, `tests/host/*`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f7-01-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
