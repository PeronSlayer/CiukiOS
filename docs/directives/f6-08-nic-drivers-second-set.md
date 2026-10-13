# f6-08 — Native NIC drivers, second set: Intel PRO/1000, AMD PCnet, DEC/Intel Tulip, 3Com 3C905, VIA Rhine

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f6-08` from `main`. Host-testable; QEMU `e1000`, `pcnet`, `tulip`.

## Task

Five `netif` drivers beside `net_e100.c`, `net_rtl8139.c`, `net_ne2000.c`, following the F6 network contract (`docs/design/network-contract.md`: admission, link polling, quarantine, bounded queues, no DMA before descriptors): `net_e1000.c` (8254x, QEMU e1000), `net_pcnet.c` (AMD 79C970/973, QEMU pcnet), `net_tulip.c` (DEC 21140/21143 and Intel equivalents, QEMU tulip), `net_3c90x.c` (3Com Boomerang/Cyclone/Tornado, hardware only), `net_rhine.c` (VIA VT86C100A/VT6102/VT6105, hardware only). Research each from the vendor manuals and the Linux drivers (GPLv2, cite; implement from the manuals). MAC, link state, send/receive, counters, the `[net]` record, Device Manager names from the hwids database. Host tests with synthetic register/DMA models per driver in `tests/host/net/`.

## Acceptance

`tests/host/net/run.sh` green; i686 kernel compiles; the lead verifies link and ARP on QEMU for the three emulated devices.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f6-08-report.md`.
