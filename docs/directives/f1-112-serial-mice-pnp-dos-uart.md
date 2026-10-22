# f1-112 — Serial PnP enumeration, serial mice and virtual UART/INT14 leases for DOS VMs

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-112` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. **PnP resources.** Take COM resources from the f1-109 inventory (PNP0500/PNP0501), as the f1-111 report asked, and reconcile them with the BIOS data area.
2. **Serial PnP enumeration** following Microsoft's "Plug and Play External COM Device Specification" 1.00: DTR/RTS toggling and ID string parsing. **Serial mice:** Microsoft 2-button, Logitech 3-button and Mouse Systems 5-byte protocols, wheel where identified. Serial mice deliver through the existing input path.
3. **Virtual UART for DOS VMs.** A leased 16550 at the VM's COM1–COM4 with IRQ 3/4 and the BIOS INT 14h services, bound to a native COM port or to a null device. Lease conflicts between native applications and VMs are refused with a reason, and Device Manager shows the owner.
4. Host tests cover PnP ID fixtures, the mouse protocol decoders, the virtual UART register model (THR/RBR/IIR/LSR/MSR, FIFO, IRQ) and the lease rules.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-112-report.md`.
