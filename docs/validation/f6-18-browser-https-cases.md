# f6-18 — QEMU runner support for the browser HTTPS cases (`f6-net`)

Written 2026-10-21 (directive `docs/directives/f6-18-runner-browser-https-cases.md`).
The f6-17 HTTPS acceptance cases, which existed only as host tests, become
executable `f6-net` suite cases on the canonical image. No image build variant.

## Selector

`f6:browser run=<8hex> url=<url> [now=1]`, at most 64 bytes (the fw_cfg
request limit of the kernel, `CIUKI_TEST_REQ_MAX`).

- `url`: 8..40 bytes, `http://` or `https://`, host of `[A-Za-z0-9.-]`,
  optional `:port` (1..5 digits), a path of `[A-Za-z0-9_./-]` starting with
  `/`; no query. The charset is the record grammar's value charset
  (`scripts/test/evidence.py`), so the kernel echoes the URL in evidence
  verbatim. With the 28-byte prefix a URL longer than 36 bytes never fits the
  request; the 40-byte cap is the struct bound.
- `now=1`: the probe launches the browser without waiting for the entropy
  pool (the readiness case). Without it the probe waits up to 90 s for
  `entropy_get()` to stop returning `EAGAIN`.
- No `platform=e500` or `safe=1` suffix: the cases run on `qemu-t23` only;
  the runner refuses the e500 profile for a browser case.

Tables: kernel `src/kernel/probes/selector.c` (`f6_names`, `browser_url()`,
`PROBE_BROWSER_NOW`, `probe_selection.url`) and the host model
`scripts/test/loader_model.py` (`BROWSER_SELECTOR_RE`) accept the grammar; the
runner (`scripts/test/run.py`) reconstructs the request with the `url=` and
`now=1` suffixes. The loader validates every selector, fw_cfg ones included,
before it boots the kernel (`menu.inc`, the `.bad` path). The first delivery
kept the loader registration as a patch because the loader had 7 free bytes;
after f0-54 (1,287 free bytes) the loader menu table registers it on the
compact parser (2026-10-21): `f6_browser_name` in the F6 list and
`.browser_suffix` with the exact grammar above for every source (` url=`,
`http://` or `https://`, host `[A-Za-z0-9.-]+`, optional `:` and 1..5
digits, `/`, path `[A-Za-z0-9_./-]*`, then nothing or ` now=1`). The 64-byte
selector cap leaves at most 36 URL bytes, so the 40-byte URL bound always
holds. Cost 224 bytes: 1,063 free with the release version define.
`test_boot_modes.LoaderParserTests` compares the assembled grammar with the
model, and `tests/host/test_runner_selectors.py` no longer lists
`(6, 'browser')` as unregistered.

## Guest side

- Kernel probe `browser` (`src/kernel/probes/f2_probes_desktop.c`,
  `probe_browser`, registered with `CIUKI_F6_PROBE`): write gate on, waits for
  the user network's DHCP address (`net_stack_address`, 30 s), arms the
  test-profile resolver entries `fixture.test` and `untrusted.test` →
  `10.0.2.100` (`net_resolve_override`, `src/kernel/net/lwip/sockets.c`, two
  slots consulted before any DNS query, cleared on every probe exit), waits
  for entropy unless `now=1`, starts the desktop in gate mode
  (`supervisor_spawn_desktop_probe`), sends `GATE_BROWSER` with the URL
  (`apps/desktop/gate.h`), validates the child PID the desktop reports
  (`case=browser child=<pid>`: child of the desktop, native personality,
  live), then waits up to 120 s for the browser's one report
  `case=browser-page status=<n> bytes=<n> tls=<verdict> url=<url>` (probe
  call 3, `native_report`, every field re-validated: status ≤ 999, decimal
  bytes, `[a-z_]` verdict, record-charset URL ≤ 96 bytes) and republishes it
  as `DATA case=page`. Records: `write-gate`, `network`, `resolver`,
  `entropy`, `fixture`, `page`, `ledger`, `END status=`.
- Desktop (`apps/desktop/desktop.c`): `GATE_BROWSER` starts
  `/Programs/netsurf --channel-fd=3 --test=browser <url>` through the ordinary
  `launch_program` path (same environment, channel and client slot as a menu
  launch; the gate launch alone treats `netsurf` as graphical since it has no
  catalogue entry before M4) and reports the PID.
