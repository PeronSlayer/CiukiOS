# f9-04 libsetup: research and integration boundaries

Research checked on 2026-10-23 (local `date -Is` before recording the date).
The transaction interface in `sdk/libsetup/include/setup_txn.h` and the
answer interface in `setup_inf.h` remain frozen and unchanged.

Primary sources checked against this tree:

- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
  cluster-chain entries and FAT[1] clean/error bits (section 4, page 19).
  Retain `fat.c`'s own ordering and libdisk's seal operation; read file
  content through `disk_read_raw` only after commits and the volume flush.
  A VFS read of a recently written file does not provide raw verification.
- [dosfstools 4.2 mkfs.fat.c](https://github.com/dosfstools/dosfstools/blob/v4.2/src/mkfs.fat.c),
  FAT32 cluster/FAT-size calculation. Reuse `disk_plan_whole`, never another
  formatter. Keep the 4 KiB cluster geometry and its 28-bit partition cap.
- [ATA/ATAPI-6 revision 3a, sections 8.12 and 8.13](https://flint.cs.yale.edu/cs422/readings/hardware/ATA-d1410r3a.pdf),
  write-cache flush completion/error semantics. A failed barrier stops the
  transaction; no canonical MBR is intentionally published before sealing.
- [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/),
  volume/file structure for the CD source. Reuse `cdview.c`/`iso9660.c`; do
  not introduce another ISO parser in libsetup.
- Windows 98 Resource Kit, chapter 10, `wrkc10_5.htm` partition deletion,
  primary/active partition and Format; `wrkc10_6.htm` cache and disk errors,
  read from the owner's read-only `CiukiOS/build/rskt`. This does not
  authorize surface scanning, silent bad-cluster marking or NTVDM changes.
- [El Torito 1.0](https://www.ardent-tool.com/storage/specscdrom.pdf) and
  [SeaBIOS 1.16.3 cdrom.c](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/cdrom.c),
  HDD emulation and the 512/2048-byte translation. Keep the f9-01 view and
  do not infer a machine-specific drive number from ATA enumeration.
- [GPLv2 sections 1–3](https://spdx.org/licenses/GPL-2.0-only.html) and
  [LGPL 2.1 sections 4/6](https://spdx.org/licenses/LGPL-2.1-only.html),
  corresponding-source and notice context of the source component. Sources
  are copied only when selected, without changing ISO source availability.
  No third-party implementation was newly vendored by f9-04.
- The [UEFI legacy MBR description](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html#legacy-master-boot-record-mbr)
  was requested but the server returned HTTP 403. The MBR is delegated to
  f9-02 and compared against the repository's canonical assembly bytes.

Repository comparison and decisions requiring the lead:

1. The native SDK has `stat`/`fstat`, but no user-accessible FAT attribute
   getter/setter or cluster-list query. Kernel `vfs_stat`, `vfs_attrib`,
   `fat_get_cluster` are internal kernel interfaces. A private host platform
   boundary exercises those facilities against real FAT; it cannot turn
   them into native syscalls. Native preflight therefore returns ENOTSUP
   before acquiring a lease or writing. The lead must reserve and implement
   a bounded, generation-safe read-only cluster query and attribute access.
2. Control Panel keyboard configuration uses `cfg_format` in
   `apps/desktop/settings.c` and durable settings I/O. The clock page only
   changes 24-hour/seconds presentation; it explicitly describes UTC and
   has no RTC write or time-zone/convention writer. The network page only
   queries status. Identity registry keys and the DHCP/static/IPX/lobby
   persistence formats have no shared writers in this tree. The private
   configuration boundary must be bound to approved shared writers; a
   fabricated registry/settings format would violate this directive.
3. `apps/control/programs.c` lists/removes existing program/link records;
   it does not export a bundled-game registration writer. This also needs
   an approved shared writer. No program or personality is launched here.
4. The actual on-disc `D:/CDMANIFE.JSN` has boot file path/size/hash/component
   records, but no directory set or attributes. Optional `directories`
   records and numeric `attributes` are supported privately for host
   qualification; this is a proposed manifest amendment, not an existing
   producer guarantee. Missing directory records permit ancestor creation
   and conservative accounting but cannot recover empty directories.
5. `build_cd.py` embeds the manifest before the ISO SHA exists. Accordingly
   `source_iso_sha` records SHA-256 of the on-disc manifest, the identity
   required for reinsert checking; it cannot truthfully contain the final
   ISO SHA. The lead should clarify the field's documented meaning.
6. The loader is not a file on C:. It is embedded at offset 512 of
   `D:/CIUKBOOT.IMG`. Libsetup reads that extent and checks `loader_sha256`
   before handing it to libdisk. Adding a C: loader payload or approving
   the existing boot-image source is a lead decision.
7. f9-01 replaces the canonical BOOT.CFG with `rstrip + ' setup=1\n'` and
   embeds only that result. The implementation removes precisely this
   suffix and restores a newline. Exact preservation of original trailing
   whitespace is impossible without a canonical template/hash field in
   the manifest. Such a field must be added by the media owner.
8. `setup_restart` is requested but absent from the frozen public header;
   it is private until the lead amends the interface. The existing POWER
   syscall accepts only the supervisor-spawned desktop (`core/power.c`),
   so native Setup also needs an authorized restart route.

These are integration gaps, not completed native installation evidence.
The host configuration stand-in is deliberately identified as such in the
harness. The report records the exact tests and remaining acceptance items.

Source-media handling uses explicit offsets for manifest refills and never
retains their descriptors across copy callbacks. File, manifest, metadata
and initial identity read retries share the five-attempt callback bound;
the host model invalidates every old source descriptor on generation change.
