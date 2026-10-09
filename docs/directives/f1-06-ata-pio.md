# Directive f1-06: ATA PIO block device, partition views and the disk probes

- **Step:** F1. **Contracts:** `vfs-storage-contract.md` (block device
  layer, durability barrier with `FLUSH CACHE`, no retry after an issued
  command fails), `device-firmware-ownership.md` ("Firmware-call boundary":
  a command issued through native ATA is never retried through the BIOS;
  timeouts abort, quarantine and allow recovery only by re-qualification;
  controller ownership; IRQ14/IRQ15 edge lines and the spurious-IRQ15 rule
  already in `trap.c`), `f1-acceptance.md` (probes `ata`, `ata-fault`,
  `partition`; 30-second deadlines for IDENTIFY and data commands; BIOS
  calls = 0; survivor advances 100 ticks).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f1-ata`. Files: `src/kernel/drivers/ata.c`,
  `src/kernel/drivers/ata_probe.c`, `src/kernel/drivers/blkpart.c`,
  `src/kernel/include/ciuki/ata.h`, `src/kernel/include/ciuki/blkpart.h`,
  `src/kernel/lib/sha256.c`, `src/kernel/include/ciuki/sha256.h`,
  `tests/host/ata_test.c`, `tests/host/sha256_test.c`,
  `scripts/test/host_kernel_tests.sh` (add the tests),
  `scripts/build_kernel.py` (add `drivers` and `fs` to the source
  directories if not already there). The frozen `src/kernel/fs/blkdev.h`
  and `fs_port.h` are the interface: do not modify them; `partition.c/h`
  from f1-01 is reused for MBR parsing, not rewritten.

## What to build

1. **Driver** (`ata.c`): legacy channels 0x1F0/0x3F6 (IRQ14) and
   0x170/0x376 (IRQ15), master and slave; device detection by the
   signature after a soft reset of the channel, ATAPI devices recorded and
   skipped (F4); `IDENTIFY DEVICE` with the 30 s deadline; capacity from
   words 60–61 or 100–103 (LBA48 when word 83 bit 10), model string
   sanitised for records; write-cache state from word 85 bit 5 (enabled)
   and word 82 bit 5 (supported), mapped to `blkdev.write_cache_state`;
   `FLUSH CACHE`/`FLUSH CACHE EXT` as the `flush` operation only when
   supported (otherwise `flush` is NULL and the VFS keeps the volume
   read-only, per the storage contract). PIO `READ SECTORS (EXT)` and
   `WRITE SECTORS (EXT)` with up to 256 sectors per command, 400 ns pacing
   with `udelay`, completion by IRQ: the handler reads the status register
   (which acknowledges the interrupt), records it and wakes the waiter on a
   `kwait` queue; the request waits with `kwait_wait_until` and the
   30 s deadline; polling is used only for the BSY→DRQ transition inside a
   data phase, bounded by the same deadline. Exactly one request in flight
   per channel, serialized by a `kmutex`. No BIOS call anywhere.
2. **Failure semantics**: any ERR or DF status, or a deadline, ends the
   request with `-FS_EIO`, records status/error/elapsed, sets
   `blkdev.quarantined` for that device and issues no further command to it
   (`blkdev_range` already refuses); a channel whose device stays BSY is
   masked and both its devices are quarantined. Boundary and overflow
   requests are refused by `blkdev_range` before any port write. Recovery
   is a fresh boot (no reset-and-retry in F1).
3. **Ownership**: registry claims for the command block, control block and
   IRQ of each present channel as `ata0`/`ata1` with generations,
   activation after the IDENTIFY succeeds, quiescence proof = no request
   in flight and interrupts disabled on the device (nIEN); IRQ14/15 use
   `irq_set_handler` (ISA edge lines), not the shared chain. Discovery
   happens in thread context after `kwork_init` (see `probes_main`), in a
   function `ata_init()` the lead calls from the probe task.
4. **Partition views** (`blkpart.c`): `struct blkdev` wrappers over a parent
   device for each MBR partition found by `partition.c` (start, length),
   adding the offset and bounding the range, forwarding `flush` and cache
   state and the parent's `quarantined` flag; the FAT32 volume of the
   canonical image is partition 1 at LBA 2048.
5. **SHA-256** (`lib/sha256.c`): a straightforward FIPS 180-4 implementation,
   integer-only, with host test vectors ("abc", empty, one-million "a"),
   for digests in evidence records.
6. **Probes** (`ata_probe.c`, `int probe_ata(void)`, `probe_ata_fault`,
   `probe_partition`, each 0 on PASS; expose in `ata.h`, register with
   `CIUKI_F1_PROBE` if the macro exists in your base, otherwise leave
   registration to the lead's plumbing):
   - `ata`: reports identity, capacity, lba48, cache state, flush support;
     reads LBA 0 and LBA 2048 and reports `lba=… sha256=…` for each (the
     runner compares with digests it computes from the image); checks that
     an out-of-range read and a zero-count read issue no command (counter);
     passes when IDENTIFY succeeded, capacity equals the image's 1,048,576
     sectors on QEMU (reported, compared by the runner), and both reads
     completed.
   - `ata-fault`: the driver exposes a register-access boundary
     (`ata_port_ops` with `in8/out8/in16/out16`) that the probe replaces
     with a scripted fake device for these subcases, one after another,
     re-arming between them: ERR status, DF status, BSY stuck, DRQ stuck,
     missing device (no signature), failed IDENTIFY, failed FLUSH; each must
     end with `-FS_EIO` (or no device) within its deadline, quarantine the
     fake device, and the next request must issue zero commands; after all
     subcases the real device is untouched (`real_commands=0` during the
     probe) and a survivor task advances 100 ticks. Deadlines in the fake
     are shortened through the boundary's time source, so the probe finishes
     within 60 s.
   - `partition`: parses the real MBR and reports each entry (type, start,
     length) plus the walk count; then feeds `partition.c` with in-memory
     fixtures (valid primary+extended, loop, overflow, overlap, protective
     GPT) and reports accept/reject per fixture; passes when the real
     partition 1 is 0x0B/0x0C at LBA 2048 and every fixture matches the
     expected outcome.

## Host tests (mandatory)

`tests/host/ata_test.c` under ASan/UBSan with a fake register model
(scripted status/error/data sequences, IRQ delivery, time): detection and
IDENTIFY parsing (capacity 28/48-bit, cache bits), read/write data phases
of 1, 2 and 256 sectors, every failure subcase with quarantine and the
zero-further-commands rule, deadline handling, `blkpart` offset/bounds,
and `sha256_test.c` vectors.

## Acceptance by the lead

Diff review; host tests; kernel build with the FPU audit (the SHA-256 and
PIO loops must be integer-only); on QEMU the `ata`, `ata-fault` and
`partition` probes pass on `qemu-t23`, `qemu-e500` and `qemu-min128`, and
the F0 suites still pass. Reply with: files, interfaces (signatures), test
output, and any contract problem found.
