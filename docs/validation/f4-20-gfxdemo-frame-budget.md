# F4-20 Graphics Demo frame budget — host validation

## Research and implementation decision (2026-10-14)

- [X Consortium Double Buffer Extension](https://www.x.org/releases/X11R7.7/doc/xextproto/dbe.pdf),
  section 3: render into a private back buffer while the front remains visible.
  Ciuki's submitted shared surface must remain immutable until PRESENTED;
  the existing private scene buffer can prepare one successor during that wait.
- [SDL RenderPresent](https://wiki.libsdl.org/SDL3/SDL_RenderPresent): a frame
  is composed before presentation. Do not treat an unfinished successor as
  submitted or count it as displayed. Resize, UI changes and errors must
  invalidate prepared work before publishing it.
- [SDL SetRenderVSync](https://wiki.libsdl.org/SDL3/SDL_SetRenderVSync): refresh
  intervals and presentation are distinct policies. Ciuki's VBE path has no
  measured refresh/vblank signal; retain its absolute 16,666,667 ns software
  slots and explicitly report the assumed 60 Hz, rather than claim hardware
  vsync. No kernel or refresh-detection change is authorized here.

Repository inspection found DESK_FRAME_NS already at 60 Hz. There is no
40 ms constant or dependency on the 2 s statistics interval in the frame
path. The demo currently waits for PRESENTED before rasterizing; it also
copies scene pixels into the toolkit back buffer, then into the shared
surface. Prepare one private scene during presentation, and submit changed
source rows directly to the shared surface after acknowledgement. Keep full
UI repaint/resize paths and compositor pixel goldens intact.

The T23 record establishes roughly 2.3 ms present and 2.7 ms composition per
recorded desktop frame, not a measured raster, scheduling or IPC breakdown.
It does not establish a unique cause for the observed 25 FPS. New per-frame
measurements must distinguish app rendering, submission, acknowledgement
wait, conversion/copy and software slot delay before a hardware conclusion.
Host scalar benchmarks are directional only: invariant-TSC ticks divided
by 1.13 GHz do not simulate Pentium III instructions, caches or PCI writes.

## Results

Reference source: `f63f6d1b28cb2f473297f63e9c8895a6d24bbd35` (worktree HEAD).
Host: AMD Ryzen 7 7800X3D; scalar Clang `-O2 -fno-strict-aliasing -fno-builtin
-mno-sse -mno-sse2 -mno-mmx`, CPU 0 affinity, serialized RDTSC, median of nine
24-frame batches. All builds/tests were sequential, at `nice -n 19`. Host
available memory was checked (initially 4,865 MiB; 4,332 MiB before the final
suite). No full image/SDK build or memory-cap change was needed.

The paired benchmark now uses the actual default client dimensions 760x660
for 2x, with 320x240 source and 640x480 viewport; 3x uses a larger 1000x900
client, clipped by the 1024x768 host scanout. The reference mode retains the
f2-59 scene → toolkit back → shared source pipeline; the current mode uses
source diff and direct changed-row submission, including timing messages.
Both run the same poses, production parser, compositor and upload loop, and
assert exact reconstruction. The isolated scale sample is excluded from totals
because that work already belongs to composition. RAM upload timing is
reported separately; it does not measure the T23 LFB.

Repeated pair, median serialized TSC ticks per frame:

| Page / enlargement | Raster before → after | Source diff/copy before → after | Composition before → after | Upload bytes (both) |
| --- | ---: | ---: | ---: | ---: |
| Flat cube / 2x | 796557 → 801643 | 77316 → 67554 | 103921 → 106086 | 307608 |
| Gouraud cube / 2x | 974058 → 975257 | 77540 → 66181 | 104660 → 108179 | 307608 |
| Sprites / 2x | 142784 → 143576 | 172124 → 98143 | 317701 → 323484 | 1141634 |

Cube source-copy work is about 13–15% lower; the rasterizer is unchanged.
Sprites' source-copy reference varied from 112811 in the initial pair to
172124 in the repeat, so its apparent 43% reduction is not a reliable speedup
claim. The initial after run also had flat-cube raster 1408664 and isolated
scale 674110 ticks, versus final 801643/308167. The clock-hook correction does
not change the benchmark's raster or isolated scale work.
These observations motivated the single repeated pair and prohibit a precise
hardware throughput inference from these host timings.

Directional work model, microseconds: host TSC work divided by **1.13 GHz**,
plus destination bytes / the historical T23 **134 MB/s** upload model:

| Page / enlargement | Before | After | Upload alone |
| --- | ---: | ---: | ---: |
| Flat cube / 2x | 3162 | 3160 | 2296 |
| Gouraud cube / 2x | 3320 | 3315 | 2296 |
| Sprites / 2x | 9081 | 9021 | 8520 |
| Flat cube / 3x | 6061 | 6065 | 5166 |
| Gouraud cube / 3x | 6221 | 6221 | 5166 |
| Sprites / 3x | 17514 | 17528 | 16912 |

This is a serial work estimate, not a frame-interval estimate. Pipelining
overlaps raster with pending presentation; software slots, scheduling and IPC
still determine actual cadence. Default-size upload is directionally below
16.67 ms, whereas the clipped 3x sprite upload alone exceeds that budget.
The actual T23 breakdown and ≥60 FPS target remain unqualified.

The supplied T23 totals imply 30/13 = 2.31 ms present and 35/13 = 2.69 ms
composition per recorded frame. Also, 13 desktop frames over the reported 2 s
is 6.5 attempts/s; that sample does not itself establish the owner's observed
25/40 FPS cadence. Capture simultaneous overlay/log samples for the same
scene, default size and image hash before assigning the remaining time.
There is no fixed 40 ms timer, no 25 Hz constant, no 2 s gating of frames and
no explicit vsync wait in the inspected app/compositor paths. Toolkit IPC is
nonblocking; it freezes shared writes for correctness, not by blocking send.
The real removed serialization is waiting for PRESENTED before rasterizing.

Commands, run separately:

```sh
nice -n 19 python3 tests/host/desktop/test_gfxdemo_performance.py --benchmark --reference f63f6d1b28cb2f473297f63e9c8895a6d24bbd35 --pipeline-reference
nice -n 19 python3 tests/host/desktop/test_gfxdemo_performance.py --benchmark
nice -n 19 python3 tests/host/desktop/test_compositor_performance.py --benchmark --frames 32
nice -n 19 python3 -B -m unittest discover -s tests/host/desktop -t tests/host -v
```

Final desktop suite: **96 discovered, 91 passed, 5 skipped**, 258.647 s,
`OK (skipped=5)`. The skips require a built desktop payload: accessory image
inventory, desktop payload FAT readback, desktop build provenance, stale
payload rejection and Tasks image inventory. Host terminal/Lua tests also pass
after supplying the pinned local archive. The runtime replay, frame pipeline,
all five demo goldens, all three compositor goldens and full-capacity settings
log preservation pass under ASan/UBSan. `git diff --check` is clean.
Raw compact benchmark/check logs and the handoff are under `build/codex/`.
No QEMU/hardware qualification, image build, commit or diary entry is part of
this task. The lead must build/inspect the integrated SDK payload and image,
run existing guest gates without changing goldens, then measure cube (flat
and Gouraud) and sprites on the T23 with Savage quarantined and WC active.
Check at default, minimum, and resized sizes, with exposure/overlap, pause,
minimize/restore, theme changes and error/backpressure recovery.

If 60 FPS is still missed: compare app render/submit/wait with ingress slot
delay, convert/copy and desktop totals; do not add overlapping render/wait or
copy/present twice. A long render calls for P3 raster profiling; a long wait
with small copy/convert calls for scheduling/IPC and missed-slot measurement;
a large upload calls for measured dirty-area/bandwidth work. These are
follow-up diagnoses, not unapproved kernel or refresh-policy changes.

## Additional verification

Seven modified production C translation units compile cleanly with the local
cached SDK's verified pinned clang, exact `i686-unknown-elf`/`pentiumpro` flags,
`-O2 -Wall -Wextra -Werror` and the 4096-byte frame warning. SDK inputs were
read only; output objects/temporary directory were inside this worktree and
removed. This is compilation evidence, not a newly linked payload/image.
The local pinned Lua 5.4.8 source archive (374332 bytes, SHA-256 from
`config/sdk-pins.json`) was copied to the worktree's ignored download cache
to satisfy the terminal host test. No external source or payload was modified.

The deterministic app initially acquired a runtime `du_now` link dependency;
the final implementation instead supplies the scene clock through `gfx_app`
from the runtime, leaving non-runtime previews independent. Its five original
Graphics Demo goldens passed after this correction.

Compositor benchmark (same flags, CPU and 32 frames):

| Theme | TSC ticks/output pixel before → after | Host ns/frame before → after |
| --- | ---: | ---: |
| Platinum | 1.648 → 1.046 | 308729 → 196014 |
| Ciuki Blue | 1.003 → 1.026 | 187964 → 192272 |
| Classic 98 | 0.719 → 0.721 | 134785 → 135110 |

All three exact SHA-256 scene goldens and tiled/one-pixel clipping assertions
match before/after, without re-recording. The compositor's normal scene work
was not optimized by this task; Platinum's large timing difference should be
read in the context of the observed host variability, not as an algorithmic
speedup claim.
