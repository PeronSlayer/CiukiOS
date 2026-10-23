# FX-9: the DOS VM clock on its own real-time task

Directive: `docs/directives/t23-423b803a-trial-fixes.md` (fx-9).
Contract: R1/R3 of `docs/design/win98-class-resource-management.md`,
`docs/design/execution-abi.md` (F0-46 Amendment 1 and its fx-9 amendment).
Written 2026-10-23 16:04 CEST. Evidence: T23 trial of preview image
`423b803a` (private, `legacy/local/physical/423b803a/`, not in the tree).

## Reference model

- Windows 95 DDK, [Adjust_Exec_Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3340.HTM):
  `Time_Critical_Boost` is "for operations that require immediate
  processing, even when another virtual machine is in a critical section.
  For example, VPICD uses this when simulating hardware interrupts."
  Simulated hardware interrupts of a VM are the most urgent event class of
  the VMM; no other device's activity lowers them.
- DDK [Execution Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S333A.HTM)
  and [Compatibility with the Primary Scheduler](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3377.HTM)
  (already cited by f1-90): the boost belongs to the thread that services
  the event; device-boosted threads are outside the time slicer's control.
- Windows 98 Resource Kit (owner media, `build/rskt/`), chapter 12 *WDM
  Audio*: the virtual Sound Blaster for MS-DOS games runs in kernel mode in
  the same mixer path as native audio (cited by f1-90).
