# f7-14: native OpenGL on the hardware 3D path (host evidence and checks)

Directive: [f7-14](../directives/f7-14-native-gl-hardware.md). Decisions:
[opengl-software.md](../design/opengl-software.md) (f7-14 amendment),
[graphics-demo.md](../design/graphics-demo.md) (f7-14 amendment). This record
holds host evidence only; no image, QEMU or hardware run is part of it. The
QEMU and T23 checks below are for the lead.

## Host evidence (2026-10-26)

`tests/host/test_libgl.py::test_native_hardware_replay` links the production
native `libgl` (shared opengl32 engine, journal, admission, native provider),
the Graphics Demo scenes (`apps/gfxdemo/scene.c`), the kernel `render3d`
service, its validator, `render3d_transport.c` and the Savage BCI emitter
against the independent fake Savage of `tests/host/savage3d_backend_test.c`
(full-size boot qualification included), under ASan/UBSan with leak
detection. The copied `CIUKI_SYS_W32` record and the lease are the only
models (`tests/host/gl_native_kernel.c`).

| Case | Result |
| --- | --- |
| No kernel transport | software, 0 exchanges, 0 device writes |
| Unqualified device (QEMU model), 640x480 and 1024x768 | 6/6 software frames, 0 GPU draws, 0 device writes; `--backend hardware` refused with `-ENODEV` |
| Qualified device, cube flat, 640x480 | 2/2 GPU frames, 26 GPU draws, 0 CPU draws, coverage identical, 139 of 614400 pixels beyond two levels (sampling envelope, max 72 at texture edges) |
| Qualified device, cube Gouraud, 640x480 | 2/2 GPU frames, 0 CPU draws, coverage identical, 81 pixels beyond two levels (max 4) |
| Qualified device, tiles, 640x480 | 2/2 GPU frames, 66 GPU draws, bit-identical to software |
| Same three scenes at 1024x768 (contract T2 size) | all GPU frames, coverage identical (cube flat 379, Gouraud 220 pixels beyond two levels; tiles identical); 3 completed GPU-copy presents under a 1024x768 lease |
| Software policy on a qualified device | provider stays closed, exact scalar frame |
| Full-screen lease | GPU copy presented; CPU frame (`W32_DISPLAY_FRAME`) under the software policy equals the software golden exactly; revoked lease `-EACCES` with nothing presented; mismatched lease `-EINVAL` |
| Process death mid-frame | failed submission replayed in software with exact pixels, whole-context latch, hardware policy refused |
| Unsupported state (fog) | whole-context fallback before side effects, exact software frame |
| Texture heap exhausted | 12 temporary refusals, CPU frame, context kept on the provider, next untextured frame on the GPU |
| Steady state (second frame of each scene) | 0 target uploads, 2 target reads (colour, D16) per frame |
| Census (`CIUKI_GL_CENSUS`) | over-long count ignored; `GL_CENSUS` lines with admit verdicts |

Other host coverage: `tests/host/desktop/desktop_test.c` (v2 `DESK_LEASE`
bounds, focus authority, grant tuple, refusals, Alt+Tab revoke and
reactivation, release, client loss), `tests/host/proc/desktop_test.c`
(`channel_connected`: opposite open ends across processes only),
`tests/host/display_present_test.c` (a native channel peer is granted the
lease, presents, loses it when the channel closes; a Win32 client cannot use
it), `tests/host/desktop/gfxdemo_frame_test.c` (production runtime and
desktop: full-screen request, leased frames counted as completions, no
windowed frames under the lease, Alt+Tab and refocus, Esc, refusal) and
`tests/host/desktop/gfxdemo_test.c` (controls, status line, frames).

## QEMU check (software path)

QEMU has no Savage 3D model, so the provider is never qualified. On the
canonical image (after `make sdk`, `make build-full`), any desktop profile:

1. Open Graphics Demo, page OpenGL cube. The status line reads
   `OpenGL software (Automatic): GPU frames 0  CPU frames N  refused 0  fallbacks 0`;
   the line above it shows the frame count, FPS, the render and window
   sizes and `(desktop-copy)`.
