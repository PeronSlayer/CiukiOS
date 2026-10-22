# F2-50 desktop rendering architecture

Research and implementation contract, 2026-10-13. The existing desktop wire
protocol, client painting, themes, approved Ciuki portrait and pixel goldens
remain unchanged. Qualification on the T23 belongs to the lead.

## Period practice and repository comparison

* Microsoft's [GDI memory device contexts](https://learn.microsoft.com/en-us/windows/win32/gdi/memory-device-contexts)
  describe drawing into a bitmap and transferring it with BitBlt. Ciuki already
  retains each client's XRGB surface: that is its backing store. Moving a
  window must not ask that client to repaint or allocate another client bitmap.
* [DirectDraw 7 Blt](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-blt)
  documents off-screen/primary surface transfers, fills and busy/error handling.
  This is the Windows 98-era surface model, rather than a modern GPU compositor.
  Ciuki's existing synchronous Savage COPY/FILL contract supplies equivalent
  operations when qualified; ordinary LFB upload remains the fallback.
* Be's original [Be Book clipping documentation](https://asleson.org/public/mirrors/www.be.com/documentation/be_book/The%20Interface%20Kit/ViewGraphicsState.html)
  (preserved mirror) assigns app_server the visible update region, excluding
  obscured/off-screen areas. Ciuki previously removed only client wells, and
  abandoned clipping when its 32-box subdivision overflowed. Use an exact
  bounded horizontal-span sweep, including opaque chrome, with no overflow
  fallback that paints underneath known opaque windows.
* The [X11 core protocol](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html)
  specifies backing-store/save-under hints and CopyArea exposure semantics.
  Period X11 generally used server clipping, copies and exposure, not the later
  Composite extension. Retain our existing client backing stores and cursor
  save-under; recompute translucent corners/shadows against the new underlay.

## Rendering and ownership

The mapped output surface `d->pixels` is the composite buffer. Finish every
RAM operation before invoking any kernel presenter. The kernel already reads
this surface directly: remove both the scene-to-output copy and the separate
`accel_front` allocation/copy. Restore the software cursor's 187-pixel underlay
before composition and save/draw its final position afterwards. Presentation
failure retains dirty regions; an engine or cursor fault invalidates scanout
and requires a full reconstruction before further copies. No API change is
required, and no hardware tear-free guarantee is made.

Keep client surfaces as per-window backing stores, and retain one compact
caption/edge cache per client slot (formerly four shared slots). Cache keys
include dimensions, theme, title, icon bytes, active/alert/shade state and title
button state. Shadow falloff/colour products remain cached independently of the
underlay; translucent shadow pixels and rounded corner squares must be blended
again. Internal dialog painters retain their existing state-aware rendering.

A clean unobscured move copies the retained opaque window pixels in RAM with
overlap-safe row ordering. It repaints exposed old space, destination corners
and shadows, and any new client/chrome damage. Coalesce consecutive move events
to the final destination while retaining the original source. When COPY and a
hardware cursor are ready, mirror that move in scanout and upload only the
remaining dirty spans. A software cursor never contaminates the copy source.
Scroll detection compares client pixels with the still-current composite before
overwriting it; accept only an exhaustive exact match, then use the same COPY
contract. Ordinary rendering uses cached chrome and client-row blits.

At each frame, convert bounded protocol damage into dirty-row span lists and
vertically join equal runs for painting/upload. This avoids rectangle explosion
for text updates. Up to 32 spans on each row retain clean gaps;
occlusion removes known opaque pixels before any painter visits them.
Exact horizontal spans split at opaque window boundaries and vertically at
geometry changes. Radius-sized top corners and shadows remain dependent on
lower layers. No visual changes or new goldens are intended.

## Pacing, latency and costs

Use a 16,666,667 ns (60 Hz policy) budget measured from frame start, not from
presentation completion. Consume queued input before the next frame. If a frame
overruns, skip elapsed deadlines and retain only the latest pending state;
never render catch-up frames. Failed presentation is retried at a later slot.
Count overruns/skipped slots explicitly. This is scheduling policy, not detected
refresh or vertical synchronization.

Input-to-present latency starts at the oldest contributing input timestamp and
ends after successful presentation, including pacing waits and retries. A
launch's first frame also retains the original click timestamp. Frames without input
measure render-start to present. Per-frame records expose latency and a bounded
1 ms histogram yields a conservative nearest-rank p95; exact maximum is retained
(latencies beyond the histogram use the sample maximum). Reset histogram with
the existing two-second sample. The verbose `[console] [display]` record adds
`frame_p95_us`, `frame_max_us`, completed frames, overruns and skipped slots;
the Task Manager wire reply stays compatible. The bounded worker log mailbox
is 768 bytes so long latency records keep their final fields and newline.

