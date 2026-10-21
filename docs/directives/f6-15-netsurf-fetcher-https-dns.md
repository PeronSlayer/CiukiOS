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
