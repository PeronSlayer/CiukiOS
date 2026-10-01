# Long file names in CiukiOS — 30 September 2026

The FAT16 kernel keeps its existing 8.3 interface. A resident `LFN.COM`
hooks INT 21h after the kernel loads and implements the Windows 95 `71xxh`
long-name calls, including search handles and VFAT directory entries. It is
loaded once per DOS VM: a VM fork has its own kernel instance and interrupt
table. Applications use the wrappers in `src/apps/rt.c`; without the
extension they fall back to 8.3 calls. `src/apps/sys.c` uses those wrappers
for tree operations, clipboard and versioned Recycle Bin records.

The desktop and Files accept names up to 255 characters, reject DOS and
Windows reserved punctuation and trailing dot/space, and keep complete names
for display and file operations. A short alias still exists on the FAT volume
for older DOS applications. Files keeps its names and navigation history in
separate DOS blocks so its 16-bit DGROUP remains below 64 KiB. Its list cap is
currently 172 entries. The Files tree sidebar navigates stable short aliases
even when the current folder path is long.

The kernel's `CIUKIDOS.SYS` size ceiling is `0xA900` bytes (43,264), enforced
by the build. It loads at segment `0x0900`; the next reserved area contains
external EXEC frames at `0x1400-0x1457`, then Stage2 starts at `0x1480`.
This is a physical memory layout limit, so the LFN parser lives in the
extension. The kernel changes are limited to correct truncate-on-create and
attribute behavior, plus writing directory timestamps. File creation reads
RTC registers directly: calling BIOS INT 1Ah recursively inside INT 21h
caused the V86 session to fail its file I/O probe.

The QEMU gate `scripts/qemu_test_long_names.py` creates and renames long
desktop and Files names, copies and moves files into long folders, recycles
and restores them, then checks VFAT entries and contents independently and
runs `fsck.fat -n`. Physical hardware has not been tested.