- Quake's DOS sound driver, id Software
  [WinQuake/snd_dos.c](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/snd_dos.c):
  `GetBLASTER` parses the IRQ but no handler is installed; `BLASTER_GetDMAPos`
  reads the 8237 count registers and acknowledges the DSP by reading
  `dsp_port+0xE`/`+0xF` every call ("acknowledge the transfer completions
  all the time so that it loops"). For Quake, `ack_max_ms` is the time
  between two main-loop sound updates and `irq_latency_*` is always 0; they
  are not scheduling measurements.

Decision: the VM clock (virtual PIT, SB16 DSP/DMA timeline, OPL timers,
COM wake, present checkpoint) is simulated-hardware time and gets the
treatment f1-90 gave the audio refill: a task of its own, woken from the
interrupt in O(1), never demoted by other devices' consumption.

## What the trial shows (serial.log line numbers)

DOOM (VM 1, generation 4573), full screen from L4842, volume keys at
L5158 (`[ec] thinkpad volume=7`), L5723 (9), L5994 (8):

| window | records |
| --- | --- |
| before the key, L4974-L5213 | device-work demoted 1-98 times per 2 s; cadence (L5154) `irq_latency_avg_us=2792 underruns=0` |
| key, L5274-L5627 | `idle_pm=0 device_pm=412-471`, `thinkpad#34` 97-148 permille (no EC traffic: no `[ec] thinkpad io=` record from L5210 to L5777), `device_overrun task=device-work#9 count=1338` (L5356), `1264` (L5499), `1026` (L5853), `thinkpad-volume#34 count=1349` (L5358), `1280` (L5640), `[cpu] deferred coalesced` +4341 to +4611 per window (L5282-L5634; +77 to +81 before the key, L4565-L4805), boot log stalled (`bootlog writes=116`, L5285-L5637) |
| cadence in the key windows | L5359 `irq_latency_max_us=842119 avg=225858 underruns=155933`; L5506 `ack_max_ms=1162 max=1167069 avg=719667 underruns=241664`; L5714 `avg=642239`; L5860 `avg=67636`; L6066 `ack_max_ms=575 avg=97974`, `cadence_late hist_gt20=161372`, `cadence_drop runs=4165 max=128` |
| after, L6223-L6432 | `irq_latency_avg_us=1146`, `1012`, `underruns=0` |

BOOT.LOG `group=vm_perf_service` for the same VM: n=9 `ticks=4288`, n=10
`ticks=3518` of 5000 (BOOT.LOG lines 3664, 3736): the clock was served in
bursts. Quake (VM 1, generation 6304): L14994 `ack_max_ms=1318
irq_latency_max_us=0 descheduled=2363` and L7828 `ack_max_ms=10641` are the
snd_dos.c polling pattern above.

## Mechanism

1. Since f0-46 (`b08c5970`) the PIT tick queued `sched_vm_tick_work` on the
   shared `device-work` ring, which queued `dosvm_clock_work`: one
   `DOSVM_AUDIO_TURN` (0.5 ms of DSP time) per callback, then a yield. A
   millisecond of VM time needed three worker turns.
2. `device-work` is an ordinary consumer of the shared 4 ms per 10 ms device
   budget (f1-90). When another device task wanted more than the budget
   (here thinkpad-volume after the key, with the storage writer), max-min
   fair share demoted device-work to level 7, below the focused VM at 9
   (`task.c` `priority_update`), and demoted device tasks get no 100 ms
   starvation boost (`task_ready_timer` is P_NORMAL only).
3. The worker then completed fewer turns than the ticks asked for; the
   per-tick entries coalesced about twice per ms; the DSP position fell
   behind the VM clock up to `DOSVM_AUDIO_BACKLOG` (1 s). Every IRQ raised
   by `sb_cause` carries that backlog in `irq_backlog_us`, and the
   acknowledgement age (`ack_max_frames`) counts from the late boundary:
   hence 0.84-1.17 s maxima, 226-720 ms averages, every PCM frame more
   than 20 ms late, the f4-35 late budget exhausted (drops) and the source
   starving the transport.
4. `descheduled` could not show any of this: from f0-46 on, `dosvm_tick`
   sampled `g_current != v->task` on the worker, where it is always true, so
   every pending millisecond read as "descheduled" and `if_clear` could
   never count (0 in every record of the trial).

## Change

- `src/kernel/vm/dosvm_clock.c` (new): task `vm-clock`, P_DEVICE and audio
  class (own 2 ms per 10 ms budget, never demoted by others), created once
  with the first VM. `dosvm_clock_signal()` runs in the PIT top half: a
  counter, an immediate service deadline (urgent dispatch, `task.c
  device_urgent`) and `kwait_wake_irq` (the audio-class O(1) path). The
  service runs every due VM to its tick allowance in turns of
  `DOSVM_AUDIO_TURN`, with `kpreempt_point()` between turns (R1), at most
  `DOSVM_CLOCK_TURNS` (130) per VM per tick.
- `task.c sched_tick`: calls `dosvm_tick()` in the top half instead of
  queueing `sched_vm_tick_work` (removed).
- `dosvm.c`: `dosvm_clock_turn(slot, &generation)` is the former
  `dosvm_clock_work` body (generation-pinned across preemption points);
  `dosvm_tick()` samples and signals only; `dosvm_create` starts the task.
- `dosvm_sb16.c`: `dosvm_audio_sample()` counts `parked`/`descheduled`/
  `if_clear` per millisecond in the top half from the interrupted task; the
  snapshot is taken with interrupts off. New protected line before each
  cadence record: `[vm-audio] vm=N cadence_clock gap_max_ms= dsp_lag_max_ms=`
  (largest gap between two clock services, largest DSP backlog).

The ABI is unchanged (no public structure changed).

## Host evidence

`tests/host/t23_vm_clock_test.c`, in `test_kernel_latency.py`: production
scheduler, ring, timers, kwait, PIT top half, cpustat and `dosvm_clock.c`
with every task as a coroutine. Calibration (stated in the test): 24 us per
device-work turn reproduces its steady 132-150 permille; 28 us more while
the shared budget is exhausted reproduces the key windows' coalescing.

| scenario | clock gap max | IRQ age max / avg | dropped | coalesced/ms | VM CPU (no key) |
| --- | --- | --- | --- | --- | --- |
| full screen, key, f0-46 (control) | 8 ms | 999.9 / 500.4 ms | 166272 | 2.33 | 60.4 % |
| full screen, key, fx-9 | 1 ms | 0.9 / 0.4 ms | 0 | 0.09 | 65.6 % |
| window, key, f0-46 (control) | 8 ms | 999.9 / 500.5 ms | 163680 | 2.32 | |
| window, key, fx-9 | 1 ms | 0.9 / 0.4 ms | 0 | 0.09 | |

Sensitivity with a constant 20-56 us turn: f0-46 diverges to the 1 s
backlog from 36 us and reproduces the trial's 1264-1338 device-work
demotions per 2 s at 52 us (1296); fx-9 keeps gap <= 1 ms and IRQ age
<= 0.9 ms at every cost, the vm-clock task is never demoted. Compiled with
the pre-fx-9 `task.c` (HEAD 53b5ce36) the same test fails (exit 1, both
transports at 999.9 ms).

## Open

- The trigger: thinkpad-volume stayed busy (97-148 permille) for about 13 s
  inside `master_refresh` after a key press in full screen, without EC
  traffic, while the storage writer rose to 65-100 permille and the boot log
  made no progress. The records do not show what it computed. A device-class
  worker persisting `C:\SYSTEM\AUDIO.CFG` under `audio.control` is
  background work in the device band; a follow-up should move the save out
  of it and time the refresh. With fx-9 it can no longer stop the VM clock,
  but it still takes CPU from the game while it runs.
- Device-class background load is 290-330 permille at an idle desktop
  (L3122: device-work 107, storage-writer 105, net-input 76): net-input
  polls every 1 ms in `net.c worker_main` with three yields per turn even
  in interrupt mode. Out of this item's authority (F6); fx-9 removes the
  VM tick's three worker turns per millisecond (device-work 14.1 % to 5.6 %
  in the replay, the VM +5.2 points).
