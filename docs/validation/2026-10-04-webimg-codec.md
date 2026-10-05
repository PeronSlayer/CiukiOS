# CiukWeb image decoder design

The browser hands the decoder a local compressed file path and a caller-owned
6,144-byte RGB888 scratch buffer. `WEBIMG.APP` is a separate CAPP module. Its
packed ABI is in `src/apps/webimg.h`: `EV_OPEN` copies the request from the
browser's segment:offset, opens and identifies the image, and reports the
dimensions; each `EV_POLL` emits at most one PNG/GIF row or one JPEG MCU into
the supplied far buffer; `EV_CLOSE` closes the file and frees all DOS blocks.
The request is copied back after each poll so status and rectangle results
remain visible to the browser.

Input dimensions are limited to 2,048 by 2,048 and RGB output to 12 MiB.
Streaming state is bounded: PNG uses one 32 KiB DEFLATE history window and two
far scanlines (each no larger than 8,192 bytes), GIF uses its 4,096-entry LZW
dictionary, and JPEG uses TJpgDec's bounded work pool. Progressive JPEG and
unsupported PNG/GIF features must report an explicit error rather than emit a
partial image as complete. Alpha/transparency is composited against white.

## Research and implementation decisions

- [W3C PNG Third Edition](https://www.w3.org/TR/png-3/) specifies scanline
  filters 0 through 4, per-byte reconstruction, and modulo-256 arithmetic.
  The PNG decoder therefore keeps prior/current rows and reconstructs each
  row before RGB conversion. PNG IDAT data is a single zlib stream split over
  arbitrary chunk boundaries, so IDAT is consumed incrementally.
- [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950/) defines the zlib wrapper;
  [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951) defines DEFLATE's 32 KiB
  history and bit/Huffman representation. The decoder allocates the history
  window separately with DOS memory and does not materialize the full image.
- [CompuServe GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt)
  defines sub-block input, variable-width LZW codes up to 12 bits, local/global
  palettes, transparent color indices, and four-pass interlace row order. The
  decoder retains only the bounded dictionary and current output row; transparent
  pixels keep the white-composited background.
- [TJpgDec official page](https://elm-chan.org/fsw/tjpgd/00index.html) publishes
  ChaN's R0.03 source and redistribution terms. The exact R0.03 source is kept
  under `third_party/tjpgd/` with its upstream copyright and no-warranty notice.
  [The official patch note](https://elm-chan.org/fsw/tjpgd/patches.html) fixes
  grayscale clipping for `JD_FORMAT == 3` and `JD_FASTDECODE >= 1`; that patch
  is preserved in the vendored source even though CiukiOS selects RGB888 and
  the 16-bit-safe `JD_FASTDECODE == 0` path. `jd_step` is a small local extension
  of upstream `jd_decomp`'s MCU loop: it stores x/y and restart counters in the
  decompression object and advances one MCU per call, keeping the callback,
  Huffman state, and DC predictors live across polls.

The upstream TJpgDec license permits use, modification, and redistribution
without restriction under the user's responsibility, disclaims warranty, and
requires retaining the source copyright notice. The GIF89a document requests
that software documentation acknowledge the format owner and service mark:
“The Graphics Interchange Format(c) is the Copyright property of CompuServe
Incorporated. GIF(sm) is a Service Mark property of CompuServe Incorporated.”

## Validation results

The production decoder passed the bounded host ASan/UBSan harness in
`scripts/tests/webimg_host_asan.c`, using fixtures produced by
`scripts/tests/generate_webimg_fixtures.py`. The fixtures cover stored, fixed and
dynamic DEFLATE, PNG filters 0–4 and split IDAT, palette/transparency,
interlaced GIF, JPEG restart markers and edge MCUs, and rejection of bad CRC,
truncation, excessive dimensions and progressive JPEG. An actual callback bug
was corrected: TJpgDec requests a seek when its input buffer argument is NULL.

The Linux QEMU run in
`build/tests/desktop-web-audio-2026-10-04/web-audio/results.json` loaded PNG,
JPEG and GIF in the native window and rejected a damaged PNG. The 96×64 PNG
raster matched the source RGB bytes exactly in the captured window. The same
run exercised a GET form and wheel scrolling. This run preceded the HTTPS,
CSS and JavaScript integration; it validates the image path, not those later
features. Logs, screenshots and the result report remain in that directory.
