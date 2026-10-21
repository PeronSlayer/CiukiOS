# NetSurf M2 implementation and host evidence

The implementation follows f6-15 and the lead decisions appended to f6-13.
Runtime qualification on CiukiOS belongs to the lead. No image or QEMU result
is claimed by these host checks.

## Sources checked before implementation

- [NetSurf 3.11 full source](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz),
  pinned by `config/netsurf.json`: `content/fetchers.h`, `content/fetch.h`,
  `content/fetch.c`, `content/fetchers/curl.c`, `content/llcache.c`.
  Implement the actual operation table, keep callbacks on the UI thread, and
  let llcache follow redirects. Its upstream redirect limit and 307 POST
  handling need a narrow patch for the approved five hops and 307/308 behavior.
- [RFC 7230](https://www.rfc-editor.org/rfc/rfc7230), sections 3–4 and 6,
  and its replacement [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html),
  sections 6–9: incremental CRLF framing, reject ambiguous TE/CL, count encoded
  and decoded bytes separately, reuse only a fully consumed response.
- [RFC 7231](https://www.rfc-editor.org/rfc/rfc7231), section 6.4, and
  [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html), section 15.4:
  resolve relative redirects in llcache; preserve POST for 307/308 and forbid
  HTTPS downgrade. No authentication or multipart support in this milestone.
- [RFC 6265](https://www.rfc-editor.org/rfc/rfc6265), sections 4–5:
  delegate Set-Cookie and request-cookie policy to NetSurf's URL database and
  its pinned public suffix library; do not create a second cookie store.
- [BearSSL engine overview](https://bearssl.org/api1.html),
  [SSL API](https://bearssl.org/apidoc/bearssl__ssl_8h.html), and
  [X.509 validation](https://bearssl.org/x509.html): drive nonblocking record
  buffers, use TLS 1.2 and forward-secret AEAD suites, supply SNI and explicit
  validation time, and retain full chain/name/date validation. No revocation.
- [zlib manual](https://zlib.net/manual.html): streaming inflate, gzip versus
  zlib-wrapped deflate, require stream termination and checksum validation.
- [curl CA extraction](https://curl.se/docs/caextract.html): retain the pinned
  Mozilla bundle and its MPL notice; generate anchors offline with BearSSL's
  own decoder, never fetch a moving trust store during a build.

## Prerequisites and policy

The f6-10 decision 3 seed is explicitly non-cryptographic. It cannot seed TLS.
The current SDK `getentropy()` returns ENOSYS. HTTPS must report unavailable
secure entropy until a qualified kernel facility is supplied; HTTP remains
usable. Host TLS tests use host OS cryptographic entropy only. RTC dates before
2026 are rejected before entropy or a handshake. Numeric HTTPS hosts remain
unsupported pending IP SAN qualification. There is no certificate bypass.

The lead's eight connections and 30 second deadlines replace the proposal's
four connections and shorter phase deadlines. Other proposed bounds remain:
two connections per origin, two idle handles with 15 second expiry, 4 KiB URL,
32 KiB header block, 8 KiB line, 128 fields, 8 MiB encoded and decoded body,
16 KiB I/O slices. URL-encoded POST is capped at 64 KiB. One resolver worker
has eight slots; cancellation invalidates tokens. Shutdown does not join an
unresolved lookup on the UI thread. The f6-10 resolve4 implementation bounds
each lookup at ten seconds; a stopping worker releases itself after that call.

Exact commands, results, and unresolved qualification are recorded in
`build/codex/f6-15-report.md` for the lead's review.

## Host results (2026-10-21)

`nice -n 19 make CAP= apps` passes, including the native NetSurf/BearSSL/zlib
link, ELF checks and payload manifest. The inspected NetSurf load memory is
3,026,944 bytes; this does not measure its complete resident-memory requirement.
The trust generator emits 121 anchors; host and target tables share SHA-256
`bb0bd46ff92a88bc09df7bb1b0537c1b51ab193297a384d376f7fed2815659f9`.

The three M1 pixel goldens remain unchanged. Supplemental UBSan runs pass all
17 M1 tests and all five M2 fixture groups. The M2 fixtures use the actual
f6-10 lwIP/native socket host seam with scripted Ethernet HTTP and BearSSL
peers, including valid/expired/future/wrong-host/self-signed/TLS-1.0-only TLS,
SNI, actual interface address loss, stale keep-alive retry, no POST replay,
header callback cancellation, bounded DNS worker shutdown, gzip expansion and
per-poll output limits, five redirects, and 200-cookie retention. Cookie tests
cover HttpOnly inclusion and Secure/path/domain/expiry isolation.

Original ASan/UBSan tests retain `detect_leaks=1`. Their functional assertions
run, but LeakSanitizer cannot finish because this sandbox denies ptrace. These
runs are blocked, not passing leak checks. The lead must rerun them outside
the sandbox before merging. No QEMU or image evidence was produced here.

For numeric HTTP destinations, no configured interface address is reported as
offline by the native transport. For hostnames, the current resolve4 ABI does
not distinguish missing interface configuration from other DNS errors or
timeouts, and this base's netctl is ENOSYS. A lead decision on resolver error
propagation or a later read-only network-status query is still needed to
classify that state before DNS without an unrelated connectivity probe.


## Amendment 1 research and implementation (2026-10-21)

- [RFC 9112 section 9.2](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.2):
  an idle connection has no outstanding response. Monitor idle descriptors,
  reject buffered TLS plaintext and socket data/EOF/error, recheck before
  admission and first send, and expire idle handles on every UI turn.
- [zlib 1.3.1 manual, inflate](https://zlib.net/manual.html): `Z_BUF_ERROR`
  without byte/output progress is nonfatal when more input is needed. A parser
  transition from chunk data to framing also counts as progress; the 16 KiB
  output slice still applies and gzip trailers/checksums remain mandatory.
- [BearSSL validation time API](https://bearssl.org/apidoc/bearssl__x509_8h.html):
  supply UTC days/seconds only after validating clock provenance and date.
  CiukiOS `uname().realtime_source == 1` identifies a qualified RTC sample;
  source 0 is the kernel build-epoch fallback and cannot authorize HTTPS.
- [NetSurf 3.11 pinned source](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz),
  `content/urldb.c`, `urldb_iterate_entries_path`: cookie callbacks previously
  visited only leaves, missing domain cookies on the host path root after URL
  insertion. Patch `0004-m2-amendment-1.patch` visits cookies on every node,
  preserving URL traversal and callback early termination.

Pending fetch metadata and owned input snapshots share a 128 KiB process-wide
budget. Full HTTP parsers and exactly sized wire requests are allocated only
when a connection slot is available; input snapshots are then released.
Allocation failure remains an ordinary fetch error. No kernel ABI changes.
Offline classification queries read-only `/dev/net` `NET_GET_STATE` after a
failed external lookup; numeric loopback and `localhost` remain usable.
Detailed host evidence and remaining lead validation are recorded in
`build/codex/f6-15-report.md`.

Amendment host discovery ran 26 tests in 653.529 s. All added behavioral cases,
all three M1 pixel goldens, all 121 production-anchor comparisons and both clean
target application builds reached the expected assertions. The suite exits 1:
52 subcase failures and 12 errors all correspond to LeakSanitizer's fatal ptrace
restriction (64 diagnostics). ASan/UBSan and leak detection remain enabled;
there is no green leak-check claim. The new hostname-offline and RTC-fallback
checks assert the error text in the currently rendered browser content.
The read-only status query resolves the base delivery's offline-classification
item above; no resolver ABI or network configuration authority was added.

`nice -n 19 make CAP= apps` also passes for the amended source, including native
link/import/ELF checks and the application manifest. NetSurf ELF SHA-256:
`774c148a94de9de167f1a2c4c4e93ac1f23519e244bb3ab0fdfcd2e53a671adf`;
13,293,220 file bytes and 3,031,040 inspected load-memory bytes. Host and target
anchor-table hashes remain identical to the base delivery. No image, QEMU or
hardware result is claimed.
