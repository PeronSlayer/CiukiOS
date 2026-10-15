# F1-70 FAT mount memory and timing

Research checked before implementation (2026-10-15):

- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf), sections 4.1–4.3 and 5: decode only actual cluster entries, mask FAT32's reserved nibble, interpret clean/error flags in both copies, and count free allocation from the FAT rather than trusting FSInfo hints.
- [Linux v6.12 FAT entry traversal](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/fat/fatent.c), `fat_count_free_clusters`: read ahead and decode consecutive entries within resident blocks, with scheduling points.
- [dosfstools v4.2 checker](https://raw.githubusercontent.com/dosfstools/dosfstools/v4.2/src/check.c): establish chain ownership before reclaiming unreferenced allocation. Incomplete checks cannot authorize reclamation.
- [Linux v6.12 USB storage lifecycle](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/usb.c): discovery is asynchronous. A diagnostic must distinguish a pending scan from unavailable controller resources.

Local driver-package evidence: the supplied `third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/added/nusb36e win98usb.exe` cabinet contains separate `USB.INF`, `USBSTOR.INF`, `uhcd.sys` and `usbstor.sys` controller/class components. Its accompanying `nusb36e win98usb IMPORTANT.txt.txt` describes flash-drive support, without prescribing FAT workspace sizes or controller-register changes. No register programming or controller ownership policy changes are justified by this task.

Selective cabinet inspection: `USB.INF` lines 90 and 166–173 bind generic
`PCI\CC_0C0300` through `UniversalHCD.Dev` to `uhcd.sys`; `USBSTOR.INF`
line 83 binds `USB\Class_08&SubClass_06&Prot_50` to `USBSTOR_BULK`.
The package SHA-256 is
`42b13ce440cc6528600a7b085226050d855c50199ecde6cbd6f31cbf83a69621`.
It was inspected read-only in the main checkout; it is absent from this
worktree. Its binaries were neither imported nor executed.

The a395ffa8 disk log reports 523260 KiB usable RAM, 2203 free DMA pages and 126816 free NORMAL pages. The failure occurs after 55 successful mount reads (309 sectors), with result -12 and no publication. The byte-per-cluster checker uses the cache's fixed workspace (512 KiB for its default 8 MiB cache), so sufficient system RAM does not prevent this failure. The physical FAT geometry is absent; the precise failed cluster count is not recoverable from this log.

The H-page log is a different failure: all three USB controllers report an IRQ resource conflict with the audio device before workers start. None reaches its first scan. Waiting longer cannot repair that ownership refusal; the page must report unavailable hosts as a failure with their reason, while allowing viable workers the item's remaining enumeration budget. The normal boot does reach its first scans and binds a 31283200-sector device.

Implementation decision: use one ownership bit per cluster in individually allocated normal-pool pages, capped at 1 MiB per active checker, with the existing cache workspace as a bounded multipass fallback. Walk directories with a bounded explicit stack, independent of cluster numbering. Inspect both FATs in 64 KiB windows for all mount paths, decoding entries directly from resident windows. The initial FAT census determines the allocated high-water mark without trusting FSInfo; dirty ownership and repair passes stop there. Validate every ownership pass before any reclamation. Batch orphan repair by FAT sector. Clean/error-free agreeing copies permit the directive's clean-mount fast path; explicit checks and dirty volumes still validate ownership. Initial read-only mounts issue no writes or flushes. Allocation failures record needed and available bytes; mount, upgrade and diagnostic records retain elapsed times.

Host qualification completed: `scripts/test/host_fs_tests.sh` and the full
`scripts/test/host_kernel_tests.sh` pass under `nice -n 19` (ASan/UBSan,
sequential jobs). The latter includes the USB register/transport suites,
log recovery and 1,287,621 power-cut assertions. The FAT suite retains its
2,122 crash cuts and independent fsck/mtools checks. New cases cover 4/32 GiB
layouts, 32 GiB with 512-byte clusters and eight ownership passes, a failed
bitmap allocation with 4 KiB fallback, an unavailable fallback, and a
cross-link discovered only in a later pass before any repair writes.

The USB transport model mounts 4 GiB with 32 KiB clusters writable in
2832 ms (23 reads, maximum 128 sectors) and 32 GiB with the same cluster size
in 18512 ms. These are model times, not physical measurements. The separate
virtual FAT layout with a far-end orphan and 32 GiB/512-byte clusters takes
15552 ms on the host; its large FAT does not carry a five-second timing
claim. Cluster geometry, directory size, actual transport rate and memory
fallback passes affect physical cost. Volume records now retain cluster
geometry for the next hardware diagnosis.

The H-page host test accepts a first scan at 9 seconds, publication at
11 seconds, and a 1 MiB read taking 320 ms within the existing 15-second
item. Resource conflicts fail immediately with the controller reason;
pending first scans and binds fail at the budget, while a completed scan
without storage may skip. Read errors stop after the first failure.

Kernel and DOS image builds pass, including the kernel FPU/SIMD audit and
DOS FAT16/T1 payload readback. Exact commands and verbatim test results are
in `build/codex/f1-70-report.md`. No QEMU, systemd scope, canonical system
image build, commit or diary was performed. Host timing and request counts
do not establish the T23's subsecond repair or the stick's physical transfer
budget; those require the lead's QEMU and hardware runs on the final image
hash. The H-page IRQ resource conflict seen in the supplied log still needs
the lead's separate controller/audio ownership investigation.
