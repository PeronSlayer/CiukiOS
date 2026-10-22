# f2-99 cursor encoding and artwork

Research checked 2026-10-22; host evidence only. The T23 trial of image
3423912f reports `pointer=hw`, with no pointer retries or fallbacks
(`t23-3423912f-BOOT.LOG:345` and serial line 476). Those logs establish the
presenter used, but contain no cursor bitmap capture.

## Primary sources and implementation decision

- X.Org [savage_cursor.c](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage/-/blob/master/src/savage_cursor.c),
  lines 115–119: interleave source/mask every 16 bits, swap source and mask,
  AND source with mask, MSB-first bit order, invert mask. Thus each group is
  two left-to-right AND bytes followed by two left-to-right XOR bytes.
  The locally retained upstream source in `build/downloads/savage/` of the
  lead checkout was consulted when the online source endpoint was unavailable.
  SHA-256: `eedad3715ffeb4c3675bf95a3b5228eb49a8100b540728853092849c4762241e`.
- The same source, lines 197–225: CR4C/CR4D hold the cursor address in KiB,
  the upload is 1024 bytes, and Savage4 requires a register read after upload.
  Lines 229–294 put Y-high last to latch position. Lines 314–355 use three
  RGB stack bytes with the streams engine, including 16-bpp scanout.
- S3 [Savage4 Registers, RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
  printed pp. 79–82: CR45 enable/right-storage, CR46–49 origin and Y-high
  latch, CR4A/B colour stacks reset by reading CR45, CR4C/D KiB address with
  four-KiB alignment, CR4E/F negative-origin clipping. Printed p. 84: CR55
  bit 4 selects the Microsoft AND/XOR rather than X11 interpretation.
- X.Org [savage(4)](https://www.x.org/archive/X11R7.5/doc/man/man4/savage.4.html)
  documents panel-expansion limitations for Savage/MX and Savage/IX. This
  does not establish a SuperSavage fault at native T23 resolution.

The old encoder wrote each 16-bit AND/XOR word low byte first. With mask
`c0000000` and white image `40000000`, it emitted `ff 3f 00 40`; X.Org's
layout requires `3f ff 40 00`. Every eight-pixel half of each 16-pixel group
was exchanged. Correct the byte order, keeping the AND plane first. Address,
hotspot subtraction, CR55 mode and Y-high latching already match the sources.
Use three RGB stack bytes consistently with X.Org's streams path, expanding
the framebuffer-native colours supplied by fbdev (including RGB565) to RGB888.
Hardware artwork needs no shadow and uses exactly RGB 000000/ffffff.
Physical confirmation that this resolves the reported T23 appearance remains
with the lead; the logs alone cannot prove that causal link.

## Artwork and software presentation

Use original CiukiOS vector drawings under GPL-2.0: a black arrow with a white
outline, monochrome hands, text and sizing pointers, a neutral segmented busy
indicator and help. Preserve existing wire values; append open hand, closed
hand and help after the Paint cursor IDs. Preserve the Paint tools.

Generate monochrome masks deterministically from the committed SVGs with
librsvg, then derive a small integer-convolution alpha shadow from that same
silhouette. Hotspots are explicit SVG metadata. Both presenters use the same
mask and hotspot; only software blends the shadow. Keep its entire footprint
inside 32x32 save-under storage and clip updates to cursor/scene damage.
Union damage before blending and detect a changed saved footprint even when
a shell caller updates pointer coordinates without a motion event.
Client ARGB pointers retain their own alpha and receive no extra shadow.
Text fields, link canvases, resize edges and launches retain the existing
cursor API and context selection.

The browser also needs a pointer bridge: NetSurf 3.11's upstream
[`gui_window_set_pointer`](https://source.netsurf-browser.org/netsurf.git/tree/frontends/framebuffer/gui.c?h=release/3.11)
(`frontends/framebuffer/gui.c:1961-1991`) selects framebuffer cursor bitmaps.
Ciuki's `sdk/netsurf/port/nsfb_surface.c` discards that bitmap callback, and
its chrome does not send cursor requests. Map the upstream
[`gui_pointer_shape`](https://source.netsurf-browser.org/netsurf.git/tree/include/netsurf/mouse.h?h=release/3.11)
(`include/netsurf/mouse.h:89-109`) to the existing Ciuki shape IDs instead.
The pinned upstream source was read locally; the online source endpoints
were unavailable. Route links/carets from the content callback, toolkit
feedback from browser chrome/dialogs, and a busy default during loading to
v3 SET_CURSOR with bitmap index zero. Suppress unchanged requests and retry
backpressure on the next UI turn, without painting into browser content.

## Qualification

The final desktop host checks passed: five presenter/cursor tests, the native
model, folder/transfer and media notification replays, the v3 lifecycle replay,
the rendering replay and thirteen related interaction/toolkit/credits tests.
The browser surface and chrome fixtures pass their assertions and UBSan-only
runs. Their existing ASan/UBSan runner retains `detect_leaks=1`; LeakSanitizer
cannot complete in this sandbox because ptrace is denied. This is a blocked
leak qualification, not a passing leak check. The frontend patch also applies
with `--fuzz=0` to the existing prepared source, without changing that source.

Exact commands, results and source fingerprint are recorded in
`build/codex/f2-99-report.md`. No image build, QEMU or hardware run is part of
this implementation. Lead integration must preserve the f2-98b compositor
changes and review the pointer-only compositor edits.
