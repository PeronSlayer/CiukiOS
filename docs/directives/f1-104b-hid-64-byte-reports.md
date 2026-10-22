# f1-104b — USB HID reports up to 64 bytes (full-speed maximum) on UHCI, OHCI and EHCI

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f1-104b` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. The f1-104 parser limits reports to 16 bytes including the report ID, because UHCI/OHCI private interrupt payloads live in 48-byte DMA slots. Raise the interrupt payload to the 64-byte full-speed maximum packet. Reallocate the DMA slots with the existing ledger accounting, measure the memory delta, and do not raise any global memory limit.
2. NKRO gaming keyboards, gamepads with long reports and composite devices then work.
3. Host tests use a descriptor corpus with 32- and 64-byte reports, adjacent-slot guard checks, and the existing HID and USB suites.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-104b-report.md`.
