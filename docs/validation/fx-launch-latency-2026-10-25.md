# fx-launch: program start latency on the T23 trial of main 60d029c9

Owner report (1.0.1 alpha trial, Pentium III-M 1.13 GHz laptop, PATA UDMA2
disk): native and Win32 programs, NetSurf first of all, still take long to
open. Evidence: the trial's disk log (`legacy/local/physical/2ca3b0b6/`, kept
private; only the record lines needed are quoted here) and the earlier
verbose trials' `[launch-io]` records. Host evidence only: no QEMU or
hardware run belongs to this change.

## Launches in the trial

The boot was quiet, so the kernel's `[launch-stage]`/`[launch-io]` records
(verbose only until now) are absent; the desktop's own records are:

```
[desktop] launch name=netsurf pid=8 error=0 queue_ms=52 spawn_ms=1277
[desktop] first-frame name=NetSurf - CiukiOS pid=8 process_ms=1331 window_ms=789 paint_ms=4013 present_ms=26 total_ms=6159
```

| # | Program | queue | spawn | process | window | paint | present | total | CPU idle ‰ around |
|---|---------|------:|------:|--------:|-------:|------:|--------:|------:|-------------------|
| 1 | freecell | 45 | 101 | 148 | 26 | 82 | 28 | 284 | n/a (before log wrap) |
| 2 | mines | 42 | 101 | 144 | 9 | 23 | 14 | 190 | n/a |
| 3 | lua (console) | 52 | 74 | – | – | – | – | – | n/a |
| 4 | netsurf | 52 | 1277 | 1331 | 789 | 4013 | 26 | 6159 | n/a |
| 5 | netsurf (again) | 53 | 496 | 551 | 627 | 671 | 57 | 1906 | n/a |
| 6 | music (again) | 72 | 68 | 142 | 43 | 74 | 31 | 290 | 572/364 |
| 7 | viewer | 10 | 437 | 451 | 46 | 291 | 49 | 837 | 0/0 |
| 8 | gfxdemo | 83 | 339 | 426 | 1561 | 218 | 96 | 2301 | 353/0 |
| – | CDPLAYER.EXE (Win32) | – | – | – | – | – | – | about 10 s of DLL reads | 105…0 |
| 9 | serial | 34 | 667 | 705 | 173 | 93 | 63 | 1034 | 7/9 |
| 10 | control | 12 | 721 | 737 | 56 | 1117 | 46 | 1956 | 116/0 |
| 11 | control (again) | 11 | 165 | 178 | 55 | 376 | 74 | 683 | 326/0 |

All times in ms. `spawn` is the kernel spawn call (image read, ELF load,
process creation); `window` is process start to CREATE_WINDOW; `paint` is
the program's first render. Loaded bytes (PT_LOAD file sizes of the
60d029c9 build): freecell 439,786, mines 477,830, lua 324,578, NetSurf
4,591,547, music 565,690, viewer 571,834, gfxdemo 525,686, serial 478,606,
control 1,277,646. CDPLAYER.EXE's start log entry lists ntdll, kernel32,
user32, gdi32, advapi32 and winmm (3.2 MB of DLL images, read whole).

Other records around the starts:

- `[mem] ... owner=cache_data resident=7161` from tick 30848 to the end of the
  20-minute session: the block cache sat at its C4 ceiling (32 MiB) the whole
  time, with 113,623 NORMAL pages free.
- `[dosvm] image verify result=verified source=hash bytes=27684864 ms=87139
  blocks=423`: the DOS image check read 27.7 MB in the background during the
  first five starts.
- `[storage] vfs_hold drive=C ms=167 task=CDPLAYER.EXE#103 op=read` and eleven
  more such holds of 103–298 ms: each 64 KiB DLL snapshot run took that long.
- `[cpu] top=CDPLAYER#103:454k446s8`, `[cpu] device_top task=CDPLAYER.EXE#103
  us=413077`: the Win32 start spent 446 ms of 2 s in the kernel.
- `[sched] inherit from=startup-log#16 to=CDPLAYER.EXE#103 ... reason=drive`,
  `inherit from=CDPLAYER.EXE#103 to=startup-log#16 ... reason=drive` and
  `stretch_max_ms=13 task=startup-log#16` in four consecutive windows: the
  start-log flusher and the Win32 start waited on each other's C: holds.
