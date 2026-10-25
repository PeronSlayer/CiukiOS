# fx-nsperf — NetSurf renders at 1–2 fps; images of one page fail with ENOBUFS

Written 2026-10-25. Evidence: real-hardware trial (Pentium III-M 1.13 GHz
class laptop) of the image built from `main` 60d029c9, private, never
committed: `legacy/local/physical/2ca3b0b6/disk/LOGS/NETSURF.LOG` and
`BOOT.LOG`. Owner's report: "NetSurf works well but keeps rendering pages very
slowly (1–2 fps)". Site names and URLs from the trial are not reproduced here.

## 1. Evidence

`NETSURF.LOG`, first session (welcome page) and second session (browsing):

```
[netsurf] exit reason=channel-eof ... layout_max_us=207000 local_max_us=207000 remote_max_us=0 ... keepalive_pumps=6 fetches=0 ...
[netsurf] fetch url=<image of an HTTPS page> stage=error code=0 errno=105 tls_err=0 bytes=0 ms=5176 id=82 at=connect ...
  (four more: ids 83-86, ms=5042-5310, same second, errno=105, at=connect)
[netsurf] exit reason=desktop-close-request ... layout_max_us=100000 local_max_us=84000 remote_max_us=100000 ... keepalive_pumps=127 fetches=95 fetch_ok=88 fetch_errors=5 last_error=connect:105:0 network=dns-ok
```

`BOOT.LOG` (the boot log dropped its `[cpu]` statistic records while NetSurf
ran, `dropped_statistic=646`, and rotated before the failing fetches):

```
[console] [netsurf] layout work elapsed_us=207000 class=local pass=1 font_opens=1 font_open_us=190000 font_measures=63 font_measure_us=10753 ...
[console] [netsurf] layout work elapsed_us=15000 class=remote pass=2 font_opens=0 font_open_us=0 font_measures=23 font_measure_us=10661 ...
[console] [desktop] accel fills=21 copies=914 writes=15 fallback=0 cache_rows=5 frames=60 lfb_bytes=3945032 ...
[console] [desktop] accel fills=0 copies=971 writes=0 fallback=0 cache_rows=10 frames=104 lfb_bytes=11464776 ...
[console] [desktop] accel fills=10 copies=582 writes=0 fallback=0 cache_rows=26 frames=75 lfb_bytes=29762672 ...
```

Layout is not where the frames go: remote layout passes after the first are
2–15 ms, the 207/84 ms first passes are one font file read (190/67 ms, once
per session). The desktop, while NetSurf showed a page, made 582–971 engine
copies per 2 s window and up to 29.7 MB of LFB writes: one desktop update per
small rectangle.

## 2. Measurement: the host frame bench

`sdk/netsurf/port/framebench.c` (`--ciuki-frame-bench`, host builds only,
patch `0017-fx-nsperf-present-and-frame-bench.patch` adds the RAM-surface
update hook and two front-end helpers). The real framebuffer front end draws a loaded page
into libnsfb's RAM surface; every libnsfb update (fb_redraw, the pan copy of
fb_pan, scroll bars) is mirrored into a production v3 surface (`surface.c`)
through `ciuki_browser_update`, the routine `nsfb_surface.c` installs on
CiukiOS, over a model of the desktop channel: 64 queued messages per
direction (`CIUKI_CHANNEL_QUEUE_MAX`, `src/kernel/include/ciuki/abi.h:469`), a
desktop service turn reads at most 16 messages of a client
(`apps/desktop/desktop.c:3147`) and answers each v3 request with RESULT, the
desktop records every DAMAGE with the production `desk_damage_add`
(`apps/desktop/compositor.c:447`). The end of a frame is the UI turn: present
and, while the channel refuses, wait for desktop turns.

T23 model per frame: browser plotting and the desktop's damage bookkeeping at
32x host time (the f6-20 T23 clock scale); kernel entries (client send and
RESULT read, desktop read and RESULT send) at 3 us; compared bytes 3 ns,
copied bytes 1.67 ns, composed pixels 12 ns and LFB bytes 7.46 ns (rates of
`tests/host/desktop/t23_cpu_model_test.c`); a desktop service turn 91 us.
Counts come from the production code; the rates are the repository's model,
not hardware timings. The model omits the desktop's per-message parse and
visibility computation (`window_damage`), so the old figures are lower bounds.

Frames: the first full paint, three identical repaints, twelve 100-pixel wheel
steps (`widget_scroll_y`, as `NSFB_KEY_MOUSE_5`), three small redraws
(240x20) and one Page Down. Viewport 800x542 content, the window of the trial.

### Standard article (`pages.article(3, 60000)`, encyclopedia-like)

