# Native FAT12 mounting

Research checked on 2026-10-23 before implementation.

The existing unified FAT driver already implements FAT12's packed entries,
including reads and writes across sectors, the fixed root region, bounded
chain walks, ownership scanning and ordered publication through the shared
cache. Storage already accepts MBR type 01 and whole-device FAT volumes;
removable publication uses the same VFS as other FAT volumes.

Primary sources and implementation decisions:

- [Microsoft, FAT: General Overview of On-Disk Format 1.03](https://www.fysnet.net/docs/fatgen103.pdf),
  pp. 9–10, 14–18, 23: determine type from data-cluster count, validate BPB
  extents and FAT capacity, preserve the neighbouring FAT12 nibble at
  `N + N/2`, and compare FAT[0] with BPB_Media. FAT12 has no clean/error
  flags. Retain the existing decoder, fixed root and ownership rules.
- [Microsoft KB75131, Standard Floppy Disk Formats Supported by MS-DOS](https://www.minuszerodegrees.net/ms_kb_articles/Q75131%20revision%202.pdf):
  exercise all eight requested standard geometries with explicit BPBs,
  including the early FE/FC/FF/FD descriptors and the 240-entry 2.88 MB root.
  Geometry selects test fixtures; valid FAT12 USB/image volumes are not
  restricted to these floppy sizes or to their CHS fields.
- [dosfstools 4.2 mkfs.fat source](https://github.com/dosfstools/dosfstools/blob/v4.2/src/mkfs.fat.c):
  use `-a`, explicit cluster/root/track/head/media settings and `-F 12`
  rather than relying on image-size heuristics. Independent fsck and mtools
  check the fixtures before and after production-driver writes.
- Microsoft Windows 98 Resource Kit, Appendix B, “Distribution Disk Storage
  Overview” (`build/rskt/wrkappb_1.htm`, owner-provided read-only text): DMF
  is a read-only distribution format. Recognize FAT12 3360-sector, two-head,
  21-sector/track volumes as DMF and retain an explicit read-only reason,
  including after a requested writable upgrade. Test 1 KiB and 2 KiB clusters.
  [Microsoft OpenVMM's floppy media reference](https://openvmm.dev/guide/reference/emulated/legacy_x86/floppy.html#supported-media)
  independently lists DMF's 21 sectors/track, 80 cylinders and 512-byte sectors;
  the implemented signature uses the corresponding two-head, 3360-sector BPB.
- The same Resource Kit, chapter 26, “Optimizing Removable Disk Drive
  Performance” (`wrkc26_4.htm`): removable write-behind is configurable.
  Retain the approved CiukiOS default of write-through, the shared cache,
  finite barriers, surprise-removal invalidation and quarantine. No new
  cache or inferred Windows flush interval is introduced.

An absent device write callback is the existing block API's write-protection
signal (USB MODE SENSE already clears it). Preserve that signal through the
partition/counting views and report `write_protected`, separately from
unqualified durability, including when the callback is withdrawn after mount.
An issued EROFS is reported with the same reason. Both late protection paths
retain the existing sticky write-error flag and refuse retries.

The f0-32 storage amendment still forbids non-atomic changes to imported live
FAT12 links spanning sectors; ordinary allocations avoid those links. This
directive does not authorize weakening that crash-safety contract. Fixed-root
growth means consuming its remaining slots; exhaustion returns ENOSPC rather
than allocating a root cluster. Subdirectories may grow cluster chains.

Controller discovery and A:/B: assignment remain dependent on f1-115. This
change supplies the existing generic FAT mount path, without inventing a
controller or representing USB media as a floppy controller.

Host verification and exact results are recorded in
`build/codex/f1-115b-report.md`; no boot or hardware evidence is claimed.
The full host filesystem suite includes the geometry tests; a focused run is
`nice -n 19 bash scripts/test/host_fs_tests.sh --fat12`. This uses the existing
runner's unchanged sanitizer settings. The Python fixture driver itself never
changes ASan's leak-detection setting.
