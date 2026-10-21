# NetSurf M1 Amendment 1: implementation and host validation

Scope: the ten findings in `docs/directives/f6-14-netsurf-m1-frontend-file.md`,
Amendment 1. This supplements the delivered M1 port; its provisional T23
responsiveness and resident-memory decisions remain with the lead.

## Primary-source checks and implementation decisions

- [NetSurf's source-release index](https://www.netsurf-browser.org/downloads/source/)
  identifies the pinned 3.11 release. Its
  [developer documentation](https://www.netsurf-browser.org/documentation/develop)
  directs port work to upstream sources and documents native host debugging.
  Compare the retained, hash-verified netsurf-all source with the applied
  `0001-offline-sdk.patch` and new `0002-amendment-1.patch`; do not edit retained
  originals or obtain replacement archives.
- [RFC 2397, sections 2–3](https://www.rfc-editor.org/rfc/rfc2397.html)
  specifies the optional media type and the `;base64` marker. The
  [NetSurf 3.11 data fetcher](https://raw.githubusercontent.com/netsurf-browser/netsurf/release/3.11/content/fetchers/data.c)
  tests the suffix without a length guard. Calculate its length once and
  require seven bytes before subtracting. Sanitized fixtures cover short and
  absent media types and actual base64 decoding.
- The pinned libdom Hubbub binding creates strings/nodes in its tree-handler
  callbacks, before NetSurf's `box_construct.c` normalizes boxes recursively.
  Apply cumulative parser allocation credits (8192), string/text bytes
  (128 KiB, individual strings at most 8192 bytes), at most 64 attributes per element, and DOM depth 64 at those
  callbacks. Check subtree depth before append/insert/reparent, with an
  iterative bounded traversal. Keep rejection sticky across streaming and
  completion: a tokenizer path can swallow a tree-handler error. Charge box
  creation and layout clones to the per-document bctx counter, reject box
  depth/text/columns before normalization, and check implied normalization
  depth and table column growth before allocation. These are conservative
  admission limits, including discarded construction work, not a measurement
  of total browser residence.
- The [GIF89a specification, Graphic Control Extension](https://www.w3.org/Graphics/GIF/spec-gif89a.txt)
  requires saving prior pixels for disposal method 3. The
  [upstream libnsgif implementation](https://raw.githubusercontent.com/netsurf-browser/libnsgif/master/src/gif.c)
  illustrates its separate `prev_frame` and frame-metadata allocations;
  implementation decisions were checked against the locally pinned 1.0.0
  `gif.c` and `lzw.c`, rather than assuming the current upstream revision is
  the pin. Use one 2 MiB ledger for bitmap pixels, bitmap administrative
  reservations, GIF objects, LZW context, frame metadata and restore buffers.
  Allocation headers are charged; framebuffer bitmap administration reserves
  a conservative 4096 bytes per bitmap. Limit GIF frame holders to 64. A
  failed restore allocation returns OOM instead of permitting a missing restore
  buffer. Temporary thumbnail surfaces use the same bitmap allocator.
- The SDK `ciuki_font_measure()` implementation shares hinted advances,
  kerning and delta adjustment with painting. Preserve that measurement API;
  use UTF-8 boundary/space searches with logarithmically many measurements
  instead of repeatedly measuring every prefix. The regression counts both
  calls and processed bytes for 8192-byte runs. The shared monotonic deadline
  is checked within min/max walks and plain-text reformatting, and all exits
  update maximum elapsed time. Text files use `text/plain` so this handler is
  exercised by local `.txt` documents.
- The [newlib OS-interface documentation](https://sourceware.org/newlib/libc.html#Syscalls)
  leaves OS operations to the target interface. Locally, desktop
  `apps/desktop/client.c` queues DAMAGE and replies before rendering;
  `apps/libdeskui/v3.c` only encodes requests. Follow the lead's explicit
  decision: render privately, copy damaged rows immediately before DAMAGE,
  and proceed without waiting for RESULT. A reply is not a consumption fence.
  Concurrent compositor reads may tear within a frame, as accepted for other
  clients. The current v3 protocol requires packed rows; invalid padded strides are
  rejected. Private storage matches the validated surface allocation.
- The pinned framebuffer click handler turns mouse-up into a click. Add an
  explicit libnsfb CANCEL control event; fbtk drops its pointer grab and the
  frontend clears press/drag state and tracks with no buttons, without
  synthesizing mouse-up. Release keyboard modifiers separately. The real
  frontend fixture checks both focus loss after press and capture loss during
  drag, including a later physical release.
- Route content wheel events through the surface mapper, preserving detent
  count and direction; chrome and dialogs retain toolkit ownership. Clear
  modal status invalidation once painted. In modal edits, consume printable
  KEY events and insert desktop Unicode TEXT, including Italian punctuation,
  accented characters and AltGr, while retaining navigation and shortcuts.
- Record history on a successful top-level CONTENT_DONE notification, not on
  URL updates or generic throbber stops. Keep profile duplicate suppression
  and surface insertion/save errors in the status line. An explicit pending
  layout-rejection flag prevents READY/DONE unwinding from recording a rejected
  page before its deferred error runs; internal error pages do not record
  visits. The late-error URL path falls back to the committed parameters when
  loading parameters have already transferred. Host captures can
  explicitly opt into a temporary profile to test navigation, restart,
  duplicate suppression and rendering History.html. Capture can finish after
  an admission error once the loading operation has stopped.
- Every installed-output cache miss discards the entire disposable working
  directory before preparing sources. A mock regression seeds modified
  working source and matching input metadata, then checks that missing and
  stale outputs both force fresh preparation.

## Verification and limits

Run tests from `tests/host/netsurf`, which is not a Python package. Commands
and exact results for this worktree are in `build/codex/f6-14-report.md`.
Sanitizer subprocesses retain `ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=halt_on_error=1`; do not classify a LeakSanitizer environment
failure as a green sanitizer run. No QEMU, systemd scope, image build, git
mutation or diary edit is part of this amendment implementation.

The lead still owns cross-review/integration, green sanitizer confirmation
where the runtime can perform leak checks, and the accepted QEMU/T23
responsiveness and 128 MiB resident-memory qualification.
