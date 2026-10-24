# G29c: Copy Disk, Backup, partition editing and blank media

Research checked 2026-10-24, before implementation:

- Windows 98 Resource Kit, local read-only `build/rskt/wrkc10_5.htm`,
  “Using Fdisk to Partition Hard Disks”, “Formatting Removable Media” and
  “Using Microsoft Backup”; `wrkc27_2.htm`, Microsoft Backup/System Recovery.
- Microsoft KB [Q255867](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/255867)
  (archived Microsoft article): primary/extended/logical creation, active
  selection, information display, destructive warnings and the large-disk
  question. FAT16 partitions are limited to 2 GiB; FAT32 needs 512 MiB.
- Microsoft KB [Q69912](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/69/912.HTM):
  MBR partition types, four primary slots, EBR addressing conventions.
- Microsoft KB [Q166172](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/166/172.HTM):
  original-location restore and replacement only when the destination is older.
- Microsoft KB [Q187526](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/187526):
  System Recovery requires a startup environment and a full backup. File
  restoration alone must not be advertised as bare-metal system recovery.
- [Microsoft FAT specification 1.03](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf):
  FAT12 bad clusters use FF7h; bad metadata sectors cannot be hidden as bad
  data clusters. Intel 82077AA FORMAT TRACK is already implemented by
  `fdc_format`; reuse it rather than synthesizing low-level format with writes.

## Decisions and safety contracts

Blank removable media get a retained raw volume identity and drive letter,
without publishing a fabricated FAT mount. Files lists retained media and
offers Format when opening an unreadable removable drive. Formatting keeps
the existing physical-generation and exclusive namespace reservation checks.
A floppy Full format uses the controller's FORMAT TRACK service before the
FAT surface check; unreadable data clusters are marked bad in both FATs.
Metadata, write-protect, media-change and durability failures remain errors.

Copy Disk snapshots an entire source diskette before asking for the destination.
This supports one physical drive and two drives without holding a stale mounted
view over a swap. Read and write each take exclusive removable ownership;
destination capacity must match and every bounded write is flushed and read back.
The kernel uses two 4 KiB filesystem pages and a pinned user range; the small
kernel heap only supports 2 KiB allocations and must not hold disk images.
Cancellation before the destination confirmation writes nothing. Source read
failure never produces a usable snapshot. Source and destination generations
are independently queried after their respective insertion prompts.

The partition editor reuses libdisk's MBR/EBR probe and raw backend, preserving
boot code and unrelated entries. It refuses malformed/GPT/overlay tables,
overlaps, out-of-range extents and deletion of a nonempty extended partition.
The current LBA28 driver ceiling remains explicit. Writes require an exclusive
kernel lease; no ordinary caller gains Setup's publish/recheck capability.
The boot disk must remain refused outside Setup.

Backup uses a CiukiOS versioned archive, not Microsoft's proprietary QIC file
format. A catalogue records paths, sizes, times and per-file checksums. Full
jobs select files and recurse folders; differential jobs compare the previous
catalogue's fixed file set, as the Resource Kit specifies (new names are not
implicitly enrolled). Verify checks the complete archive before restoration.
Restore supports original/alternate roots and never/older/always replacement;
paths are validated and symlinks are refused. Each file is staged and synced
before rename, preserving the previous destination on a failed read/checksum.
No archive contents authorize writing device nodes or escaping the restore
root. An interrupted backup does not replace an existing successful archive.

Copy system files needs a bootable **unified** CiukiDOS floppy loader contract.
The canonical image boots the protected-mode VMM from FAT32; the historical
floppy loader is expressly forbidden by AGENTS.md. Do not revive that loader
or claim that copying kernel files creates a bootable diskette. Record this
dependency explicitly if no approved unified floppy boot path exists.
