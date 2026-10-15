# F0-32 bounded FAT writes

Research/source comparison, 2026-10-16; host qualification only.

Review fix research, 2026-10-16: [Microsoft FAT 1.03, Date and Time
Formats, page 25](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
specifies two-second packed write timestamps. The in-place `fat_write` can
therefore commit new contents while every byte of `fat_entry` stays unchanged;
`fs_timestamp` also uses a fixed fallback when its calendar provider fails.
Both native `file_io_locked` and the legacy `px_legacy_changed` hook previously
treated entry equality as no change. Decision: a confirmed nonzero write count
must notify ctime independently of entry equality, including counts retained
after a post-commit error. Preserve metadata-difference notification when no
bytes are confirmed. Pass an explicit committed-write flag through the legacy
hook; other mutations keep the existing equality check. Regressions advance
kernel time with fixed packed timestamps for both a valid calendar and the
fallback, and check native/legacy overwrites plus zero-length no-ops.

Review fix validation: both requested host suites exit 0. The new overwrite
ctime regression passes for native and legacy writes with byte-identical FAT
entries; native files now complete 15,390 checks with no descriptors, pins or
pages left. The filesystem suite retains 41,105 in-place checks and 1,681 crash
cuts; the kernel suite retains 1,704,533 power-cut checks. The requested
`nice -n 19 python3 scripts/build_kernel.py` exits 0, including the FPU/SIMD
audit, producing `build/f0/VMM.ELF` (5,248,572 bytes). Full command output and
verbatim result excerpts are linked in the report's `Review fix` section.

[Microsoft FAT 1.03](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
defines allocation chains separately from the directory's byte length, packed
FAT12 entries and FAT32's preserved upper nibble. [FreeDOS rwblock/map_cluster](https://github.com/FDOS/kernel/blob/master/kernel/fatfs.c)
and [FatFs f_write](https://github.com/zephyrproject-rtos/fatfs/blob/master/ff.c)
update affected sectors and extend allocation, rather than copying existing
file prefixes. [FatFs critical sections](https://elm-chan.org/fsw/ff/doc/appnote.html#critical)
describe interrupted overwrites and appends as distinct recovery cases.

Previously, fat_write called full-file replace for every write; the native
syscall transferred at most one page while holding the VFS lock. A 3 MiB cache
therefore wrote every growing prefix. Decision: ordinary writes update sectors
in place, using read/modify/write for partial sectors and zero-filled seek gaps.
New clusters are selected under the caller's namespace lock without publishing
allocation. Their data is initialized and flushed first, then a detached chain
is constructed and flushed, then the previous EOF link is extended and flushed,
then the directory entry publishes the new size once. FSInfo follows publication.
A volatile per-entry cursor avoids rescanning the complete prefix at each append;
COW truncation resets it. No additional allocation workspace or cache is needed.

The existing COW path is retained exclusively for explicit fat_truncate,
including zero-filled extension and shrink. POSIX does not itself require a
whole-file copy; retaining this implementation preserves the existing single
owning-entry switch and FAT12 packed-link safety of truncation. No write/pwrite
uses that path. In-place overwrites do not promise atomic old/new file contents
across a power cut; directory length remains old or new under sector-atomic
metadata writes and truthful device barriers. Torn metadata sectors remain a
corruption case, rather than a new atomicity claim.

The completed ownership scan now identifies every regular file's published EOF.
Only an EOC/link disagreement there can be repaired as interrupted extension or
tail detachment; disagreement inside the published extent, invalid links,
cycles and crosslinks retain the write gate. After sole ownership is established,
recovery trims excess regular-file tails before reclaiming detached allocation.
Repair remains restartable after a further power cut. New write allocations
avoid FAT12 entries crossing sectors; extension of an imported split FAT12 live
tail safely returns EOPNOTSUPP, as existing directory growth already does.

Native read/write transfers to regular files use at most 64 KiB of temporary
contiguous pages. Larger calls return a short count and drop the namespace lock
before the caller submits the next run. The full request still undergoes user
range and EFBIG validation before any short transfer. Append selection and offset
updates remain serialized for each call. This bounds a 3 MiB application's lock
ownership to individual runs; it does not unlock partially published FAT updates.
No ABI, memory limit or build profile changes.

Exact host commands/results and sector counts are recorded in
`build/codex/f0-32-report.md`. QEMU f0-smoke, f2-desktop, f3-vm and physical T23
latency remain the lead's qualification on a newly built canonical image.

Final filesystem host run: 41,105 new write checks; 1,681 existing crash cuts
(FAT12/16/32, four persistence modes), zero crosslinks/corrupt owners and no
unknown fsck diagnostics; three clean fsck checks and independent mtools
comparisons. All 39 selected translation units compile with the exact kernel
CFLAGS. A 3 MiB FAT32 append writes 6,144 payload sectors with either call size;
metadata adds 4,606 sectors at 4 KiB and 382 at 64 KiB. Settings/registry/overlay
updates write two/one/one payload sectors respectively and retain allocation.

Final kernel host suite: exit 0 with ASan/UBSan. The expanded f0-25 harness
passes 1,258 cuts (1,129 workload events and 129 repeated-repair events),
1,704,533 checks and clean ownership scans after recovery. Native file tests
pass 15,269 checks, including 64 KiB short transfers, a directory poll between
runs, append/positioned offsets, full-request EFBIG, allocation rollback and
post-commit FSInfo error counts. The optional DOS guest runner declares its
existing absent-libx86emu/DOSBOOT.IMG skip; DOS VM host tests pass separately.
