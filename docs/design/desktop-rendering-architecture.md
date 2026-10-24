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

## fx-2: stacking changes repaint what they uncover (2026-10-23)

T23 trial of image `9a60215f` (software engine, `accel caps=0 pointer=sw`):
windows repainted over windows that were on top of them. Every screen write
of the desktop is a present of the composed RAM scene, and the scene painter
clips each window to the spans left by the windows above it, so a window can
only appear over a higher one when the screen still shows an older stacking
than `order` describes. `desk_v3_restack` (run by every focus change, v3
placement and v3 window order while any v3 window exists, NetSurf included)
renumbers the whole stack: internal shell windows (Run, Programs, AutoPlay,
messages) above all others, the topmost band above normal windows, owned
windows above their owners. Only the window being raised was repainted. The
windows that moved up kept their old screen appearance while compositing
treated them as higher: their next update was painted over the windows that
still looked on top, and the others' updates were cut out beneath them.

Rule (Windows USER: SetWindowPos repaints, unless SWP_NOREDRAW, what a
position or z-order change uncovers; owned windows stay above their owner;
the topmost band stays above normal windows): before each frame,
`desk_stack_damage` compares the stacking of the last composed frame with
the current one and damages the overlap of every pair of visible windows
whose relative order changed, whatever path changed it. The same audit
cancels a coalesced window-move copy once damage has been recorded at an
intermediate position of the moving window (the copy can exclude damage only
at its source or final destination), and repaints task buttons when a v3
window appears or a window's responsiveness changes.

`tests/host/desktop/test_occlusion.py` drives the production desktop with
the software upload, the 2D engine and the f2-101d command batches (MONO,
INVERT, KEYED, WRITE through production preflight and swdib) at 32 and
16 bpp: a covering window's screen pixels stay unchanged while the window
under it animates, scrolls, moves, retitles and draws with client-side
MONO/INVERT/KEYED; owned/topmost/internal restacks and v3 window order are
repainted; randomized sequences compare every frame with a full composite
and, in 32 bpp, every unoccluded client pixel with its surface.

Open lead decision: internal shell windows form a band above every other
window only while a v3 window exists; without one they are ordinary
windows. Windows 9x bands only topmost windows (and owned above owners).

Sources:

* Microsoft SDK, [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos):
  SWP_NOREDRAW is the exception that leaves invalidation to the application;
  owned windows of a topmost window are topmost.
