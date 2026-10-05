# CiukWeb graphical renderer

## Decision, 2026-10-04

The existing `BROWSER.APP` occupies 63,728 bytes of its 64 KiB code/data/stack
segment. Adding image arrays there cannot produce a usable graphical browser.
Keep the desktop event interface, put page/display and decoded-image storage
in checked XMS allocations, and load image codecs as a separate bounded CAPP
module. Decode incrementally from downloaded files and draw RGB pixels into
the compositor's current, clipped band, as the DOS window presenter does.
Neither navigation nor decoding changes the physical video mode.

Research used before implementation:

- [HTML 4.0](https://www.w3.org/TR/REC-html40-971218/) defines embedded images,
  hyperlinks, tables, forms and style/script integration. Image display alone
  is not Internet Explorer 4 compatibility.
- [RFC 3986, reference resolution](https://www.rfc-editor.org/rfc/rfc3986#section-5)
  applies to document links, redirects and image resources alike.
- [PNG specification](https://www.w3.org/TR/2003/REC-PNG-20031110/)
  requires binary transport, chunk validation, DEFLATE and scanline filters.
- [TJpgDec](https://elm-chan.org/fsw/tjpgd/00index.html) provides a small,
  portable baseline JPEG decoder with RGB output and bounded working memory.
- [MicroWeb](https://github.com/jhhoward/MicroWeb) and
  [MicroWeb-X](https://github.com/dmitrygerasimuk/microweb-x) demonstrate the
  segmented-memory tradeoffs, but neither supplies CSS or JavaScript. Replacing
  the application with either DOS binary would not meet the requested target.
- [NetSurf framebuffer frontend](https://github.com/netsurf-browser/netsurf/blob/master/docs/building-Framebuffer.md)
  is a possible future engine port, requiring a substantially broader process,
  C library and network ABI than the current CN32 execution prototype.

Each HTTP transfer has a decoded-byte limit and deadline. The image worker
returns after a bounded unit of decoding; parser and layout work also return
to the desktop between batches. Compressed bytes and page documents are
retained outside the main application's segment. Allocation failure must leave
the browser responsive and provide a specific error.

The network stack is now a separate `WEBNET.APP` module so the main window has
room for layout and resource state. `WEBSTYLE.APP` computes a bounded CSS
cascade. TLS and classic JavaScript execute in the preemptible 32-bit
`WEBWORK.EXE`; see [the worker design](web-worker.md),
[CSS engine](ciukweb-css-engine.md) and
[TLS implementation](worker-tls-2026-10-04.md).

This is a bounded native renderer, not an IE compatibility claim. The supported
HTML, CSS, image and DOM behaviors, and their validation limits, are recorded
separately. The layout parser returns to desktop input dispatch every 256 input
bytes. Image painting uses incremental source coordinates and direct RGB/BGR
conversion instead of division and variable-width shifts for every pixel.
