# f1-76 — ATA discovery issues IDENTIFY to an ATAPI master when a second ATAPI unit shares the channel

- **Phase:** F1 (ATA). **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f1-76` from main `56f8b659`.
- **Authority:** `src/kernel/drivers/ata.c` (`ata_channel_discover` two-pass signature read, `identify`, `identify_packet`), `tests/host/ata_test.c`, `docs/validation/` ATA records (amend).

## Evidence (QEMU `qemu-t23`, case `volume-resolution-cd-usb-qemu-t23` of `tests/suites/f4-media.json`: QEMU's default empty CD on the secondary master plus the xorriso fixture as `ide-cd` on the secondary slave; same on images a34118c1, 401f3caf, 64a531f9, 27b35378)

```
[ata1] fault unit=0 command=ec issued=1 status=41 error=04 elapsed_ms=0
[ata1] identified=0 quarantined=1
```

With one ATAPI unit on the channel (every other case and the T23) the same code logs `[ata1] unit=0 status=00 signature=010114eb` and `kind=atapi`. With two ATAPI units the master is classified as ATA and receives IDENTIFY (0xEC), which an ATAPI device aborts (status 0x41, error 0x04 ABRT), and the whole channel is quarantined: the CD never mounts (`[storage] optical slot=3 drive=D … reason=mounted` missing). The two-pass discovery (5e494aa3) reads each unit's signature right after SRST; with two ATAPI devices the master's signature registers read 00/00 by the time the slave has been selected and the master re-selected, or the first read happens while the master still presents BSY/00 (ATA/ATAPI-6 9.12, 9.1: a PACKET device may take longer to present its signature after SRST and clears DRDY).

## Required

1. Reproduce on the host ATA model with two ATAPI units on one channel (and with ATA master + ATAPI slave, ATAPI master + ATA slave, two ATA units) through the production `ata_channel_discover`; identify why the master's signature reads as ATA.
2. Fix against ATA/ATAPI-6 §9.12 and the QEMU ide/core.c reset behaviour: wait for the reset signature per unit (BSY clear and the signature registers stable) before recording it, keep the two-pass structure (no command before all signatures are read), and if a unit answers IDENTIFY with ABRT and presents the PACKET signature, fall back to IDENTIFY PACKET instead of quarantining the channel.
3. Host coverage for the four topologies; run the ATA host test, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f1-76-report.md`. The lead reruns `volume-resolution-cd-usb-qemu-t23`, `optical-l1-qemu-t23` and f1-storage on QEMU.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
