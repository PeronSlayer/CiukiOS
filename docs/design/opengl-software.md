# Software OpenGL subset (F7-01, 2026-10-15)

## f7-14 amendment (2026-10-26): one GL engine for native and Win32 programs

Directive: [f7-14](../directives/f7-14-native-gl-hardware.md). This section
supersedes the F7-01 statements below that native programs use TinyGL's own
pipeline, that `glTexParameteri`/`glTexEnvi` are no-ops and that the library
never calls the kernel; the rest of the record stays as history.

Research checked 2026-10-26:
[OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf)
sections 3.8 (texture parameters and environment) and 5.5 (`glFlush` submits,
`glFinish` completes); the F7 contract sections 6 (presentation lease),
7.1 (backend policy `auto`/`software`/`hardware-required`) and 7.2 (whole
context in software for unsupported state; do not alternate CPU/GPU
fragments); [Mesa 7.7 savagetris.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c)
(`savageRenderStart`: software fallback for unsupported raster state); and
[IDirectDraw7::SetCooperativeLevel](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdraw7-setcooperativelevel)
(exclusive full screen is bound to the application's focus window and is
given up and restored with activation), the model for the native lease below.

Repository comparison: before f7-14 `sdk/libgl` linked TinyGL's raster with
its own `GL/gl.h`, so native programs could never reach `render3d`, while
`sdk/win32/opengl32` carried the engine, the bounded journal, the admission
encoder and the provider (`sdk/win32/gdi32/render3d.c`). Two GL semantics
existed, against contract 7.2.

Decisions:

- `libgl.a` now builds the same engine sources as opengl32 (`engine`, `state`,
  `texture`, `pixels`, `validate`, `geometry`, `backend`, `census`, and the
  context helpers moved to `common.c`) over the retained TinyGL front-end
  (transform, lighting, clipping), the scalar backend and a native provider.
  TinyGL's public names are renamed `tgl_*` inside the library
  (`sdk/libgl/native/tinygl_rename.h`); the engine owns every public `gl*`
  entry point. `sdk/win32/opengl32/generate.py` generates the native
  `<GL/gl.h>` (the 336 OpenGL 1.1 functions plus the advertised extensions of
  `extensions.json`) and the cdecl forwarding unit `sdk/libgl/native/api.c`
  from the same `abi.json`/`extensions.json` as the DLL exports.
- The provider body is shared: `sdk/win32/opengl32/provider.inc` is included
  by gdi32 (Win32) and by `sdk/libgl/native/provider.c` (native). Both send
  the copied `W32_RENDER3D` record of `CIUKI_SYS_W32`; the kernel accepts that
  record from native processes (`records.c`), its handler is process keyed
  and personality neutral (`core/render3d_transport.c`). Capability
  snapshots, admission (`encode.h`), the journal and the whole-context
  fallback are therefore the same code. The native provider caches the
  capability query for one frame (the engine probes before every call
  outside `glBegin`); a submission failure still falls back as in opengl32.
- `<ciuki/gl.h>` adds `ciuki_gl_set_backend` (automatic, software,
  hardware; hardware returns `-ENODEV` and keeps rendering in software when
  no qualified provider answers, or after a permanent fallback),
  `ciuki_gl_status` (completion accounting: frames, hardware frames whose
  every draw and clear ran on the provider, software frames, hardware and
  software draws, temporary refusals, permanent fallbacks, presents and GPU
  presents), `ciuki_gl_present` (full-screen presentation through a desktop
  lease: GPU copy on a qualified provider, otherwise the finished pixels by
  `W32_DISPLAY_FRAME`) and `ciuki_gl_get_proc_address`.
- Full screen for native programs uses the common present lease: the
  desktop's v2 `DESK_LEASE`/`DESK_LEASE_EVENT` carry the v3 lease payloads;
  the kernel grants the lease to a native process holding a channel with the
  desktop (`channel_connected`), beside the Win32 service links. Focus loss
  revokes it and refocus reactivates a wanted lease, as for v3 windows.
