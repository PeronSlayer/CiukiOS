# f1-108 — ATA chipset timing, cable detection and native-mode resources (PIIX/ICH, then VIA, SiS, ALi, NVIDIA)

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning xhigh, `--search`. **Worktree:** `wt/f1-108` from `main`.
- **Owner request (2026-10-22 ~23:20):** universal drivers for the peripherals of any retro PC, not only the T23. **Owner (2026-10-22 23:55):** every licence is allowed; keep notices and origin lines, and add every imported file to the credits.
- **Contract:** `docs/design/peripheral-driver-coverage.md` and `docs/design/peripheral-driver-source-audit.md` (f1-102, with the lead decisions and the owner's licence decision at the end), binding. Read this directive's row, its class section and the device-model section completely. For audio, also read `docs/design/f4-audio-media-contract.md` and `docs/design/f4-audio-second-set.md`: the transport, mixer, DOS SB16 virtualisation and reporting rules every audio engine follows.

## Required

1. Follow the contract's storage section: a per-chipset timing and mode negotiation contract written into the contract first.
   - Implement PIIX/ICH first, then VIA, SiS, ALi and NVIDIA in separate, separately tested modules, with the Linux `ata_piix`, `pata_via`, `pata_sis`, `pata_ali` and `pata_amd` drivers as donors.
   - Cover cable detection (80-wire), UDMA mode selection with drive IDENTIFY, native-mode PCI resources, and errata.
2. The PIO fallback stays explicit and recorded.
3. Never change a mode while I/O is in flight. The existing DMA and quarantine rules hold.
4. Host tests use per-chipset register models, IDENTIFY fixtures, cable and mode matrices, and errata paths.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, the donor driver files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f1-108-report.md`.
