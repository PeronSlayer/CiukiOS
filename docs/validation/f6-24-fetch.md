# F6-24 fetch investigation

The 14-line directive has no appended amendments or lead notes. The copied
T23 evidence proves a DHCP lease, successful DNS, and ready entropy, but
contains no fetch record and no qualified RTC date. It cannot establish a
TCP or TLS failure. Chrome interaction is owned by f6-23.
The exit file has a plausible `2026-10-22T22:44:43Z` stamp; this alone
establishes neither RTC provenance nor correct UTC calibration.

Research and implementation decisions (before implementation):

- [RFC 9112 sections 7.1 and 9.2](https://datatracker.ietf.org/doc/html/rfc9112):
  retain bounded chunked/gzip decoding and conservative connection reuse.
  Chunk-size lines are body framing, not response header fields; their
  cumulative size must not consume the response header allowance.
- [RFC 9293](https://datatracker.ietf.org/doc/html/rfc9293) and
  [RFC 7323 section 2.2](https://datatracker.ietf.org/doc/html/rfc7323#section-2.2):
  retain MTU 1500, MSS 1460, and the bounded unscaled receive window. A peer
  offering scaling cannot enable scaling unless both SYNs offer it.
- [RFC 6298](https://datatracker.ietf.org/doc/html/rfc6298) and
  [lwIP 2.2.1 TCP source](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp.c):
  keep lwIP's SYN retransmission timers. Retransmission exhaustion calls the
  sequential error callback with ERR_ABRT, not ERR_TIMEOUT. Report an
  uncompleted, nonlocally aborted connect as a timeout.
- [lwIP sequential API source](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/api/api_msg.c)
  and [options](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/include/lwip/opt.h):
  capture TCP connect/reset/timeout at the existing adapter boundary, with
  bounded records per time window; do not add allocating core timers.
- [BearSSL API](https://bearssl.org/api1.html#reset-and-sni) and
  [X.509 validation](https://bearssl.org/x509.html): keep TLS 1.2, SNI,
  hostname/chain verification, the exact bundled roots, qualified RTC time
  and entropy gates. Record raw engine errors and the validation clock.
- [Cloudflare's published suites](https://developers.cloudflare.com/ssl/edge-certificates/additional-options/cipher-suites/supported-cipher-suites/):
  all four existing ECDHE AES-128-GCM/ChaCha20 RSA/ECDSA suites have modern
  overlap. No evidence supports weakening TLS or adding legacy suites.
  [Fastly](https://www.fastly.com/documentation/guides/concepts/routing-traffic-to-fastly/)
  and [Akamai CPS profiles](https://techdocs.akamai.com/cps/docs/cipher-profiles)
  also publish ECDHE-RSA-AES128-GCM-SHA256 support. This is configuration
  overlap, not proof about any particular CDN tenant or its chain.
- [zlib manual](https://zlib.net/manual.html): retain gzip/deflate checksum
  validation and the existing 16 KiB decoded-slice and 8 MiB body limits.

Host verification and remaining hardware/internet evidence are recorded in
`build/codex/f6-24-report.md`. Synthetic Ethernet evidence does not qualify
the physical e100 or a live CDN.

Verification in the implementer worktree:

- Production network compile/link and FPU/SIMD audit passed for 51 translation
  units; the measured ceiling proof is 3,244,032 bytes, below 4 MiB.
- The original three page pixel goldens and both QMP dry-run input plans
  passed. Independent DER/OpenSSL equivalence passed for all 121 bundled
  RSA/EC trust anchors.
- The real-stack gateway cases transferred 32 KiB over HTTP and HTTPS, with
  one lost SYN, peer scaling offered, and full 1500-byte IP packets.
  Stage/privacy, 301/302 HTTPS redirect and rendered DNS/connect/TLS/clock/
  entropy/timeout page assertions completed. The 12,000-chunk regression
  and SYN exhaustion/refusal/summary assertions completed too.
- LeakSanitizer fails at exit under this sandbox's ptrace restrictions.
  Leak detection remains enabled; sanitizer runs are not green evidence.
  Host AF_INET socket creation is also denied with EPERM, so live CDN and
  physical e100 verification remain with the lead.
