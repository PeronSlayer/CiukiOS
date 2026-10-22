# f1-103 — PS/2 IntelliMouse wheel and Explorer five-button negotiation

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f1-103` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for peripherals (mouse, keyboard, serial ports, …) so CiukiOS works on any retro PC.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions at the end), binding; read the row for this directive, the protocol section of its class and the device-model section completely.

## Required

1. In `src/kernel/drivers/i8042.c`, negotiate AUX ID 0, 3 and 4 with the sample-rate sequences of the Microsoft IntelliMouse specification, as the contract's Input section prescribes: 200/100/80 for ID 3, then 200/200/80 for ID 4. Decode four-byte packets with the wheel and buttons 4 and 5, and resynchronise on a bad packet. Preserve the E500 firmware-first matching, the command/reply separation and the existing quarantine.
2. Deliver wheel (`INPUT_WHEEL`) and buttons 4 and 5 through the existing input events, and record `[i8042] aux id=<n> protocol=<name>`.
3. Host tests on the existing i8042 model cover ID 0, 3 and 4 negotiation, refusal paths, packet decode, resync after lost bytes, and an unchanged three-byte path.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-103-report.md`.