* Microsoft SDK, [Window Features, Z-Order](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#z-order):
  an owned window is always above its owner; topmost windows overlap all
  non-topmost windows.
* Microsoft SDK, [Redrawing in the Update Region](https://learn.microsoft.com/en-us/windows/win32/gdi/redrawing-in-the-update-region):
  painting is clipped to the update region within the visible region.

## fx-15: the Windows 9x window-manager model, every row (2026-10-23)

T23 trial of preview image `e831e685` (Savage 2D engine: `[desktop] accel
... caps=3 vram_rows=1280 pointer=hw`): windows stayed drawn when another
window opened, and windows still opened slowly. fx-2 fixed one path (the
restack). This section is the whole model: every event that changes the
screen, what Windows 9x repaints for it, the CiukiOS path, and the defect
the audit found there.

### The model

Windows 9x keeps no retained screen: USER invalidates, applications paint.
Every operation that changes what is visible adds the changed area to the
update region of the windows concerned (`InvalidateRect`); `WM_PAINT` and
`WM_NCPAINT` repaint exactly that region, clipped to the visible region
(`BeginPaint`); `WM_ERASEBKGND` first erases it with the class brush.
`SetWindowPos`/`ShowWindow` invalidate what a move, size, z-order or
visibility change uncovers or covers (`SWP_NOREDRAW` is the only opt-out);
`EndDeferWindowPos` applies several such changes "in a single
screen-refreshing cycle". CiukiOS retains a surface per window and composes
the screen from them, so its rule is the same postcondition: **the damage
of a frame contains every pixel whose correct rendering changed since the
last frame shown, and an exposure repaint of any part equals a full
repaint of it.** The table is the inventory; every row is a host test
(`tests/host/desktop/test_window_model.py`, rows named in the last column)
that compares each frame on screen with a full redraw on five presenters:
the software upload, the 2D engine (COPY/FILL, hardware pointer), the 2D
engine with the f2-92 offscreen caches in the T23 configuration
(offscreen rows two thirds of the screen: windows cached, the background
not), and the f2-101d command batches at 32 and 16 bpp.

| # | Event (Windows 9x) | Repainted | CiukiOS path | Defect found (fixed) | Test row |
| --- | --- | --- | --- | --- | --- |
| 1 | Window created and shown: `CreateWindow`, `ShowWindow`, `WM_NCPAINT`, `WM_ERASEBKGND` with the class brush, then `WM_PAINT` | the new window's outer box (frame, shadow, client) over everything it covers; its task button; captions and focus indicators of the activation it takes | `client.c` `DESK_CREATE_WINDOW` (native, v3); libdeskui `window_open` | the client area was the zeroed (black) surface until the program's first frame; the desktop icon outline did not follow the activation | create |
| 2 | Client update: `InvalidateRect` + `WM_PAINT` clipped to the visible region | the reported rectangle, visible part only; with a pending resize the stretched image of that rectangle | `client.c` `DESK_DAMAGE`, `DESK_GFX_FRAME`, v3 `DESK_DAMAGE`/`PRESENT`; `compositor.c` `window_damage` | native and graphics-presenter updates during a pending resize (maximise, restore, size) were not mapped through the stretch: parts stayed stale until the program re-laid out; updates of a window over the top bar were dropped there | stretched, gfx, topbar |
| 3 | `ScrollWindowEx`: retained pixels moved by BitBlt, exposed strip invalidated | moved pixels by an engine screen copy only where they equal the program's pixels and nothing is painted above them | `compositor.c` `scroll_plan`, `copy_region` | the copy ignored what `scene()` paints above windows: the minimise/restore outline and the taskbar; a plain page under the outline lost (or carried) outline pixels on the engine presenters | scroll overlays |
| 4 | Window moved: `SetWindowPos` move ("show window contents while dragging", `SPI_SETDRAGFULLWINDOWS`) | old and new outer boxes; the window's visible pixels by BitBlt | `client.c` `desk_move`, `compositor.c` move copy | a window reaching under the taskbar moved taskbar pixels with it (RAM and screen copy) | move |
| 5 | Window sized, maximised, restored, shaded (`WM_SIZE`, `CS_HREDRAW`/`CS_VREDRAW`) | old and new outer boxes; the stretched backing until the program presents its new surface | `desk_resize`, `desk_zoom`, `desk_collapse`, `replace_client_surface` | (row 2's stretch defect) | minimise/restore, stretched |
| 6 | Z-order change: activation raise, `SetWindowPos` z, owned and topmost bands | the overlap of every pair whose relative order changed | `desk_stack_damage` (fx-2) | none new | activation, show/hide |
| 7 | Hidden and shown: `ShowWindow(SW_HIDE)`, `SW_SHOW`, activation of a hidden window | the old outer box exposed; the new outer box painted; the task button removed or added | `v3_place`, v3 `DESK_V3_ACTIVATE` | a Win32 window hidden by `SW_HIDE` left its task button drawn; a hidden window shown by activation while already the highest window was never painted | show/hide |
| 8 | Minimised and restored, with the caption animation (`DrawAnimatedRects`, `IDANI_CAPTION`) | outer box exposed or painted; task button state; the outline at each step | `desk_minimize`, `desk_animate`, `desk_animation_tick` | none new | minimise/restore |
| 9 | Close requested (`WM_CLOSE`: "Closing..."), declined, destroyed (`DestroyWindow`, owned windows destroyed with it) | caption and task button while closing and after a refusal; the outer box exposed at destruction | `desk_request_close`, `DESK_CLOSE_ACK`, `DESK_CLOSE`, `desk_drop_client` | the task button kept its title while the caption said "Closing..."; a refused close (native `DESK_CLOSE_ACK`) restored the caption state without any repaint | close |
| 10 | Process death, broken channel, lost Win32 server (`IsHungAppWindow` aside) | every window of the process exposed, task buttons | `desk_child_exited`, `desk_drop_client` (EPIPE, v3 peer) | none new | death |
| 11 | Activation: `WM_NCACTIVATE` caption tint, `WM_SETFOCUS`/`WM_KILLFOCUS` (caret destroyed and created, focus rectangle), the desktop list view's focus | captions and frame edges of both windows; focus indicators inside built-in windows; the selected desktop icon's focus outline | `desk_focus`, `focus_damage`, `desk_internal_paint`, `draw_icons` | the Run dialog's caret and focus ring stayed drawn after another window took the focus; the selected icon's outline appeared or stayed without a repaint | activation, create |
| 12 | Caption text and state: `WM_SETTEXT`, "Not responding" after 5 s (`IsHungAppWindow`), title buttons hot and pressed | caption and task button; the hot or pressed button only | `DESK_SET_TITLE`, ping timeout, `title_track`, `title_press` | (row 9: the close button acts on press) | caption |
| 13 | Menus: Start menu, submenus, context and window menus (`TrackPopupMenu`; popup classes with `CS_SAVEBITS` restore the saved image or the exposure repaints) | the menu's outer box when it opens, moves or closes; the old and new hot rows | `desk_menu_open`, `desk_menu_close`, `submenu`, `menu_highlight` | none new | menus |
| 14 | Tooltips | the tooltip box when it appears, disappears or follows its anchor | `desk_shell_tick`, `draw_tooltip` | a task-button tooltip kept its old box for one frame when the task buttons were re-laid out (window opened or closed) | tooltip |
| 15 | Tray popups, notification balloons (entry slide, expiry) | the popup's outer box per state | shell volume/network/media popups, `desk_shell_notify`, `desk_shell_tick` | none new | tray popups |
| 16 | Alt+Tab switcher | its box per step | `switcher_start`, `switcher_step`, `switcher_finish` | none new | switcher |
| 17 | Built-in dialogs (Run, About, messages, Shut Down, Programs) | their content when their state changes | `desk_internal_*` | About > System lists the number of desktop windows: it stayed stale when windows opened or closed | built-in windows |
| 18 | Application start feedback (`STARTUPINFO` `STARTF_FORCEONFEEDBACK`: ends when the process shows a window) | pointer shape only; a launched slot without a window paints nothing | `input.c` `desk_cursor_update`, `client_frame_ready` | a v3 program (NetSurf, Win32) was launched on its transport slot, whose first frame never came: AppStarting stayed 5 s and the launch never completed | create (and the latency record) |
| 19 | Desktop icons: list view insert, delete, selection | old and new icon boxes | `desk_links_refresh`, `icon_select`, `desk_shell_apply` | a shortcut refresh rebuilt and re-laid out the icons without repainting them, and dropped the selection | desktop icons |
| 20 | DOS full screen entered and left (the display returns, `RESYNC`) | the whole screen; scanout untrusted | `display_poll` (`desk_accel_reset`), `input.c` `RESYNC` | none new | full-screen return |
| 21 | DirectDraw exclusive mode (f5-08 display lease) granted and revoked | the whole screen after the revoke | `desk_v3_revoke`, `ciuki_win32_presenter_lease` | none new | display lease (software presenter) |
| 22 | Theme or display mode change | the whole screen | `desk_theme_changed`, `desk_display_relayout` | none new | theme |
| 23 | Any exposure repaint | a repainted strip equals a whole repaint | every painter's clip | text glyphs were culled by the pen position and the capital line, not by the glyph box: a strip ending inside a negative left bearing ('j', 'p') or above an ascender lost those pixels (`theme.c`, libdeskui `draw.c`) | exposure strips |
| 24 | Pointer motion and shape | none (hardware cursor) or the save-under footprint (software) | `desk_cursor_motion`, `pointer_rows` | none new (f2-96, f2-99 tests) | every row (pointer moves); `test_accel` |
| 25 | Desktop icon selection (fx-19): click, Ctrl and Shift clicks, keyboard (Ctrl+A, arrows, Shift+arrows, Ctrl+Space); the list view repaints each item whose selection or focus state changes (`LVN_ITEMCHANGED`) | the icon's box (selection tint, label halo, focus rectangle) | `shell.c` `mark`, `icon_focus`, `icon_repaint` | none (fx-15b audit) | selection |
| 26 | Rubber band (marquee): an XOR focus rectangle (`DrawFocusRect`) drawn and erased as the pointer moves | the band's old and new outline | `band_begin`, `band_update`, `band_end`, `draw_band` | fx-15b: a band ended by an icon-list rebuild (`icons_build`, run by a shortcut or folder refresh) vanished from the state without any repaint: its outline stayed on screen (the merge failure: `engine+cache ... fuzz run=0 step=49`) | selection |
| 27 | Drag image (`ImageList_DragMove`/`DragShowNolock` in the Windows 98 shell): the image follows the pointer above every window | the image's old and new box; screen copies are cut by it | `dnd_image_box`, `dnd_image_damage`, `draw_drag_image`, `copy_region`, `desk_shell_chrome_intersects` | none reachable found; the "as painted" record that the copy cut and the direct client path use is updated by the input path only, so fx-15b's reconcile now keeps it equal to the painter's box whatever moves the pointer | drag and drop |
| 28 | Drop-target highlight (`DROPEFFECT` feedback: the target item drawn selected while it would accept the drop) | the old and new target icon | `dnd_evaluate` (`drop_index`), `icon_repaint` | none | drag and drop |
| 29 | Taskbar hover during a drag (Windows 98: holding a dragged object over a task button for about a second activates that window) | the restored window (rows 7, 8, 11) | `desk_shell_tick` drag hover | none (covered by the window rows) | drag and drop |
| 30 | Right-drag menu, Esc and cancelled drags | the menu box (row 13); the image box when the drag ends | `desk_shell_drag_button`, `desk_shell_drag_cancel`, `dnd_finish` | none | drag and drop |
| 66 | G26: browser New Window (toolbar or Ctrl+N), download Save/cancel dialog, Internet Properties edits and Apply status | new window frame/client/task button, dialog box when shown/erased, edited widgets and status | v3 `DESK_LAUNCH` URL request uses the existing desktop launcher; browser `downloads_poll`/`event`; Control Panel `internet_event` | new transitions; no alternate drawing path | `row_network_tools` (`WM_ROW=29`) plus `test_user_tools`, Internet page host assertions and NetSurf chrome test |
| 67 | Disk maintenance (G29b): analysis, Settings, Show Details/legend, Start, Pause/Resume, Stop, write restart, completion/error, converter warnings, cancellation and result | Files client damage and exposed content after modal close | `apps/files/maintenance_ui.inc`, libdeskui property-sheet invalidation | none | disk maintenance (`maintenance_ui_scenario`, production controller on all five presenters) |
| 31 | Taskbar menu (G4, WM-12): `TrackPopupMenu` at the cursor on the empty taskbar or the empty top bar; its Cascade/Tile/Minimize All items enable with the windows, and Undo Cascade/Undo Tile/Undo Minimize All replace or follow them | the menu's outer box on open, close and each hot row | `desk_shell_button` (empty bar), `build` `SH_MENU_TASKBAR`, `menu_key`/`menu_letter` (access keys) | none | taskbar menu |
| 32 | `CascadeWindows`/`TileWindows` of the taskbar menu: `SetWindowPos` on every arranged window in one pass (`EndDeferWindowPos`); maximised and shaded windows are restored first | the old and new outer box of each window that moved or changed size; its client through the stretch of rows 2 and 5 | `desk_arrange`, `arrange_place`, `arrange_settle` (`client.c`) | none found; `desk_move` plans one screen copy for one moved window, so `arrange_prepare`/`arrange_settle` turn a pending copy into plain damage before and after the batch | arrange |
| 33 | Minimize All Windows, Show Desktop (Win+D toggles), Win+M, Shift+Win+M, Alt+M and their Undo: the windows go to the taskbar at once (no caption animation), the desktop becomes active | every window's outer box, its task button, the caption of the window that loses the activation | `desk_minimize_all`, `desk_show_desktop`, `desk_arrange_undo` | none | minimize all |
| 34 | Windows-key shortcuts and the taskbar's keyboard focus (Win+Tab, Ctrl+Esc then Esc, Tab/arrows, Enter, Esc): the focus rectangle on a task button or the lit Start orb | the old and the new button; the Start orb's bounds | `taskbar_set`, `taskbar_blur`, `logo_chord`, `desk_shell_reconcile` (flag 8, `start_focus_shown`) | none | windows keys |
| 35 | Caption icon (Ciuki Blue): a click opens the window menu under it, a second click within the double-click time closes the window (`WM_SYSCOMMAND` `SC_CLOSE`) | the menu's outer box; the caption and task button ("Closing...") | `desk_shell_caption_icon` (`shell.c`), `desk_caption_icon_box` (`compositor.c`) | none | caption icon |
| 67 | Startup programs (G21): background logon batch shows, activates and closes ordinary windows | existing CREATE_WINDOW, activation and close damage; no batch-wide repaint | startup worker → shared launcher → `client.c` | none new | startup programs |
| 37 | Window move while other windows repaint (fx-26): preserve valid client bits while repainting damage outside the carried rectangle | the exposed area, damage from the other window, and the moving window's damaged pixels | `desk_move`, `copy_region`, `window_damage` | unrelated damage disabled new move copies; damage on the shadow ring cancelled continuing copies | move with other damage (`WM_ROW=30`) |
| 38 | Native program fault dialog (G13, ER-1; Windows 98 "This program has performed an illegal operation" box): a message window with Close and a Details panel that opens and closes in place | the dialog's outer box, its task button, the Details text swap | `desk_w32_fault_dialog` (`client.c`), `fault_text.h`, `shell.c` message window | none (the Win32 crash dialog already used the same message window; this adds the native record) | native fault dialog |
| 39 | Screen saver idle/SC_SCREENSAVE/preview start and animation | full screen with pointer hidden; application backing stores remain untouched | `desk_saver_start`, `desk_saver_tick`, `saver_scene` | G10: dedicated overlay bypasses window copies and caches | saver |
| 40 | Saver input dismissal and monitor wake | full desktop, windows, overlays and pointer | `desk_saver_input`, `saver_wake`, full invalidation | G10: dismissing input consumed, failed wake remains retryable | saver |
| 41 | Monitor off after idle or SC_MONITORPOWER | no presentation while off; wake invalidates the complete scene (row 37) | `desk_saver_tick`, `monitor_power`, `desk_redraw` | G10: suspend drawing until firmware wake succeeds | saver |
| 42 | Find entry points: Start > Find submenu, desktop F3, Win+F; Files criteria pages, validation, results publication, sorting, Stop and navigation | menu boxes, normal window/client damage; Files invalidates its changed client state | `desk_shell_find`, Files `find_dialog`, `scan_poll`, `files_update` | G7 adds no compositor special case | `row_find` in `test_window_model`; `files_search_test.c` compares every client transition with a full redraw |
| 43 | G8 desktop menus: Undo, Paste Shortcut, New Bitmap/Shortcut/registered document, enabled Create Shortcut(s) Here | old and new menu boxes, clipped to the screen | `shell.c`, `desk_menu_open/close` | none | `test_window_model` `row_g8_menus`, all five presenters |
| 44 | G8 Copy To / Move To folder chooser, navigation, acceptance and cancellation; Undo progress and resulting file list | dialog plus shadow, changed list rows and selection | `du_folder_dialog`, `operations_ui.inc`, normal transfer publication | none | `test_window_model.test_g8_file_operation_dialogs_redraw`: production Files, partial versus full frame |
| 45 | G8 Open With, Always use, Browse and File Types edits (extension, description, icon, default action, open/edit/print, download confirmation) | old and new dialog boxes, controls, focus and validation text | `open_with`, `file_types`, libdeskui dialogs | custom dialog geometry explicitly damages its complete box after layout | same G8 dialog replay |
| 46 | G8 Send To and registered New menus; new/paste/create shortcuts, Bitmap and ShellNew publish new rows | popup boxes and file list after refresh | `operations_ui.inc`, `files_update`, `scan_poll`; desktop folder refresh reuses row 19 | none | same G8 dialog replay; G8 model tests check resulting files |
| 47 | G12 input settings: large/inverted scheme, trail movement and expiry, language indicator and left-handed buttons | previous and new trail footprints; full scene on scheme/layout changes, including hardware pointer hide/restore | `desk_preferences_changed`, `desk_input`, `desk_shell_tick`, `cursor`, `pointer_rows` | preference cursor modes must hide the hardware sprite before software drawing | `row_input_preferences` in `test_window_model` (all five presenters) |
| 48 | G12 native edit/textarea and shell Run/rename caret blink, composed character insertion, configured wheel line/page scrolling | client caret/widget invalidation becomes ordinary client damage | `du_tick`, `desk_shell_tick`, `du_input`, `du_widget_wheel` | keep selection focus while only the caret visibility changes | `row_input_text` in `test_window_model` |
| 49 | G17 DOS toolbar initial frame and pressed buttons | toolbar; first frame also includes the scaled guest viewport | `dosbox/window.h` `db_paint_rows`, client `DESK_DAMAGE` | guest surface kept separate from window controls | DOS toolbar/properties/selection |
| 50 | G17 Mark/QuickEdit begin, rectangle grows, Copy/Enter/Esc clears it | old/new selected cells (client invalidates the viewport); notices | `db_select`, `db_mark`, `db_copy`, `db_end_mark` | none | DOS toolbar/properties/selection |
| 51 | G17 Properties opens, changes tab/value, Apply/OK/Cancel closes | property sheet and exposed viewport | `db_properties`, `db_page`, `db_input`, `db_apply` | none | DOS toolbar/properties/selection |
| 52 | G17 Font size, Background label, clipboard/error notice and viewport scroll; incremental guest frame | toolbar/viewport for control changes; only scaled destination rows sampling changed VGA rows for video | `db_source_x/y`, `db_paint_rows`, `dosbox.c` damage mapping | hidden property-sheet backing is not uploaded over the sheet | DOS toolbar/properties/selection |
| 53 | G17 font-size dropdown opens, changes highlighted choice, selects or cancels | list box and exposed guest rows | `db_input` font menu, `db_paint_rows` | guest updates do not overwrite the open list | DOS toolbar/properties/selection |
| 54 | G22 volume-tray double-click: close the single-click popup and launch Volume Control | old volume popup and the newly created window (rows 1, 6) | `shell.c` double-click before popup outside-click dismissal | popup footprint explicitly invalidated | tray popups |
| 55 | G31 Printers folder/Add Printer, default selection and queue refresh (queued, paused, resumed, cancelled or purged); Print and Page Setup opening, changing fields, cancelling or accepting | dialog/client damage; old dialog area exposed on close; covered queue damage retained until exposure | `apps/printers/main.c`, `apps/libprint/dialog.c`, libdeskui `du_invalidate`/`DAMAGE` | completion notices wait while another modal dialog (such as an unsaved-document prompt) is open | printing |
| 56 | G32 logon / user selection / masked password / checking / failure / Cancel / password lock and unlock | whole screen for the opaque gate and its removal; application input is consumed before routing | `desk_logon_*`, `desk_session_lock`, `input.c`, `desk_shell_chrome_intersects` | none in the host model | `row_logon` in `test_window_model` (all five presenters) |
| 57 | Regional Settings (G24): region preset selected | client content; updates clipped by overlapping windows | `apps/control/regional.inc`, libdeskui invalidation, `DESK_DAMAGE` | none | regional settings (WM_ROW=40): Italian preset; applet semantics in `regional_control_test.h` |
| 58 | Regional Settings: tab changed | client content | `regional_event`, `regional_build`, `DESK_DAMAGE` | none | regional settings: tab change |
| 59 | Regional Settings: format field edited | changed field and validation status | toolkit edits, `regional_pull`, `DESK_DAMAGE` | none | regional settings: field edit |
| 60 | Regional Settings: Apply started | save status | `regional_save`, `DESK_DAMAGE` | none | regional settings: Apply status |
| 61 | Regional Settings: save completed or failed | completion status and error dialog | `cp_settings_saved`, `DESK_DAMAGE` | none | regional settings: save failure; real applet retry/OK tested by `regional_control_test.h` |
| 62 | Disk tools (G29): Cleanup scan, category selection, permanent-delete confirmation, progress, cancellation/completion, Format options and confirmation, busy/error report, low-space warning and its Cleanup action | the Files client's damaged region; exposed client content when its modal dialog closes | `apps/files/disk_ui.inc`, libdeskui dialog invalidation and native `DESK_DAMAGE` | none | disk tools (`disk_ui_scenario.h`, production controller on all five presenters) |
| 63 | Win32 FlashWindow (G18): TRUE alternates caption and task button appearance without activation; FALSE restores it and returns the prior drawn-active state | the caption and task button; activation remains unchanged | authenticated `DESK_V3_FLASH`, `client.c` and `compositor.c` reconcile | none | G18 flash |
| 64 | Win32 system tray integration (G18): add/modify/delete icons, tooltip, queued balloons with timeout/cancel and owner cleanup; Win32 file drops use immutable snapshots | tray layout, tooltip, balloon bounds and the normal drop target | `shell_tray.h`, v3 client, shell drag target | snapshot ownership and explicit damage cover tray and drop changes | G18 tray and shell integration |

| 68 | Context-help caption button (G15; `WS_EX_CONTEXTHELP`, `DS_CONTEXTHELP`): the "?" takes the minimise slot, a window with it has no minimise or zoom, it is hot and pressed like every title button, and a press sends `WM_SYSCOMMAND` `SC_CONTEXTHELP` to the program (v3 `HELP`) | the caption when the style appears or goes away (the whole title bar: the buttons changed); the "?" box while hot or pressed | `compositor.c` `title_box`, `frame_style`, `desk_title_code`; `input.c` press; `client.c` `desk_context_help`; v3 `PLACE`/`CREATE` style bit `DESK_V3_CONTEXTHELP` | none | caption |
| 69 | Power meter (G11, `power-policy.md`): the battery icon appears with the first record, changes with each reading, is hidden or shown by "Show power meter on taskbar"; its tooltip follows the readings; the scheme menu; a battery alarm's message window | the tray (re-laid out when the icon count changes), the old and new tooltip box, the menu box (row 13), the message window (row 17) | `desk_shell_power_update`, `desk_shell_power_config`, `power_tooltip_damage`, `desk_shell_tray_damage`, `SH_MENU_POWER`, `power_alarm` | none | power |
| 31 | Resource Meter tray icon (G30; Windows 98 `Shell_NotifyIcon` of rsrcmtr.exe): appears with the running Resource Meter window, changes its level bar, tooltip with the three percentages, click opens the window, removed when the program ends | the tray box and everything right of the changed icon (the clock and icons shift by one slot), then only the icon box; the tooltip box | `desk_shell_resources_update`, `draw_tray`, `desk_shell_tray_damage`, `hover_target` 6 | none | resource meter tray icon |
| 70 | NetSurf system-clipboard paste and completion of a sampled large raster (FX-24B) | changed content rectangles, clipped to the browser client; overlapping windows remain intact | existing `DESK_DAMAGE`, `ciuki_browser_damage`, `ciuki_browser_present` | no new protocol event; browser changes use the normal damage path | browser clipboard and raster |
| 71 | DOS clipboard settings (G5b): QuickEdit and Exclusive mode commands in the DOS window menu | menu outer box on open, hot row and dismissal; normal client damage when a selection starts/ends | `shell.c` DOS menu, `dos_clip_action`, shared `clipboard_settings.h` | none new; settings are persisted before applying them | DOS clipboard settings (appended after saver) |
| 72 | CF_OWNERDISPLAY viewer size/paint/scroll and owner replacement (G5b) | viewer client damage through the ordinary native/Win32 backing surface; old viewer box when closed | `clipbrd_main.c` preview, USER `clipboard_display.c`, normal DAMAGE/PRESENT | handles/HDC never cross processes; owner paints a bounded local surface whose DIB is returned to the viewer | clipboard owner display (appended after DOS clipboard settings) |
| 73 | G14 Safe mode and Command prompt only: Windows 98 writes "Safe mode" in the four corners of the desktop; the command-prompt start shows no desktop icons until the Terminal's WIN | the whole background once (labels drawn with the icons into the background cache); afterwards a window over a label exposes it like any background | `recovery_view.c` `desk_recovery_paint` (from `draw_icons`), `desk_recovery_win`; `shell.c` `draw_icons` skips icons while `terminal_shell` | none | safe mode and command prompt |
| 74 | G11B Stand by: notice for running DOS sessions | the notice rectangle | `standby_service`, `desk_power_policy_tick` | none | power suspend/resume: DOS notice |
| 75 | G11B resume (including a failed firmware request) | entire desktop because firmware may have changed the scanout | `standby_service`, `desk_power_policy_tick` | resume previously retained stale scanout pixels | power suspend/resume: resume |
| 76 | G29c Copy Disk source prompt, read progress, destination choice and swap/destruction confirmation; write/read-back progress, completion, source-still-inserted and I/O errors | dialog boxes and client content only | Files `disk_ui.inc` worker publication and `du_invalidate` | none new | copy disk (`WM_ROW=52`) |
| 77 | G29c Backup full/differential/verify/restore job options, overwrite choices, confirmation, progress, completion and checksum/I/O error | dialog boxes and client content | Files `disk_ui.inc`, worker progress and completion invalidation | none new | backup (`WM_ROW=53`) |
| 78 | G29c unformatted removable media appear in Computer; opening offers Format | drive list, Format dialog and exposed client area | retained media slots, navigation failure and format query worker | none new | blank media (`WM_ROW=54`) |
| 79 | G29c Terminal FDISK large-disk/destructive prompts, partition information/result/refusal; BACKUP/RESTORE command output | existing Terminal content invalidation and client DAMAGE | `term_execute` grid invalidation, desktop client damage; command semantics tested separately | no Terminal layout or protocol change | disk terminal (`WM_ROW=55`) |
| 80 | G22B playback source volume, balance and mute | changed Volume Control client controls; occluded bits remain in the backing store | `mixer_view.c`, toolkit damage → `DESK_DAMAGE` | none new; source columns now report their changed controls | multimedia controls (`WM_ROW=33`) |
| 81 | G22B playback/recording view and recording source selection | swapped control captions, availability, sliders and Select states | `mixer_event`, `lines_show` | mode button sits below the menu; master disabled in Recording | multimedia controls |
| 82 | G22B CD database load/save/refresh and track selection | CD client list, artist/title and track edit fields | `cdplayer.c` USER/GDI client damage | none new; CD model tests cover identity and persistence | multimedia controls |
| 83 | G22B Multimedia CD-volume slider | changed native Control Panel client slider/status | `control/app.c`, toolkit damage | none new | multimedia controls |

A window drag shows the window contents, as Windows 98 does by default; the
caption animation of row 8 is the only window outline. Rows 25-30 came with
fx-19 (drag and drop, rubber-band and keyboard selection) and were added by
fx-15b after the merge, with the reconcile extended to the band and the
drag image. Rows 31-35 came with G4 (`docs/design/taskbar-arrangement-windows-keys.md`),
row 36 with G21, row 37 with fx-26, row 38 with G13 (`docs/directives/g13-g16-os-gaps-batch-3.md`)
and rows 39-41 with G10 (`docs/directives/g7-g12-os-gaps-batch-2.md`).
Rows 42–46 cover G7 and G8; G12 occupies executable rows 35–36, G17 row 37,
printing row 38, G32 logon row 39, G24 Regional Settings row 40 and G29 Disk
tools row 41. G5b appends inventory rows 71–72 for DOS clipboard settings and
the CF_OWNERDISPLAY viewer without shifting those existing executable rows.
The randomized sequences gained operations 42–47 (taskbar menu, Win+D/M and
Alt+M, taskbar focus keys, Alt+F6/Menu key/Shift+F10/F3, caption icon,
Win+E/R/F/Break/F1). Row 37 adds fx-26's drag with concurrent damage.
The executable row table groups several inventory events into one scenario:
main's indexes 0–32 stay unchanged, including G4 at `WM_ROW=24` through
`WM_ROW=28`, startup at 29, fx-26 at 30, fault at 31 and saver at 32.
G7 appends executable scenario `row_find` at `WM_ROW=33`; G8 uses
`row_g8_menus` at `WM_ROW=34`; G12 follows at `WM_ROW=35` and `WM_ROW=36`.
G17 uses `WM_ROW=37`, printing 38, G32 logon 39, G24 40 and G29 41.
G5b uses executable rows 48–49; G11B uses row 50 and G29b uses row 51. G29c appends Copy Disk, Backup, blank-media and Terminal scenarios at rows 52–55. Existing main indexes 0–32 remain stable.

### One rule instead of one fix per path: the frame reconcile

Rows 1, 7, 9, 11, 12, 14 and 17 were paths that changed state without
adding the damage the model requires. `desk_scene_reconcile`
(`compositor.c`, with `desk_shell_reconcile` in `shell.c`) compares, before
every frame decision of the service loop and in `desk_redraw`, what the
last composed frame showed with the current state, the way
`EndDeferWindowPos` applies a batch of position changes: a window that
appeared, disappeared (closed, crashed, minimised, hidden) or changed its
outer box invalidates its old and new outer boxes (a move the compositor is
copying is described by its move damage); a caption whose text, icon,
activation, alert, shade, zoom or buttons changed is repainted with the
frame edges (and the client area of a built-in window, whose focus
indicators follow the activation); a hovered or pressed title button
repaints only itself; a task button whose box, title, icon, activation,
hover or minimised state changed is repainted (the shell hook's
`HSHELL_WINDOWCREATED`/`DESTROYED`/`ACTIVATED`/`REDRAW`); a popup (menu,
tray popup, notification, switcher, tooltip) that opened, closed or moved
exposes its old box and paints its new one; the selected icon's focus
outline follows the desktop's activation; the rubber band repaints its old
and new outline, and the drag image its old and new box, whatever path
changed them (fx-15b). Explicit damage at each path
stays (content changes are only known there); the reconcile guarantees that
a path which forgets it can no longer leave anything drawn. Its cost is a
comparison of sixteen window records and sixteen task buttons per service
turn, without pixel work.

### The presenter paths (software and Savage)

Every screen write is still a present of the composed RAM scene or an
engine operation proven against it; the test harness asserts at every
engine COPY and FILL that the RAM scene equals a full composite (the T23
configuration included: offscreen window caches, hardware pointer). The
engine-only defects were rows 3 and 4: a screen copy is now cut by
everything `scene()` paints above windows (taskbar, the caption-animation
outline, shell popups with their shadows, the Start orb, notifications), at
both its source and its destination.

### First frame and launch latency

libdeskui erases a new window's surface with the theme's face colour before
`CREATE_WINDOW` (rule 1: `WM_ERASEBKGND` with the class brush), so the
window appears at once with its frame and an erased client area, never
black, while the program initialises. NetSurf opens its two body faces on a
worker from the first moment of start-up (the trial's first layout spent
617 ms in two font opens). A quiet-boot record `[desktop] first-frame
name=... process_ms window_ms paint_ms present_ms total_ms` gives, for every
launched program, click to process, process to window, window to first
frame and first frame to screen; the trial records had only the spawn.

Sources:

* Microsoft SDK, [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos):
  `SWP_NOREDRAW` "If this flag is set, no repainting of any kind occurs ...
  any part of the parent window uncovered as a result of the window being
  moved"; `SWP_NOCOPYBITS` discards the client area instead of copying the
  valid contents back; topmost and owned window rules.
* Microsoft SDK, [EndDeferWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enddeferwindowpos):
  "Simultaneously updates the position and size of one or more windows in a
  single screen-refreshing cycle."
* Microsoft SDK, [WM_ERASEBKGND](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-erasebkgnd):
  sent "to prepare an invalidated portion of a window for painting";
  `DefWindowProc` erases with the class background brush.
* Microsoft SDK, [WM_PAINT](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-paint),
  [Invalidating and Validating the Update Region](https://learn.microsoft.com/en-us/windows/win32/gdi/invalidating-and-validating-the-update-region),
  [Redrawing in the Update Region](https://learn.microsoft.com/en-us/windows/win32/gdi/redrawing-in-the-update-region),
  [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex).
* Microsoft SDK, [Window Class Styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-class-styles):
  `CS_SAVEBITS` saves the screen image under small, short-lived windows
  (menus, dialog boxes) and restores it when they are removed, unless other
  screen actions invalidated the stored image; `CS_HREDRAW`/`CS_VREDRAW`.
* Microsoft SDK, [DrawAnimatedRects](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-drawanimatedrects):
  `IDANI_CAPTION`, the caption animation of minimise and maximise.
* Microsoft SDK, [About Carets](https://learn.microsoft.com/en-us/windows/win32/menurc/about-carets):
  "A window should create a caret only when it has the keyboard focus or is
  active. The window should destroy the caret before losing the keyboard
  focus or becoming inactive."
* Microsoft SDK, [RegisterShellHookWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registershellhookwindow):
  `HSHELL_WINDOWCREATED`, `HSHELL_WINDOWDESTROYED`,
  `HSHELL_WINDOWACTIVATED`, `HSHELL_REDRAW`: the notifications on which the
  taskbar repaints its buttons.
* Microsoft SDK, [IsHungAppWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow):
  not responding after the internal timeout of 5 seconds.
* Microsoft SDK, [STARTUPINFO](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfoa):
  start-up feedback ends when the process shows a window.
* Windows 98 Resource Kit, main checkout read-only `build/rskt/wrkc28_8.htm`
  (Core System Components): User manages output to the user interface
  (windows, icons, menus) and keeps the window management code 16-bit; GDI's
  DIB engine draws, display minidrivers accelerate.

## fx-26: events on the next frame, window operations at the frame rate (2026-10-23)

T23 trial of preview `f441c5bd` (source `18dcfcb7`): the owner found that
notifications and the volume popup always lag and that window management
still feels slow. Records read (serial capture and `LOGS/BOOT.LOG`):

* Volume keys. `[ec] thinkpad poll_ms=2000 source=default`, one
  `[ec] thinkpad io=7..9 transactions=1` per 2 s window, and in the same
  windows `[kernel] smi ... gaps=1` with `smi_coincide ... ec_near=1`: one
  2,094-2,121 us SMI per RD_EC (two in 5a10eef4). Across the whole session
  every SMI gap lies within 1 ms of our own EC access; the keys raise no
  SMI of their own that the tick sampler sees. A press therefore reached
  `audio.master` only at the next 2 s RD_EC, and the desktop read the master
  every 250 ms (`mixer_poll`): key to popup/notification 0-2.25 s plus a
  frame. The tray's own changes (slider, multimedia keys) were immediate.
* Notifications. Volume as above. Media (USB stick, CD, floppy) and network
  state were read on the shell's 2 s tick (`want_reload` 16|32), the media
  query on its worker: up to 2 s plus the query.
* Frames while interacting: `[desktop] accel ... frames=` 41-103 per 2 s
  window (20-52 fps, never the 120 of a 60 Hz drag). In most of those
  windows `[cpu] idle_pm` was 200-800 with `desktop#34` at 100-340 per
  mille: the desktop was not CPU-bound; in the others (NetSurf laying out,
  a DOS VM running) idle was 0.
  `[kernel] stretch_max_ms`/`resched_lat_max_ms` 3 for the desktop, 9 with a
  DOS VM running (`resched_task=desktop`). With DOS Box busy (fx-23) the
  desktop group held `desktop_share group_pm` 95-127 and drew 2-3 frames
  per 2 s; Quake in a window drew 5-21 frames per 2 s at up to 127 engine
  copies per frame (both fx-23/fx-27 subjects).

### Windows 98 reference behaviour

* A mixer driver notifies every client that opened it with a callback
  window: [MM_MIXM_CONTROL_CHANGE](https://learn.microsoft.com/en-us/windows/win32/multimedia/mm-mixm-control-change)
  "is sent by a mixer device to notify an application that the state of a
  control ... has changed. The application should refresh its display";
  a volume display opens the mixer with a callback window and follows the
  change when it happens, instead of polling the control.
  (Learn lists Windows 2000 as the minimum for every pre-2000 API; the
  mixer API shipped in Windows 95's MMSYSTEM.)
* The ThinkPad volume buttons have no scancode: the firmware drives the
  console amplifier itself ([Linux thinkpad-acpi, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/Documentation/admin-guide/laptops/thinkpad-acpi.rst):
  "the firmware also handles volume control for the headphone and speakers
  on these ThinkPads"; on models whose keys raise no ACPI event the driver
  polls "the CMOS NVRAM at least 10 times per second", "the volume up/down/
  mute keys are synthesized according to changes in the mixer").
  [thinkpad_acpi.c v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/platform/x86/thinkpad_acpi.c):
  `TP_NVRAM_ADDR_MIXER` 60h, level bits 0-3, mute bit 6, bit 7 toggled by
  every volume key (`TP_NVRAM_MASK_HKT_VOLUME`); `hotkey_poll_freq = 10`;
  the default `TPACPI_VOL_MODE_ECNVRAM` keeps this byte and EC register 30h
  in the same layout; the T23 is an IBM `\CMOS` model there. IBM's own
  Windows 98 on-screen display is a vendor utility; the operating-system
  part is the mixer notification above.
* Device arrival and removal are broadcast as
  [WM_DEVICECHANGE](https://learn.microsoft.com/en-us/windows/win32/devio/wm-devicechange)
  (`DBT_DEVICEARRIVAL` "a device or piece of media has been inserted and is
  now available", `DBT_DEVICEREMOVECOMPLETE`) when they happen, not found
  by a periodic scan of the shell.
* A moved window keeps its bits:
  [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
  copies the valid client area unless `SWP_NOCOPYBITS`, and only the
  invalid parts are repainted (WM_PAINT). Other windows' updates do not
  stop the copy. Windows 98 has no compositor frame clock: User and GDI
  draw a move as soon as it happens; nothing waits for a refresh phase.

### What changed

| Event | Before fx-26 | fx-26 | Host evidence |
| --- | --- | --- | --- |
| ThinkPad volume key -> `audio.master` | next 2 s RD_EC: 1,992 ms worst | NVRAM byte 60h read every 100 ms (two CMOS port accesses, no EC transaction); a change triggers one RD_EC at once: <= 100 ms at every phase of the 2 s poll. After a mirrored change the EC is read every 250 ms for 3 s, so presses the NVRAM does not show still follow within 250 ms (counted `missed`). Key-driven reads at most one per 100 ms | `thinkpad_ec_test.inc` `test_thinkpad_ec_keys` |
| `audio.master` -> popup and notification frame | 250 ms observation: 4-249 ms, 249 ms with other updates running | `/dev/mixer` is pollable: POLLIN until the description has read the current master (fingerprint of the whole record); every master change wakes `poll()`; the desktop polls its mixer description and reads it on that turn: 0 us idle, <= 9 ms with frames running (the next frame) | `event_latency_test.h` (`test_event_latency`), `audio_test.c` readiness |
| Idle cost of the volume mirror | 41 mixer queries and 42 loop turns per 10 s | 0 queries, 12 turns (one deadline left: the quiet-period save of a key-applied master) | `event_latency_test.h` |
| Drive published or withdrawn -> media notification | shell 2 s tick, then the worker query | `/dev/media` is pollable: every `generation[d]` change in `vfs.c`/`mount.c` bumps one serial and wakes `poll()`; the loop polls the media worker's own description while the worker is idle and starts the query on that turn (the 2 s tick stays for CD and floppy changes, which no path publishes) | `test_storage_namespace.c` readiness |
| Drag while another window draws | any pending damage disabled a new move copy; damage touching the moved window's shadow ring cancelled a continuing copy: an 800x560 drag over a playing DOS window recomposed both outer boxes in software, 34-71 ms frames | a new copy starts unless damage lies inside the window's own carried rectangle; a continuing copy is cancelled only by damage inside the carried rectangle at the intermediate position (the compositor already cuts every damage box and its source image out of the copy) | `t23_cpu_model_test.c` TIMED drag rows, `window_model_test.c` row `move with other damage` |
| Frame after an overrun | a frame that ran past the next 60 Hz boundary made the next one wait for the boundary after (a lost slot: a frame started 8 ms late with 10 ms of work left 15 ms idle) | the next frame starts one slot after this one started, or when it ended if later (never later than before); frames that end before the boundary keep the absolute phase | `compositor_rows_test.c`, TIMED rows |

T23 timed model (`t23_cpu_model_test.c`, the f2-95 rates; the virtual clock
advances by each frame's modeled cost; the desktop's wake-ups late by up to
the trial's 0/4/9 ms):

| Drag | wake late | HEAD fps | HEAD motion->frame mean/max ms | fx-26 fps | fx-26 mean/max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 800x560 over a DOS window at 70 Hz | 0 | 65.8 | 15.8 / 76.0 | 68.2 | 14.5 / 21.0 |
| 800x560 over a DOS window at 70 Hz | 4 | 51.1 | 56.5 / 82.7 | 68.2 | 14.5 / 23.4 |
| 800x560 over a DOS window at 70 Hz | 9 | 39.6 | 60.8 / 81.9 | 68.2 | 15.1 / 27.6 |
| 560x420, software presenter | 9 | 54.7 | 23.0 / 39.9 | 57.0 | 22.7 / 32.4 |
| 560x420, 800x560, 1000x680 alone (engine) | 0-9 | 59.9-60.0 | <= 28.3 | 59.9-60.0 | <= 28.0 |

The DOS rows reproduce the trial's 25-47 fps drags with the CPU idle.
Minimise, restore and maximise of an 800x560 window over a playing DOS
window: operation to first frame 21-25 ms mean, 45-51 ms worst, the first
frame itself 17.6 ms (exposed area composed and sent to the LFB), the
150 ms caption animation in 5-6 frames; unchanged by fx-26 (gap below).

Interfaces: `MIXER_GET_MASTER`, `MIXER_REFRESH_MASTER` and a successful
`MIXER_SET_MASTER` record what the description has seen; `MEDIA_GET_STATE`
records the media serial before it builds the state
([execution ABI](execution-abi.md), fx-26 amendment). A kernel that answers
POLLNVAL for these descriptions leaves the desktop on its former 250 ms
mixer observation (`mixer_events=0`).

Records for the next trial: `[ec] thinkpad keys watch_ms=100 source=nvram
active_ms=250 hold_ms=3000 trigger_ms=100` once at start, and in windows
with key activity `[ec] thinkpad keys nvram_polls=<n> nvram_changes=<n>
triggered=<n> active_reads=<n> missed=<n> nvram=<byte> ec=<byte>`.

| Observation | Meaning |
| --- | --- |
| `nvram_changes` and `triggered` grow with each press, `missed=0` | the T23 firmware keeps byte 60h: keys on screen within 100 ms plus a frame |
| `missed` equals the presses, `nvram_changes=0` | byte 60h does not follow the keys in legacy mode: the first press of a burst waits for the 2 s poll, the next ones follow at 250 ms |
| `smi` gaps with `ec_near` below `gaps` at about 10/s | CMOS reads would be trapped (none armed per f1-100's `smi_config`): report |

Gaps, explicit:

* Network state notifications stay on the 2 s shell tick (Windows 98 shows
  none; `/dev/net` is not pollable).
* CD and floppy media changes still need the 2 s tick (no kernel path
  publishes them before the query).
* The first frame of a minimise/restore/maximise of a large window costs
  about one frame period on the T23 (composition plus LFB upload of the
  exposed area); serving exposed windows from the offscreen caches is not
  done here.
* DOS Box busy loops and Quake's windowed presentation (frames per 2 s in
  single digits) are fx-23 and fx-27.

### fx-26 merge compatibility validation

The merge preserves both the namespace serial/readiness added by fx-26 and
main's G6 volume-label controls and fx-21b USB-unrecognised count. The serial
is sampled before the media snapshot; only the current media version reports
the USB count, leaving the v1 reserved word zero. This keeps the event-driven
arrival/removal semantics of
[WM_DEVICECHANGE](https://learn.microsoft.com/en-us/windows/win32/devio/wm-devicechange)
and the separate mixer-display refresh semantics of
[MM_MIXM_CONTROL_CHANGE](https://learn.microsoft.com/en-us/windows/win32/multimedia/mm-mixm-control-change).
These references were rechecked against the merged implementation; no public
ABI numbers change. G4's scenario indexes and inventory rows stay fixed,
with fx-26 appended after them as described above.
