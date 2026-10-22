# F1-93b drive C holds of the desktop's folder worker (T23 b453bc92)

Directive: `docs/directives/f1-93b-readdir-holds-remaining.md`.
Evidence: the T23 trial of image `b453bc92` (serial capture
`build/codex/t23-b453bc92-serial.txt`, 2026-10-22). Written 2026-10-22 by
the implementer; host evidence only, no QEMU, no image.

## What held drive C in the trial

41 `vfs_hold` records on drive C (each at least 50 ms; one
`vfs_contention` summary, nothing suppressed):

| op | task | records | longest | with waiters |
| --- | --- | --- | --- | --- |
| `readdir` | desktop#31 | 18 | 1904 ms | 6 |
| `stat` | desktop#31 | 9 | 1386 ms | 5 |
| `lookup` | desktop#31 | 5 | 392 ms | 0 |
| `lookup` | desktop#32 | 3 | 100 ms | 1 |
| `readdir` | desktop#29 | 2 | 392 ms | 0 |
| `stat` | desktop#29 | 3 | 255 ms | 2 |
| `metadata` (untagged, `site=vfs`) | desktop#29 | 1 | 1214 ms | 0 |

desktop#31 is the folder worker of `apps/desktop/folder.c`
(`folder_wait=directory-compare` in the `[desktop] stall stage=ipc` record
at the 1549 ms hold). Every second it runs `fs->list("/Documents/Desktop")`:
`opendir` = `open(O_DIRECTORY)` (op `lookup`) and `fdopendir`'s `fstat`
(op `stat`, `px_stat_locked`), then one `CIUKI_SYS_GETDENTS` record per
`readdir` call (op `readdir`, `px_getdents_locked`), then `closedir`.
All three are paths that f1-93 had already converted.

### The holds are preemptions of a starved holder, not device time

- The durations are quantized at 100 ms: 100 (4 records), 102, 102, 103,
  200, 200, 296, 300, 300, 392, 392; 16 of the 41 records lie within 5 ms
  of a multiple of 100 ms. 100 ms is the ready timer of the scheduler: a
  ready `P_NORMAL` task that did not run is boosted 100 ms after it became
  ready (`task.c` `task_ready_timer`, `ready_since + 100`), the boost lasts
  until the next tick (`task.c`, the tick clears `boosted`).
- In the windows of the long holds the foreground DOS VM took the CPU:
  `[cpu] top=dosvm#54:625k..736k` (62–74 %), `idle_pm=0`,
  `dosvm#54 ... effective=9 inherited=9` (11 and 24 at times), while the
  desktop workers run at base 7 (`desktop#31 class=2 base=7 effective=7`)
  and desktop#31 is not in the top five (below 3 %).
- The ATA queue issued 13–41 commands per 4 s around the long holds
  (`issued=5745→5786`, `5786→5799`, `6074→6094`), almost all under 2 ms
  (`svc=4436/1264/57/28`): the device cannot account for 1.5–1.9 s.

The kernel is not preemptible except at explicit points (`arch/trap.c`
reschedules only on return to user mode; `kpreempt_point` elsewhere).
Inside a filesystem section that point is `fs_service()` (`fs_port.c`). A
desktop worker starved by the DOS VM gets the CPU only at its starvation
boost, runs until the next tick, and loses the CPU at the next
`fs_service()` with `g_need_resched` set. After f1-93 the cached steps still
reached `fs_service()` while holding drive C's guard:

| path (trial op) | preemption point inside the guard (main) |
| --- | --- |
| `fstat`/`stat` chain walk (`stat`) | `fat_chain_clusters`: `fs_service()` at cluster 0 of every section and every 128 clusters |
| readdir step (`readdir`), DOS/Win32 FindNext (`find`) | `get_copy`: `fs_service()` every 128 FAT decodes; `dir_read` resolved every slot's cluster from the chain start (`chain_at`), O(slots × clusters) decodes per step |
| guarded lookup (`lookup`: native resolver attempts 3–66, legacy `resolve`), `vfs_chdir` canonical walk (`chdir`) | the whole directory scanned in one cache-only section, same `get_copy` points, plus a full path restart (or step restart) after every warmed sector |

Each such point reached after the boost had expired kept drive C for
another 100 ms. This is the unbounded priority inversion of the Linux
rt-mutex design document ("A tries to grab a lock that C owns ... B
executes, and since B is of a higher priority than C, it preempts C ... in
fact preempting A", https://docs.kernel.org/locking/rt-mutex-design.html):
the desktop's UI or its other workers wait for drive C (the `waiters=` of
the records) while the DOS VM preempts the holder. The Windows VMM avoided it
by boosting the owner of a critical section: "When the critical section is
first claimed, the execution priority of the current VM is boosted by the
Critical_Section_Boost value" (Windows 3.1 DDK, Virtual Device Adaptation
Guide, chapter 24, `Begin_Critical_Section`,
https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/; the existing
research note `docs/design/research/kernel-latency-2026-10-14.md` lists the
boost ladder).

## Remaining callers (item 1)

