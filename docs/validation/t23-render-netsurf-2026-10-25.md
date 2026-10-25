# t23-render — pages never shown in NetSurf, slow rendering under load

Written 2026-10-25. Evidence: real-hardware trial of image `c42f556e…`
(built from `main` a347ca76b; Pentium III-M 1.13 GHz laptop, S3
SuperSavage IX at 1024x768x32 with the Savage 2D engine, e100 network
card), private, never committed: `legacy/local/physical/c42f556e/disk/LOGS/`
(`BOOT.LOG`, 6,602 lines; `NETSURF.LOG`, one exit record). Previous trial
for comparison: `legacy/local/physical/2ca3b0b6/`. Owner's report: NetSurf
opened (slowly) but showed no web page; rendering in general is still slow.
Site names and full URLs from the trials are not reproduced here: an
"encyclopedia main page" and a "search home page" stand for them, and
quoted records have their URLs shortened to `<...>`.

## 1. Evidence

### NetSurf session (BOOT.LOG lines 4842-6449, NETSURF.LOG)

```
[user 37] [desktop] first-frame name=NetSurf - CiukiOS pid=20 process_ms=1024 window_ms=2537 paint_ms=793 present_ms=186 total_ms=4540
[console] [netsurf] layout work elapsed_us=171000 class=local pass=1 font_opens=1 font_open_us=142000 ...
[netsurf] exit reason=desktop-close-request detail=0 layout_rejections=0 layout_max_us=171000 local_max_us=171000 remote_max_us=0 ... presents=22 damage_messages=31 present_refused=0 connect_retries=0 fetches=49 fetch_ok=49 fetch_errors=0 last_error=none network=dns-ok
```

Only the welcome page was ever laid out (`remote_max_us=0`, a single
`layout work` record, local). All 49 fetches succeeded. Timeline of the
fetch records (`utc=` and `ms=` fields; the session's clock was correct,
`rtc=1`):

| Time (UTC) | Event |
| --- | --- |
| 20:10:21 | click on the encyclopedia link of the welcome page (`input target=page ... type=4`) |
| +1.1 s | main page response 200 (TLS handshake done at 1,020 ms) |
| +1-2 s | fetches 2-26 created while the HTML still arrives: 3 stylesheet/script resources, 4 header images, 18 thumbnails (ids 9-26) |
| +6.6 s | main page done, 256,912 bytes (`ms=6577`) |
| +7.3 / +7.7 s | stylesheet 196,947 bytes and script 70,658 bytes done |
| +6 to +12 s | the 18 thumbnails done (ids 9-26) |
| +13 to +18 s | the same 18 thumbnail URLs fetched again (ids 29-46), one after the other, each request issued just after the previous one completed |
| +19 s | click on the search link of the welcome page, still the page shown |
| +19 to +24 s | search page: two 301 redirects, then 200, 71,199 bytes (`ms=2869`) |
| 20:10:45 | window closed (`desktop-close-request`) in the same second |

```
[console] [netsurf] fetch url=<thumbnail 1> stage=done code=200 ... bytes=29662 ms=4653 id=9 ...
[console] [netsurf] fetch url=<thumbnail 1> stage=done code=200 ... bytes=29662 ms=593 id=29 ...
```

### CPU (BOOT.LOG `[cpu]` windows of 2 s, permille of the CPU)

| Line | idle | device-work#9 | desktop#37 | other top tasks |
| ---: | ---: | ---: | ---: | --- |
| 1795 | 24 | 41 | 282 | startup |
| 2595 | 649 | 53 | 110 | — |
| 3596 | 10 | 165 | 155 | Q3ADemo 213+77, w32srv 68 |
| 4091 | 8 | 274 | 64 | Q3ADemo 391, w32srv 119 |
| 4130 | 5 | 297 | 77 | Q3ADemo 354, w32srv 109 |
| 4425 | 0 | 342 | 336 | Q3ADemo 95+26 |
| 4924 | 0 | 329 | 159 | netsurf#87 207, Q3ADemo 43 |

