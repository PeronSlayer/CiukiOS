# f6-20: NetSurf welcome page, remote-only layout time limit, exit path

Status (2026-10-22): host fixtures pass; **QEMU menu probe and T23
confirmation pending** (no image, QEMU or hardware run in this delivery).

## Evidence (lead, directive `docs/directives/f6-20-*.md`)

- QEMU `qemu-t23`, image state `ec82e48b`: `about:welcome` rejected,
  `[console] NetSurf layout rejected: local page exceeds admission/time limit.`;
  the window stays on the error page.
- T23, image `b453bc92`: NetSurf runs about two 2 s scheduler windows, then
  the disk log holds only `netsurf_lwc_iterator` WARN lines
  (`[2] resource`, `[1] favicon.ico`). **No rejection line**: on the T23 the
  welcome page was laid out.

## Diagnosis

1. **Rejection (QEMU).** `layout_document` and `textplain_reformat` gave every
   document the 75 ms deadline meant for hostile remote content. The first
   layout also pays for opening the system fonts (`face_for` reads a ~410 KiB
   Liberation face and initialises FreeType inside the measured span) and for
   the first glyph rasterisations, so a slow guest (QEMU) misses it.
2. **Exit (T23).** The leak set is the one a QUIT produces on the first UI
   turn after a page completes, while the default `resource:favicon.ico`
   fetch is in flight: the host frontend fixture (`test_amendment`
   `test_real_frontend_input`, QUIT right after DONE) prints exactly
   `[2] resource` and `[1] favicon.ico`. A QUIT comes only from the Ciuki
   surface (`s->closed`): no `die()`, rejection or empty-window path calls
   `netsurf_exit`. The surface consumed **one input packet per UI turn**
   (`input()` stopped at the first queued event). The desktop routes every
   pointer motion over the window (`apps/desktop/input.c:397`) without
   coalescing for v3 clients, and a v3 peer whose 64-message queue
   (`DESK_TX_MAX`) fills is marked `v3_failed` and dropped
   (`apps/desktop/client.c:91`, `:158`), after the kernel channel's own
   64 messages (`CIUKI_CHANNEL_QUEUE_MAX`). With the pointer moving over the
   window, a UI turn slower than one motion interval (8–10 ms at 100–125 Hz,
   plausible on the T23 during the first paint) falls behind until the
   desktop drops the client; NetSurf then reads channel EOF and exits
   normally. This is static analysis; the new exit line will confirm or
   refute it on the T23. In QEMU the rejected welcome page left only the
   light error page to lay out and paint.

## Decisions and changes

- **Remote-only time limit** (`sdk/netsurf/port/budget.c`, patch `0005`):
  `about:`, `resource:` and `file:` documents are local: structural admission
  only (8192 boxes, depth 64, 8192-byte runs, 64 columns, 128 KiB text, the
  parser limits). Every other scheme, including `data:`, keeps the 75 ms
  limit. NetSurf refuses `file`/`about`/`resource` redirect targets from
  remote fetches (`content/llcache.c`), so remote content cannot become
  local. A span nested in a timed layout (an HTML object) inherits the outer
  deadline instead of clearing it.
- **Rejection never ends the browser:** unchanged error-page path, with the
  cause in the text ("needs more layout time than the browser allows for web
  content" or "exceeds the browser's layout size limits") and on stderr
  (`NetSurf layout rejected: <local|remote> page, <time|admission> limit,
  elapsed_us=N.`). `browser_window__handle_error` returns the page's
  parameters to `loading_parameters` when the current content fails after
  READY, so the error page's **Try Again** re-fetches the page (by code
  reading, `browser_window_content_ready` had already moved and cleared
  them, so the retry had no URL; not reproduced on the old build).
- **Channel drain and keepalive** (`nsfb_surface.c`, `surface.c`): each UI
  turn drains up to 256 packets, held ones first; consecutive pointer motion
  is coalesced in the event queue. Long layouts (`ciuki_layout_expired`) and
  paints (`ciuki_font_plot`) call a keepalive that, at most every 20 ms, reads
  the channel into a 64-packet hold ring (motion coalesced) without
  processing anything: PONG stays a UI-turn acknowledgement. A full event
  queue resynchronises (key-ups, CANCEL) instead of closing.
- **Exit reason:** `[netsurf] exit reason=<capture|close-button|
  desktop-close-request|desktop-closed|channel-eof|channel-error|
  window-refused|surface-open-failed|window-create-failed|quit-event> detail=N
  layout_rejections=N layout_max_us=N local_max_us=N remote_max_us=N
  input_overflows=N keepalive_pumps=N` on stderr before `netsurf_exit()`
  and its lwc report; `die()` prints `[netsurf] exit reason=fatal`. Layouts over 37.5 ms print
  `[netsurf] layout slow elapsed_us=N class=<local|remote> accepted=N`.

## T23 model and margin

Host: AMD Ryzen 7 7800X3D. PassMark single-thread ratings:
[Ryzen 7 7800X3D 3759](https://www.cpubenchmark.net/cpu.php?id=5299),
[Pentium III Mobile 1133 MHz 218](https://www.cpubenchmark.net/compare/1150vs702/Intel-Pentium-III-Mobile-1133MHz-vs-Intel-Celeron-M-440)
(chart value), ratio about 17. The host tests charge layout time **32x**
(`--ciuki-clock-scale=32`, host builds only): about twice that ratio, for
i686 code and the T23's 512 KiB L2/PC133 memory. This is a model, not a
measurement; the `layout slow` line gives the T23's own number.

`about:welcome` at 800x484 (the T23 window's content area), fresh process
(font faces and glyphs cold, files in the host page cache):

| Run | Host layout | Charged at 32x |
| --- | ---: | ---: |
| scale 1 | 0.80 ms | — |
| scale 32, first pass (three runs) | — | 23.9 / 23.9 / 27.4 ms |
| scale 32, final pass (three runs) | — | 22.4 / 23.2 / 23.5 ms |

Best 22.4 ms: **3.3x under** the 75 ms remote budget; break-even scale
about 107. Local content is exempt in any case: at scale 1,000,000 the
welcome page (charged > 75 ms) still lays out. The model excludes the T23's
disk time for the first font file reads.

## Host checks

```sh
nice -n 19 python3 -B -m unittest test_surface -v      # in tests/host/netsurf
nice -n 19 python3 -B -m unittest test_amendment -v
nice -n 19 python3 -B -m unittest test_local.LocalTests test_local.ProfileTests -v
nice -n 19 python3 -B -m unittest test_network -v
nice -n 19 python3 -B -m unittest test_testmode -v
```

## Open (lead)

- QEMU menu probe and T23: the welcome page lays out, the window stays while
  the pointer moves over it during the first paint, and the serial shows the
  exit reason when the window is closed.
- Desktop policy (not changed here): a busy v3 client is still dropped once
  128 messages are pending. Coalescing pointer motion for v3 peers in
  `desk_queue`, as v1/v2 do, would bound that for every v3 client.
- Font faces open inside the first layout (disk time on the T23 is charged to
  a remote page's budget); four face slots with round-robin replacement can
  reopen fonts on pages using more than four styles.
