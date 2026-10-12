# f6-01 — NIC drivers on the registry: Intel 8255x (e100), RTL8139, NE2000 PCI

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` allowed. **Worktree:** `../CiukiOS-wt-f6-01` from `main`. Host-testable (register-level models in the kernel host harness like the ATA/AC97 drivers); the lead qualifies on QEMU (`-device rtl8139`, `-device ne2k_pci`, `-device i82559er`, user-mode networking) and the T23 (PRO/100 VE).

## Scope (independent of the socket API; the netif interface follows `docs/design/network-foundations.md` decisions and must be adjusted to `network-contract.md` when f6-00 lands)

1. Driver framework: PCI probing through the registry (`src/kernel/core/registry*`, firmware-ownership rules), a `struct netif_driver` with init/start/stop/transmit/receive-poll/link-state/MAC, interrupt registration with the IRQ budget rules, receive ring to a bounded packet queue consumed by a kernel thread (the lwIP input later), transmit with completion, statistics, quarantine on fault like the other F1 drivers. Record grammar lines `[net] driver=.. mac=.. link=.. irq=..`.
2. Drivers: Intel 8255x (CU/RU command blocks, EEPROM MAC, link via MII), RTL8139 (receive buffer ring, 4 transmit descriptors), NE2000 PCI (DP8390 register set, page-mode DMA). Datasheet-driven, no code copied from GPL drivers.
3. A kernel loopback/ping probe for the runner: `f6-net` suite cases on the QEMU twins that send an ARP request and receive the reply from QEMU's user-mode stack (`10.0.2.2`), with records `arp=ok mac=..`.
4. Host tests: register models for the three devices (EEPROM read, ring handling, errors, link up/down), framework tests (quarantine, budget, queue bounds).

## Constraints

clang i686 `-Werror`, no QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f6-01-report.md`.