- The scene goldens (`tests/host/opengl-goldens.json`) changed with the
  engine: the shared scalar raster replaces TinyGL's, and the demo's texture
  parameters (bilinear, edge clamp, modulate) are now honoured.
- The SDK ships `licenses/opengl-engine-LGPL-2.1.txt` beside the TinyGL
  notice: the engine files carry SPDX `LGPL-2.1-or-later`.

Host evidence: `tests/host/test_libgl.py` (`test_native_hardware_replay`
runs the gfxdemo scenes through `libgl`, the copied wire, `render3d`, its
validator, the Savage BCI emitter and the fake Savage of
`tests/host/savage3d_backend_test.c`; see
[the f7-14 validation record](../validation/f7-14-native-gl.md)).

## Research and implementation decision

The [OpenGL 1.1 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
sections 2.10, 2.13, 3.8 and 4.1, defines transformations, lighting, texture
mapping and depth testing. The [TinyGL upstream](https://github.com/C-Chads/tinygl)
implements a subset, with lit textures and a portable CPU zbuffer. This port
does not claim full OpenGL 1.1 conformance. The supplied source snapshot is
recorded by archive and per-file SHA-256 in `sdk/libgl/upstream.json`.

Repository comparison: `apps/gfxdemo/scene.c` previously transformed and
rasterized triangles itself; `app.c:scene_layout` selected a 320x240 source
for its default 640x480 viewport. There was no SDK GL library. FPS counted
completed presents on screen but had no periodic log or timed benchmark.
The clean `a4f67ac1/disk-BOOT.LOG` only records launch; the serial acquisition
contains interleaved boots and cannot establish scene throughput or a unique
hardware bottleneck.

Use the supplied TinyGL CPU pipeline, 32-bit XRGB colour and 16-bit depth.
Preserve upstream source organization and put the portable surface adapter
beside it. No kernel, hardware driver or presentation-policy change is needed.
The approved Ciuki portrait is converted from XRGB to RGB bytes without
resampling or changes to pose or colour. Disable upstream's magic-pink discard
feature so every portrait colour remains drawable.

Upstream `ZB_open` rounds widths down to four pixels and its copy helper uses
the destination stride as a copy length. The adapter instead pads odd widths
up internally and copies only the visible width. Aligned packed targets render
directly into caller-owned pixels. A replacement zbuffer is allocated before
releasing the old one. Applications must render into private pixels while a
shared client surface awaits PRESENTED, as the existing demo pipeline does.

Port checks found additional upstream issues: the compatibility test shifts
into the signed sign bit; triangle slopes shift negative signed values;
`glGenBuffers` reserves an 8 KiB stack array; and `error_check.h` compares GL
enum names in preprocessor conditions, which changes invalid-enum errors into
out-of-memory errors. The recorded fixes use defined arithmetic, the caller's
buffer-name output, and C error assignments. Raster state is initialized and
preserved when replacing zbuffer storage. The target retains its 4096-byte
stack-frame limit. `port.json` identifies every altered upstream file.

## Interface and limits

`<GL/gl.h>` exposes TinyGL's immediate mode, vertex arrays, matrix stacks,
frustum, texture objects, RGB/unsigned-byte textures, nearest sampling,
flat/smooth shading, directional/positional lighting, material, depth, blending
and display-list entry points. Textures are stored at 256x256; upload that
size to avoid implicit nearest resampling. No stencil, shaders, mipmaps or
complete extension/conformance contract is provided. Use implemented entry
points only; header constants alone do not establish support. No GLU helper
is required by these scenes. `glTexEnvi` and `glTexParameteri` are upstream
no-ops: the configured lit-texture path and nearest sampling are fixed.
Depth comparison is fixed; `glClearDepth` does not select a different clear
value. Error checking remains partial and is not a conformance guarantee.

`<ciuki/gl.h>` binds an XRGB8888 target with a stride in pixels, finishes
visible rows, and closes the context. It uses only C library facilities.
There is one context per process, used from one thread. Targets are bounded
to 1600x1200. GL errors are enabled; allocation failure during upstream
context initialization remains process-fatal. The SDK uses the existing
i686 x87 ABI and excludes SSE/MMX. Link native programs with `-lgl`.
The library does not present or call the kernel. libdeskui owns the window,
surface transport and completion acknowledgement.

## Measurement and qualification

The demo opens on the cube. The Run dialog accepts
`gfxdemo --benchmark 10` and `gfxdemo --benchmark 10 --scene tiles`.
Both require the approved portrait and a 640x480 source; smaller clients fail
explicitly. Changing page, shading or pause aborts the timed run. Duration is
an integer from 1 to 60 seconds. At the monotonic deadline it prints completed
frames, average FPS, average frame microseconds and nearest-rank p95 completion
interval microseconds, then exits. The first interval begins at measurement
start; the final incomplete interval affects average FPS but is not a p95
sample. At most 4096 intervals are retained; overflow and zero completions
produce an explicit failure. The bounded 60 Hz protocol allows at most 3600
completions in the longest supported run. Samples include scheduling, transport,
composition and presentation. The per-second record also reports cumulative
frames and source dimensions; paused/hidden clients report zero FPS.

Host offscreen throughput measures CPU rendering, excluding IPC, composition
and scanout. It excludes initial context/texture creation, uses the production
scenes and exact approved texture, and reports a separate p95 render time.
The T23 targets are cube 640x480 >60 rendered FPS and tiles >40 rendered FPS.
Presented FPS is separately limited by the desktop's assumed 60 Hz software
slots. Host results do not qualify either target on physical hardware.

Sequential host results, AMD Ryzen 7 7800X3D, Clang 23.1.1 `-O2`, default
host ISA, contraction disabled in the GL recipe, `CLOCK_MONOTONIC`,
`nice -n 19`. Each scene ran for ten seconds at 640x480 after one warm-up
frame. These are observed host figures with no frequency extrapolation.

| Scene | Frames | Average FPS | Average frame us | p95 render us |
| --- | ---: | ---: | ---: | ---: |
| Cube, flat lighting | 39705 | 3970.45 | 251 | 271 |
| Cube, smooth lighting | 39611 | 3961.06 | 252 | 272 |
| Tiles | 74771 | 7477.07 | 133 | 137 |

Commands, run separately:

```sh
nice -n 19 python3 -B sdk/build_sdk.py --jobs 1
nice -n 19 python3 -B apps/desktop/build_desktop.py
nice -n 19 python3 -B tests/host/test_libgl.py -v
nice -n 19 python3 -B tests/host/test_libgl.py --benchmark 10
nice -n 19 python3 -B tests/host/desktop/test_gfxdemo_performance.py -v
nice -n 19 python3 -B -m unittest discover -s tests/host/desktop -t tests/host -v
```

The target library builds twice to identical archives, matches the installed
SDK archive and passes the x87/no-SSE/MMX audit. ASan/UBSan host tests cover
clear colours, depth occlusion, texture colours, odd-width/padded-row guards,
resize state, context close/reopen, buffer names, errors, deterministic scene
digests and elapsed-time motion. LeakSanitizer cannot run under this sandbox;
ASan/UBSan remain enabled with leak detection disabled. All 101 desktop host tests pass with no skips. The production
transport replay covers frozen submitted pixels, present retry, backpressure,
resize commit, pause/hidden frames and timed benchmark summaries. Guest and
T23 results remain unqualified. A >60 completed-present FPS cube target
conflicts with the retained 60 Hz slot policy; the host renderer figures
must not be relabelled as presented FPS or T23 results.

## Later F7 acceleration

Keep application GL calls and surface ownership separate from the renderer.
A later driver contract must define buffer ownership, command validation,
synchronization, capability discovery and fallback before replacing CPU
rasterization. A 2D copy engine does not implement this GL pipeline. Hardware
qualification remains with the lead; this change adds no hardware 3D claims.
