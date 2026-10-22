# f1-111 — UART generation detection, COM resources and the native COM ABI with logger arbitration

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-111` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for peripherals (mouse, keyboard, serial ports, …) so CiukiOS works on any retro PC.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions at the end), binding; read the row for this directive, the protocol section of its class and the device-model section completely.

## Required

1. Detect 8250, 16450, 16550, 16550A, 16650 and 16750 as the contract's Serial section prescribes. Find COM resources from the BIOS data area, ISA PnP and PCI serial cards.
2. Write the native COM ABI into the contract first: open/close, line settings, modem control and status, timeouts, and bounded RX/TX queues.
3. Implement an IRQ-driven RX/TX driver beside the kernel log transmitter, with arbitration so that a COM port opened by an application never corrupts or blocks the boot log. The log keeps one port, and the record states which.
4. Device Manager lists COM ports with resources and generation.
5. Host tests use the UART model for every generation, FIFO trigger levels, overrun and break, IRQ sharing on 3 and 4, open/close loops, and the logger arbitration.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-111-report.md`.
