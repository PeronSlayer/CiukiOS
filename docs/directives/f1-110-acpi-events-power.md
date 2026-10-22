# f1-110 — Generic ACPI events and power: SCI/GPE, EC, power button, lid, AC, battery, thermal, soft-off

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-110` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md`, with the lead decisions and the owner's licence decision at the end, binding. Read this directive's row, its class section and the device-model section completely. For DOS VM services, also read `docs/design/unified-dos-contract.md`: the native VMM owns the hardware, and VMs get leased virtual devices in the Windows 95/98 V86 model, not NT NTVDM.

## Required

1. On the f1-109 read-only inventory, add ownership takeover as the contract's Power section and lead decision 3 prescribe:
   - ACPI_ENABLE with a bounded wait, then SCI routing through the IRQ registry;
   - fixed events and GPE dispatch;
   - the generic EC transaction service on 62h/66h, with the ThinkPad EC moved behind it as a quirk so that there is never a second EC engine.
2. Implement the power button (orderly shutdown request to the desktop), lid status, AC adapter (`_PSR`) and control-method batteries (`_BIF`/`_BIX`, `_BST`, with unit validation).
3. **Thermal zones** (ACPI 6.5 ch. 11): trip points, passive and active cooling evaluation, and firmware policy kept where unsupported. This is a prerequisite for takeover.
4. Soft-off (S5) after the desktop's application closure, native flush and quiesce, and `_PTS`/`_S5`. No suspend.
5. **Desktop surface:** a battery and AC tray indicator and a Power page in the Control Panel showing what firmware reports. Unknown values stay unknown.
6. Host tests use AML fixtures (button, lid, battery, thermal and EC operation regions), event storms with bounded handling, takeover failure fallback to firmware ownership, and shutdown ordering.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (specifications, datasheets, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-110-report.md`.
