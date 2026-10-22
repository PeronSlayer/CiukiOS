# f1-104 — Common USB HID report parser: report keyboards, consumer keys, joysticks and gamepads; versioned input API

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-104` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for peripherals (mouse, keyboard, serial ports, …) so CiukiOS works on any retro PC.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions at the end), binding; read the row for this directive, the protocol section of its class and the device-model section completely.

## Required

1. Write the versioned input ABI (keyboard usages, consumer controls, joystick axes, hats and buttons) into the contract's Input section first.
2. Add a bounded, common HID report-descriptor parser in `src/kernel/usb/`, replacing the mouse-only parser without changing mouse behaviour: global and local items, report IDs, usage ranges, logical and physical limits, and arrays versus variables. Use it for:
   - report-protocol keyboards, with boot protocol kept as the fallback;
   - Consumer page keys mapped to explicit actions (volume up, volume down, mute, play/pause, next, previous) routed to the existing volume and media services;
   - generic desktop joysticks and gamepads, with normalised axes, hat and buttons published through the new API.
3. Host tests use a descriptor corpus (boot keyboard, NKRO report keyboard, consumer control, composite keyboard plus mouse, two-axis joystick, gamepad with hat) and malformed descriptors refused without writes.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-104-report.md`.
