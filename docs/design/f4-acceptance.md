# F4 implementation and validation

## f4-02

Directive: [f4-02](../directives/f4-02-atapi-iso9660.md). Host implementation
evidence only; QEMU and physical qualification belong to the lead.

Research before implementation (2026-10-11):

- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  §§5, 7 and 10: PACKET command phase CoD/IO=1/0, data-in=0/1,
  completion=1/1; only the final data phase may have an odd byte count.
  IDENTIFY PACKET DEVICE establishes CD-ROM type and 12/16-byte packets.
  Decision: polling PIO with nIEN set, one channel mutex through packet
  completion and REQUEST SENSE, no overlap, DMA, retry or firmware calls.
  Ordinary packets have a 2 s elapsed deadline, explicit start/eject 10 s.
  Timeout/malformed phase quarantines BOTH units on the channel, including
  an ATA peer; ordinary CHECK CONDITION uses sense without quarantining a
  proved completed transport. Unit attention/media disappearance expires
  the media generation before another mount transaction.
- [QEMU v9.2 ATAPI source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/ide/atapi.c):
  READ CAPACITY reports last LBA and 2048-byte blocks; TOC format 1 is
  encoded in byte 9 bits 7:6 and gives the last data session. Decision:
  select the data session through TOC; audio-only media is unsupported.
- [ECMA-119 sixth edition](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf),
  §§7–9: descriptors, directory bounds and ordered sections of a file.
  [Microsoft Joliet specification](https://pismotec.com/cfs/jolspec.html):
  `%/@`, `%/C`, `%/E` and UCS-2 big endian. Decision: validate both-endian
  fields, terminator, records and extents; prefer the strongest advertised
  Joliet hierarchy, refuse malformed advertised Joliet. No Rock Ridge/UDF.
- [QEMU QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html):
  eject/change removable media with explicit ARM synchronization. Decision:
  CD-ROM uses IDE index 3 (secondary slave), with ATA fixture peer at index
  2 when declared. Refuse any collision with FAT fixture indices 1–3;
  optical cases can declare at most two FAT fixtures.

Repository comparison: `ata.c` formerly skipped ATAPI after reset signature;
`cache.c` stored only 512-byte blocks; the two VFS views assumed FAT volumes.
The shared cache now keys device, generation, block size and LBA. Each 4096-byte
allocation includes metadata, one 2048-byte slot and three 512-byte slots;
512-byte requests may use either capacity. This keeps the same byte budget
and preserves healthy-device progress when a failed dirty FAT block remains
pinned in the minimum 8192-byte cache. A uniform 2048-byte layout failed that
existing host regression because only one slot remained after workspace.
The generic `fs_ro.h`
interface keeps 64-bit identities/sizes and a bounded opaque cursor; it
requires no FAT or ISO layout. VFS owns bounded per-directory name/alias
views, constructed in on-media enumeration order before lookup. This makes
aliases independent of lookup order. Exhaustion/case collisions refuse the
directory visibly. Media loss invalidates cache, aliases and mount identities,
resets legacy cwd, expires native cwd identities, makes handles return EIO,
and removes the letter before reuse. Old references never acquire the new
mount generation. Read-only sync/unmount never invokes write/flush callbacks.

The VFS bounds each directory view to 256 entries and a mount to 32 views;
each allocation holds four metadata nodes. The parser bounds descriptor sets
and multi-extent chains to 128 records. Exhaustion fails visibly. Generation
checks occur on namespace/handle operations and explicit storage refresh;
readiness is queried before serving cached names or blocks. Reinsertion uses
a new mount transaction; stale legacy searches, handles and native cwd/file
identities retain the expired VFS generation. No background media retry is
introduced. The probe bounds its iterative fixture walk to 16 directory
levels and records names separately from file hashes to respect the existing
240-byte record envelope.

Host evidence (2026-10-11, `wt/f4-optical`):

- `make test-host`: PASS, including production packet/ATA peer mocks, native
  and legacy read-only lifetime/mutation checks, loader statics, independent
  ISO comparisons and 250 Python tests (7 existing skips: six absent local
  physical captures and one f2-15 evidence-policy case). All six Level/Joliet
  combinations are deterministic; synthetic multi-extent and malformed
  descriptor/extent cases pass. Host block writes/flushes are zero and image
  before/after hashes agree. ASan/UBSan enabled; LSan disabled because ptrace
  is unavailable in the sandbox. i686 layouts are compiled/extracted, but
  Linux cannot execute the native `-m32` test binary in this sandbox.
- `scripts/test/host_fs_tests.sh`: PASS, 14 groups, three fsck checks and
  three independent mtools comparisons. Its production cache regression
  proves healthy-device progress after a different device's sticky write
  error. Crash classification covers 2122 cuts with no unknown diagnostics.
  The helper now extracts the builder's constant formatted frame-limit flag
  without executing/importing the builder.
- SDK and Lua builds use two jobs, desktop/kernel/image builds run
  sequentially. SDK compile/link/audits, Lua/luac and desktop/demo inspections
  pass. Final kernel: 1,690,876 bytes, build `798a92da51e3-dirty`, 4096-byte
  frame checks and FPU/SIMD audit PASS. Image T1 passes MBR/FAT32 geometry,
  fsck, loader size/CRC32 and independent read-back of all 55 payloads.

Canonical image `build/f0/ciukios.img`, 512 MiB, SHA-256:

```text
ed2af39ae4f177ca16bf25343df5119653a34fa5ba536d9f9f4424cf48296283
```

No QEMU or systemd scopes were run by the implementer. The lead must review
and integrate the diff, build the integrated image and run `f4-media`,
`f2-all` and the earlier applicable gates on that exact image. Actual packet
device, QMP tray behavior and hardware optical qualification remain unproved
by host evidence. Complete commands/output and handoff are retained in
`build/codex/f4-02-report.md` and the referenced logs.
