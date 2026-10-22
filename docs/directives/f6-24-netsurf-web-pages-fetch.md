# f6-24 — NetSurf does not open web pages (HTTP/HTTPS fetch) and logs nothing about it

- **Phase/model:** F6, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f6-24` from `main`.
- **Evidence:** T23 trial of image 3423912f… (2026-10-22 ~22:45): the network is up (`[net] dhcp state=bound ip=192.168.1.123 gw=192.168.1.254 dns=192.168.1.254`, `[net] resolver first_answer ip=104.18.5.215 … elapsed_ms=155`, NETSURF.LOG `network=dns-ok`), yet the owner reports that NetSurf cannot open web pages; the logs contain no fetch, connect, HTTP or TLS record at all (serial `build/lead-scratch/t23-3423912f-serial.txt`, disk `build/lead-scratch/t23-3423912f-BOOT.LOG`). The QEMU HTTPS cases of f6-18 run on e1000 with a fixture, never on the T23's e100 against the internet.

## Required

1. **Records:** bounded protected records for every fetch stage in the port: `[netsurf] fetch url=<scheme://host/path-prefix> stage=dns|connect|tls|request|response|redirect|done|error code=… errno=… tls_err=… bytes=… ms=…` (no query strings, at most one line per stage per fetch), and the kernel socket side `[net] tcp connect …`/`reset`/`timeout` summary per window.
2. **Find and fix** why pages do not open on real networks: walk the full path on the host lwIP harness against real-world conditions (TCP to port 80/443 through a gateway, MSS/MTU 1500 with the e100, window scaling, SYN retransmit timers, RST handling, chunked transfer, gzip, redirects 301/302 to HTTPS, SNI, TLS 1.2 cipher suites offered by BearSSL against common CDNs such as Cloudflare/Fastly/Akamai, certificate chain validation with the bundled CA set, the entropy readiness gate, clock: certificate validity checks need the RTC date — check that the system time is right, otherwise TLS fails) and fix each defect found; cite RFCs and the BearSSL/lwIP documentation.
3. **User-visible errors:** every failure shows NetSurf's error page with the stage and reason in plain English (DNS, connect, TLS certificate, timeout).
4. **QEMU evidence for the lead:** extend `scripts/test/netsurf_probe.py` (f6-23 writes it; if absent, write it) with a `--url` option; the lead runs it with QEMU user networking against `http://example.com` and `https://example.com`.
5. Host tests: the fetch records, each fixed defect, the error pages. Touch only fetch/network/TLS files of the port and the kernel network stack — not `chrome.c`/`nsfb_surface.c` (f6-23).

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.