2. Click `Backend: Automatic` once: `Backend: Hardware`, the scene keeps
   rendering and the status line ends with `; hardware unavailable`. Click
   twice more to return to Automatic.
3. Click Full screen: the cube fills the screen at the desktop resolution
   (Native resolution checked). Esc returns to the window. Repeat, then
   Alt+Tab to another window and back: full screen resumes. The desktop is
   restored within 2 s each time.
4. Run (one at a time, each prints to the program's standard output, the
   serial capture):
   - `gfxdemo --benchmark 10`: `[gfxdemo] benchmark seconds=10 scene=cube
     source=640x480 ... status=ok backend=software policy=Automatic
     gpu_frames=0 ... present=desktop-copy pacing=60hz-slots`, exit 0.
   - `gfxdemo --benchmark 10 --fullscreen`: `[gfxdemo] fullscreen=granted
     source=<display WxH> ...`, then `status=ok backend=software
     gpu_frames=0 ... present=cpu-copy pacing=uncapped`, exit 0, desktop
     restored.
   - `gfxdemo --benchmark 5 --backend hardware`: `[gfxdemo]
     backend=Hardware result=-19 (software rendering)` and
     `status=not_hardware`, exit 1 (expected without 3D hardware).
5. The kernel log has no `[3d] fullsize result=qualified` record.

## T23 check (hardware path)

Same image, booted with `3d=probe` as for f7-11, desktop 1024x768x32:

1. Kernel log: `[3d] probe phase=armed ...`, `[3d] fullsize result=qualified
   ... width=1024 height=768 ...` and `[3d] precision result=...`.
2. Graphics Demo, OpenGL cube: the status line reads `OpenGL hardware 3D
   (Automatic): GPU frames N  CPU frames 0  refused 0  fallbacks 0`. Switch to
   Software (CPU frames grow, GPU frames stop), back to Automatic (GPU frames
   grow again). Same on OpenGL tiles.
3. Full screen with the button, Esc, Alt+Tab out and back, as in the QEMU
   check; GPU frames keep growing and the desktop is restored within 2 s.
4. Run, one at a time:
   - `gfxdemo --benchmark 30 --backend hardware` and
     `gfxdemo --benchmark 30 --backend hardware --scene tiles`: `status=ok
     backend=hardware policy=Hardware cpu_frames=0 fallbacks=0
     present=desktop-copy` (windowed 640x480 render).
   - `gfxdemo --benchmark 30 --backend hardware --fullscreen` and the same
     with `--scene tiles`: `fullscreen=granted source=1024x768
     native=1024x768x32 scale=1`, then `status=ok backend=hardware
     cpu_frames=0 fallbacks=0 present=gpu-copy pacing=uncapped`; record
     `average_fps` (completed-copy FPS), `frame_us`, `p95_us`.
5. Evidence: image SHA-256, serial capture, disk logs, the `[gfxdemo]` and
   `[3d]` lines. A `status=not_hardware` or `fallbacks>0` line is a failure
   to report with the preceding `[3d]` records.

Expected cost on the hardware path: like opengl32, every completed frame
(finish or present) reads the colour and D16 target back into RAM as the
journal's checkpoint: 1.8 MB per 640x480 frame, 4.7 MB per 1024x768 frame.
The completed-copy FPS above includes that readback; it is not the contract
T2 gate (which also needs the textured T2 scene).

Superseded for full-screen frames by the integration with f7-15 (GPU-resident
targets, 2026-10-27): a `ciuki_gl_present` that the provider completes on the
GPU reads nothing back into the caller's pixels (as opengl32's GPU swap); the
kernel presents the completed linear copy directly on the scanout when the
lease is unscaled with identity gamma, otherwise it reads that copy back for
the CPU presenter. A rebind or close right after a GPU present releases the
frame without a readback. Windowed frames (`ciuki_gl_finish`) still read the
colour and D16 target back, because the window presents the caller's pixels.
Host replay: 0 target reads and 0 uploads over three 1024x768 lease frames.
