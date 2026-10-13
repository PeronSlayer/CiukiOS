# f6-02 — Network state for the desktop: `/dev/net` and the tray icon

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f6-02` from `main` (f9f112a0). Host-testable. Files: `src/kernel/drivers/net.c` (+ a small `net_dev.c` modelled on `src/kernel/audio/audio_dev.c`), `src/kernel/include/ciuki/net.h`, the SDK header that exposes the devctl, `apps/desktop/shell.c` (tray only), `shell_icons.h`/`shell-icons.json`/`tango-store.json`, `tests/host/desktop/`, `tests/host/` kernel net tests. Do not edit `compositor.c`, `desktop.c` wallpaper code or `wallpaper.c`.

## Evidence (T23, 2026-10-13)

```
[net] driver=e100 mac=00:d0:59:b6:43:8f link=down irq=11 mode=poll state=running reason=admission
… (cable plugged later)
[net] driver=e100 mac=00:d0:59:b6:43:8f link=up irq=11 mode=poll state=running reason=admission
```

The kernel polls the link every second (`net.c:317`) but nothing reaches the user. Owner request: a network icon in the tray next to the speaker that shows whether the cable is connected, like any OS.

## Task

1. Kernel: public read-only node `/dev/net` (same pattern as `/dev/mixer`: open, devctl, close) with `NET_GET_STATE` → `{version, size, present, driver[16], mac[6], link (0 unknown/1 down/2 up), state (running/quarantined/stopped), reason[24], rx_frames, tx_frames, address_configured=0, ipv4[4]=0}`; consistent with `docs/design/f6-00-network-contract.md` (add the node to that contract; IP fields are reserved for lwIP). No write path.
2. Desktop tray: icon left of the speaker (widen `SH_TRAY_W`), Tango `network-wired` (link up), `network-offline` (link down), disabled/greyed `network-error` when no adapter; tooltip text "Network: cable connected (e100 00:d0:59:b6:43:8f)", "Network: no cable", "No network adapter"; refresh every 2 s through the existing timer/`want_reload` path, damaging only the tray box when the state changes; click opens a popup like the volume one with driver, MAC, link and state (no settings yet). Icons go through `convert_icons.py` like the others and the inventory tests.
3. Host tests: the kernel net host test covers `NET_GET_STATE` for present/absent/quarantined adapters; the desktop host suite covers the three icon states and the popup; `shell_menu.py`/`f2-desktop.json` expectations updated if the tray geometry changed.

## Acceptance

Both suites green; `make CAP= apps` builds; the SDK API reference regenerated (`python3 scripts/gen_api_reference.py --output docs/developer-api.md`).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f6-02-report.md`.
