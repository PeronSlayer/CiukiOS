# NetSurf HTTPS entropy wiring (f6-17)

## Research and implementation decisions

Checked 2026-10-21 against the vendored BearSSL 0.6 sources
(`third_party/bearssl/src/ssl/ssl_engine.c`, `src/rand/sysrng.c`,
`src/inner.h`) and the recipe `sdk/netsurf/build.py`:

- [BearSSL SSL API](https://bearssl.org/apidoc/bearssl__ssl_8h.html):
  `br_ssl_engine_inject_entropy()` feeds the engine-owned HMAC_DRBG and marks
  it properly seeded (`rng_init_done = 2`). `ciuki_tls_create()` injects 32
  fresh bytes from kernel `getentropy()` (syscall 91) before
  `br_ssl_client_reset()` on every new connection, so the ClientHello random
  and the ECDHE key never come from an unseeded generator. A pooled TLS
  connection keeps its seeded engine; the admitted-transport retry path and
  every new connection create a new context and therefore reseed.
- `br_ssl_engine_init_rand()` tries `br_prng_seeder_system()` once per
  engine. The recipe builds BearSSL with `-DBR_USE_URANDOM=0
  -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0` for host and target, so no seeder
  exists: without the kernel injection the reset fails with
  `BR_ERR_NO_RANDOM`, and the injected kernel seed is the only seed. Any
  `getentropy()` failure returns before the context is allocated and erases
  the seed buffer, including partial output.
- [BearSSL PRNG API](https://bearssl.org/apidoc/bearssl__rand_8h.html):
  the engine's HMAC_DRBG is kept; no second generator is added in the port.
- `br_ssl_engine_set_default_rsavrfy` configures RSA signature verification,
  not randomness; `br_ssl_client_init_full()` already installs it
  (`ssl_client_full.c`). TLS 1.2 only, the pinned CA store, hostname and
  date validation and `BR_OPT_NO_RENEGOTIATION` are unchanged from f6-15.

Readiness handling (`sdk/netsurf/port/tls.c`, `net.c`):

- `EAGAIN` from `getentropy()` maps to `CIUKI_TLS_NOT_READY`; every other
  error (for example `ENOSYS`, `EIO`) stays `CIUKI_TLS_ENTROPY`. Neither
  path waits on the browser thread (`ciuki_entropy_wait()` and the aborting
  5 s `arc4random*` helpers are not used), performs DNS, or opens a socket.
- The readiness page text is built by `ciuki_tls_describe()` into the job's
  own buffer: "Secure connections are not ready yet." plus the estimated
  remaining part of the first boot minute, computed from `CLOCK_MONOTONIC`
  (kernel `g_ticks`, milliseconds since boot, `src/kernel/proc/clock.c`).
  This is a heuristic derived from the `[entropy]` design (readiness is
  normally reached within the first boot minute), not a kernel readiness
  query: after sixty seconds the page says the estimate has elapsed and
  readiness is not guaranteed; if the clock is unavailable it says so.
  Reload issues a new fetch and a fresh `getentropy()` call; nothing is
  cached. Found while testing: NetSurf's `browser_window_reload()` re-fetches
  the content URL, which on its internal error pages is
  `about:query/fetcherror`; without the page's form data
  `browser_window_navigate()` returns `NSERROR_NEED_DATA`, so the chrome's
  Reload button was a silent no-op on every error page. The chrome now
  re-navigates to the site URL the core displays (`ciuki_chrome_reload()`,
  no history entry, no POST replay) when the content is an `about:query/`
  page, and reloads normally otherwise.
- Clock provenance is as f6-15 amendment 1 defines it: `uname()`
  `realtime_source == 1` (qualified RTC) and a plausible date, checked before
  the entropy call.

## Host verification

`cd tests/host/netsurf && nice -n 19 python3 -B -m unittest discover`
(results in `build/codex/f6-17-report.md`):

- `tls_entropy_test.c` (`tls-entropy`, wraps `getentropy`, `clock_gettime`,
  `br_ssl_engine_inject_entropy`, `br_ssl_client_reset` at the real BearSSL
  boundary): 32 bytes of the exact kernel output are injected before each
  reset, a different seed on each of three connections, `rng_init_done == 2`
  and `BR_OPT_NO_RENEGOTIATION` at reset; `EAGAIN`, `ENOSYS`, `EIO`, `EINTR`
  never reach inject or reset; a later call recovers (reload); an
  unqualified clock refuses before any entropy call; the readiness text is
  bounded (48 s at 12 s uptime, 1 s at 59 s, elapsed at 60 s, unavailable on
  clock failure).
- `browser_network_test.c` with `stack_fixture.c` (real NetSurf core,
  fetcher, BearSSL engine and the native lwIP socket shim; the fixture CA is
  linked into this harness only, production anchors and the installed
  resources are unchanged):

| Case | Fixture route | Required result |
| --- | --- | --- |
| Trusted HTTPS | `https://fixture.example:8443/page` | Ciuki page rendered |
| Certificate refusal | same route, production roots only | certificate validation error page, no page content |
| HTTP to HTTPS redirect | `http://fixture.example:8080/upgrade` | redirect to `https://.../tls-cookie-start` followed, cookie round trip completes over TLS |
| Cookie over TLS | `https://fixture.example:8443/tls-cookie-start` | `Secure; HttpOnly` cookie returned in `/tls-cookie-end` over TLS |
| 64 KiB gzip over TLS | `https://fixture.example:8443/gzip64` | exactly 65,536 decoded bytes rendered |
| Not ready | `/page` with `EAGAIN` at 12 s uptime | readiness page with "up to 48 seconds", no connection, no request |
| Not ready, late | `/page` with `EAGAIN` at 75 s uptime | readiness page saying the estimate has elapsed |
| Reload | `/page`, `EAGAIN` then ready | readiness page, reload, Ciuki page over HTTPS in the same browser |

## QEMU acceptance (`f6-net`), pending lead integration

The directive's QEMU cases reuse the routes and results of the table above
on the canonical image, bound to its SHA-256 and to the `[entropy]` and RTC
provenance records. They cannot be added as executable `tests/suites/f6-net.json`
cases yet: the suite has only `net-arp` probe cases, the F6 selector accepts
only `net-arp` (`src/kernel/probes/selector.c`), and `scripts/test/run.py`
refuses the user network backend for any other probe. No unexecutable case
was inserted. The lead decides on:

1. A runner case kind that boots the desktop, launches `/Programs/netsurf`
   with a URL and collects the rendered page or its error text as evidence
   (serial markers or a capture file on the overlay).
2. The pinned fixture server on the host side of the user network
   (`guestfwd` to a host TLS server serving the routes above with a pinned
   test CA and server certificate under `tests/host/netsurf` style
   generation), and hostname resolution inside the restricted `slirp`
   network: the browser refuses numeric HTTPS hosts by design, and `slirp`
   offers no hosts override, so the kernel resolver needs a test-only static
   entry carried by the test selector.
3. The test-profile trust store: the browser passes the production anchors
   to `ciuki_net_create(NULL, 0)`; a test-only anchor file must be honoured
   only in the test profile (not a user-visible exception mechanism). Until
   that exists the "trusted HTTPS" QEMU case can only target a public site
   from the private list, and the "certificate failure" case any
   self-signed host.

No image build variant is needed or proposed.