- `[sched] device_overrun task=storage-writer#15 count=10 consumed_us=3711`
  in every window: the storage writer's passes take about 3.7 ms each.
- `[desktop] client_drop window=0 pending=128 silent_ms=5000` 6 s after
  NetSurf #4's first frame, then `[netsurf] exit reason=channel-eof`: the
  first NetSurf window was dropped by the desktop and the user started it
  again (#5).

## Where the time goes

Calibration from earlier verbose trials on the same machine: `[launch-io]`
records of 4bcd1d43 (27 commands, 1,699,840 B, `dma_ms=56`; 39 commands,
1,671,168 B, 59 ms) and 82fadce3 (94 commands, 2,052,096 B, 77 ms; 21
commands, 1,084,416 B, 36 ms) give 0.1 ms per command plus 31 MB/s. The disk
therefore explains about 150 ms of NetSurf's 1,277 ms spawn, and 15 ms of
freecell's 101 ms. The rest is CPU in the read path.

The block cache indexes its 512-byte slots with a fixed table of 256 buckets
(`src/kernel/fs/cache.h:83` and `bucket()` at `cache.c:55` before this
change), sized in F1 for the fixed 8 MiB cache (13,440 slots, about 52 per
chain). C4 (f1-85, 2026-10-19, `cache_adaptive_enable`, `cache.c:502`) lets
the cache grow to 32 MiB without resizing the table: 57,288 slots on the
trial, about 224 per chain. Every lookup walks its chain (`find`,
`cache.c:370`); a missing sector costs three walks in `cache_read_run`
(`cache.c:973`, the first pass, the recheck at 1004 and the run extension at
1013) plus the eviction's `unhash` (`cache.c:234`). The production cache in
the host replay below visits about 600 descriptors per sector read cold and
17–22 per sector warm. The descriptors (3.2 MB on i686) do not fit the
512 KiB L2, so each visit is about one memory latency; at 0.15 µs per visit
the replay reproduces the uncontended cold starts:

| Start | Trial spawn_ms | Model before | Disk part |
|-------|---------------:|-------------:|----------:|
| freecell | 101 | 98 | 15 |
| mines | 101 | 107 | 16 |
| lua | 74 | 72 | 11 |
| NetSurf | 1277 | 1060 | 155 |

The same table explains why the earlier verbose trials 82fadce3 and 4bcd1d43
(builds 3357582f and c180f28e, both before f1-85's merge 17c8d56a, so a
fixed 8 MiB cache) measured about 100 ms per MB cold and 20–30 ms per MB
warm, and this one 210–280 ms per MB cold: the per-sector cost grew with the
cache.
The cost is paid by every file read in the system (FAT and directory sectors,
fonts, icons, resources, the DOS image check, the music stream) under the
cache lock, and by the storage writer's read-ahead passes at device priority.

Later starts (viewer, gfxdemo, serial, the first control) ran with CPU idle
at 0–9 ‰: the desktop compositor, a desktop worker with 150–358 ms of kernel
time per 2 s window (`vfs_hold ... task=desktop#44 op=readdir` 141–204 ms),
w32srv and the Win32 start competed with the launcher thread, which runs at
the normal level. Their spawn_ms is 3–6 times the uncontended model.

Win32: every DLL is snapshotted whole, copied into the process, and
W32_LDR_IMAGE hashed it twice with SHA-256, once in `w32_loader_image`
(`sections.c:896`) and again in the section lookup
(`image_section_map_inner`, `sections.c:739`) for the same pinned bytes.

The start log: `startup-log` flushed `CIUKIST.LOG` one second after a start
(`STARTUP_LOG_DELAY_MS`, `mount.c:1677`), so during a Win32 start, whose EXE
and DLLs are queued over seconds, the flush (a FAT transaction with a device
flush) ran several times inside the launch.

Quiet boots, used by every owner trial, never recorded the kernel side of a
launch (`proc_launch_start`/`proc_launch_elf`, `syscalls_proc.c:23` and 32):
no earlier fix could be verified on hardware.

## Why the earlier fixes did not hold

- f2-39 (2026-10-12) added launch records, positional icon reads and larger
  ELF batches. Its records were verbose-only, and it did not touch the cache
  lookup; the cache was then a fixed 8 MiB.
- f1-85 (2026-10-19) made the cache adaptive up to 32 MiB with the F1 index
  unchanged. Every lookup became about four times longer: the regression
  this change removes.
- f1-93/f1-93b (2026-10-21/22) stopped holding the volume reservation across
  device reads. Holds became shorter, the per-sector CPU did not.
- fx-8 (2026-10-23) moved spawns to a launcher worker and reads only the
  loadable segments (NetSurf no longer reads its debug sections). The bytes
  went down; each byte still cost the same descriptor walks.
- fx-15 (2026-10-23) opened NetSurf's fonts on a worker and added the
  first-frame record; fx-23 (2026-10-23) bounded the storage writer's kicks
  and made background reads drop-behind. Neither changed the lookup.
- G29d (2026-10-24) took the start log out of exec, but its one-second settle
  still placed the flush inside multi-second launches.

## Research and decisions

Review corrections (2026-10-25, directive `review-fixes-fx-launch.md`):
[Microsoft MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile)
distinguishes read-only view permissions from coherent shared backing, and
describes copy-on-write pages becoming private on writes. This agrees with
CiukiOS `map_view`/`ua_alias`: a read-only extent can still have shared backing.
`ua_pin` keeps mappings and pages alive but does not exclude alias writes.
After pinning, the loader must therefore check every extent for private
object backing, no shared object, and no write permission before validating,
hashing or seeding. Checking the backing kind also covers native shared
aliases whose extent kind is `UA_ANON`. The ordinary `W32_LDR_OPEN` private
read-only snapshot remains accepted even when its original file changes.
This preserves one hash per accepted snapshot without copying it again.

[Microsoft time-interval guidance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)
defines latency using the start and end events of the measured activity.
Here the start event is entry to `proc_spawn`, including user argument
copying and child preparation. Capture the existing `proc_launch_now()`
clock unconditionally there; the console's quiet setting must not change
the measured interval. This uses CiukiOS's existing 1 ms clock resolution;
it does not adopt the modern Windows counter API or its implementation.
The local Windows 98 Resource Kit chapter 26, `wrkc26_10.htm` paragraphs on
VCACHE/VMM and `wrkc26_4.htm` “Optimizing Paging”, was checked for the
existing launch/cache context; neither specifies immutable loader buffers
or CiukiOS instrumentation boundaries. No private kit text is redistributed.

The owner's Windows 98 Resource Kit (`build/rskt/`) chapter 26 “WinAlign and
MapCache” (`wrkc26_10.htm`): restarting a program shortly after closing it
runs much of it from VCACHE rather than from disk; chapter 26 “Optimizing
Paging” (`wrkc26_4.htm`): executables and memory-mapped files go through the
cache, which is designed not to be overwhelmed by them; chapter 28
(`wrkc28_5.htm`) and chapter 10 (`wrkc10_6.htm`): VCACHE's pool is dynamic,
based on free memory. Chapter 10 “Disk Defragmenter” (`wrkc10_5.htm`): Task
Monitor records start-up access patterns asynchronously. VCACHE's block
lookup structure is undocumented (the DDK interface is listed by service
name only, [Ralf Brown, INT 20h VxD 048Bh](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-2527.htm);
[VCMon](https://jacobfilipp.com/DrDobbs/articles/DDJ/1996/9605/9605c/9605c.htm)
only hooks its hit/miss lookup). A cached block found in near-constant time
is therefore a CiukiOS implementation property, not a Windows constant; it
changes no C4 policy (sizes, windows, LRU order, read-ahead).

Decisions:

1. **Sector index sized to the cache** (`cache.h`, `cache.c`): at least one
   bucket per two slots the cache may hold (a power of two, at least the 256
   inline buckets), in whole NORMAL pages charged to `MM_CACHE_META`; built
   by `cache_init` for its slots and by `cache_adaptive_enable` for the
   ceiling (32 MiB: 32,768 buckets, 32 pages on i686), so growth never
   outruns it. The bucket function keeps the 128 sectors of an aligned
   64 KiB run in distinct buckets and scatters runs and devices
   multiplicatively. A failed page allocation keeps the current table
   (correct, slower). `cache_stats` reports buckets, pages, lookups and
   probes. The table pages sit outside the C4 byte budget (0.4 % of it).
2. **One SHA-256 per W32_LDR_IMAGE** (`sections.c`): the section lookup
   reuses the digest `w32_loader_image` computed over the same pinned,
   private read-only bytes, as enforced by `w32_loader_call` after pinning.
   `w32_image_section_map` (process creation) still hashes once.
3. **Start log after the burst** (`mount.c`, `vfs.c`): the flusher waits
   until no start has been queued for 10 s, at most 60 s, and still writes
   at once at 3/4 of the queue; Shut Down and the defragmenter's analysis
   flush it themselves as before.
4. **Spawn stage in quiet boots** (`syscalls_proc.c`, `spawn.c`): every spawn
   logs one `[launch-stage] name= pid= spawn_us= elf_us= read_bytes=
   disk_bytes= disk_requests= dma_ms=` line at the end of image loading
   (also for Win32 images, after the image and ntdll are mapped); quiet
   accounting stops there. The window and first-frame stages and the
   `[launch]` summary stay verbose-only.

Not changed, for the lead: the C4 ceiling `min(32 MiB,U/8)` (a reviewed
contract number; on the trial 440 MiB stayed free while launch images
competed with the music stream for 32 MiB), the launcher thread's priority
(the scheduler contract), and the storage writer's full descriptor scan in
`cache_writeback_tick` (`cache.c:1550`, about 57,000 descriptors per due
tick; a candidate for the 3.7 ms `device_overrun` passes, which the log does
not attribute).

## Host evidence

`tests/host/fs/test_launch_replay.c` (in `host_fs_tests.sh`, alone with
`--launch-replay`): the production
cache sized as on the T23 (`cache_initial_bytes`/`cache_adaptive_enable`
with the trial's ledger: usable 126,816, free 113,623, high 7,926 pages),
grown to its ceiling by C4 windows, filled with 40 MiB of boot data, then
the trial's starts in order with the DOS image check's 423 background
blocks and the music stream on a second device in between. Before this
change (same source, built against the previous `cache.c`):

| Start | Trial | Commands | Sectors | Visits/sector | Model ms |
|-------|------:|---------:|--------:|--------------:|---------:|
| freecell | 101 | 10 | 863 | 598 | 98 |
| mines | 101 | 11 | 938 | 601 | 107 |
| lua | 74 | 8 | 638 | 588 | 72 |
| NetSurf | 1277 | 82 | 8980 | 626 | 1060 |
| NetSurf again | 496 | 0 | 0 | 17 | 85 |
| music, cold | – | 13 | 1110 | 605 | 128 |
| viewer | 437 | 13 | 1122 | 629 | 133 |
| gfxdemo | 339 | 13 | 1032 | 630 | 123 |
| CDPLAYER + 6 DLLs | – | 76 | 6399 | 629 | 760 |
| serial | 667 | 11 | 939 | 635 | 112 |
| control | 721 | 25 | 2502 | 633 | 298 |
| control again | 165 | 0 | 0 | 4 | 19 |
| music again | 68 | 0 | 0 | 22 | 11 |

21 checks fail (visits per sector ≤ 10, modelled time bounds). After
(visits vary between runs because the device's address enters the bucket
function: 1.8–6.5 per cold sector over 40 runs, about 1.2 warm; the model
times vary by less than 1 ms):

| Start | Commands | Sectors | Visits/sector | Model ms |
|-------|---------:|--------:|--------------:|---------:|
| freecell | 10 | 863 | 2–5 | 22 |
| mines | 11 | 938 | 2–5 | 24 |
| lua | 8 | 638 | 2–6 | 16 |
| NetSurf | 82 | 8980 | 3–4 | 226 |
| NetSurf again | 0 | 0 | 1.2 | 65 |
| music, cold | 13 | 1110 | 2–6 | 28 |
| music again | 0 | 0 | 1.2 | 8 |
| viewer | 13 | 1122 | 2–5 | 29 |
| gfxdemo | 13 | 1032 | 2–5 | 26 |
| CDPLAYER + 6 DLLs | 76 | 6399 | 3–4 | 163 |
| serial | 11 | 939 | 2–5 | 24 |
| control | 25 | 2502 | 3–5 | 63 |
| control again | 0 | 0 | 1.2 | 18 |

Physical commands and sectors are identical before and after: only CPU
changes. The model counts the disk (as calibrated above), 0.15 µs per
descriptor visit, 0.2 µs per lookup and four copies of each loaded byte at
300 MB/s.

Other tests: `tests/host/win32/kernel_core_test.c` maps a fresh DLL through
W32_LDR_OPEN/W32_LDR_IMAGE and requires exactly one SHA-256 pass over it
(the previous `sections.c` hashes 2 × 9,216 bytes and fails);
`tests/host/proc/launch_test.c` requires the quiet spawn record, its I/O
figures and no accounting after it; `tests/host/proc/startup_image_test.c`
requires the quiet period to run from the newest start and the maximum to
bound it; `test_cache.c` and `test_cache_adaptive.c` iterate the sized
index (the reclaim case rebuilds the F1 chain geometry with a stride of
1/32 of the buckets, so the scanner's bounded continuation is still
exercised); `bounded_work_test.c` (f0-44 spans) still builds one chain of
every slot, now by choosing LBAs whose bucket is 0, and its find/unhash/
victim walks keep their preemption bound.

Review regressions: the kernel-core harness presents a read-only section
view with a writable alias, holds image publication busy and changes the
alias from the serialization wait callback. Before the review fix, loading
succeeds with the old digest; afterward the source is rejected before the
hash or wait and its pin is released. A private snapshot is accepted while
the same callback mutates its original shared source during the wait, with
exactly one hash and the original bytes mapped. A writable later source
extent is rejected too. Changing the file after `W32_LDR_OPEN` leaves its
snapshot's mapped contents intact. The launch harness advances the clock
25 ms in the first argument range walk before `proc_launch_start`: the
quiet record must retain the entry timestamp and include this delay.

## Expected effect on the T23

From the model, for uncontended starts: native cold spawn about 45–55 ms
per MB of loaded image instead of 210–280 (NetSurf about 0.23 s instead of
1.3 s, the Control Panel about 65 ms instead of 0.3–0.7 s), warm spawn
mostly the copies (NetSurf about 65 ms). A Win32 start's DLL reads cost
about 160 ms of cache work instead of 760, plus one SHA-256 pass fewer per
newly mapped DLL (3.2 MB less hashing for CDPLAYER). All other file I/O
(fonts, resources, directory reads, the DOS image check, music) loses the
same per-sector walk. These are model figures; the next trial's
`[launch-stage]` records measure the real spawn stage in a quiet boot.

## Residual gaps

- Warm restarts in the trial were slower than any cache model (NetSurf 496
  vs 85 ms modelled before the change, control 165 vs 19): CPU competition
  with idle at 0, or eviction; the new quiet `[launch-stage]` records
  (`disk_requests`, `dma_ms`) will tell them apart.
- Window and paint stages (NetSurf 789 + 4,013 ms cold, gfxdemo 1,561 ms,
  control 1,117 ms) are the programs' own start-up work and file reads; only
  the cache part of their reads is addressed here.
- The desktop dropped NetSurf's first window after 5 s of silence with 128
  pending events; Windows 98 marks such a window “Not responding” instead.
- The desktop's folder poll (every second: a list of the desktop folder
  and every fixed drive's recycle bin) and Files' polling of E: hold volumes
  for 60–260 ms; Windows 98 Explorer uses change notifications.
- The launcher thread runs at the normal level while the input thread that
  received the click is interactive; Windows 98 runs CreateProcess on
  Explorer's foreground thread. A scheduler-contract decision.
- C4 ceiling: with 440 MiB free, Windows 98's VCACHE would have grown; the
  32 MiB bound and the storage writer's O(slots) write-back rescan are
  proposals for the lead (the index now makes a larger cache cheap to search,
  the rescan would still grow with it).
- Per-process DLL images: each Win32 process still snapshots and copies
  every DLL before the section lookup reuses the shared section.
