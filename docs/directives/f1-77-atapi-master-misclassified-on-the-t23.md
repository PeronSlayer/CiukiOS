# f1-77 — the T23's optical drive is classified as ATA and its channel quarantined since f1-76

- **Phase:** F1 (ATA). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f1-77` from main `2d6990cf`.
- **Authority:** `src/kernel/drivers/ata.c` (discovery signature sampling, EC-abort fallback), `tests/host/ata_test.c`, the f1-76 validation note.

## Evidence (T23, image a79f840c, `legacy/local/physical/a79f840c/serial.log`, three boots, read-only)

```
[ata0] unit=0 status=50 signature=01010000
[ata1] fault unit=0 command=ec issued=1 status=51 error=04 elapsed_ms=1
[ata1] identified=0 quarantined=1
```

On image e8dfd0cd (before f1-76) the same drive logged `[ata1] unit=0 status=00 signature=010114eb`, `kind=atapi type=5 packet_bytes=12`, `identified=1`. f1-76 (two matching signature samples after bounded BSY polling, EC-abort fallback to IDENTIFY PACKET when the error register is 04 and the registers read 01/01/14/EB with BSY/DRQ/DF clear) passed 32 host topologies and the QEMU gates, yet on the real ATAPI master it issues IDENTIFY (EC), receives status 0x51 (DRDY, DSC, ERR) with error 0x04 (ABRT) and quarantines the channel. Either the sampled signature was 01/01/00/00 (the drive presents its signature later than the two samples, or status 0x00 with 14/EB is now rejected), or the fallback's register conditions do not match this drive after the abort (status 0x51: DRDY set; ATA/ATAPI-6 says a PACKET device may set DRDY after an aborted non-PACKET command). The per-unit signature log line is no longer printed before the fault, so the sampled values are unknown.

## Required

1. Restore the per-unit `[ata%u] unit=%u status= signature=` line for every sampled unit (both samples when they differ) so the next T23 log shows what was read.
2. Treat the PACKET signature (LBA Mid/High 14/EB) as decisive regardless of status (0x00 or DRDY/DSC) and of SC/LBA Low transients; keep waiting for BSY to clear. After an EC abort with error ABRT, fall back to IDENTIFY PACKET whenever LBA Mid/High read 14/EB, without requiring DRDY/DSC clear (ATA/ATAPI-6 §§9.12, 8.12, QEMU ide/core.c and the Linux libata-sff classification which accepts both).
3. Host coverage: the T23 sequence (single ATAPI master presenting 14/EB with status 0x00 at the first sample and DRDY|DSC after; EC abort with status 0x51), plus the f1-76 topologies; run the ATA host test, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f1-77-report.md`. The lead runs f1-storage, f4-media optical-l1 and volume-resolution on QEMU and the T23 confirms `kind=atapi`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
