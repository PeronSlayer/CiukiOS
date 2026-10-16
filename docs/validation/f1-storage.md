# F1-78 storage concurrency and metadata reads

Research checked on 2026-10-16, before implementation:

- [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 3.3–3.4 and 5.3: each command has CBW, optional data, and CSW;
  another command must wait for the preceding status. Combining consecutive
  sectors amortizes command overhead. Keep the existing CSW validation/recovery.
- [Intel UHCI Design Guide 1.1](https://ftp.netbsd.org/pub/NetBSD/misc/blymn/uhci11d.pdf),
  sections 1.1.1 and 1.3: the schedule advances in 1 ms frames and bulk traffic
  uses available bandwidth after periodic traffic. The T23's measured command
  latency is device/driver evidence, not a latency guaranteed by UHCI.
- [Linux v6.12 usb-storage sizing](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/scsiglue.c),
  `device_configure`: transport limits and device quirks bound requests; some
  devices require smaller transfers. Retain CiukiOS's qualified 128-sector cap.
- [Linux v6.12 FAT directory read-ahead](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/fat/dir.c),
  `fat_dir_readahead`: fetch directory sectors within a cluster on a cold
  first sector. CiukiOS will use its shared `cache_read_run`, bounded by the
  cluster, FAT copy/root extent, partition and 128-sector transport limit.

Initial repository comparison: `vfs.lock` serializes legacy and native
namespace operations through FAT I/O. The shared cache also holds its mutex
through reads, dirty eviction and flush. Runtime FAT metadata reads fall back
to single-sector reads after the temporary mount windows are released.
The implementation must reserve cache fills before unlocking and validate
media identity again before publication. Failed dirty blocks remain pinned.

Implementation and host evidence are recorded in `build/codex/f1-78-report.md`.
QEMU/media and physical T23 qualification belong to the integrating lead.

Implemented evidence: the USB host model's cold 64-entry VFAT root listing
plus 64 pathname stats falls from 13 READ(10) commands to one. Runtime
read-ahead uses the existing 128-sector maximum, with a temporary buffer on
cold misses and demand-sector fallback if allocation fails. Cache reads,
dirty eviction and flush now release the cache mutex during transport;
joined fills reject invalidation, generation changes and quarantine.
Kernel locks use FIFO tickets and retain the 1000 ms safety wait.

This is a partial F1-78 change. Native callers outside the directive's listed
authority also hold the namespace lock through I/O. The namespace/volume
split, production lock-order proof, fast-volume latency test and verbose
wait diagnostics remain outstanding. Passing host durability/removal tests
and the command-count improvement do not qualify the desktop-freeze fix.
