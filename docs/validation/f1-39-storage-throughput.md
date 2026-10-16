# F1-39 storage throughput: implementation decisions

Host implementation record; QEMU and T23 timing qualification remain with the
lead. No hardware throughput result is claimed here.

Research consulted before implementation:

- [T13 ATA/ATAPI-6, 1410D revision 3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf):
  8.15.22 (word 47 maximum block size), 8.15.28 (word 59 current setting
  validity), 8.49 (C6h SET MULTIPLE MODE), 8.30/8.31 (C4h/29h READ
  MULTIPLE and EXT), 8.60/8.61 (C5h/39h WRITE MULTIPLE and EXT), 9.4–9.6
  (non-data, PIO input/output handshakes), 10.2.2/table 67 (bus timing).
- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
  “FAT Data Structure” and “File/Directory Size” determine cluster links,
  cluster-to-sector translation and the directory size that bounds file reads.

The repository previously read FAT data via one cache lookup and one disk
command per 512-byte sector. ATA already accepted a sector count, but waited
for one IRQ per sector and imposed a software delay after every word.

Decisions within directive f1-39:

- Gather full sectors across consecutive FAT cluster numbers, bounded by
  the file size, chain-walk limit and 64 KiB. Partial head/tail sectors keep
  the existing sector path. Fragmented links end a run.
- Read missing contiguous sectors directly into the caller's bounded buffer,
  then populate the existing shared cache. Copy all cache hits first,
  including dirty hits, before eviction can displace them; split disk reads
  at hits. Hold the cache mutex throughout and publish no failed read.
  This follows `vfs-storage-contract.md`, “Layers”, “Open-file model” and
  “Block cache and write ordering”: one cache, shared bytes, sticky write
  errors, original sector writes and durability barriers. The existing mixed
  slot layout and pinned failed dirty blocks follow `f4-acceptance.md`,
  “f4-04”, implementation item 9 (“Cache slots on demand”). There is no private
  file cache or speculative read-ahead.
- Configure the advertised power-of-two multiple block size through C6h;
  word 59 bit 8 qualifies only its current-setting field, not word 47's
  support. Zero/invalid support keeps READ/WRITE SECTORS. An issued failure
  still quarantines the channel and never retries. EXT commands keep the
  existing high-then-low task-file programming.
- Use string port I/O for native ATA data, leaving firmware-configured
  chipset DIOR-/DIOW-/IORDY timing intact. Keep command and post-block pacing,
  bound each string burst to 2 KiB and yield between bursts/DRQ blocks with
  interrupts enabled. IDENTIFY/ATAPI data handling stays unchanged.
- Run boot-log draining and delayed cache writeback in the existing
  device-priority storage task instead of a callback on the shared work
  queue. This follows `device-firmware-ownership.md`, “Latency budgets”, and
  `execution-abi.md`, “Scheduling”/“Locking (uniprocessor)”: sleeping disk
  transactions belong in thread context, with cooperative yields and the
  existing sleeping mutexes. No work budget is increased or hidden.
- Snapshot executable disk bytes and service native file reads in batches
  of at most 64 KiB, with temporary page-backed buffers released on every
  exit. Writes retain their existing page-sized syscall and sector-sized
  cache operations. ELF reads from the immutable RAM snapshot retain their
  existing small stack buffer; `read_max` measures volume requests.

Host filesystem and kernel suites pass with ASan/UBSan; the 17 target
translation units also compile with the exact kernel flags and frame guard.
Production native snapshot accounting for a contiguous 1 MiB file reports
16 volume requests, 39 physical requests including metadata, and both maxima
at 65,536 bytes. The original FAT12/16/32 crash suite retains zero cross-links
over 2,122 cuts. The guarded-stack replay passes at 8,256 / 32,768 bytes.
Full Python discovery records 461 tests with 29 skips and three fixture errors
(two untouched desktop link lists and an absent image-build payload; the stack
tests pass with the equivalent existing host payload). Detailed host results,
fixture limitations and remaining lead checks are in
`build/codex/f1-39-report.md`.

## F1-76: two-unit reset signatures and PACKET classification (2026-10-16)

