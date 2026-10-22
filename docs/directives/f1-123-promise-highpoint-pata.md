# f1-123 — Promise and HighPoint add-in PATA controllers (plain disks)

- **Phase/model:** F1, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-123` from `wt/f1-108`, whose chipset modules, native-mode resources and mode contract this builds on.
- **Owner (2026-10-22):** universal drivers for any retro PC. Every licence is allowed; keep notices, origin lines and credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md`, row f1-123 and the storage section, plus the f1-108 mode and resource contract.

## Required

1. Add backends for Promise Ultra33/66/100/133 (PDC20246/20262/20265/20267/20268/20269/20271/20275/20276/20277) and HighPoint HPT366/368/370/372/374, as plain-disk PATA controllers on native PCI resources. Use the Linux `pata_pdc202xx_old`, `pata_pdc2027x`, `pata_hpt366`, `pata_hpt37x` and `pata_hpt3x2n` drivers as donors.
2. Cover timing and PLL setup per chip, cable detection and UDMA mode negotiation with the f1-108 rules, with an explicit PIO fallback.
3. **RAID metadata:** a disk that is a member of a RAID set is reported and never written through a single-disk path.
4. Disks on these controllers mount like any ATA disk. Setup (f9) can install to them only after the lead's later qualification; record that gate.
5. Host tests use per-chip register and PLL models, mode matrices, RAID-member refusal and quarantine.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Host tests one at a time with `nice -n 19`. Write `build/codex/f1-123-report.md`.
