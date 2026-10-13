# Graphics Demo (f2-52)

Research checked 2026-10-13 against the upstream
[X.Org Savage XAA source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c)
(solid fills, screen copies, screen-origin MSB-first monochrome patterns and
idle synchronization), and the
[OpenGL 2.1 specification, §3.5.1](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf)
(pixel-centre coverage, shared-edge ownership, barycentric interpolation,
perspective-correct texture coordinates and depth testing). These are behaviour
references; no upstream implementation is copied and this demo is not an OpenGL
implementation. The existing engine contract remains [savage-2d.md](savage-2d.md).
The [Linux man-pages clock reference](https://man7.org/linux/man-pages/man2/clock_gettime.2.html)
confirms implementation-dependent resolution and repeated monotonic timestamps.
Ciuki's `clock.h` and `sdk/libciuki/time.c` specify 1 ms resolution. Benchmark
samples therefore batch until at least 8 ms, with a 64-operation cap; insufficient
elapsed time is unavailable. Four batches are accumulated per result, and the
table labels their average duration as ms/batch. No clock service is changed.

Repository comparison: `fbdev_accel_request` exposes synchronous scanout fills,
copies and patterns through the desktop's exclusive, non-inheritable grant.
`ciuki_present` uploads XRGB8888 with the CPU; it is not an accelerated upload.
Only the desktop may access the display. Native clients use bounded messages.

Decisions:

- Add an optional output to the existing DISPLAY_ACCEL/QUERY call. It returns
  a locked, read-only kernel snapshot (driver, capabilities, quarantine and
  operation/idle/fallback counters), without register access or activation.
  Existing QUERY callers and the request layout remain compatible.
- Only the desktop-launched `/Programs/gfxdemo` endpoint may request graphics
  information or a benchmark. The desktop fixes every rectangle and submits
  one bounded batch per request, rate limited to one sample per 100 ms. No client
  supplies display coordinates, grants or register commands.
- Hardware results require rectangle capability, an unquarantined engine and
  a completed kernel operation-counter increment. Fallback is unavailable,
  never relabelled as hardware. Software results measure RAM drawing plus
  ordinary CPU upload to the same display. Decimal MB/s counts destination
  bytes (native bpp), including copies once; restoration is outside the timer.
  Screen fills/patterns briefly replace scanout only after Run benchmark.
  Restore the authoritative desktop output immediately, including failures;
  invalidate scanout reuse and schedule a full repaint if needed.
- The cube uses integer fixed-point transforms, pixel-centre triangle coverage,
  a depth buffer and perspective-correct nearest-neighbour texturing of the
  existing deterministic approved Ciuki portrait. Flat/Gouraud lighting are
  switchable. No 3D driver or hardware 3D claim is added (that remains F7).
- Scaling uses nearest-neighbour rows. Under the
  [f2-50 rendering architecture](desktop-rendering-architecture.md), composition
  finishes directly in the mapped output before presentation. The desktop
  uploads the first of equal non-solid row spans and uses synchronous screen
  copies for the remaining rows when available. It compares adjacent rows of
  that completed output, and only copies from a span already presented in the
  same call. No front-frame allocation, copy or old-frame comparison is kept.
  This fits the upstream Savage screen-copy and idle synchronization behavior
  cited above and the existing synchronous kernel presenter contract. A failed
  copy invalidates scanout and schedules f2-50's full reconstruction from RAM.
  The UI identifies CPU upload plus optional 2D copies, and exposes actual
  kernel ops. Deferred activation uses the existing per-frame capability query;
  a display-generation reset invalidates scanout and requires a full upload
  before any screen copies. Benchmark restoration invalidates scanout reuse.
- Animation follows monotonic elapsed time with a 60 Hz target and no catch-up
  burst. FPS/ms counts submitted client frames, not measured scanout/vsync.
  The system strip averages present/composite time from the desktop's existing
  two-second performance samples. Deterministic tests supply fixed elapsed
  time and the same converted portrait used in production.

Host evidence and remaining guest/hardware checks are in
`build/codex/f2-52-report.md`; the f2-50 merge resolution and host checks are
recorded in `build/codex/f2-52b-report.md`. QEMU is reserved for the lead.