Between lines 2595 and 3596 the owner started the Quake III Arena Demo
installer four times (`[win32] start pid=11/13/15/17 exe=.../Q3ADemo.exe`,
168.7-180.9 s); their threads appear in the `[cpu]` records until the
NetSurf session. From then on the device-work thread grew from 41-53 to 274-342
permille, with `[sched] device_overrun task=device-work#9 count=1169-1198`
per window and these reports:

```
[work] budget exceeded site=c01485f0 us=1082 pending=3 violations=8
[work] budget exceeded site=c01485f0 us=1008 pending=3 violations=9
[work] budget exceeded site=c01485f0 us=1010 pending=2 violations=10
```

`c01485f0` is `shared_tick_work` (`src/kernel/arch/pic_pit.c`; `nm` of the
kernel built from the same kernel sources, which are unchanged between
a347ca76b and 50a843517). In the window of the encyclopedia download
(line 4924) the CPU was saturated: device-work 329, NetSurf 207, desktop 159
permille, and `[sched] desktop_share group_pm=218 turns=2038 lent_turns=50
turn_ms=250`: the desktop ran 250 ms of its 437 ms on its guaranteed
minimum share (f1-98), i.e. it was starved.

### Desktop rendering records

All `[console] [desktop] accel` records of the session have `fallback=0`
(no software fallback of the Savage engine); frames per 2 s window 4-63.
During the NetSurf session: `fills=18 copies=34 writes=6 fallback=0
frames=42 lfb_bytes=1098260 pointer=hw`. The boot log keeps no per-frame
composition record (`frame_p95_us` is a verbose console record and never
reaches BOOT.LOG), and the 2 s `[cpu]`/`[desktop]` statistics were partly
dropped by the boot-log budget (`budget_dropped ... kinds=cpu/task:57-75`),
so the trial gives the desktop's CPU share, not its per-frame cost.

## 2. Causes

### 2.1 Every Win32 process cost the whole system up to a third of the CPU

All Win32 processes map one and the same section page at `0x7FFE0000`
(`sections.c` `w32_shared_map`, one kind-4 section, the CiukiOS counterpart
of Windows NT's `KUSER_SHARED_DATA`, which carries tick count, interrupt
time and system time for every process,
<https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-kuser_shared_data>).
The PIT top half queues `shared_tick_work` every millisecond; it called
`w32_shared_update` once per live Win32 process, and each call made one
`ua_range`, one `ua_read` and three `ua_write` calls on that page. Each of
them resolves the address with `ua_find`, a linear walk of the process's
extent list from its lowest address; `0x7FFE0000` is above every other
extent, so every call walks all of them: five full walks per Win32 process
per millisecond, on the device-class worker that runs ahead of every
application and of the desktop. The host test below counts 7,650 walk steps
per tick for a 1,500-reservation process and a small one (5 x 1,530); the
trial's four installers and their DLLs, heaps and mapped archives made one
tick take 1.0-1.08 ms, so the worker ran continuously within its device
budget (274-342 permille, about 1,200 budget overruns per window) and
everything else, the desktop and NetSurf included, shared what was left.
The number of extents of the trial's processes is not recorded; the growth
of device-work over minutes with the same four processes is what more
extents (mapped archive views, heap growth) would give.

### 2.2 NetSurf needed about 270 UI turns before it could lay out a large page

