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
