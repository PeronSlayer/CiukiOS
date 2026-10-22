# F1-93 volume holds across cold device reads

Directive: `docs/directives/f1-93-native-volume-holds-across-cold-reads.md`.
Evidence: the T23 trial of the 1.0.0 alpha image `8f59e221` (serial capture,
2026-10-21). Written 2026-10-22 by the implementer; host evidence only, no
QEMU, no image.

## What held drive C in the trial

The trial's 288 `vfs_hold` records on drive C (each at least 50 ms; the
diagnostic budget suppressed the rest of each 2 s window):

| op (site) | records | longest | holder path in the code |
| --- | --- | --- | --- |
| `stat` (native) | 240 | 1601 ms | `px_stat_locked`: `st_blocks` walked the file's whole cluster chain with one `VFS_IO(fat_get_cluster)` per cluster under the volume reservation. `UNIFIED.IMG` is 128 MiB: 32768 clusters, 32768 guarded round trips plus the cold FAT runs. |
| `readdir` (native) | 36 | 900 ms | `px_getdents_locked`: one `getdents` call fills its buffer under one reservation, each entry by `VFS_IO(fat_next)`, cold directory sectors read under it. |
| `lookup` (native) | 10 | 2294 ms | `image_open` (`fdtable.c`): the launch read the whole executable with `VFS_IO(fat_read_cursor)` under the reservation taken by the path walk, whose last tag was `lookup`. Matches `[desktop] stall stage=launch no_frame_ms=2149`. |
| `mkdir` (dos) | 1 | 1910 ms | DOS `INT 21h 39h` on the VM's C:, which is the logical volume over `UNIFIED.IMG` (`dos_file_volume`): `transaction_once` ran `VFS_IO(fat_create)` with the VM's volume guard held across every provider read, write and flush (each an I/O on the physical C:). `waiters=0`: only the VM itself can wait on that space. |

The ATA queue reported `svc=3947/1129/79/24` (<2, 2–7, 8–31, ≥32 ms) and
`svc_max_ms=106`. Most commands are short; the long holds are many commands
(and, for `stat`, per-cluster CPU) inside one hold, not single slow commands.

## Why the f1-87/f1-89 replay reported 0 ms

`test_t23_holds` replayed the b51f034f trial, whose desktop holds were
untagged (`op=other`). f1-87 modelled them as 513 reads of an open resource
file, made cold before each read; those reads use the open-file reader path,
which holds no volume reservation at all, so they could only ever measure
0 ms. The log writes, commits, mkdir, rename and unlink of the replay ran
through f1-89's reserve/transfer/publish transactions and were correctly
bounded. None of the paths above (chain walk, directory listing, executable
image read, DOS logical-volume operations, DOS find) was in the request mix,
and `files_holds_test` covered only native mutations. The 0 ms was therefore
a property of the request mix, not of a warm cache: the model never exercised
the operations that held the volume. The f1-87 tags made them visible in this
trial.

## Changes

1. **Directory steps and chain walks without cold reads under the guard**
   (`vfs_dir_step_locked`, `vfs_chain_clusters_locked`, `vfs.c`). A physical
   step runs from resident cache sectors only (`fat_next_cached`,
   `fat_lookup_cached`, `fat_chain_clusters` with `cached`); the first miss is
   read with nothing held (`warm_sector`, now reading the sector's whole
   read-ahead run through `fat_warm`) and the step restarts from the same
   cursor. A completed step saw one consistent state under the guard, so no
   value validation is needed. Each restart warms a run, so a step converges;
   64 restarts (256 for a chain walk) then fall back to the old guarded cold
   read, counted in `vfs.cold_fallbacks` (zero in every gate).
   - `px_stat_locked`: one FAT sector read per FAT sector visited
     (`fat_chain_clusters`, at most 128 sectors per guard section, then a
     yield); the walk restarts from the first cluster when a publication
     happened while the guard was released. The node is retained across the
     walk (`px_drop_locked` reclaims an unlinked chain afterwards).
   - `px_getdents_locked`, DOS/Win32 `vfs_find_next`, `vfs_chdir`'s canonical
     walk: through `vfs_dir_step_locked`.
