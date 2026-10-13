# f1-39 — Disk throughput: one 512-byte request per sector makes every program start take seconds

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-39` from `main` (f9f112a0). Host-testable.

## Evidence (T23, image 3299a24f, 2026-10-13)

```
[launch-stage] name=dosbox pid=5 spawn_us=133240000 elf_us=133753000 read_bytes=436172 disk_bytes=432128
[launch-io] pid=5 read_bytes=436172 read_requests=107 read_max=4096 disk_bytes=433664 disk_requests=847 disk_max=512
[launch] name=files pid=4 … spawn_us=97075000 elf_us=98722000 read_bytes=1313028
[launch] name=control pid=6 … spawn_us=235402000 elf_us=237283000 read_bytes=1499756
[work] budget exceeded site=storage_work us=1214 … (repeated every run)
```

Reading an ELF takes 1.6–1.9 s per 1.3–1.5 MB (about 800 KB/s): the VFS issues 4 KiB reads and the storage path turns each into **single-sector** ATA PIO commands (847 requests of 512 bytes), each with its own command, IRQ wait and work-queue round trip. The desktop itself (`/Programs/desktop`, 1.74 MB), the wallpaper (`Ciuk2.png`, 2.56 MB) and `DOOM1.WAD` (4.2 MB) pay the same price. The T23's drive supports PIO mode 4 (16.6 MB/s nominal); 5 MB/s is a realistic floor.

## Task

1. **Coalescing:** the FAT read path (`src/kernel/fs/fat.c`, the block cache, `blkdev`) issues one block request per run of contiguous clusters, up to 64 KiB (128 sectors) per request; ELF loading and file reads in the kernel (`read_max`) use 64 KiB requests; optional read-ahead of the next 64 KiB for sequential reads of one file. Keep the cache's coherency rules (`docs/design/` storage/F1 contracts: cite the sections you rely on).
2. **ATA multi-sector:** `command_data` already takes `n`; use it. Add `SET MULTIPLE MODE` + `READ MULTIPLE`/`WRITE MULTIPLE` (C4h/C5h, block size from IDENTIFY word 47 bits 7:0, validity in word 59 bit 8) so one IRQ serves a block; `rep insw`/`outsw` for the data block; keep the sector-granular fallback when the device reports no multiple support (QEMU IDE supports it, the hardware does too). ATAPI unchanged.
3. **Work budget:** `storage_work` exceeds its budget on every run already; do not grow it blindly: move long transfers to the storage task or split per block with `task_yield`, as the work contract (`docs/design/`) prescribes; the `[work] budget exceeded` records for `storage_work` must disappear in the QEMU `L` boot (the lead checks).
4. Extend `[launch-io]` with `disk_max`/`disk_requests` already present; add the elapsed transfer time per request class if cheap.

## Acceptance

Host tests: the fake ATA device of the kernel host tests accepts multi-sector commands and counts them; FAT tests prove identical bytes for coalesced reads (fragmented and contiguous chains, run boundaries, end of file, cache hits in the middle of a run); a request-accounting test shows `disk_max` ≥ 32768 for a contiguous 1 MB file. Writes keep their current proof rules. Measured by the lead on the T23: a 1.3 MB ELF in ≤ 0.4 s (`[launch-stage]`).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-39-report.md` with the ATA/ATAPI-6 clause references.