- Browser (`sdk/netsurf/port/testmode.inc`, included by `chrome.c`;
  `testmode.h` for the fetcher): `--test=browser` arms the profile only when
  the kernel test request read through probe call 5 starts with
  `f6:browser run=<8hex> url=` (`ciuki_test_gate`); the argument alone enables
  nothing. Active profile: `fetch_ciuki.c` creates the network with the fixture
  CA anchors (`test_anchors.h`, generated at build time by
  `sdk/netsurf/anchors.c` with the `ciuki_test` prefix from
  `tests/suites/fixtures/f6-browser/ca.pem`; `build.py` records it under
  `netsurf-trust.json` `test_profile` and in the recipe inputs) instead of the
  production bundle; every terminal http/https fetch is observed (status,
  decoded body bytes, TLS verdict, error text); the first throbber stop sends
  the one report. Verdicts: `ok`, `none` (plain HTTP), `error` (HTTP-level
  error with TLS ok), `memory`, `clock`, `entropy`, `host`, `cert`, `date`,
  `protocol`, `io`, `eof`, `not_ready` (f6-17's `CIUKI_TLS_NOT_READY`),
  `other`.

Nothing above runs on a normal boot: the probe exists only under the test
request, the resolver slots are empty unless the probe arms them, and the
browser's profile needs the kernel test request.

## Host side

`scripts/test/https_fixture.py` (standard library): per browser case the runner
starts one `FixtureServer` on 127.0.0.1 (ephemeral ports; HTTP/1.1 keep-alive;
TLS 1.2 only, as BearSSL offers) and passes
`-netdev user,id=ciuknet0,restrict=on,guestfwd=tcp:10.0.2.100:80-cmd:<relay>,guestfwd=tcp:10.0.2.100:443-cmd:<relay>`;
`<relay>` is `python3 scripts/test/https_fixture.py relay 127.0.0.1 <port>`,
which libslirp runs through `/bin/sh -c` for every guest connection
(`restrict=on` keeps explicit forwarding rules). SNI `fixture.test` gets the
CA-signed certificate, every other name the self-signed `untrusted.test`
certificate. The request log goes to `result.json` (`browser_fixture`) and
`fixture.log` of the run.

| Case | Selector URL | f6-17 route | Required `DATA case=page` |
| --- | --- | --- | --- |
| `https-trusted` | `https://fixture.test/page` | `/page` | `status=200 tls=ok url=https://fixture.test/page bytes≥100` |
| `https-untrusted` | `https://untrusted.test/page` | `/page`, production roots only | `status=0 tls=cert bytes=0` |
| `http-redirect-https` | `http://fixture.test/upgrade` | `/upgrade` → `/tls-cookie-start` | `status=200 tls=ok url=https://fixture.test/cookie-end` |
| `https-cookies` | `https://fixture.test/cookie` | `/tls-cookie-start` → `/tls-cookie-end` | `status=200 tls=ok url=https://fixture.test/cookie-end` |
| `https-gzip-64k` | `https://fixture.test/gzip64` | `/gzip64` | `status=200 tls=ok bytes=65536` |
| `https-not-ready` | `https://fixture.test/page now=1` | `/page` with `EAGAIN` | `status=0 tls=not_ready bytes=0`, `entropy waited=0` |

Route names are shorter than f6-17's host-test names so that every selector
fits 64 bytes (`/tls-cookie-start` would make the cookies selector 65 bytes);
`/cookie` sets `ciuki=1; Path=/; Secure; HttpOnly` and redirects to
`/cookie-end`, which answers 200 only when the cookie comes back over TLS
(403 otherwise, and over plain HTTP). All six cases also require
`network address=10.0.2.15 dhcp_error=0` (slirp's DHCP), `resolver armed=0`,
`fixture path=/Programs/netsurf`, `ledger objects_equal=1 processes_equal=1`
and `END status=PASS`.

## Host verification (this worktree, 2026-10-21)

- `tests/host/test_browser_runner.py` (12 tests): the grammar in the model,
  the runner and the kernel (`selector.c` through ctypes) accept and refuse
  the same requests, including the 64/65-byte boundary; `qemu_args` emits the
  two `guestfwd` entries, refuses a browser case without a NIC, without
  fixture ports, with a non-F6 probe or on the e500 profile; the six suite
  cases' predicates accept the documented outcomes and reject the wrong
  verdict, scheme, status, byte count and a ready case that did not wait; the
  fixture server over Python's `ssl`: trusted page on TLS 1.2 with the fixture
  CA only, `untrusted.test` refused by a verifying client and served the
  self-signed DER, redirect and Secure-cookie round trip, 65,536 decoded gzip
  bytes, 404, request log; the relay pumps a request over a socketpair as
  libslirp does.
- `tests/host/netsurf/test_testmode.py`: the `.inc`'s gate (argument alone
  never arms; foreign or short requests refused; 64 accepted, 65 refused),
  sanitiser and report format, with every verdict inside `[a-z_]`.
- `tests/host/lwip/run.py` (host, ASan) with the resolver override checks:
  grammar refusal, two slots, no DNS query for an overridden name, clearing;
  `--kernel` (target flags) still links the 51 units.
- `test_runner_selectors`, `test_boot_modes`, `test_nic_runner` (ARP cases
  only), `test_runner` (91), `tests/host/dos/test_protocol` (kernel selector
  C test), `tests.host.win32.test_core_observers`, `record_scope_test.py`,
  `tests/host/fs/kernel_compile.py` (51 TUs with the canonical flags): green.
- Syntax checks against the SDK (main tree `build/tools/ciuki-sdk`):
  `apps/desktop/desktop.c` clean; `sdk/netsurf/port/fetch_ciuki.c` clean;
  `chrome.c` shows only the pre-existing `NSOPTION_fb_toolbar_layout`
  frontend-option diagnostic outside this change. The target NetSurf and
  desktop payloads were not rebuilt here (no image builds in this worktree).
- `sdk/netsurf/anchors.c` with no prefix reproduces the production
  `anchors.h` byte for byte.

QEMU evidence is the lead's: with the loader registration above,
`python3 scripts/test/run.py --suite f6-net` on the canonical image
runs the 24 ARP cases and these six.