At 1024x768, removing two duplicate frames saves 6 MiB when acceleration is
available (3 MiB in software). Row metadata costs 195 KiB at this resolution.
### F2-55 stable first-frame memory (2026-10-14)

Newlib's [allocator documentation](https://sourceware.org/newlib/libc.html#malloc)
distinguishes allocated objects from free space retained in the malloc arena;
[mallopt](https://sourceware.org/newlib/libc.html#mallinfo) describes returning
topmost free space through a negative `_sbrk_r`. In this repository,
`sdk/libciuki/posix.c:_sbrk_r` passes changes to `ua_brk`; this does not promise
that freeing a particular buffer immediately removes its extent. Do not change
the allocator, memory limits or crash-isolation equality rule to hide a cache
allocation. Reserve the desktop's steady-state storage before its first frame.

The host allocation harness reproduced a 31,056-byte chrome allocation when
a 240x160 client first appeared in an unused slot, including the victim slot
after the survivor baseline. This fits the observed eight-page growth, although
only the lead's QEMU run can establish the guest allocator's exact extent change.
The native probe uses `--gate`, which skips audio, folder polling, AutoPlay and
image-file wallpaper loading; those cannot explain that probe's growth.

Reserve one chrome pool before composition, with sixteen fixed slices. Each
slice contains `4 * (W * (DESK_TITLE + DESK_BORDER) + 2 * DESK_BORDER * H)`
bytes, where W/H are the display dimensions capped at 2048. At 1024x768 this
is 129,024 bytes per slot, 2,064,384 bytes total, plus cache keys. It covers
caption, both sides and bottom at every size within those bounds. Reusing a
slot, changing its size/title/theme, or recovering a crashed client only
repaints its slice. Larger frames use the same uncached theme painter. Failed
pool allocation also selects that painter until renderer teardown; it never
retries on a later client event. Display-mode changes explicitly release and
prepare storage for the new dimensions before the replacement frame.

AutoPlay reserves one content-discovery workspace when the shell is enabled;
its bound is two directory-scan records plus `AP_INF_MAX+1` bytes (10,392 bytes
on the 64-bit host). It is owned by that desktop and reused serially. Standalone
Files/Control Panel discovery retains the existing independently owned API.

Audio initialisation owns one worker block containing all eighteen
`SOUND_FILE_MAX+1` WAV slots, player metadata and the 22,050-sample hardware-test
tone. The file slots total 1,661,346 bytes; initial scheme/registry loading
finishes before thread creation. WAV reads and playback remain asynchronous on
the worker, including when its first turn occurs after the first frame. Reload
reuses the same WAV slots; explicit preference changes may still parse registry
data. Volume/window/menu/device events never allocate decode or mix buffers.
No allocator limit, E820 policy or supported hardware memory floor changes.

### F2-56 shell before the first frame (2026-10-14)

Microsoft's [initial client-area painting example](https://learn.microsoft.com/en-us/windows/win32/gdi/drawing-in-the-client-area)
requests painting immediately after creating and showing the window. This
supports treating initial content as startup work independent of later input.
The [Wayland surface specification](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface)
separately defines committed content and pending damage; damage identifies
changed pixels, rather than choosing whether a shell exists. These are lifecycle
references, not changes to Ciuki's rendering or wire protocol.

The host production-main harness reproduced the missing first-frame mark only
with `--gate`; normal startup already painted the button. In `desktop.c`, gate
startup skipped `shell_setup`, leaving `d->shell == 0`. The first F12/F9/F11/F10
press both selected a scenario and enabled the shell. Consequently `scene()`
correctly selected its no-shell fallback; direct composition and chrome-cache
damage were not responsible.

Run `shell_setup(d, gate)` before any frame in both modes. Gate setup retains
built-in settings and skips audio, folder polling, AutoPlay and image-file
wallpaper loading. Select a runtime scenario using the existing `scenario_key`
state, independently of shell presence, without repeating setup or reallocating
shell storage at that input event. Keep the fallback for renderers without a
shell. Capture the first production-presented frame with input reads returning
EAGAIN and compare every fully opaque cell of the approved portrait's 18 px
area average at (13,7) in both startup modes. QEMU qualification remains with
the lead.

Budget estimates for a 1 GHz Pentium III are hypotheses, not qualification:

| Operation | Expected work / target |
| --- | --- |
| 400x300 drag | 0.5 MiB RAM blit plus exposure/shadow strips; <=8 ms |
| Start open | menu raster plus visible underlay and upload; <=15 ms |
| Full redraw | one visible-layer composition, one 3 MiB LFB transfer; <=30 ms |
| Input to present | at most one pacing slot plus ordinary rendering; p95 <=33 ms |

The measured old full-frame LFB transfer alone was 23 ms (134 MB/s), leaving
only 7 ms composition to meet 30 ms. Host serialized-cycle estimates at 1 GHz
and actual host nanoseconds must be reported separately. Host CPUs, memory,
syscall substitutes and scheduling do not reproduce PIII/LFB performance.
The replay exercises drag, Start open/close and launch/first DAMAGE, publishes
every frame's latency and f2-48 painter breakdown, and reports p50/p95/max.
Only lead-owned T23 measurement can establish the hardware targets.

### F2-95 damage regions, frame cadence and the T23 CPU budget (2026-10-22)

T23 trial of image a4d9454e: the desktop took 40-50 % of the CPU with only
the Music Player open. The `[cpu] top` records name the threads:
`desktop#30` (12-50 %, almost all kernel time, holding the block cache that
netsurf and the compositor then waited for) is the shortcut worker, not the
compositor; `desktop#31` (7-10 %) is the sound worker; `desktop#28` (the
compositor and service loop) took 9-18 %. The `[desktop] accel` records of
the music session show 20-50 frames per second of a few KiB each, the
hardware-cursor frames of pointer motion; the player itself repaints its
labels 2-5 times a second (`repaint_mps` 1799-5396 per thousand seconds).

Period practice, primary sources:

* Windows keeps an [update region](https://learn.microsoft.com/en-us/windows/win32/gdi/the-update-region)
  per window: only the invalid part is redrawn, and
  [WM_PAINT](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-paint) is
  generated only when the message queue is empty, so input always runs first
  and repeated invalidations merge into one paint.
* The X server represents clip and damage as regions (y-x banded rectangle
  sets, the sample server's `miregion.c`, today `dix/region.c` and pixman's
  [pixman-region.c](https://gitlab.freedesktop.org/pixman/pixman/-/raw/master/pixman/pixman-region.c));
  the [DAMAGE extension](https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/raw/master/damageproto.txt)
  reports exactly those regions (raw rectangles, deltas, bounding box or
  non-empty) to whoever repaints.

Decision (lead amendment, 2026-10-22): the X11/Windows 98 model, with no
full-frame composition. The compositor's damage lists and frame regions are
pixman 0.46.4 regions (MIT, `third_party/pixman`, pinned there): every
damage addition is a region union (exact, disjoint, coalesced bands); a
frame composes and presents exactly the region's rectangles minus a copied
destination (region subtraction), and the present-damage skip uses
`pixman_region32_contains_rectangle`. The former per-row span lists
(`rows_add`/`row_boxes`, 195 KiB at 1024x768) are gone. Region storage is a
fixed pool inside the compositor (64 blocks of 32 rectangles, 4 of 512):
the desktop still allocates nothing after its first frame, and when the pool
cannot hold a region the frame uses the list's covering box instead of
failing. A list holds at most 32 rectangles; beyond that the pair whose
bounding box adds the least area is joined, never the whole list into one
screen-sized box. An incomplete frame keeps its own damage: only an engine
failure (scanout unknown) still damages the whole screen.

Frame cadence: input and pointer frames keep the 60 Hz slot. Continuous small
damage without input for 250 ms (under a sixteenth of the screen, no
frame-paced presenter, no launch) is composed at most every 33.3 ms (30 fps);
an isolated update is never delayed. A refused frame (display owned
elsewhere, a leased program) is retried at the frame slot, never on every
1 ms service turn.

Service loop and workers: the shortcut worker takes types, sizes and times
from the directory enumeration (no stat), reuses unchanged shortcut records
and rescans on events or every 10 s (the bundled-games query, which mounts
the games image in the kernel, only at start and after a program change);
the sound worker closes its stream when one resampled frame is left (the
mixer keeps it waiting for an interpolation partner unless drained) and then
sleeps; an empty service turn costs 17 kernel entries instead of 24; input
that arrived during a turn is read before its sleep; the stall observer
counts hardware-cursor frames as frames; a `[desktop] loop turns=...`
record reports service turns per 2 s and the deadline that shortened them.

Not taken this round: pixman's image and compositing code (fast paths,
`pixman-mmx.c`). The compositor's opaque rows already use `rep movsl`
(fast-string moves on the P6 family) and its blends cover shadows, corners
and glyphs only; on the T23 model below composition is 0.12 % of the
CPU with the player open and 1.4 % while dragging, so MMX
OVER paths could save at most a fraction of that. Using them also needs the
compositor's integer-only build rule (`build_desktop.py` audit) changed and
the MMX state of desktop threads qualified on the T23.

T23 model (`tests/host/desktop/t23_cpu_model_test.c`, rates stated there):

| Scenario | Frames/s | Composed px/frame | CPU bytes to LFB/s | Desktop CPU |
| --- | ---: | ---: | ---: | ---: |
| Player open, nothing moving (a4d9454e) | | | | 40.6 % (desktop#30 30 %, #31 8.5 %, #28 2.1 % modeled) |
| Player open, nothing moving (f2-95) | 5.0 | 19,296 | 16,080 | 2.1 % (3.1 % at 10 us per kernel entry) |
| Player window dragged at 60 fps (f2-95) | 60.3 | 19,558 | 3.6 MB | 17.4 % (19.9 %) |

While dragging, the RAM copy of the moved window (60 MB/s, about 10 % at
600 MB/s) and the exposed strips sent to the LFB (3.6 MB/s, 2.7 %) dominate;
the engine copies the window in scanout. Budgets: under 10 % with the
player open, under 40 % dragging. The
a4d9454e worker shares are the trial's measurements; the model does not
reproduce the trial's 9-18 % for desktop#28, which included pointer frames
and user time the host cannot attribute: the new `[desktop] loop` record
measures it on the next T23 image. Host counts are evidence for the code
paths, not T23 timings.


## F2-98a: native client update regions

The toolkit accumulates bounded client rectangles until a render turn, clips
all widget and application painting to those rectangles, and compares/copies
only those rectangles into the shared surface. Separate dirty widgets retain
separate rectangles. A bounded list merges the least costly pair when full;
allocation failure cannot add a fatal path. Surface replacement still sends
a complete first frame. Explicit external back-buffer presenters retain their
full-surface compatibility path. Scroll operations move retained pixels in
RAM and invalidate exposed strips; moved pixels also need surface publication.
Custom painters must intersect, never widen, the supplied clip. Timer callbacks
invalidate only changed visible state. A cold first frame precedes optional
file reads, directory scans and image decoding; results are published on the
UI thread, with worker-owned buffers until completion.

Sources checked against the toolkit and applications for this directive:

* Microsoft SDK, [Invalidating and Validating the Update Region](https://learn.microsoft.com/en-us/windows/win32/gdi/invalidating-and-validating-the-update-region):
  accumulate invalidations while processing input, then paint the combined
  update region, rather than painting synchronously for each event.
* Microsoft SDK, [Redrawing in the Update Region](https://learn.microsoft.com/en-us/windows/win32/gdi/redrawing-in-the-update-region):
  the paint DC clips to the update region; merely narrowing the final present
  while running a full custom painter does not satisfy this model.
* Microsoft SDK, [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex):
  retain translated pixels and invalidate the exposed area. The current SDK
  minimum-version table is not evidence of historical availability.
* Microsoft Windows 95 DDK, [Display Minidriver Guidelines](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S27F2.HTM)
  (archived original Microsoft text): DIB engine drawing with optional driver
  acceleration; this is the 9x driver family, not the NT EngBitBlt contract.
* Microsoft KB [Q224152, Windows 95/98 Don't Use the Display Driver's FastBorder Interface](https://helparchive.huntertur.net/document/108076)
  (archived original Microsoft text, Windows 95/98 DDK applicability): the
  specific FastBorder example synthesizes GDI/USER functionality from more
  fundamental DDIs and documents the Windows 9x DIBEngine DLL.
* Windows 98 Resource Kit, main checkout read-only `build/rskt/wrkc28_8.htm`
  (Core System Components, User and Graphics Device Interface) and
  `wrkc24_3.htm` (Display Driver Overview): asynchronous input queues,
  optimized 32-bit DIB operations and display minidriver fallback. These
  chapters establish architecture, not application timing guarantees.

T23 budgets are estimates from counted production operations with explicitly
stated memory/entry rates, alongside measured host CPU time. Host milliseconds
are not T23 measurements; disk/decode latency requires separate evidence.
An unchanged view performs no raster or surface-compare work. The native
service loop still polls asynchronous completions every 10 ms. Eliminating
those wakes needs a channel/deadline wait and worker-completion wake contract;
switching to the current kernel poll loop alone still wakes every millisecond.
The budget charges a service-loop baseline instead of claiming literal zero
CPU. Small Tango icon conversions retain their existing lazy process cache
on the UI thread; startup workers never mutate that cache.
