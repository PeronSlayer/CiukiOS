# f1-114 — Parallel port: SPP and bidirectional first, EPP/ECP next; INT 17h and raw leases for DOS VMs

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-114` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. Detect LPT ports from the BIOS data area, ISA PnP (f1-109 PNP0400/0401) and PCI parallel cards. Support SPP and PS/2 bidirectional modes first, then EPP and ECP with FIFO and DMA, with stop proof.
2. **Native:** a `/dev/lptN` device with an ABI written into the contract first: write with timeouts, status and init.
3. **DOS VMs:** INT 17h printer services and a raw register lease for one VM at a time for dongles and devices, with the honest limitation that timing-sensitive dongles are device-specific.
4. Host tests use the register model for SPP, PS/2, EPP and ECP, BUSY/ACK handshakes, timeouts, FIFO and DMA stop, and the INT 17h fixture.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-114-report.md`.
