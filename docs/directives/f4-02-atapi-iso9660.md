# Directive f4-02: ATAPI PIO, ISO 9660/Joliet read-only mounts and the read-only filesystem interface

- **Step:** F4. **Contracts:** `f4-audio-media-contract.md` ("ATAPI and
  ISO 9660 read-only mounts", acceptance rows T0 optical/NTFS, T3
  `f4-optical`; lead decision 8), `vfs-storage-contract.md` (filesystem
  drivers keep no private block cache; host VFS owns cache, aliases and
  visibility; drive-letter enumeration; `EROFS`), `device-firmware-ownership.md`
  (one ATA owner per task file, elapsed deadlines, no BIOS fallback after
  an issued command, quarantine), `hardware-baseline.md`,
  `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f4-optical`, from `main` at or after `67f6397`.
  Files: `src/kernel/drivers/ata.c` and `ata.h` (ATAPI on the existing
  owner; today ATAPI units are detected and `skipped=F4`); new
  `src/kernel/fs/iso9660.c`; new `src/kernel/include/ciuki/fs_ro.h` (the
  read-only filesystem driver interface, see point 3) with its integration
  in `src/kernel/fs/mount.c`, `vfs.c`, `path.c` and `files.h` (px nodes of
  a read-only volume; every mutation `EROFS`); `blkpart.c` only if a
  whole-medium view is needed; new `scripts/test/iso_fixtures.py` and
  `scripts/test/run.py` (fixture generator `xorriso`, attachment of a
  CD-ROM drive); `src/kernel/probes/f4_probes_media.c` registered in
  `scripts/build_kernel.py`; `tests/suites/f4-media.json`;
  `docs/design/f4-acceptance.md` (your section `## f4-02`; create the
  file if absent); `docs/build-and-run.md` (fixture tooling); host tests.

## Scope

1. **ATAPI PIO on the ATA owner.** IDENTIFY PACKET DEVICE (device type,
   packet size), PACKET `A0h` with bounded DRQ phases (CoD/IO direction,
   byte counts, total limits, odd final word), status/sense handling;
   required packets: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ
   CAPACITY, READ(10) in 2,048-byte blocks, READ TOC, START STOP UNIT and
   PREVENT ALLOW MEDIUM REMOVAL for explicit eject; 2 s ordinary and 10 s
   spin-up/eject deadlines measured outside IRQ-off sections; no-medium/
   not-ready/unit-attention as media conditions; transport timeouts and
   malformed phases quarantine the controller with the consequences for
   the ATA peer recorded; no DMA, no overlap, no BIOS fallback. Sources S7
   of the contract and QEMU `hw/ide/atapi.c` as the twin.
2. **ISO 9660.** Primary volume descriptor, descriptor set terminator,
   both-endian agreement, Level 1–3 directory and file records,
   multi-extent files, path validation and extent bounds; Joliet
   supplementary hierarchy preferred when valid (`%/@`, `%/C`, `%/E`,
   big-endian UCS-2 → the VFS UTF-8 names and 8.3 aliases); malformed
   Joliet fails visibly; Rock Ridge, UDF and audio CDs out of scope.
3. **Read-only filesystem interface.** `fs_ro.h` defines the driver
   operations the VFS calls for a read-only volume (probe, mount, unmount,
   root, lookup, readdir with an opaque bounded cursor, read, node
   metadata, generation), the mount-time media generation, and the
   invalidation rule: a media change or unit attention bumps the
   generation, invalidates cached blocks, nodes, cwd pins and aliases,
   makes existing handles fail `EIO`, and frees the drive letter before
   reuse. ISO 9660 is the first driver; the NTFS read-only library of
   f4-03 is attached by a later directive, so keep the interface free of
   ISO-specific assumptions and document it in the header.
4. **Runner and fixtures.** Fixture generator `xorriso` (host tool
   available) producing deterministic ISO images (fixed timestamps,
   declared file set with SHA-256 manifest, optional Joliet, Level 1–3,
   a deliberately malformed variant), attached to QEMU as an IDE CD-ROM
   on the secondary channel (choose the index that does not collide with
   the existing FAT fixture disks 1–3 and record it); the manifest lists
   the independent host hashes (`xorriso -osirrox` extraction or
   `xorriso -find`) the probe's records are compared with.
5. **Probe `f4:optical`.** Enumerate the drive, mount the fixture, list
   directories, hash the declared files through the VFS, attempt every
   mutation (`EROFS`), read through an ATA peer before and after, eject,
   check stale handles fail `EIO` and the letter disappears, re-insert
   (QMP `change`/`eject` by the runner) and mount again; record write
   counters (must be zero), generations and deadlines. Cases on
   `qemu-t23` and `qemu-desktop-2002`; on a machine without a packet
   device the probe records `not_run reason=device_absent`.
6. **Host tests:** the T0 optical row (packet phases, bad direction/count,
   sense/no-medium, timeout and peer serialization on a mock task file;
   ISO parser on generated fixtures incl. Levels 1–3, Joliet, multi-extent
   (synthetic records if no tool generates small multi-extent files) and
   malformed extents; zero writes verified by block-call instrumentation
   and before/after image hashes).

## Acceptance by the lead

Host tests; kernel and image build; QEMU `f4-media` optical cases PASS
with hashes equal to the host manifest and zero writes; `f2-all` unaffected.