| Frame | Before: DAMAGE msgs | Before: channel refills | Before: model | After: msgs | After: model |
| --- | ---: | ---: | ---: | ---: | ---: |
| First paint | 2,967 | 46 | 315 ms | 1 | 23 ms |
| Identical repaint | 0 | 0 | 8 ms | 0 | 8 ms |
| Wheel step (median of 12) | 8,523 | 133 | 835 ms | 3 | 22.4 ms |
| Wheel step (dense paragraphs) | 13,820–24,107 | 215–376 | 1.35–2.35 s | 3 | 23–25 ms |
| Small redraw | 0 | 0 | 0.3 ms | 0 | 0.2 ms |
| Page Down (full redraw) | 29,546 | 461 | 2.89 s | 3 | 31 ms |

Before, a refused send restarted the comparison of the whole damaged region,
so a wheel step compared 50–154 million pixels. The old figure, 0.75–2.3 s
per wheel step, is the owner's 1–2 fps.

Where a frame goes after the fix (wheel step on the article, model):
plotting 1.5–3.7 ms (FreeType glyphs from the cache, text and boxes of the
exposed strip), present 3.6–4.6 ms (row memcmp of the moved area, copy of
changed spans), desktop 16–17 ms, almost all of it composing and uploading
the moved 385,000 pixels to the LFB. Forum thread and a portal page with
eight text styles: wheel step median 21.1 and 21.3 ms, Page Down 23 and 28 ms.
A photo page (1024x768 JPEGs shown at 480x360): 23 ms per wheel step, plus
about 105 ms once per image when it first scrolls into view (JPEG decode,
host time x32).

Glyph cache (libfont, 128 bitmaps per face): identical repaints hit it 100 %
(577–1,362 glyphs, 0 misses); wheel steps miss 0–26 glyphs, about 70 us each
on the model (2.3 us host per cold glyph). Not a dominant cost; unchanged.

## 3. Causes

1. **One DAMAGE message per changed pixel run.** f2-98b replaced the
   covering box with "only changed pixel runs ... coalescing equal adjacent
   row runs" (`sdk/netsurf/port/surface.c` at 60d029c9: `present_area`,
   lines 120-143; `publish`, 105). Text changes on nearly every row in many
   short runs, so runs almost never have equal x ranges on adjacent rows.
   Every message costs a channel send, a desktop read, desktop damage
   bookkeeping (a pixman union and, past 32 rectangles, the pairwise merge
   of `damage_reduce`, `compositor.c:428`, about 17 us host each when
   saturated), a RESULT and its read. The channel holds 64 messages and the
   desktop reads 16 per turn, so a frame took hundreds of round trips, and
   each refusal restarted the region's comparison.
2. **A present per libnsfb update.** `update` presented immediately
   (`nsfb_surface.c:231` at 60d029c9): a wheel step presented the pan copy,
   the exposed strip and the scroll bar separately.
3. **ENOBUFS failed the image.** `open_connection` (`sdk/netsurf/port/net.c:345-355`
   at 60d029c9) turned any socket()/connect() error into a fetch error; the
   failed job freed its slot, the next queued image took it and was refused
   in turn (five failures in the same second in the trial). The CiukiOS socket
   layer answers ENOBUFS when its TCP objects run out (32 for the system,
   closing ones included until the control worker deletes them,
   `src/kernel/net/lwip/sockets.c:686`), when its 8 request slots are busy
   (`sockets.c:16`), or when lwIP's PCB/netconn/segment pools or 1 MiB heap
   (`src/kernel/net/lwip/lwipopts.h`) refuse; lwIP 2.2.1's `tcp_alloc` reclaims
   TIME-WAIT, LAST-ACK and CLOSING PCBs and lower-priority active ones, not
   FIN-WAIT or same-priority active ones. The
   trial's boot log lost the `[net]` records of that minute, so which of
   these ran out is not recorded.

## 4. Why earlier work did not reach a usable speed

- fx-24 and fx-11 removed layout costs (allocator locks, heap growth, clock
  reads, font measuring, layout limits): layout and first display, never the
  frame presentation, which no host test measured.
- f2-98b minimised copied bytes and kept separate rectangles exact; its host
  test checked pixels, not message counts or frame cost. Its run-length
  messages are the 1–2 fps.
- f2-95, f2-98a and the desktop-share work made the desktop cheaper per
  frame and per native-toolkit update; they did not change the number of
  messages a NetSurf frame sends.

## 5. Fixes

- `surface.c`: rows are compared with `memcmp`; a changed row copies its
  first-to-last changed pixel span; changed rows closer than 16 rows form one
  band sent as one DAMAGE of its bounding box (everything else in it is
  already equal), at most 8 bands per damaged rectangle, the region's extents
  beyond 16 rectangles. Published bands leave the region, so a refused send
  resumes without comparing or resending them. An identical frame still sends
  nothing and separate rectangles stay separate.