2. **Executable image read on the reader path** (`image_fat`, `fdtable.c`):
   64 KiB runs through `vfs_read_identity_locked` with no reservation, the
   DOS record locks checked per run, and the snapshot validated afterwards
   (entry, chain stamp, identity and the f3-51c write generation, new
   `vfs_change_generation`); a file changed meanwhile is read again (at most
   three times, then `EAGAIN`). The hold left is the path walk, tagged `open`.
3. **Device windows for logical providers** (`io_window`, `VFS_IO_MODE`,
   `fat_volume.io_window`). A logical volume has no block cache, so the
   cache-only approach cannot apply. Every provider call (`logical_device`
   in `fat.c`: reads, writes, flushes; `dos_file_volume_raw`) is a window of
   the caller's `VFS_IO_MODE`: the drive's guard is given up around the call
   (the hold segment ends and is reported), and readmitted through the
   ordinary FIFO reservation, uncancellably, with the same attribution;
   `transaction_users` keeps the letter attached. Modes:
   - `FREE`: the owner holds the drive's writer turn (logical mutations now
     take it: `turn_drives` includes logical volumes, and
     `vfs_fat_transaction_locked` takes the lazy turn for a logical mutation),
     or the call is a barrier (`FAT_TX_COMMIT`). No other mutator can
     intervene; readers may see the intermediate states between two provider
     writes of one operation (the same C2 boundaries a crash can expose; LFN
     slots without their short entry are skipped as orphans).
   - `VALIDATE`: readers without the turn (lookup, next, stat, find, chdir
     on the VM's C:). A writer announces each write by bumping
     `mutation_sequence` before giving its guard up; a reader whose snapshot
     changed gets `-FS_EAGAIN` and restarts its step or walk.
   - `NONE` (default `VFS_IO`): legacy and teardown paths (detach, unmount)
     keep the guard, unchanged.
   A raw INT 13h write takes the turn and is announced the same way.
4. **Read-compatible admission beside open-file readers.** In the
   concurrent replay the desktop's stat waited 106–212 ms with
   `holder=-`: not for a hold, but because every volume reservation waited
   for in-flight open-file readers (`readers[d]`), i.e. for another task's
   64 KiB device read. f1-89 already admits readers beside read-compatible
   holds; the converse is now true: a reservation whose operation changes no
   chain (`stat`, `lookup`, `open`, `find`, `readdir`, `chdir`, `commit`,
   `mkdir`, `read`) is admitted while readers are in flight. Legacy entry
   points tag before admission (`path_lock_op`, `vfs_find_next`), and the
   read-only transaction steps (enter and retire of lookup/next/commit) no
   longer wait for readers or close reader admission. Mutation tickets and
   every publication step still wait for readers (`guard_busy`).
5. **Readers of a block under write-back** (`cache.c`, `readable_busy`). The
   remaining 106 ms wait was the desktop's lookup reading a root-directory
   sector that the DOS VM's commit was writing back: `get()` waited for any
   busy block. A dirty block under write-back holds stable bytes (writers wait
   for busy, publication returns `EAGAIN`), as `cache_peek` already assumed;
   readers now copy it. A block being filled is still waited for.

The op tags in the records are unchanged in meaning; the executable image's
remaining hold is now tagged `open` instead of inheriting `lookup`.

## Research

- Microsoft, *Windows 98 Resource Kit*, chapter 28, "VFAT File System"
  (owner's local extraction `wrkc28_5.htm`): the 32-bit VFAT driver "is
  reentrant and multithreaded" and works with VCACHE, the protected-mode
  cache driver; IOS queues the file service requests. This supports
  per-request arbitration with cached metadata, not a volume-wide hold over
  device work. Stan Mitchell, *Inside the Windows 95 File System* (cited in
  `f1-89-reserved-publication.md`) shows VFAT's operation-long claim for
  writers; CiukiOS keeps that claim as the writer turn, not as a reader hold.
- Linux pathname lookup ([docs.kernel.org](https://docs.kernel.org/filesystems/path-lookup.html)):
  RCU-walk samples `d_seq`, "checks to see if a change has been made, and
  aborts or retries if it has", and "restarts from the top with REF-walk"
  when it cannot continue. Precedent for the `VALIDATE` window and the
  restart of a walk; the physical steps instead follow f1-89's guarded
  cache-resident attempt.
- Linux stable pages ([LWN, "Stable pages"](https://lwn.net/Articles/442355/)):
  "any attempt to write to a page which is under writeback will simply wait
  until the writeback completes". Writers wait for write-back; readers of
  the page's current bytes do not need to. Precedent for item 5.

## Host evidence (2026-10-22)

All runs with `nice -n 19`, one at a time, `ASAN_OPTIONS=detect_leaks=0`.

`files_holds_test` (native px paths, cold cache, 106 ms per command):

| operation (trial tag) | commands | main: hold | f1-93: hold | main: namespace takes | f1-93 |
| --- | --- | --- | --- | --- | --- |
| stat 12 MiB chain (`stat`) | 4 | 212 ms | 0 ms | 24581 | 12 |
| fstat same file (`stat`) | 2 | 212 ms | 0 ms | 24577 | 8 |
| executable image 1 MiB (`lookup`) | 19 | 1802 ms | 0 ms | 21 | 22 |
| readdir 120 entries (`readdir`) | 18 | 1908 ms | 0 ms | 124 | 160 |
| open existing (`open`) | 14 | 0 ms | 0 ms | 7 | 7 |
| native mkdir (`mkdir`) | 37 | 0 ms | 0 ms | 17 | 17 |

plus the trial's record mix replayed cold (240 stat, 36 readdir, 10
executable reads, interleaved): no hold above 0 ms, no `vfs_hold` record.

`test_t23_holds` (new phases; the DOS VM's C: is the production
`dos_file_volume` over an image file on the physical C:, the provider reading
and writing it through the VFS as `dosvm_disk.c` does):

Cold steps at 106 ms per command (device commands per step: DOS mkdir 23,
DOS stat 1, DOS open + read 5, DOS find on the VM's C: 1, raw INT 13h read +
write 5, DOS find of 100 entries on C: 15, stat on C: 9, chdir on C: 1).
Longest hold per operation tag:

| op | main: VM's C: | f1-93: VM's C: | main: physical C: | f1-93: physical C: |
| --- | --- | --- | --- | --- |
| mkdir | 2438 ms | 0 | 0 | 0 |
| stat | 106 ms | 0 | 0 | 0 |
| open | 106 ms | 0 | 0 | 0 |
| find | 106 ms | 0 | 212 ms | 0 |
| raw sector (`metadata`) | 318 ms | 0 | 0 | 0 |
| read, lookup, chdir, commit, write | 0 | 0 | 0 | 0 |

f1-93 also reports 164 device windows on the VM's C:, 0 window restarts,
14 warm restarts on C:, 0 cold fallbacks, no `vfs_hold` record.

Real time, 106 ms per command: a DOS find walks a cold 100-entry directory
on C: while the DOS VM creates and removes directories on its C:; the
desktop stats a resident file every 2 ms. Main: `desktop_stat_max_ms=529`
(9 stats in 4.1 s). f1-93: `desktop_stat_max_ms=1` (661 stats in 3.2 s);
gate 30 ms. Intermediate f1-93 states, before items 4 and 5: 211 ms (waits
on in-flight readers) and 107 ms (a block under write-back).

The f1-93 gates in `test_t23_holds` are unconditional (the test fails on the
pre-f1-93 tree, as shown above); `files_holds_test` checks every row.

Suites on the final sources (2026-10-22): strict FS gate
(`CIUKI_F187_REQUIRE_BOUNDED=1 scripts/test/host_fs_tests.sh`) green,
including the 4678 crash cuts; `test_kernel_latency` (14 tests),
`tests/host/dos/run_file_api.sh`, `files_test`, `test_app_gate`, and the
storage harnesses (`test_storage_namespace`, `test_storage`,
`test_log_recovery`, `test_powercuts`) green.

`test_vfs_contention` asserted that a stat queues behind an outstanding
desktop reader; it now asserts the stat is admitted beside the reader, and
keeps the "deadline reader passes a queued metadata ticket" check with a
metadata mutation (`vfs_attrib`).

## Limits

- The read-only optical/NTFS providers (`fs_ro_*` under `VFS_IO`) still read
  under their drive's reservation; they are other letters than C: and not
  part of this directive.
- `VALIDATE` readers of the VM's C: restart only when the same logical volume
  is written meanwhile; with one VM thread per volume this does not happen in
  practice. The bound (64 restarts, then the guarded read) is counted.
- CPU time inside a guard section is not in the clock model; the structural
  evidence is the namespace-take count (stat: 24581 to 12).
