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