- A tick that is late by several guest timer periods still merges PIT edges
  (`dosvm_pit_advance` raises IRQ0 once); fx-9 keeps such gaps at 1 ms.
- Physical qualification: the next T23 trial must show `cadence_clock
  gap_max_ms` <= 2 and `dsp_lag_max_ms` <= 2, `if_clear` separated from
  `descheduled`, and no `audio_refill_overrun task=vm-clock`.

## fx-9b: the trigger (volume-key persistence)

Lead decision 2026-10-23: move the key-press persistence off the device
band, coalesced, keeping fx-3's coalesced EC write.

What a key press did (f4-29, `1614e79f`): the EC worker (`thinkpad-volume`,
P_DEVICE) polls every 2 s; when the EC level moved, `master_refresh`
(`audio_dev.c`) applied the master and called `save_master()` under
`audio.control`: `vfs_open(C:\SYSTEM\AUDIO.CFG, DENY_ALL)` (a C:
reservation), a 48-byte record, and `vfs_commit`, which is
`fat_commit` -> `cache_barrier_record`: every dirty block of the whole
system disk written with write admission frozen, then FLUSH CACHE (host
fixture: 2 sectors and 3 cache flushes on an otherwise clean cache). In the
trial each press (L5158, L5723, L5994) was followed by `thinkpad#34` at
97-148 permille with no EC traffic, `storage-writer` at 65-100 permille and
a stalled boot log (`bootlog writes=116`, L5285-L5637) for up to about 13 s.
SAVED also hid the change from the desktop, whose settings writer saves an
applied-unsaved master in the authoritative registry value (F4-18 rule
above); the registry's older value then won at the next boot.

Change: `master_refresh` only applies (applied-unsaved, error 0) and marks
the master pending; the desktop writer (`apps/desktop/desktop.c`,
normal priority, existing `cfg_writer`) saves the registry value once it
has been unchanged for 3 s (`MASTER_QUIET_NS`, longer than the 2 s EC poll)
and before a shutdown (first step of its shutdown wait); `power.c`
`power_sequence` calls `audio_settings_flush()` before the storage flush,
which writes AUDIO.CFG once if a key-applied master is pending; a tray save
clears it. fx-3's coalesced EC write is unchanged.

Host evidence: `tests/host/audio/thinkpad_ec_test.inc`
`test_thinkpad_ec_key_persistence` (six presses over six polls: six RD_EC,
0 disk reads/writes/flushes, 0 AUDIO.CFG records; shutdown flush one record;
the reloaded settings hold the burst's value; a later tray save
supersedes), `tests/host/power_runtime_test.c` (`ASNQP`: flush before the
storage flush, tolerated failure, repeated only while storage is not
durable), `tests/host/desktop/memory_footprint_test.h` (no save inside the
burst, one save 3 s after the last value, a press before shutdown saved by
the shutdown wait), `t23_vm_clock_test.c` fx-9 + fx-9b cases (key phase VM
share 65.6 % full screen and 56.6 % window, equal to the no-key runs; no
device demotion; one writer save within 148-276 ms of the quiet period).
Residual: the shutdown wait reads the master again before saving, so a
press applied after the desktop's last mixer poll is saved too. A press the
EC worker applies after that wait (between the wait and the power request)
reaches AUDIO.CFG through the kernel flush but not the registry, which wins
at boot; a press within the last 2 s EC poll before shutdown is never
applied, as before f4-29.
