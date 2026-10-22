# f1-106 — USB controller inventory (f1-105) and the EHCI high-speed engine

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-106` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for peripherals (mouse, keyboard, serial ports, …) so CiukiOS works on any retro PC.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions at the end), binding; read the row for this directive, the protocol section of its class and the device-model section completely.

## Required

1. **f1-105 inventory.** Inventory controllers and companions with truthful route-only status in Device Manager, including chipset ownership quirks. Extend `usb_core.c:ehci_route`; never add a second handoff owner.
2. **f1-106 engine.** An EHCI control, bulk and interrupt engine with asynchronous and periodic schedules, unlink and stop, companion routing and port ownership. Add transaction-translator support for full- and low-speed devices behind high-speed hubs, or report it as unsupported until implemented.
3. Mass storage (BOT) and HID run on EHCI ports. The DMA stop proof and quarantine follow the existing USB contract.
4. Host tests use the EHCI register and memory model: enumeration, bulk transfers with short packets and errors, interrupt polling, unlink, controller stop, companion hand-back, and the BIOS semaphore handoff.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-106-report.md`.
