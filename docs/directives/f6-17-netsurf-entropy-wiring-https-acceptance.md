# f6-17 — NetSurf HTTPS: seed BearSSL from the kernel entropy (f6-16), readiness handling, HTTPS acceptance cases

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-21 by the lead after merging f6-15 (fetcher, fails closed without secure entropy) and f6-16 (`getentropy` syscall 91, ChaCha20 DRBG over the jitterentropy collector, `EAGAIN` until ready).
- **Worktree:** `../CiukiOS-wt-f6-17`, branch `wt/f6-17`
- **Read first:** `sdk/netsurf/port/tls.c` (the fail-closed entropy check and the BearSSL engine setup), `sdk/libciuki/entropy.c` (`getentropy`, `ciuki_entropy_wait`, `arc4random*` with the 5 s deadline), `docs/design/kernel-entropy.md`, `docs/directives/f6-15-*.md` amendments 1–3, `docs/directives/f6-16-*.md`, the BearSSL API for `br_ssl_engine_inject_entropy`/`br_ssl_engine_set_default_rsavrfy` and the PRNG seeding (`br_hmac_drbg`).

Required:
1. Seed BearSSL's client context from `getentropy()` (≥ 32 bytes injected before the handshake; reseed per connection); keep the fail-closed path for `EAGAIN`: the browser shows the existing "secure connections are not ready yet" page with the remaining wait estimate (the kernel's `[entropy]` readiness is usually reached within the first boot minute) and retries automatically when the user reloads; never fall back to a weak seed.
2. Clock provenance stays as f6-15 amendment 1 defines it (RTC only).
3. Acceptance cases for the lead's QEMU run (`f6-net` suite, the runner's user-mode network): `https://` to the pinned test server fixture in the suite (self-signed CA added to the test store only in the test profile), certificate failure page, redirect http→https, cookie round trip over TLS, a 64 KiB gzip body over TLS; host tests for the seeding path (injection before handshake, no handshake without entropy, reseed per connection).
4. Documentation: `docs/desktop.md` browser section says what HTTPS needs (RTC-set clock, entropy ready) and what the error pages mean.

Report `build/codex/f6-17-report.md`. `rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; English.
