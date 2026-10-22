# f1-109 — ACPI and PnP read-only inventory with ACPICA 20260930 and the CiukiOS OS Services Layer

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/f1-109` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for peripherals (mouse, keyboard, serial ports, …) so CiukiOS works on any retro PC.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions at the end), binding; read the row for this directive, the protocol section of its class and the device-model section completely.

## Required

1. Vendor the audited ACPICA 20260930 runtime manifest, exactly the files the source audit lists, under `third_party/acpica/`. Keep the original notices and elect GPL-2.0 as the audit prescribes. Add a notice to the credits.
2. Write the Ciuki OSL: work queue, waits, locks, monotonic timer, bounded allocation, and port, MMIO and PCI access checked against registry ownership. Map tables read-only, preserve ACPI NVS, and enforce execution budgets with no new panic path.
3. **Read-only only:** table discovery and validation, namespace load, the device hierarchy, and `_HID`/`_CID`/`_CRS` resources. Do **not** call ACPI_ENABLE, install the SCI, or run event activation; that is f1-110.
4. Device Manager shows firmware-described devices and power capabilities through a paged inventory ABI, written into the contract first.
5. Host tests on synthetic tables and AML (DSDT and SSDT fixtures) cover namespace walk, resources, malformed tables, out-of-range addresses, budget expiry and allocation failure.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-109-report.md`.
