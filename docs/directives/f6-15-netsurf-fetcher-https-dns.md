# f6-15 — NetSurf M2: CiukiOS HTTP/1.1 fetcher on the native sockets, HTTPS through BearSSL with the pinned CA bundle, DNS through `resolve4`, cookies and gzip

- **Owner:** Codex, `gpt-6.1-sol`, effort `xhigh`, `--search` allowed (NetSurf fetcher API in the pinned sources, BearSSL documentation, RFC 7230/7231/6265)
- **Written:** 2026-10-21 by the lead; **launch after f6-10 and f6-14 are on `main`** (the worktree branches from that `main`)
- **Worktree:** `../CiukiOS-wt-f6-15`, branch `wt/f6-15`
- **Plan and decisions:** `docs/design/netsurf-port-plan.md` ("CiukiOS fetcher", "HTTP/1.1 behavior", "HTTPS and prerequisite failures") and the lead decisions in `docs/directives/f6-13-netsurf-port-plan.md` (TLS 1.2 only, no revocation, 8 connections, 30 s timeouts, keep-alive, 5 redirects, bounded URL-encoded POST, no proxy/multipart/auth; dated-clock refusal).

## Required

1. **Fetcher** `content/fetchers/ciuki` registered for `http:` and `https:` through NetSurf's fetcher API (the plan's boundary): connection pool on the f6-10 sockets (78–89) with the port-local `fd_set` bridge and the bounded DNS worker over `resolve4`; HTTP/1.1 with keep-alive, chunked transfer, `Content-Length`, redirects (5), `gzip`/`deflate` content coding through the pinned zlib, request/response header limits, the plan's caps and timeouts; cookies through NetSurf's own cookie code; `data:` and `file:` untouched.
2. **HTTPS** through BearSSL (`third_party/bearssl`, built for the target by the SDK recipe): TLS 1.2 only, the pinned CA bundle (`third_party/cacert`) converted at build time into BearSSL trust anchors, hostname verification (SNI), no revocation; a clock that reads an implausible date (RTC year < 2026) makes HTTPS fail with a dated-clock error page, never a weaker check; entropy per f6-10 decision 3.
3. **Error UX**: NetSurf error pages for DNS failure, refused/timed-out connections, TLS failures (certificate, hostname, protocol), dated clock, offline (no address on the interface), in English.
4. **Tests (T0, host)**: a scripted HTTP/1.1 server fixture on the host netif shim (keep-alive, chunked, gzip, redirects, cookies, slow and truncated responses, header limits), a BearSSL test server with the fixture CA (valid, expired, wrong host, self-signed, TLS 1.0-only → refused), DNS stub failures, the dated-clock case; the M1 goldens kept green; `make CAP= apps` once. No QEMU: the lead runs QEMU user networking (`-netdev user`, e1000) and the T23 against the private site list.

## Constraints

Upstream NetSurf files unmodified (patch files with reasons); `rg`/`sed` only; no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; English. Report `build/codex/f6-15-report.md`.

## Amendment 1 (lead, 2026-10-21): findings of the Codex read-only cross-review of the delivery (d38119b5)

Apply this amendment only. Sources: RFC 9112 §9.2 (connection reuse), the zlib manual (`Z_BUF_ERROR` with no progress), BearSSL API (validation time), NetSurf `urldb.c` (cookie iteration).

1. **P1 — unsolicited bytes on pooled connections become another URL's response** (`sdk/netsurf/port/net.c:148,222,262`): idle sockets are not monitored, so a server's extra response is consumed by the next request. Monitor idle connections and refuse reuse when application data, EOF or an error is pending (including TLS-buffered plaintext); re-check before sending; fixture with an unsolicited response after response A.
2. **P1 — domain cookies bypass the 200-cookie limit** (`fetch_ciuki.c:42`): `urldb_iterate_entries_path()` visits only leaf nodes while domain cookies sit on the host root path. Patch the iterator (in our port layer or a documented upstream patch in the pinned tree) to visit cookies on every path node; test >200 accepted domain cookies after adding URL children.
3. **P1 — TLS accepts the build-time fallback clock** (`tls.c:58`, kernel `src/kernel/proc/clock.c:44` falls back to the build epoch): require qualified clock provenance (`uname().realtime_source` = RTC) besides a plausible date; refusal case with an invalid RTC and a recent build epoch, with a clear user-visible error.
4. **P2 — queued fetches allocate ~140 KiB before admission** (`net.c:73`, `fetch_ciuki.c:137`): pending-fetch allocation budget; defer parser/request buffers until admission; size requests to their actual length; test resource fan-out under the budget.
5. **P2 — valid chunked gzip fails at the 16 KiB decoded-slice boundary** (`http.c:179`): count parser-state transitions as progress before the no-progress guard; test a 16 KiB expansion with the gzip trailer in the next chunk.
6. **P2 — idle expiry and offline classification** (`net.c:222,230`): service pool expiry independently of active fetches and check age before reuse; classify failed external lookups as offline using the existing read-only `/dev/net` `NET_GET_STATE` (`address_configured`), preserving loopback; hostname-offline fixture. The error page shown to the user is asserted by `tests/host/netsurf/test_network.py`, not only the exit code.

Also add the missing cases the review lists: RSA/EC root equivalence against the production anchors, malformed and intermediate chains, certificate-buffer overflow, TLS truncation and reuse, DNS TTL/server change, navigation during a blocked lookup. Verification as before (`cd tests/host/netsurf && python3 -B -m unittest discover`); report section "Amendment 1".

## Amendment 2 (lead, 2026-10-21): re-review of the amendment-1 delivery (a292f2b6): one P1 left, one coverage regression

Apply this amendment only.

1. **P1 — TLS reuse window** (`sdk/netsurf/port/net.c:267`, `tls.c:111`): `j->sent` advances when plaintext enters BearSSL, while the socket send happens in a later pump; an unsolicited response arriving before the first socket send is accepted as B's response because the cleanliness checks are skipped once `sent != 0`; and `clean_socket()` does not reject incomplete incoming ciphertext already buffered inside BearSSL. Track the first transport transmission separately from plaintext submission (checks run until the first ciphertext byte has actually been sent), and treat any buffered or partial incoming record in the BearSSL engine as "not clean" for reuse. Deterministic fixtures for both timings (unsolicited bytes after plaintext submission but before the first send; a partial record consumed by the pump before reuse).
2. **P2 — coverage regression** (`tests/host/netsurf/network_test.c:161`): the stale cases now expect a fresh connection and no longer exercise the stale-GET retry and the POST-no-replay behaviour. Restore those as separate cases that fail an already admitted transport; keep the new disposal cases.
3. The browser-network test for navigation during a blocked lookup should also assert the browser's behaviour when the late result arrives (ignored, no navigation, no crash).

Report section "Amendment 2".
