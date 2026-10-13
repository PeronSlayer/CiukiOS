# f1-44 — ATA bus-master DMA (UDMA) on the common IDE controllers

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-44` from `main`. Host-testable; QEMU PIIX3/PIIX4 bus master.

## Why

After f1-39 the T23 reads at about 3.3 MB/s in PIO mode 4 with the CPU in the loop. Every chipset of the baseline (Intel PIIX4/ICH, VIA VT82C586–VT8237, SiS 5513, ALi M5229, AMD 756/768, nVidia nForce) has a PCI IDE bus master (SFF-8038i): PRD tables, UDMA 33–100, one IRQ per transfer, CPU free.

## Task

1. Contract section in `docs/design/` (storage): PCI IDE class 01/01 programming interface bits, bus-master base from BAR4, PRD table rules (64 KiB entries, no 64 KiB boundary crossing, EOT), UDMA mode selection through IDENTIFY words 88/93 and SET FEATURES (03h/4xh) within what the firmware already enabled (cable detection: never program a UDMA mode above the firmware's choice; 80-wire check), fallback to PIO per device on any DMA error with a quarantine record, ATAPI stays PIO in this step.
2. Implementation in `ata.c`/a new `ata_dma.c`: DMA reads and writes for ATA devices, bounce buffers when the request is not physically contiguous, IRQ completion, the storage task unchanged otherwise. Verbose records `[ata%u] dma mode=udma%u …` and the throughput counters in `[launch-io]`.
3. Host tests: synthetic bus master with PRD validation, boundary cases, error and timeout, fallback; the existing fat/throughput tests pass through the DMA path.

## Acceptance

Host suites green; the lead measures on the T23 (target ≥ 15 MB/s on the 1.3 MB ELF) and on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-44-report.md`.
