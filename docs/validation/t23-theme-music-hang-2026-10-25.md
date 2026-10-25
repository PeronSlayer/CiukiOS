# T23 trial 2ca3b0b6: freeze while changing the appearance with music from USB

Source `main` 60d029c9, hardware image 2ca3b0b6. Owner report (1.0.1
alpha list): music played from a USB stick, the appearance scheme was
changed a few times and the system "crashed". Evidence: the session's
`LOGS/BOOT.LOG` (4,368 lines, private trial material; only record lines
are quoted here). No serial capture. Host evidence only: no QEMU, no
image, no hardware run of this change.

## What the log contains

- Control Panel pid 26 (`control#107`, Display Properties, Appearance tab)
  applied twice: `[control] display state=appearance ... ui_wait=event` at
  about 1241 s and 1249 s of uptime.
- The CPU stayed saturated from the first apply to the end of the log:
  `idle_pm` 467 and 428 before, then 176, 2, 4, 0, 0. The consumers were
  the desktop (`desktop#38` 24-35 %), its wallpaper worker (`desktop#47`
  10-32 %, mostly kernel time) and its Start-menu worker (`desktop#44`
  7-15 %, kernel), beside the music decoder.
- Block-queue service maxima since boot: `[storage] queue name=ata0 ...
  svc=22163/2061/223/1398 svc_max_ms=105` (1,398 commands of 32 ms or more)
  and `name=usb-storage ... svc=0/8/7/86 svc_max_ms=1079`.
- Inheritance records of the last windows: `inherit from=storage-writer#15
  to=desktop#40/#42/#43/#44 level=24 reason=drive`, `from=control#107
  to=desktop#40/#42/#44 reason=drive|cache`, `to=files#88 reason=drive`.
- No panic, no `vfs_hold_long`, no `[desktop] stall` record. The file ends
  after `[sched] desktop_share ...` of a statistics window.

The end of the file is not the moment of the failure. BOOT.LOG is drained
about once a second, but its directory-entry size is published every
`BOOTLOG_METADATA_MS` (5000, `bootlog.h`), so after a power-off the readable
log can trail the last drain by up to about 6 s. The black box (128 raw
sectors inside the FAT reserved area, written by the same storage writer
on every pass) keeps the last ~60 KB of `klog` lines without that delay, and
the next boot of the same disk replays them into BOOT.LOG as `[blackbox]`
lines; the captured set does not include them. A panic also leaves no disk
record whenever the ATA channel was busy (`blackbox_panic` writes only on an
idle channel) and the fatal screen is the only trace. The log therefore
cannot tell a storage stall, a display-engine or bus lock-up, a panic, or a
frozen desktop ended by power-off apart.

## Hypotheses checked against the code

- **The appearance change touches the removable drive.** No. The apply path
  (`cp_display_store`, `DESK_SETTINGS_RELOAD`, `desk_shell_apply`,
  `wallpaper_update`, `custom_refresh`, `theme_poll_async`) reads and
  writes C: only: `desktop.cfg`, the stock wallpaper, the Start-menu
  folders; the folder poll's Recycle Bin check skips removable drives
  (`folder_fixed`).
- **Repaint holds a lock across I/O.** No. Every desktop worker (settings
  writer, wallpaper, folder poll, Start menu, links, sounds) does its file
  I/O with its mailbox mutex released; the main thread takes those mutexes
  only around copies, display fence waits are bounded to 1 ms, channel
  sends are `DONTWAIT`.
- **A lock cycle in the VFS, the cache or USB.** None found: order
  namespace → volume → cache, every cache wait drops the mutex, no C: path
  takes the USB transfer lock. The earlier `[sched] donation_cycle
  waiter=97 owner=42` records are a waiter's dependency edge still set
  while it retakes the namespace lock (`reserve_file_request`), transient.
- **A USB transfer without a bound.** Every UHCI and Bulk-Only stage is
  bounded (10 s per bulk stage, 5 s reset); only the controller reservation
  (`usb_host_take`) and an issued request's waiter have no deadline.

## The defect the trial measured: storage executors without priority

Each controller's queue runs on a `storage-io` task created at P_NORMAL
(`blk_queue_start`). A command needs that task twice: to issue it, and to
continue after the completion interrupt (and a USB command once per 1 ms
poll). Its requesters slept on an event without an owner or polled with a
1 ms pause, so nobody lent the worker a priority. While renderers at the
interactive level 9-11 keep the CPU busy, a level-7 task runs only through
the 100 ms starvation dispatch (R2). The trial's `svc_max_ms=105` on ata0 is
that dispatch; a full-speed 64 KiB USB command takes about 65 ms
(`blkdev.h`), so `svc_max_ms=1079` is that dispatch repeated across the
command's polls. Every C: user waited behind it: the storage writer's boot
log and write-back, the Control Panel's write, fsync, rename and fsync, the
desktop's settings read, and every client's 2-second `desktop.cfg` poll on
its UI thread.