- `ciuki_browser_update` (libnsfb update) only records damage; each
  main-loop turn presents once, after drawing and before the scheduler can
  start a long layout (`ciuki_chrome_present`, patch 0017), and the UI turn's
  input wait presents what is left, as a Windows application accumulates its
  update region until WM_PAINT
  ([InvalidateRect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-invalidaterect)).
  While input is queued (several wheel detents) the turns draw and one
  present covers them.
- `net.c`: a socket refusal for resources (ENOBUFS, ENOMEM, EAGAIN, EMFILE,
  ENFILE) keeps the job and its slot without a socket (state WAITING),
  limits the sockets the browser holds to those open at the refusal (at
  least one), retries after 250 ms doubling to 4 s within the job's 30 s
  limit, and closes a kept-alive connection no waiting job can use before
  retrying. A waiting job takes a kept-alive connection of its own origin as
  soon as one is idle (RFC 9112 9.3; 9.4: "A client ought to limit the number
  of simultaneous open connections that it maintains to a given server",
  <https://www.rfc-editor.org/rfc/rfc9112.html>; WinInet's default of two
  connections per HTTP/1.1 server, Microsoft KB 183110). Each established
  connection raises the limit by one. Kept-alive connections: 4, at most 2
  per origin (was 2 in total). Records: `[netsurf] connect-wait url=...
  errno=... open=... limit=... retry_ms=...` once per job (the session's first
  also in NETSURF.LOG) and `connect_retries=N` in the exit line, which also
  reports `presents`, `damage_messages` and `present_refused`.

## 6. Host tests (fail before, pass after)

- `tests/host/netsurf/surface_test.c` (`test_surface`): a text-like scroll
  frame through the production present path. Before: 40,843 DAMAGE messages,
  all sent from the updates; after: 1, none from the updates. A status-bar
  text change is one exact DAMAGE; a refused send resumes without resending
  the published band.
- `test_local.LocalTests.test_frame_cost_scroll_and_small_updates`: the frame
  bench on the article, a forum thread and the portal page. Before: 2,967–6,679
  messages for the first paint, wheel-step medians 174–457 ms even without
  the comparison cost the old counters lacked; after: at most 8 messages per
  frame, no channel waits, identical repaints send nothing, wheel-step median
  under 50 ms and Page Down under 100 ms on the model.
- `test_network.NetworkTests.test_page_with_40_images_under_a_socket_limit`:
  real NetSurf core and front end, the CiukiOS socket layer and lwIP of
  `src/kernel/net/lwip` on the host, a page with 40 images over HTTPS, the
  system holding one TCP socket (`stack_fixture.c` `browser-socket-limit`).
  Before: `img01.png stage=error code=0 errno=105 ... at=connect` and more;
  after: all 40 images `stage=done code=200`, one refusal, at most one live
  socket, one connection for the page and its 40 images.

Also fixed: `test_network`'s setUpClass did not compile since G26b (`dns.c`
reads `<ciuki/net.h>` outside `CIUKI_HOST`; the dns fixture is now built with
`-DCIUKI_HOST`). Running it again showed a second hidden G26b regression: its
proxy fields grew a pending job to 1,564 bytes on i686, so the fetcher's
128 KiB pending budget held at most 83 fetches NetSurf had set up but not
started, and NetSurf fails a fetch whose setup is refused
(`content/fetch.c` `fetch_start`): further images of a large page broke. The
budget is now 256 KiB (`CIUKI_NET_PENDING_BUDGET`, about 167 queued fetches),
which the f6-15 fan-out test (`network_test.c` `fanout`, more than 100
queued) checks again.

## 7. Residual gaps

- The desktop recomposes and uploads the whole moved area of a wheel step
  (about 16 ms of the 22 ms model frame). A v3 scroll request that moves the
  window's pixels on screen with the 2D engine and composes only the exposed
  strip (the
  [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex)
  model) would bring a wheel step to about 6 ms. It is a desktop protocol
  change and is not part of this fix.
- JPEGs larger than their displayed size are decoded at full size when first
  shown (about 105 ms on the model for 1024x768); DCT-domain scaling to the
  displayed size is a NetSurf core change.
- A session's first layout reads one font face from disk (190 ms in the
  trial) when the page uses a face the start-up prefetch did not open.
- The kernel resource that refused the trial's sockets is not identified;
  the next trial's exit line (`connect_retries`) and `[netsurf] connect-wait`
  record (`open=`, `limit=`) show whether refusals still happen and at how
  many open browser sockets.
- The figures are the host model. The T23 confirms them with the exit line's
  `presents` and `damage_messages` and the desktop's `[desktop] accel copies=`
  per 2 s while scrolling.
