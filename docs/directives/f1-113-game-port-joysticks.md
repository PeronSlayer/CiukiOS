# f1-113 — Analog game port (201h) and DOS joystick virtualisation; one joystick API for game port and USB

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-113` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. Detect the native game port on ISA 201h and on PCI audio game-port functions (Creative and others in the hardware-id database). Read the analog axes with RC timing calibrated against the PIT (two two-axis or one four-axis stick, four buttons), with a calibration UI in the Control Panel. Publish through the f1-104 joystick API, so that game port and USB sticks look the same to applications.
2. **DOS VMs:** a virtual 201h port and the INT 15h AH=84h services, fed from the selected joystick (game port or USB HID) with the timing DOS games expect. Device Manager and the game's properties choose which joystick.
3. Host tests use an RC timing model with jitter, calibration, the virtual 201h port read by a DOS polling-loop fixture, INT 15h/84h, and USB-to-virtual-port mapping.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-113-report.md`.
