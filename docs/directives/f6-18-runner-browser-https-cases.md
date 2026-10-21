# f6-18 — QEMU runner support for the browser HTTPS acceptance cases (f6-net suite)

- **Owner:** a Claude agent (Sonnet) while Codex is unavailable; `gpt-6.1-sol` / high otherwise
- **Written:** 2026-10-21 by the lead from the f6-17 report: the `f6-net` suite and the runner support only `net-arp` with the QEMU user network, so the HTTPS acceptance cases f6-17 implemented as host tests cannot run on QEMU yet.
- **Worktree:** `../CiukiOS-wt-f6-18`, branch `wt/f6-18`
- **Read first:** `scripts/test/run.py` (suite definitions, the `f6-net` cases, `device_exceptions.network`, the QEMU argument builder), `tests/suites/f6-net.json`, `docs/validation/f6-17-https.md` (routes and expected results written by f6-17), `docs/design/test-architecture.md` (run-time selection through fw_cfg, qcow2 overlays), the f6-15/f6-17 directives.

Required:
1. A runner case kind that launches the browser in the guest with a URL (selector `f6:browser run=<8hex> url=<…>` registered in the kernel selector, loader table and loader model: the lead reserves the token `f6:browser`; keep the loader within its extent, coordinate with f0-54's budget), and collects the `CIUKI_TEST` records the browser emits for a page outcome (status, final URL, bytes, TLS verdict).
2. A fixture HTTPS server on the host, started by the runner for the case's duration, reachable through `-netdev user,…,guestfwd=tcp:10.0.2.100:443-cmd:…` (or the equivalent `hostfwd`/guestfwd form that works with the pinned QEMU), serving the test pages f6-17 defined (trusted page, redirect, cookies, 64 KiB gzip), with a test-only CA.
3. Name resolution: a test-profile static host entry in the kernel resolver (`fixture.test` → 10.0.2.100) enabled only by the selector, since slirp has no hosts override and the browser refuses numeric HTTPS hosts; a test-profile trust store containing the fixture CA, enabled only by the selector; nothing of this reaches the normal boot path (host tests prove the selector gate).
4. Cases in `tests/suites/f6-net.json`: `https-trusted`, `https-untrusted`, `http-redirect-https`, `https-cookies`, `https-gzip-64k`, `https-not-ready` (readiness page with the entropy not yet ready, if the runner can start the case before the first minute), with expected records; host tests for the suite definition, the selector grammar and the fixture server.

Report `build/codex/f6-18-report.md`. `rg`/`sed` only; no QEMU (the lead runs the suite), no `systemd-run`, no image builds, no commits, no diary edits; English.