Reference model:
- Contract R2 (`docs/design/win98-class-resource-management.md`): "Priority
  donation follows actual mutex/request dependencies, including transitive
  owners, and is withdrawn/recomputed on unlock, cancellation, timeout and
  death."
- Windows 95 DDK, [Multitasking](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22ED.HTM):
  "The VMM often raises the execution priority for a virtual machine that
  needs to service a high-priority device event". Disk completion is device
  service, not ordinary-thread work.
- Microsoft Learn, [Priority Boosts](https://learn.microsoft.com/windows/win32/procthread/priority-boosts):
  the waiting thread is boosted when its disk wait completes. Its
  constants are NT's; the Resource Kit gives none for 98 (P34), so CiukiOS
  keeps its own R2 values.

Decision: a requester waiting for a queued or issued block request names
the queue's executor as the holder (`ksync_depend`, new reason
`KSYNC_DEPEND_IO`, recorded as `[sched] inherit ... reason=io`). The
executor runs at the requester's level, transitively through lock owners,
until `finish()` releases the request; the edge is renewed on every wake
outside the queue lock, so an edge refused as a cycle is retried. Device
time is unchanged; only the CPU dispatches of the executor stop waiting for
the starvation dispatch. `executor_donation` is cleared only by the replay's
negative control.

Replay (`tests/host/fs/wait_latency_test.c`, production queue, scheduler,
`ksync_depend` and kwait): worker at 7, a never-blocking renderer at 9, a
5 ms device. Without donation (negative control) both the interactive
reader and the device-class storage writer measure issue 100 ms, service
206 ms. With donation: reader issue 10 ms, service 21 ms, requester 31 ms
(one renderer slice per step); storage writer issue 0 ms, service 6 ms.

## Records added for the next occurrence (diagnostic only)

- `[storage] writer_stall` (two records): the cpustat task, which uses no
  VFS, checks the storage writer's per-stage progress stamp every second.
  A stage older than 5 s is reported, then every 10 s: stage (`window`,
  `blackbox`, `bootstat`, `bootlog`, `writeback`, `census`), passes, what
  the writer waits for (`lock:<owner>`, `drive|cache|io:<holder>`), the
  namespace owner, C:'s state, holder, operation and age, writer turn, gate,
  queued tickets, readers, cache owner, dirty bytes and batch flag. It then
  writes the black box's sectors itself (`blackbox_checkpoint`: raw ATA
  writes, at most 2 s and 512 sectors), so the records preceding a stuck
  writer reach the disk and the next boot.
- `[storage] lock_wait`: a task still waiting for a filesystem lock after
  5 s names the lock's rank and drive, its owner (`none` while a served
  ticket's task has not run), the tickets and the wait, once per wait
  (`fs_lock_take`, `fs_lock_take_request`).
- The desktop observer now samples the idle stage once a second (it slept
  until the next busy stage, so a loop stuck at the turn's end, e.g. in the
  offscreen-cache upload after an appearance change, was never reported).
  An idle loop stamp older than 3 s is reported; one-second shell sleeps are
  not; standby is excluded.

Recommended for the next trial: keep the serial capture; after a freeze,
boot the same disk once and collect the new BOOT.LOG, whose `[blackbox]`
prefix holds the crashed session's last records; note whether the pointer
moved, whether the music kept playing and whether a fatal screen appeared.

## Host validation

- `tests/host/fs/wait_latency_test.c` (in `test_kernel_latency.py`): the
  executor replay above, beside the existing F1-80/F1-98 cases.
- `tests/host/fs/test_storage.c`: `storage_stall_due` (5 s, 10 s repeat,
  reset by progress, millisecond wrap) and the exact text of both
  `writer_stall` records, both within 240 bytes at maximum widths.
- `tests/host/test_storage_throughput.py`: the storage task stamps start,
  each pass and stop.
- `tests/host/desktop/theme_wallpaper_sequence_test.h`: idle-stage bounds,
  standby exclusion, and the observer thread reporting a stuck idle loop.
- i686 compile of every kernel file that includes the touched headers with
  `build_kernel.py`'s flags. The only failures are untouched files that
  need their own recipe's include paths or build-identity defines (ACPI,
  lwIP, a probe) and `console.c`, which fails the same way on `main`.