The Ciuki framebuffer main loop runs one scheduler callback per UI turn
(`frontends/framebuffer/schedule.c`, patch 0001: "one callback, then drain
desktop input"); upstream NetSurf's box construction converts 10 elements
per callback and reschedules itself
(`content/handlers/html/box_construct.c` `convert_xml_to_box`,
`max_processed_before_yield = 10`, unchanged on upstream master,
<https://github.com/netsurf-browser/netsurf/blob/master/content/handlers/html/box_construct.c>).
An encyclopedia main page has about 2,300 elements: about 230 turns of box
construction, on top of the turns of fetching, parsing and CSS, before the
first layout. On the host replay (section 5) the page became READY at UI
turn 264-273.

The trial's pace. A fetch is sent by the UI turn after the one that created
it; on an idle kept-alive connection the second-round thumbnails show 45-82
ms from creation to `stage=request`, so a UI turn took roughly 50-100 ms
while the CPU was saturated (2.1). The second round is also created by box
construction as it reaches each `img` element: the first such thumbnail is
about element 400 of the page, the last sister-project logo about element
913, requested at +13 s and +18 s (about 100 elements, 10 slices, per
second; the fetches may also have waited in NetSurf's fetch queue, so box
construction may have been somewhat faster). Box construction could start
only after the stylesheets, at about +9 s; 230 slices at 50-100 ms put the
first layout at about +20 to +32 s. The owner left at +19 s; the search page was closed
in the second its HTML arrived.

### 2.3 Every thumbnail was downloaded and decoded twice

NetSurf fetches an `img` element's image as soon as the parser inserts the
element (`content/handlers/html/dom_event.c` `html_process_inserted_img`,
"Speculatively fetch the image", no box), and box construction later asks
for the same URL with its box (`html_fetch_object`). When the first fetch
is still running the low-level cache shares it. When it has completed,
the cache must judge the stored response fresh, and llcache refuses to
keep a response to a URL with a query unless it has an explicit expiry
(`content/llcache.c` `llcache_fetch_process_data`, the RFC 2616 13.9
rule). The encyclopedia's thumbnail URLs carry a query
(`?utm_source=...&utm_content=thumbnail`) and their responses have only
`Last-Modified`, `ETag` and `Age`: every thumbnail was fetched a second
time, 18 of the 49 fetches, plus its decode. This only happens when box
construction lags behind the network, which is exactly the slow-turn case
of 2.2 (on the host with fast turns no thumbnail was fetched twice; with
5-20 ms turns all 18 were).

RFC 9111 4.2.2 notes that the RFC 2616 13.9 prohibition "has not been
widely implemented"
(<https://www.rfc-editor.org/rfc/rfc9111.html#section-4.2.2>); independently
of HTTP caching, the HTML Standard gives each Document a "list of available
images" whose entries, added after a fetch, ignore higher-layer caching
(4.8.4.3.3,
<https://html.spec.whatwg.org/multipage/images.html#list-of-available-images>):
an image already obtained for the document is used again by the document.

### 2.4 JavaScript

This image is the first trial with JavaScript on by default (dev diary
2026-10-25-02). Its cost was not separated here: the host replay runs with
JavaScript on, as the trial image did, and the whole page took 49-69 ms of
host CPU to READY (about 1.6-2.2 s on the 32x model at full CPU), which
does not explain 19 s without a layout; the turn count (2.2) and the
saturated CPU (2.1) do.

### 2.5 The rest of the load in the same minutes

The Win32 hang record (`docs/validation/t23-win32-hang-shutdown-2026-10-25.md`
in wt/t23-w32hang) lists the load while the installers ran: no idle time
from 183 s to 362 s, device-work 17-34 % in kernel mode with no DOS VM,
long C: holds, `donation_cycle` records and a 250 ms synchronous w32srv
poll by every Win32 UI thread. What each means for rendering:

- **device-work#9 is the shared tick of 2.1.** The only device-work
  sites the log names are `shared_tick_work` (`c01485f0`, 1,008-1,082 us,
  3 reports, all after the four installers started) and `timer_work`
  (`c01972d0`, 2,107-2,126 us, 5 reports from line 506 on, the first
  before any Win32 process). The `timer_work` ones match the 2.09-2.11 ms
  firmware gaps the same boot records (`[kernel] smi ... max_us=2093-2110`,
  `irqoff_long ... phase_us=2093-2103`): SMI time, not work. Only the first
  ten budget overruns are reported (`work.c`, then one in 1,024); the
  share before the installers was 41-53 permille. Model: the host test's
  old per-tick update costs about 105,000 TSC cycles (25 us on the 4.2 GHz
  host, `-O2`, three runs 104,706-106,722) for 1,530 extents, 13.7 cycles
  per walk step; on the repository's 32x T23 model that is 0.8 ms per tick,
  so the trial's 1.0-1.08 ms matches about 2,000 extents in all, about 500
  per installer. Indicative: the trial does not record extent counts.
- **C: holds.** BOOT.LOG has 52 `vfs_hold drive=C` records (only holds of
  50 ms or more are logged); 28 are installer reads, 3.66 s in total,
  median 100 ms, the longest 401 ms, up to 3 waiters; 6 of them (55-137 ms)
  fell in the NetSurf session. The desktop's compositor thread does not
  read C: for a frame; programs and workers that do wait: NetSurf's first
  frame took `window_ms=2537` here against 627-789 ms in trial 2ca3b0b6,
  and one folder-worker directory read held C: for 600 ms
  (`task=desktop#43 op=readdir`). Not changed here.
- **donation_cycle.** 17 records: 11 between installer threads, 3 between
  an installer thread and the desktop's folder (#43) or settings (#44)
  worker (lines 4456, 4458, 6554), 3 inside the desktop (at boot the
  compositor #37 and the sound worker #42 waiting on the print spooler #52,
  lines 362-363, right after `[desktop] stall stage=idle ...
  no_frame_ms=531`; later links #41 on folder #43). The compositor is in
  one cycle only, at boot. These delay folder and settings windows, not
  every frame. Not changed here.
- **w32srv and the installers in kernel mode.** w32srv used 68-119
  permille, 67-97 of it in kernel mode; `Q3ADemo.#82` spent 189 of its 213
  permille in the kernel. The log does not record where. One candidate
  with the same cause as 2.1: every user copy and page fault resolves its
  address with the same linear `ua_find`, up to one walk of the process's
  extents per 4 KB page. At about 500 extents that is up to about 50 us per
  page on the 32x model, up to about 0.65 s of kernel time per pass over
  the installer's 48 MB archive. A lookup structure for `ua_find` (sorted
  array with binary search, or a last-hit hint invalidated on every
  extent change) is a kernel address-space change for its owners; not
  changed here. The 250 ms poll by each USER thread is four installers'
  worth of RPCs (about 16 per second); its cost is not in the log either.

## 3. Why earlier work did not solve it

- fx-nsperf (643479b2b) fixed the frame presentation of a page already
  shown (thousands of DAMAGE messages per scroll step) and ENOBUFS on
  images; its frame bench starts after the page is loaded. The load path
  (box construction turns, duplicate fetches) was not measured.
- fx-11 and fx-24 removed layout costs (allocator, clock reads, font
  measuring, limits); box construction precedes layout and was not
  measured. In trial 2ca3b0b6 pages appeared: no Win32 process ran during
  its NetSurf sessions (its only `[win32] start` comes later) and its
  recorded windows show device-work at 69-76 permille.
- f2-95, f2-98 and the compositor and Savage work reduced the desktop's own
  per-frame cost; this trial has no software fallback at all. The desktop
  was slow because it was starved, not because composition was expensive.
- fx-9's T23 replay (`tests/host/t23_vm_clock_test.c`) modelled the Win32
  shared tick as a constant 4 us device-work turn; the per-process extent
  walk was never part of a model.

## 4. Changes

- `src/kernel/proc/win32/counter.c`, `src/kernel/proc/process.c`,
  `src/kernel/proc/win32/sections.c`, `src/kernel/include/ciuki/w32_objects.h`:
  `w32_shared_tick` publishes the shared page once per tick
  (`w32_shared_publish`), through the kernel direct map, with interrupts
  off for the six stores; no extent walk and no per-process work, as NT
  updates its one shared data page from the clock interrupt. `sections.c`
  records the page's frame (`w32_shared_frame`) when the kind-4 section is
  mapped and clears it, interrupts off, before the last unmap frees it.
  The page layout and the odd/even sequence protocol are unchanged; the
  sequence now advances by 2 per tick instead of 2 per process per tick.
  The PIT still queues `shared_tick_work` every tick (the fx-9 replay
  extracts that code verbatim); the tick is now a few stores.
- `sdk/netsurf/patches/0018-t23-render-image-reuse-and-box-slices.patch`:
  - `html/object.c` `html_adopt_speculative_object`: a box asking for an
    image URL takes the document's existing no-box object for the same URL
    and permitted types (callback switched to the box callback). A
    raster image already READY/DONE is attached at once; a LOADING one
    counts as an active fetch and receives the rest. Content that can
    reformat (SVG) and was already READY/DONE is not attached during box
    construction: its READY (and DONE) events are replayed to the box
    callback right after the document's first layout
    (`html.c` `html_box_convert_done` -> `html_object_ciuki_replay`), as a
    fetch finishing after box construction delivers them; the READY step
    sizes the SVG from the laid-out box (review fix: attached at box
    construction, an automatically sized SVG stayed without its intrinsic
    size, and a width-only one kept its intrinsic height). The HTML
    Standard's list of available images, limited to the parser-then-box
    pair.
  - `html/box_construct.c`: `convert_xml_to_box` converts elements until
    `CIUKI_BOX_SLICE_NS` (8 ms, half a 60 Hz frame) of budget time has
    passed, reading the clock after every element (one clock read per
    element: about 2,300 per encyclopedia page), then yields; a slice
    overruns the budget by at most one element and its text (review fix:
    with a read every 10 elements, costly elements made slices of 33-37 ms,
    81 ms on the costly-element test page). The UI keeps a turn between
    slices; Windows only calls a window not responding after 5 s without
    message processing (`IsHungAppWindow`,
    <https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow>).
- `sdk/netsurf/port/budget.c/.h`: the slice clock (charged with the layout
  clock scale, so host tests see T23-sized slices) and counters;
  `chrome.c`: the exit record adds `box_slices`, `box_elements`,
  `box_slice_max_us`, `box_element_max_us` (the longest step between two
  clock reads) and `image_reuse` (before `connect_retries`).
- `tests/host/netsurf/test_surface.py`: `test_registered_surface` links
  the cached host prefix. It failed at `nsfb_test.c:122` (`pongs==1`)
  against the shared prefix built on 2026-10-25 00:31, before fx-nsperf
  changed the Ciuki surface and this test, and passes against a prefix
  built from the same sources (both reproduced here). The class now
  verifies the cache through the recipe, as `test_local` does, and
  rebuilds it only when its inputs changed. Not related to the trial.

## 5. Host evidence

- `tests/host/win32/kernel_core_test.c` `test_shared_tick` (run by
  `tests/host/win32/test_kernel_core.py`): two real spawned Win32
  processes, one with 1,500 extra reservations. The previous per-process
  update costs `old_walk_steps=7650` for one tick; `w32_shared_tick` makes
  no walk step, both processes read the same updated page (even sequence,
  tick, interrupt and system time), the sequence advances by 2 per tick,
  the page stays published while one process maps it, and the frame is
  forgotten before the last unmap frees it (no page or mapping left).
  `w32 shared tick: extents=1515+15 old_walk_steps=7650 ... new_walk_steps=0 ... PASS`.
- `tests/host/netsurf/test_network.py`
  `test_images_fetched_once_with_slow_ui_turns`: the real NetSurf core
  and front end, the CiukiOS socket layer and lwIP on the host, HTTPS,
  20 ms UI turns (`browser-slow-ui`, budgets on the 32x T23 model), a page
  with 1,500 elements and then twelve thumbnails whose URLs carry a query
  and whose responses have no expiry. After: each thumbnail done once,
  `box_slices=29-30 box_elements=1517 box_slice_max_us=8335-8440
  image_reuse=12`, `box_element_max_us=1839` (with the clock read every
  10 elements: 25-27 slices, 9,237-10,069 us). Without the `object.c` hunk:
  `thumb00.png` done twice (fails). Without the box hunk the slice fields
  stay 0 (fails).
- `test_reused_svg_keeps_its_intrinsic_size` (review): two 100x60 red
  SVGs, one automatically sized and one with only `width=50`, adopted
  after their fetches completed (a stylesheet at the end of the page holds
  box construction). The normal late path and `<object>` show 6,000 and
  1,500 red pixels (100x60 and 50x30). Committed 0018: 1,950 and 1,050;
  with a READY reformat at adoption: 6,000 and 3,000 (50x60); with the
  replay after the first layout: 6,000 and 1,500 (passes).
- `test_box_slices_stay_near_budget_on_costly_elements` (review): 200
  `:nth-child` rules on 400 sibling elements, several ms of model time per
  element. Clock read after every element: 196-209 slices, longest
  16,927-17,530 us with a 12,063-15,295 us element (the longest step
  between two clock reads); every slice at most 8 ms plus one element. Clock read every 10 elements: 40
  slices, longest 80,744 us (fails the 32,000 us ceiling).
- Investigation replay (scratch harness, not committed): the encyclopedia
  main page of the same day mirrored (2,309 elements, 28 resources) and
  served over local HTTP to the real front end, each UI turn delayed by a
  fixed time. Before (50a843517): READY at turn 264 (2 ms turns) and 273
  (20 ms turns, 5.5 s); 28 requests with 2 ms turns, 46 with 5 and 20 ms
  turns (the trial's 18 duplicates). After, 20 ms turns, 32x clock: READY
  at turn 135 (2.7 s), 28 requests, `image_reuse=22`, 75 box slices.
  Host CPU to READY 64-69 ms before, 49-59 ms after (about 1.6-2.2 s on
  the 32x model).
- Suites on this source state (worktree wt/t23-render, host NetSurf built
  from it): `tests/host/netsurf` `test_amendment` 8 OK, `test_surface` 3
  OK, `test_local` 15 OK (`ReproducibilityTests` not run: it builds two
  clean target trees under `build/external`, which this worktree links to
  the main tree), `test_probe` 2 OK, `test_testmode` 2 OK, `test_network`
  25 of 26 OK when run alone (`test_styled_page_over_http` imports
  `styled.py`, which is on `sys.path` only after `test_local` is imported;
  it passes in the same run as `test_local`, unrelated to this change);
  `tests/host/win32` `test_kernel_core` 5 OK, `diagnostics` 3 OK,
  `user32` 3 OK, `dialogs` 20 OK, `input` 3 OK, `test_kernel_loader` 15
  of 16 (the built-in payload test needs the cached Wine upper DLLs, not
  present in this worktree); `tests/host/proc/proc_test.c` PASS; the
  changed kernel files compile with the kernel's `CFLAGS`.

## 6. What the lead must check (QEMU, then T23)

1. NetSurf, an encyclopedia main page: it appears; `NETSURF.LOG`/BOOT.LOG
   exit record `box_slices` well below `box_elements`/10 (the old slice
   count), `box_slice_max_us` at most 8,000 plus `box_element_max_us`,
   `image_reuse` > 0; SVG images keep their size;
   every thumbnail `stage=done` once; a `layout work ... class=remote`
   record.
2. A Win32 program kept running (for example the Quake III installer
   left open, or several): `[cpu] device-work` stays near its idle level
   (40-70 permille in this trial before any Win32 process), no `[work]
   budget exceeded site=<shared_tick_work>`; Win32 programs still see time
   advance (GetTickCount, timers, the installer's progress).
3. Desktop under the same load: `[sched] desktop_share` and the desktop's
   `[cpu]` share; if rendering is still reported slow with no Win32 or
   NetSurf load, capture the verbose `[display] ... frame_p95_us`
   records, which BOOT.LOG does not keep.
4. With installers running after this change: the installers' and
   w32srv's kernel-mode share (`[cpu] top=... NNNkKKKs`); if it stays high,
   the next suspect is the linear `ua_find` of 2.5.

## 7. Risks and follow-ups

- A Win32 process that had a private copy of the shared page would no
  longer see updates; the page is mapped `w32_locked`, read-only, and no
  path makes a private copy.
- Repeated uses of one image URL in a page (two boxes) still fetch twice
  when the first fetch has completed and the URL has a query; llcache's
  RFC 2616 13.9 rule could follow RFC 9111 4.2.2 (heuristic freshness for
  query URLs) as a separate change.
- The one-callback-per-turn main loop still costs a UI turn per fetch or
  conversion callback (about 60 of the 135 turns in the replay); running
  due callbacks within a time budget per turn would cut them further.
- The four Quake III installer processes that never showed their wizard
  and took 4-39 % of the CPU are the Win32 hang work (wt/t23-w32hang).