Every path that can still hold drive C's reservation or guard across a
device read or a preemption point, after this change:

| caller | operation (tag) | status |
| --- | --- | --- |
| `px_getdents_locked` → `vfs_dir_walk_locked` | `readdir` | converted (below) |
| `px_stat_locked` → `vfs_chain_clusters_locked` | `stat` | converted |
| `vfs_fat_transaction_locked` guarded `LOOKUP`/`NEXT` (native `px_resolve_locked`, legacy `resolve`/`lookup`) | `lookup`, `open`, `stat`, `chdir` | converted |
| `vfs_find_next` (DOS INT 21h 4Fh, Win32 FindNextFile) → `vfs_dir_step_locked` | `find` | converted |
| `vfs_chdir` canonical walk → `vfs_dir_step_locked(name)` | `chdir` | converted |
| `dos_file_volume.c` refresh → `vfs_dir_step_locked` on the VM's logical C: | DOS | unchanged: logical volumes use the f1-93 device windows |
| bounded fallbacks: `vfs_dir_walk_locked` after 64 restarts caused by publications or warms without progress, `vfs_chain_clusters_locked` after 256 | any | kept as the last resort, counted in `vfs.cold_fallbacks`; zero in every gate |
| Win32 `ReadFile` (`proc/win32/files.c`, `W32_F_READ`): `vfs_lock(drive)` for the whole call, so `vfs_read_identity_locked` takes its reserved path, `VFS_IO(fat_read_cursor)` | `read` | **not converted**: a file-data path (not in this trial's records); needs a per-description lock across the relay chunks to keep the synchronous file pointer atomic; proposed follow-up |
| `vfs_pagefile_map` (`pagefile_walk` under `VFS_IO`) | pager | not converted: pagefile inactive in the trial (`swap_capacity=0`) |
| writer publication steps (`transaction_publish` → `cache_write`) | `write`, `rename`, `unlink`, ... | not converted: may wait for write-back when the dirty budget is exhausted, and `writesec` keeps its `fs_service()` outside transactions; f1-89 territory |
| mount, unmount, shutdown commit, `BOOT.CFG` edit, optical/NTFS providers | `mount`, `commit` | unchanged: administrative or other letters |
| final close (`file_description_close`), `lseek` | were untagged (`metadata`/`vfs`) | now tagged `close` and `seek` (`site=native`) so the next trial attributes the 1214 ms desktop#29 record |

## Changes

1. **A cached section never reaches a preemption point** (`fat.c`). In a
   cache-only section (`cache_only`, the caller holds the guard) `get_copy`
   and `fat_chain_clusters` no longer call `fs_service()`. Instead a
   section is bounded by work, `FAT_CACHED_BUDGET` (128 units, about one
   microsecond each on the T23: a resident sector copy, a FAT decode or a
   decoded entry cost 2), counted once the step has read its first
   directory sector; a cached chain walk visits at most
   `FAT_CHAIN_CACHED_SECTORS` (32) FAT sectors. At the bound the step
   returns `-FS_EAGAIN` with `FAT_CACHE_YIELD` (or `UINT64_MAX` for chain
   walks); the VFS gives the guard up, calls `fs_service()` with nothing
   held (`warm_sector`), readmits and continues.
2. **Resumable directory steps** (`fat_dir_cached`, `vfs_dir_walk_locked`).
   A stopped section reports the first entry not yet settled (the first
   slot of an LFN run being assembled); the step resumes there while the
   volume's `mutation_sequence` is unchanged, and restarts from its start
   after a publication (counted in `vfs.step_restarts`). This is the
   detect-and-retry rule of Linux name lookup: "`rename_lock` is a seqlock
   that is updated whenever any dentry is renamed. If `d_lookup` finds that
   a rename happened while it unsuccessfully scanned a chain in the hash
   table, it simply tries again"
   (https://docs.kernel.org/filesystems/path-lookup.html); every FAT,
   directory or data write bumps `mutation_sequence` (`writesec`,
   `transaction_publish`). Warms and yields that make progress no longer
   consume the 64 attempts. The guarded lookup of
   `vfs_fat_transaction_locked` resumes in place the same way instead of
   returning `-FS_EAGAIN` after every warmed sector (the path walk restarts
   only after a publication).
3. **Directory walk positions** (`struct dir_walk`, `struct fat_dir_hint`).
   A walk keeps the cluster of its current and previous slot range and the
   last sector read: a scan costs one FAT decode per cluster and one sector
   read per 16 slots, instead of a chain walk from the start and a sector
   read for every slot (the MS-DOS `FNDCLUS` rule already used for file
   cursors in f3-51c). The hint survives across calls only while the
   volume's `mutation_sequence` and media generation are unchanged; a
   native readdir description keeps it (`file_description.dir_hint`).
   `fat_next`, `fat_next_dos`, `fat_lookup` and the alias check use the
   same walk.
4. **Directory warm-ahead** (`fat_warm`): a cluster run continues into the
   next cluster while the resident FAT names the adjacent cluster as its
   link, up to one 64 KiB command. A cold 2,000-entry directory is warmed
   in 4 commands instead of 47 (4 KiB clusters) or 375 (512-byte clusters).
5. Tags for the two untagged native holds (`close`, `seek`).

## Host evidence (2026-10-22)

`files_holds_test --desktop31 <image> fat16.img` (new, in
`scripts/test/host_fs_tests.sh`): a `/Documents/Desktop` folder of 2,000
files (`iNNNN Desktop item.txt`, two LFN slots and the short entry each) and
a last subdirectory `Zz Sub`, copied in sorted order with mtools onto the
T23 C: geometry (FAT32, 4 KiB clusters, as `scripts/build_image.py`) and
onto 512-byte clusters. Every device command costs the measured 106 ms;
every `fs_service()` costs the 100 ms of a starved holder, and counts as a
held preemption when drive C is held. Gate: no hold above 20 ms, no
`vfs_hold` record, no held preemption, no cold fallback.

T23 geometry. Longest drive C hold in model time, `vfs_hold` records,
preemption points reached inside a hold, and the longest hold in host CPU
time (ASAN build, noisy, only a measure of the work inside one section):

| row (trial op) | main | f1-93b |
| --- | --- | --- |
| directory compare, cold (`lookup`, `stat`, `readdir`) | 200 ms, 502 records, 1095 points, 60 µs | 0 ms, 0, 0, 169 µs (6 commands, main 50) |
| directory compare, warm (the next poll) | 200 ms, 433, 1078, 44 µs | 0 ms, 0, 0, 16 µs |
| `stat` of the last entry, cold (unguarded lookup) | 0 ms, 0, 0, 15 µs | 0 ms, 0, 0, 16 µs |
| guarded lookup of the last entry, cold | 107600 ms, 46, 17300, 25666 µs | 0 ms, 0, 0, 48 µs (5 commands, 35 yields) |
| DOS FindFirst/FindNext, 2,000 entries, cold | 200 ms, 503, 1093, 269 µs | 0 ms, 0, 0, 17 µs (7 commands) |
| DOS `chdir` to the last entry, cold | 107600 ms, 1, 1076, 25598 µs | 0 ms, 0, 0, 42 µs (37 yields) |

512-byte clusters: main 900 ms (compare, find), 877700 ms and 189468 µs of
host CPU in one hold (`chdir`), and the guarded lookup fails with `EAGAIN`
after its 64 attempts (375 warms needed, 23700 ms of holds meanwhile);
f1-93b 0 ms everywhere, 5–7 commands per cold guarded or listing walk
(main 379), no fallback, longest hold 124 µs of host CPU.

`test_fat_transactions` asserted the old contract of the guarded lookup
(`attempts>1`: each call returned `-FS_EAGAIN` after warming a sector); it
now asserts that the call completes in place with more than one warm,
still with no device read under the guard.

## Limits

- The model charges the starvation delay at `fs_service()` only. A holder
  can still wait inside a hold for a cache lock whose owner yielded inside
  a long cache loop (`cache.c` calls `fs_service()` under the cache lock,
  which is `task_yield()` with a preemption hold); fs locks have priority
  donation (`kwait_set_owner`), volume reservations do not.
- The root cause is the scheduler: the focused DOS VM at effective 9 leaves
  the desktop's workers about 1 % of the CPU (1 ms per 100 ms). Their own
  work (launch, folder refresh, settings) is just as slow outside any hold.
  A volume-owner boost in the VMM style (`Critical_Section_Boost`) or a
  minimum share for the System-VM-equivalent workers is a scheduler
  decision for the lead.
- A cold `stat` of the last entry of a large directory still issues one
  command per cluster from the unguarded lookup (no hold); the warm-ahead
  applies to the guarded steps only.

## Suites (2026-10-22, `nice -n 19`, one at a time, final sources)

- Strict FS gate, `CIUKI_F187_REQUIRE_BOUNDED=1 scripts/test/host_fs_tests.sh`:
  green (exit 0), including `test_t23_holds --require-bounded` (f1-87,
  f1-89 and f1-93 gates unchanged: alpha cold replay 0 ms, desktop stat
  during a cold walk 1 ms), `files_holds_test` (existing rows and the new
  `--desktop31` rows on both geometries), `test_fat_transactions`, the
  4678 crash cuts, `kernel_compile.py` (51 units with the kernel's
  4096-byte frame limit) and the mtools/fsck oracles.
- `test_kernel_latency` (14 tests), `tests/host/dos/run_file_api.sh`,
  `test_storage_namespace`, `test_storage`, `test_log_recovery`,
  `test_powercuts`, `files_test`, `test_app_gate` (17 tests): green.
- One earlier gate run stopped at `test_fairness`
  (`throughput*1000>=old_throughput*999`, mixed-USB-source variant 0.16 %
  below the old path in the same run) while other host jobs were loading
  the machine; the binary then passed three consecutive runs (both paths
  within ±0.08 %) and the full gate passed. Both paths run the same FAT
  code, so this is the 0.1 % wall-clock tolerance, not a regression.

