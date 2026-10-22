# F1-98: reservation-holder boost and the desktop's minimum share

Directive: `docs/directives/f1-98-reservation-holder-boost-desktop-share.md`.
Contract: R2 of `docs/design/win98-class-resource-management.md` (effective
priorities, boosts and inversion). Evidence: the T23 trial of image
`b453bc92` (serial capture `build/codex/t23-b453bc92-serial.txt`) and the
f1-93b record `docs/validation/f1-93b-readdir-holds.md`. Written 2026-10-22
by the implementer; host evidence only, no QEMU, no image.

## Reference model

- Windows 3.1 DDK, *Virtual Device Adaptation Guide*
  ([pcjs copy](https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/)),
  chapter 16 "Scheduler": "At the lowest level, the Primary Scheduler
  maintains execution priorities, and the VM with the highest priority is
  allowed to run ... The second level of scheduling is handled by the Time
  Slicer, which boosts a VM's execution priority for a given time slice."
  Chapter 24, `Adjust_Exec_Priority`: the boost ladder from
  `Cur_Run_VM_Boost` ("Time-slice scheduler boosts each VM in turn by this
  value to force them to run for their alloted time-slice") to
  `Critical_Section_Boost`; `Begin_Critical_Section`: "When the critical
  section is first claimed, the execution priority of the current VM is
  boosted by the Critical_Section_Boost value ... task switches to other
  VMs will only occur for time-critical operations".
- Same guide, chapter 25 "Time-Slice Scheduler Services": "Every VM has a
  foreground (focus) and a background time-slice priority ... The VM with
  the largest time-slice priority will run more often than other VMs but it
  will not necessarily prevent other VMs from executing." "The algorithm
  used to allocate time determines the percentage of CPU time each VM
  should get based on their percentage of the total of all the time-slice
  priorities", with the worked example of a focus VM at 100 beside
  background VMs at 50 and 75 (45 %, 22 %, 33 %). `Set_Execution_Focus`:
  "All VMs except the focus VM, background VMs, and the system VM will be
  suspended": the System VM keeps running beside any focused VM unless that
  VM is exclusive.
- Windows 95 DDK, [Multitasking](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22ED.HTM):
  "Every virtual machine has a foreground and a background time-slice
  priority in addition to the virtual machine's execution priority. The
  virtual machine with the largest time-slice priority receives a greatest
  share of CPU time." [Compatibility with the Primary Scheduler](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3377.HTM):
  threads boosted by the primary scheduler are outside the time slicer.
  [Execution Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S333A.HTM)
  and [Adjust_Exec_Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3340.HTM)
  as used by f1-90.
- Windows 98 Resource Kit (owner's media, `build/rskt/`, read-only):
  chapter 28, *Process Scheduling and Multitasking* (`wrkc28_4.htm`): one
  System VM "in which all system processes run", each MS-DOS application in
  its own VM, preemptive Win32 threads; the shell example: "each folder
  window that opens is a separate thread of execution ... You can still use
  the other windows in the shell without interruption". Chapter 25,
  *Technical Notes* (`wrkc25_5.htm`): "the Windows 98 kernel schedules the
  time allotted for running applications. This results in smoother
  concurrent processing and prevents any one application from using all
  system resources without permitting other tasks to run. (An exception is
  when you run an MS-DOS-based application in MS-DOS mode ...)". Chapter 25,
  table 25.2 (`wrkc25_3.htm`) keeps the PIF flags `bgd` (Background), `exc`
  (Exclusive mode) and `dit` (Detect idle time). The Resource Kit gives no
  quantum, share or boost numbers (P34 of the contract).
- Windows 95 SDK [priority inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm)
  and the Linux [rt-mutex design](https://docs.kernel.org/locking/rt-mutex-design.html)
  (already cited by R2 and f1-93b) for owner boosting.

Conclusions. (1) The holder of a reservation is the owner of a critical
section in the VMM's sense: it runs at least at its highest waiter's
priority until it releases (`Critical_Section_Boost`, Win95 priority
inheritance). (2) A focused MS-DOS VM does not starve the System VM, where
the shell, its folder threads and every Win32 thread run: the time slicer
gives each runnable VM a share proportional to its time-slice priority and
boosts each in turn to make it run. The share CiukiOS reserves for the
desktop is a CiukiOS number: 25 % (the directive's proposal) is below the
33 % that the DDK's example priorities (focus 100, background 50) would give
a background System VM. The game keeps the rest (75 % of the CPU less the
device band, which stays ahead of both) and loses only what the desktop
actually uses; R2's cap of 80 % of the remainder for the focused DOS
preference is the same rule seen from the VM's side.

## Trial facts (b453bc92)

- dosvm#54 at effective 9 (base 7, inherited 9 from dosbox#53) took
  62-74 % of each 2 s window, `idle_pm=0`, its perf counters
  `busy_pm=726..752` with no idle detection (a busy-polling game).
- Device class 13-20 % (`device_pm=126..206`): audio-refill 4.5-7 %,
  device-work 7.5-8.6 %, net-input 2.8-3.3 %.
- `boosts_starvation=150..197` per window: the desktop workers (base 7) ran
  only at their 100 ms starvation boost, one tick each; desktop#27 (the
  compositor, P_INTERACTIVE 9) 4-6 %.
- Drive C holds of desktop#31 up to 1904 ms quantized at 100 ms;
  `[desktop] stall stage=ipc worker=idle/settings-read no_frame_ms=1703
  ... folder_wait=directory-compare`: the UI waited for a settings reader
  that waited for drive C held by the starved folder worker. Holder and
  waiter were both base 7, so priority inheritance alone cannot help that
  chain (the replay below confirms it); the desktop share does.

## Changes

### 1. Reservation-holder boost (`sync.c`, `fs_port.h`, `vfs.c`)

`ksync_depend(owner, object, reason)` records that the calling task waits
for `owner`, the holder of `object`; `ksync_depend_release(object)` is
called by the holder when it gives that object up. Edges are kept on an
intrusive list (`task.depends_on`, `depend_next`) and take part in the
existing donation walk (`donation_update`), transitively and with the same
cycle refusal as kwait owners. An edge survives the waiter's pause wakeups
and ends at the waiter's withdrawal (admitted, cancelled, timed out), at the
holder's release of that object, or at either task's exit
(`ksync_task_exit`), so the inheritance never outlives the hold. The kmutex
lock-order and cyclic-timeout checks keep the previous kwait-only graph
(`lock_cycle`): no new panic path.

The filesystem names holders through `fs_depend(holder, object, reason)`
(a weak call into sync.c in the kernel, an observer on the host):

| wait | holder named | object | release |
| --- | --- | --- | --- |
| volume admission loop (`reserve_file_request`: request pause or `fs_event_wait`) | the drive's reservation/guard holder (`vfs.hold_token[d]`, set at admission), else the queued waiter served next | `&hold_token[d]` | `vfs_unlock`, device-window pause, a cancelled ticket |
| namespace lock taken by a request (`admit_request`) | the lock's owner | the lock | `fs_lock_drop` |
| any `fs_lock` taken by a request (`fs_lock_take_request`, `fs_lock_retake`): the block-cache lock (rank 3, `reason=cache`), namespace and volume locks (`reason=drive`) | the lock's owner (`wait.owner`) | the lock | `fs_lock_drop` |
| f1-89 writer turn (`turn_queue_locked`, `vfs_turn_resume_locked`) | `turn_owner[d]` | `&turn_owner[d]` | turn drop to depth 0, turn suspension |

Non-request `fs_lock_take` waiters already donated through their kwait.
In-flight open-file readers are not named (several short device phases,
not one owner).

Record: when a dependency raises its holder, the donation walk calls
`cpu_inherit`, aggregated per 2 s window by cpustat:
`[sched] inherit from=<task>#<id> to=<task>#<id> level=<n> reason=drive|cache count=<n>`
(eight pairs per window, `[sched] inherit lost=<n>` beyond).

### 2. Desktop minimum share (`task.c`, `supervisor.c`, `service.c`)

- Group: every thread of the desktop process (`supervisor_spawn` with
  `desktop`) and of the Win32 window server (`w32srv`,
  `W32_SERVICE_W32SRV`): `process.share_group`, copied to each native thread
  at creation (`proc_share_group` marks existing threads).
- Credit: 250 permille of every millisecond, at most 2.5 ms banked and
  2.5 ms of debt (`SHARE_PERMILLE`, `SHARE_BURST_US`); every member's CPU,
  as charged by cpustat at switches and timer interrupts, is debited.
- Preference (`rq_pop`): after urgent device deadlines, service-level
  inheritance, the starvation dispatch and every level from 15 up (device
  service), and before the focused VM's 8/10 preference: when the group
  has credit and some other ready task below the device band would run
  first (the focused VM, or any non-member queued at a level at least as
  high as the best ready member), the highest ready member runs. A share
  turn lasts at most its credit (one to three ticks) and ends at the tick
  where the credit runs out.
- Lending: a holder that a member waits for (kwait owner or reservation
  dependency, transitively) is lent the share (`task.share_lent`), runs on
  it and is debited, exactly while the dependency lasts.
- Nothing changes while the desktop has no ready thread: the game keeps
  every millisecond the desktop does not use. Audio keeps its f1-90
  budget: device-class service is ahead of the share.
- Record per window: `[sched] desktop_share group_pm=<n> turns=<n>
  lent_turns=<n> turn_ms=<n>` (CPU of the group and of lent holders, share
  turns, turns given to lent holders).

## Host evidence

All runs use the production scheduler, sync.c and cpustat with modeled
CPU time (1 us resolution, 1 ms PIT ticks). Negative controls: the
pre-f1-98 scheduler (`desktop_share=false`, waiters naming no holder).

### Reservation-holder boost

- `tests/host/fs/wait_latency_test.c` (production VFS, kernel `fs_lock`,
  kwait, scheduler): a priority-9 waiter (P_INTERACTIVE) for drive C held by
  a base-7 task blocked in three 25 ms device reads. In every holder
  dispatch the holder runs at inherited 9; after `vfs_unlock` (or
  `fs_lock_drop`) it is back at 7 with no edge left. Three waits: the volume
  admission loop through `vfs_lock` (`fs_event_wait`), a request admission
  through `vfs_admit` with the native 1 ms pause, and the block-cache lock
  taken by a request (`fs_lock_take_request`, `reason=cache`). Each yields
  one `[sched] inherit ... level=9` event with the right reason.
- `tests/host/scheduler_priorities_test.c`: the holder is raised to the
  waiter's 9; a repeated poll changes nothing; releasing another object
  does not withdraw; release, withdrawal and both exits restore 7; the
  chain desktop(9) -> middle (drive) -> worker (cache) raises both and
  records both hops; the waiter's +2 input boost propagates to 11 and its
  expiry back to 9; a dependency closing a cycle is refused; a desktop
  member lends the share only while it waits; the record
  `[sched] inherit from=desktop#4 to=worker#2 level=9 reason=drive count=1`.
- Dispatch beside a busy focused VM at 9 (production run queues): the 7
  holder's remaining 5 ms inside its hold complete in **505 ms** without
  the boost (one tick per 100 ms starvation boost, the trial's
  quantization) and in **29 ms** with it.

### T23 replay (`tests/host/t23_desktop_share_test.c`)

The model reproduces the trial's mix: a focused DOS VM at effective 9
running a busy loop; device class at 18.3 % (trial 13-20 %): audio refill
350 us every 5 ms, device-work 800 us every 10 ms with the measured 2110 us
section every 250 ms, net-input 300 us every 10 ms; the desktop: compositor
(P_INTERACTIVE 9) repainting the clock 1 ms every 50 ms, folder worker
comparing `/Documents/Desktop` every second (20 ms of CPU in 40 readdir
steps, each holding drive C for 100 us; the f1-93b 2,000-entry geometry at
about 2 us per decoded entry plus syscall and compare work, an assumption),
and a window open every 2 s (3 ms of drawing, a launch worker's 10 lookups
of 1 ms, one of them keeping drive C across a 10 ms device read, 5 ms of
launch work, a 100 us stat by the compositor, 8 ms of drawing). Drive C is
modeled at the f1-98 contract: waiters poll every 1 ms (native
`file_pause`) and name the holder; holds are not preemptible (f1-93b).

12 s, focused VM at 9:

| policy | folder compare max (runs of 12) | window open max (5 of 5) | refill gap max | VM wait max |
| --- | --- | --- | --- | --- |
| pre-f1-98 (negative control) | 2120 ms (4) | 1610 ms | 7.4 ms | 4.8 ms |
| holder boost only | 2120 ms (4) | 1610 ms | 7.4 ms | 4.8 ms |
| desktop share only | 88 ms (12) | 97 ms | 6.3 ms | 7.4 ms |
| f1-98 (share + boost) | **88 ms** (12) | **97 ms** | 6.3 ms | 7.4 ms |

The negative control reproduces the trial: compares of 2.1 s, a window open
of 1.6 s. The boost alone changes nothing here because holder and waiters
are all base 7, as in the trial. With the share the VM's CPU goes from
79.3 % to 76.6 % and the desktop's from 2.3 % to 5.0 % (device 18.3 %
unchanged); the audio refill's longest service gap does not grow; the VM's
longest wait grows by one share turn plus a tick (gate: at most the
negative control's plus 3.5 ms).

Other rows of the same model:

- Win32 game at HIGH_PRIORITY_CLASS (13), not a focused VM: folder compare
  and window open do not finish in 12 s without the share; with it 90 ms
  and 102 ms.
- A non-desktop holder on drive C (a base-7 Win32 task whose `ReadFile`
  keeps the drive across four 10 ms device commands every 500 ms, the path
  f1-93b left unconverted): window open 483 ms with the share but without
  holder edges (the share cannot reach a non-member holder), **98 ms** with
  f1-98 (the compositor lends the holder 9 and the share, 5 lent turns);
  pre-f1-98 neither finishes.
- Share accounting: a desktop member that never blocks beside the busy VM
  receives 23.75-26.24 ms in every sliding 100 ms window after the first
  second (25 % give or take one tick-granular turn); the VM keeps at least
  54.45 ms of every window. Negative control: 0.90-10.90 ms (the 1 ms
  starvation boosts plus the compositor's turns at 9).

### VM frame cadence with the share in place

A game renders W us of CPU work per frame; with vsync it then polls for the
next 70 Hz retrace (a frame finished late slips one retrace), free-running
it starts the next frame at once. Same trial device and desktop load, 12 s,
frames without and with f1-98:

| game | frames pre-f1-98 -> f1-98 | slower | gate |
| --- | --- | --- | --- |
| vsync, 6 ms per frame (41 % of the CPU) | 840 -> 827 | 1.55 % | <= 5 % |
| vsync, 7 ms per frame (48 %) | 840 -> 816 | 2.86 % | <= 5 % |
| vsync, 8 ms per frame (55 %) | 839 -> 798 | 4.89 % | <= 5 % |
| free-running, 8 ms per frame | 1188 -> 1149 | 3.28 % | <= 5 % |
| vsync, 9 ms per frame (62 %) | 831 -> 786 | 5.42 % | informational |
| vsync, 8 ms, desktop saturating its share | 837 -> 531 | 36.56 % | informational |
| free-running, 8 ms, desktop saturating its share | 1184 -> 849 | 28.29 % | <= 25/75 of the VM's CPU |

The VM loses what the desktop uses and no more: a CPU-bound game slows by
the desktop's actual consumption (3.3 % with the trial's desktop load). A
vsync-locked game loses the retraces that fall inside the desktop's bursts
(folder compare, window open) once its frame needs more than the CPU left
during a burst (100 % - 25 % - 18 % device = 57 %, 8.1 ms per 14.3 ms
frame); at 62 % it loses 5.42 % of its frames.

## Limits and open points

- The vsync row at 9 ms per frame (62 % of the CPU) exceeds the 5 % bound
  by 0.42 points, and a desktop that keeps a thread ready (a long copy, a
  thumbnail pass) takes its full 25 % from the game (28-37 % fewer frames
  for a game that needs 55 %). These are the price of the guarantee, not
  defects of the accounting; a lower share (15 % measured: 5.05 % at 9 ms,
  window open 157 ms) or a frame-aware deferral of share turns is a lead
  decision. The trial's game polled without idling, so its real per-frame
  need is not known.
- Turns are tick-granular: a turn ends at the PIT tick where its credit
  runs out, so a 100 ms window holds 23.75-26.24 ms, and the VM can wait
  one turn (at most 2.5 ms plus one tick) more than before; a pending
  virtual IRQ of the focused VM waits for the end of the turn too.
- The share applies against any non-member below the device band at the
  same or a higher level, not only VMs and games: a CPU-bound focused
  native application (a browser) also leaves the desktop 25 % while the
  desktop has work. The boot task (`probes`, P_INTERACTIVE 9) is such a
  competitor at boot.
- Holder tokens (`hold_token`, `turn_owner`, a lock's `wait.owner`) are
  dereferenced by `ksync_depend`: they stay valid for the hold because a
  native task is stopped only after its system call unwinds (`task_kill`,
  `proc_stop`) and kernel workers do not exit inside VFS calls. A task that
  died holding a reservation would leave the drive wedged regardless.
- In-flight open-file readers (`readers[d]`) are not named as holders; a
  metadata writer waiting for them lends nothing.
- The `[sched] inherit` and `[sched] desktop_share` records are emitted with
  the other periodic `[sched]` records, so only in emitted windows.
- Physical confirmation is pending: the next T23 trial with a DOS game
  should show `[sched] desktop_share` turns, desktop workers in `[cpu] top`
  at their demand, no `vfs_hold` of desktop#31 above a few ms and no
  `[desktop] stall stage=ipc` with `folder_wait=directory-compare`.
- Contract text: R2 still states only the 80 % cap of focused DOS
  preference; the share (25 %, 2.5 ms banked, ahead of the VM preference,
  behind device service and starvation dispatch), share lending and the
  reservation dependencies need the lead's R2 amendment, as for f1-90.

## Suites

2026-10-22, `nice -n 19`, one at a time, no other suite of this worktree
running:

- `test_kernel_latency` (15 tests, including the new
  `t23_desktop_share_test.c`, the extended `scheduler_priorities_test.c`
  and `fs/wait_latency_test.c`): green, run on the final sources (results
  identical before and after the last move of the share declarations).
- Strict FS gate, `CIUKI_F187_REQUIRE_BOUNDED=1 scripts/test/host_fs_tests.sh`:
  green (exit 0), including `kernel_compile.py` (51 units with the kernel
  CFLAGS, no new runtime helper: the share's slice bound avoids 64-bit
  division) and `test_t23_holds --require-bounded`, with every FS source in
  its final state.
- `scripts/test/host_kernel_tests.sh` (the FreeDOS 1.3 zips symlinked from
  the main checkout for the DOS steps and unlinked afterwards): green
  (exit 0) on the final sources. The first run stopped at two fixtures that
  extract parts of `task.c`: `proc/signal_test.c` (renamed production
  `task_set_share_group` like the other task setters) and
  `usb/storage_run.sh` (extracts `rq_remove`/`rq_link`/`rq_pop` and the
  declarations before `all_tasks`; the share state and `share_pick` now
  live in that declaration region).
- Also run alone: `tests/host/usb/storage_run.sh`,
  `test_signal_fault_records`, `test_system_stalls`: green.