Research checked before implementation:
[ATA/ATAPI-6 revision 3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
9.2 and 9.12, specifies waiting for BSY after SRST and the complete
SC/LBA signature. PACKET signatures persist until a command establishes
DRDY or a host task-file write overwrites them; IDENTIFY DEVICE restores
the PACKET signature. DRDY clear does not establish absence.
[QEMU 9.2 IDE source](https://github.com/qemu/qemu/blob/v9.2.0/hw/ide/core.c)
(`ide_ctrl_write`, `ide_bus_perform_srst`, `ide_perform_srst`, `ide_reset`,
`ide_set_signature`, `cmd_identify`, `ide_ioport_write`) resets the two
units through a scheduled callback, publishes their individual task files,
and restores a CD signature before aborting EC. Task-file writes reach both
units; selecting a unit does not reconstruct its reset signature. Upstream
code alone does not establish the timing of an observed guest failure.

Read-only evidence in
`/home/peronslayer/Desktop/CiukiOS/build/test-runs/zz-media/640a7e4c/`
(image `401f3caf2a8f68cf5e1a2f55275a496d675fbc535ecce2cf4dcdabd8dd6924f2`)
and `33cfcf83/`
(image `64a531f9b087ee68904f40a5b428ac5ebf09e23096af1431bcdddf613be4be65`)
shows secondary-master EC ending at status 41/error 04 and whole-channel
quarantine. Neither log has a secondary-channel reset-signature line before
the fault. Since the current driver logs pass 1 before pass 2 IDENTIFY,
the pre-SRST firmware-mode probe must also be covered; these logs do not
prove that pass 2 was reached or measure the master's missing signature.

The host regression uses production `ata_channel_discover` with individual
unit task files, delayed reset publication, PACKET reset status zero,
broadcast host writes, and EC restoring a PACKET signature before ABRT.
It covers PACKET/PACKET, ATA/PACKET, PACKET/ATA and ATA/ATA with and without
the pre-reset DMA-mode measurement. Injected one-millisecond ATA-looking
tuples demonstrate the old single-sample misclassification. A separate
injected transition from idle status to ABRT between the firmware probe's
sample and its DRQ poll demonstrates premature quarantine before SRST.
These are host mechanisms, not measurements of QEMU scheduling.

Decision: preserve both discovery passes; bracket each complete signature
with BSY-clear status and require matching snapshots separated by a
one-millisecond sleep, under a per-unit bounded deadline. An EC-only
classification path accepts solely ABRT with the complete PACKET tuple and
BSY/DRQ/DF clear. Retire its generation/IRQ, restore nIEN, then issue A1
once. Apply the same classification to the firmware probe's DRQ poll.
Other command errors, missing IRQs, timeouts and failed A1 still quarantine
the channel without replay or reset. Unsupported PACKET types remain skipped.

The focused ATA harness and complete kernel host script pass with ASan/UBSan;
the kernel-only build passes its target frame checks and FPU/SIMD audit.
Commands ran sequentially under `nice -n 19`; the optional prebuilt DOSBOOT
replay was skipped. Detailed results are in `build/codex/f1-76-report.md`.
The lead must qualify the integrated image on
`volume-resolution-cd-usb-qemu-t23`, `optical-l1-qemu-t23` and `f1-storage`;
no new guest or hardware evidence is claimed here.

## F1-77: T23 PACKET signature and EC abort (2026-10-16)

Research checked before implementation:
[ATA/ATAPI-6 revision 3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
7.15.6.2, 8.15.5.2 and 9.12: PACKET devices restore their signature and
abort IDENTIFY DEVICE; DRDY may be set at command completion. IDENTIFY
DEVICE is section 8.15 in this revision (8.12 describes a different command).
[QEMU 9.2](https://github.com/qemu/qemu/blob/v9.2.0/hw/ide/core.c)
(`ide_set_signature`, `cmd_identify`, `ide_abort_command`) publishes 14/EB
and completes EC with DRDY|ERR and ABRT.
[Linux 6.12 libata-core](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libata-core.c)
(`ata_dev_classify`) checks only LBA Mid/High because some devices do not
publish valid SC/LBA Low values.
[libata-sff](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libata-sff.c)
(`ata_sff_dev_classify`) rejects status zero for ATA, not for ATAPI; its
reset wait also accounts for ATAPI devices with missing SC/LBA Low values.

The read-only T23 logs under `legacy/local/physical/a79f840c/serial.log`
contain three secondary-master EC failures at status 51/error 04, before
any secondary-channel signature line. The earlier `e8dfd0cd/serial.log`
contains status 00/signature 010114eb and successful ATAPI identification.
The firmware DMA-mode probe runs before the reset-signature logging, so
these logs do not establish which task-file registers failed its ABRT
check. The current check already permits DRDY/DSC, but requires SC/LBA Low
01/01; reset classification also requires that complete tuple.

Decision within f1-77: keep bounded BSY waits, the two discovery passes,
and two reset samples. A stable 14/EB pair establishes PACKET presence
and classification even with zero status, changing DRDY/DSC or transient
SC/LBA Low. Keep complete matching snapshots for other signatures. For EC
only, ABRT plus 14/EB permits the existing A1 classification path without
SC/LBA Low requirements; BSY/DRQ/DF, non-ABRT errors and missing IRQs keep
their existing containment. Log sampled signatures before classification,
including absent units and differing reset samples, and log the firmware
probe and EC-abort samples so pre-reset failures remain diagnosable.

The focused ASan/UBSan ATA harness passes 53 added discovery scenarios and
all 32 f1-76 topology combinations, with the existing containment controls.
The full sanitized kernel host script and kernel-only target build pass;
the optional prebuilt DOSBOOT replay is skipped. All commands ran
sequentially under `nice -n 19`. The target build passes its frame checks
and FPU/SIMD audit. Results and the initial regression replay against the
unmodified driver are recorded in `build/codex/f1-77-report.md`. The lead's
QEMU storage/media/volume-resolution gates and T23 `kind=atapi` confirmation
remain outstanding; no new image or physical qualification is claimed.
