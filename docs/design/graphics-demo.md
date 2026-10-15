# Graphics Demo (f2-52)

F7-01 amendment (2026-10-15): the scene implementation and default source
resolution described below are superseded by
[Software OpenGL](opengl-software.md). The cube and tiles now use the SDK
TinyGL subset and render at 640x480 in the default 760x660 client. The existing
frame ownership, graphics messages, 2D benchmark and 60 Hz presentation slots
remain in use. FPS counts completed presents, with a cumulative on-screen
frame counter, a periodic log and a fixed-duration benchmark. Earlier sections
remain the record of the previous renderer.

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

## f2-59 software frame budget (2026-10-14)

Research: [Intel's Pentium II/III optimization manual, 245127-001](https://download.intel.com/design/PentiumII/manuals/24512701.pdf)
(integer scheduling, division, memory access), the
[X.Org MIT-SHM completion contract](https://xorg.freedesktop.org/archive/X11R7.5/doc/Xext/mit-shm.html)
(the client must wait for completion before modifying shared pixels), and
the OpenGL 2.1 coverage/interpolation reference above. Repository comparison:
the old rasterizer divides 64-bit barycentric numerators per fragment;
`gfx_paint` scales with two divisions per destination pixel and repaints all
widgets; `du_window_present` compares every client row; the desktop visits
chrome even for damage wholly inside opaque client content. Repeating toolkit
timers and the desktop redraw both re-anchor deadlines to the current time.
There is no refresh/vblank observation in this VBE software path.

Decision: retain exact pixel-centre coverage, depth and perspective texturing,
but walk covered scanline spans with quotient/remainder interpolants.
Interpolate the cube's former 10 Hz rotation steps for distinct 60 Hz poses. Render
at the viewport's native size only when the client fits 640x480; otherwise retain
320x240 and compose a nearest-neighbour scene region inside the full-resolution
UI surface. The preferred client size is 760x660, fitting a 640x480 scene (2x) at
1024x768. Smaller non-integral viewports keep the available 4:3 area.
Wide windows below 404 pixels high compact the status strip to reserve at
least 320x240 without overlapping the packed source and controls.
Integer enlargement expands one row and duplicates it with
`rep movsd`; arbitrary ratios use exact quotient/remainder stepping, without
per-pixel division or a large index table on the 4 KiB audited stack. Animation
updates compare only that source region, sending its changed bounds. Static
widgets and status text repaint only on UI changes or the FPS/status sample.

The optional v2.1 `GFX_FRAME` request is launch-authorized like GFX_QUERY,
without display access. Its ordinary form reuses the attached client surface.
The 64-byte encoding is:
serial, destination x/y/w/h, source w/h (source origin equals destination
origin), source damage x/y/w/h, then ordinary UI damage x/y/w/h, all uint32.
UI damage and the scene descriptor are applied atomically; this avoids an
intermediate unscaled scene on the first frame. Source and destination must fit
the client's existing surface, source dimensions cannot exceed destination
dimensions, and damage must fit the source. A nonzero serial with the scene/damage
words zero disables the region (ordinary UI damage may still be present).
The descriptor persists for exposure/full
reconstruction and is cleared on a surface swap. `GFX_PRESENTED` is 32 bytes:
serial, signed result, completion ns (uint64), next nominal slot ns (uint64),
and reserved zero. The client keeps at most one request outstanding and freezes
surface writes until its reply; failed uploads retry without acknowledging
success. A saturated reply queue retains one completion and its timestamp
until the FIFO drains, keeping the producer frozen. Minimized/collapsed
windows return EAGAIN and do not increase
FPS. Successful replies count completed desktop presents, not timer ticks or
measured vsync. The next slot follows an absolute 60 Hz phase, skipping missed
slots without a catch-up burst. Overruns compare actual processing time with
the full 16.67 ms budget; idle slots or a quick update crossing a phase boundary
do not count as processing overruns. Slow renders retain the overrun/drop
record. Sleeps are floored to the SDK's 1 ms clock;
the final fraction yields instead of requesting a sleep beyond the slot.

A replacement uses the 68-byte GFX_FRAME form: append the existing configure
serial and attach one replacement surface. It obeys the existing v2.1 resize
bounds/serial rules and commits the surface and scene descriptor together,
before painting; CONFIGURE(COMMITTED) still releases the old client mapping.
Both forms participate in the launch's first-damage/first-present markers.
Ordinary toolkit clients retain their existing DAMAGE and resize messages.

For an opaque client-only clip with no intersecting higher window/shadow or
shell overlay, copy content straight to the mapped output without visiting
chrome/background. Preserve XRGB's ignored high byte by normalization; never
infer opacity from it. Mixed damage and occlusion retain the existing painter.
The software LFB upload remains the kernel's existing presenter.

Validation and the identical-flags before/after cycle breakdown are in
`build/codex/f2-59-report.md`. Modern serialized TSC ticks divided by 1 GHz
are directional estimates, not Pentium III retired cycles; model the T23
upload separately at its previously measured 134 MB/s. Hardware FPS is the
lead's acceptance check.

### F4-20: one prepared successor and frame timing (2026-10-14)

Research and evidence: [host validation](../validation/f4-20-gfxdemo-frame-budget.md).
The client prepares one successor in its existing private scene buffer while
the submitted shared source remains frozen. Preparation compares against that
source without copying through the UI back buffer; acknowledgement permits
one changed-source copy and another submission. The animation phase predicts
one nominal frame ahead during that wait, without advancing the elapsed-time
accumulator. No extra full-window allocation, thread or second in-flight request
is needed. UI repaint, resize, pause/tab changes and EAGAIN supersede prepared
work. Full UI repaints retain toolkit publication and do not copy source rows
again. The server keeps its existing absolute 60 Hz slot policy.

The optional 84-byte FRAME preserves offsets 0–63 and puts a configure serial
at 64 (zero without replacement), the last acknowledged frame serial at 68,
and its render/submit/wait microseconds at 72/76/80. Attach exactly one surface
only with a nonzero configure serial. Old 64/68-byte requests remain accepted.
An instrumented request gets a 64-byte PRESENTED: the original 32 bytes plus
uint64 conversion, copy, ingress slot-delay and nominal refresh-period ns at
32/40/48/56. Old requests retain their 32-byte reply. Reserved words, lengths,
directions and attachment rules remain validated.

Render includes raster and source-damage discovery (and source storage during
UI repaint). Submit measures explicit changed-source publication/encoding/FIFO
enqueue; toolkit UI painting/publication is additional work on repaint turns.
Wait measures enqueue through receipt of PRESENTED, including scheduler and
IPC/polling latency. Conversion times clipped source enlargement in the
compositor; copy times actual present calls intersecting the demo content.
Merged upload spans can include other damage. Retries accumulate attempted
conversion/copy time; completion-queue backpressure retains the original
measurements. These are components of existing work, not additional work to
add to desktop totals. Clock resolution is 1 ms; sub-millisecond measurements
can be zero. There is no claim of hardware vsync or calibrated app TSC timing.

The overlay shows the most recently acknowledged timings. The existing 2 s
disk sample adds a `[display] gfx_serial=...` line for the latest completed
frame, with `app_serial` identifying the previous acknowledged frame carried
by that request. Window/source/viewport geometry and ingress slot delay belong
to `gfx_serial`; render/submit/wait belong to `app_serial`. Two serials make
the unavoidable one-frame wait-telemetry delay explicit. This is a sampled
frame record, not an average and not a complete per-frame disk trace.
