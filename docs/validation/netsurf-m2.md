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
