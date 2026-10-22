# F1-90: device budget attribution and the audio refill class

Directive: `docs/directives/f1-90-device-budget-attribution-audio-refill.md`.
Contract: R2/R3 of `docs/design/win98-class-resource-management.md`.
Research checked on 2026-10-21.

## Reference model

- Windows 95 DDK, [Execution Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S333A.HTM):
  the VMM "often temporarily raises the execution priority for a thread
  that needs to service a high-priority device event, such as an interrupt
  that must be serviced in a timely manner". The boost belongs to the
  thread that services the event.
- DDK [Adjust_Exec_Priority](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3340.HTM)
  and [Call_Priority_VM_Event](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31E4.HTM):
  boosts from `Low_Pri_Device_Boost` to `Time_Critical_Boost` are granted
  per event to one VM/thread and withdrawn after its callback;
  `Time_Critical_Boost` is what VPICD uses "when simulating hardware
  interrupts", even past another VM's critical section. Nothing in the
  model lets one device's work lower another device's priority.
- DDK [Priority Model](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S336B.HTM):
  32 priorities, real-time class 16-31; "a dynamically boosted thread
  automatically loses a specified amount of boost for every time slice
  that it executes" (timed decay of the thread that runs).
  [Compatibility with the Primary Scheduler](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3377.HTM):
  device-boosted threads are outside the time slicer's control.
  [Multitasking](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22ED.HTM)
  restates the execution-priority rule for VMs.
- DDK [Considerations for Interrupt-Driven Drivers](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2AB6.HTM):
  "Most audio device drivers are interrupt-driven. For example, a waveform
  output device interrupts when the device needs another data block"; the
  blocks are page-locked so the driver fills them at interrupt time, i.e.
  outside any thread time slice.
- Windows 98 Resource Kit (owner media, `build/rskt/`), chapter 28
  "Virtual Machine Manager", *Process Scheduling and Multitasking*
  (`wrkc28_4.htm`): preemptive Win32 threads, cooperative Win16; no device
  CPU budget. Chapter 12 "Overview of Multimedia", *WDM Audio*
  (`wrkc12_1.htm`): all WDM audio processing, the system-wide kernel-mode
  mixer and the virtual Sound Blaster Pro/MPU-401 for MS-DOS games run in
  kernel mode; the DOS VM's sound goes through the same mixer path.

Conclusion: Windows 9x has no aggregate device CPU budget, so the shared
4 ms per 10 ms budget and the 2 ms refill budget below remain CiukiOS
policy (R2/R3). What the reference does fix is the direction of the
penalty: priority and its decay follow the thread that runs, and the wave
refill is interrupt-driven work that other devices cannot delay. A
CiukiOS budget may therefore demote only the consumer, never the refill
because of someone else's consumption.

## Trial facts (T23, image b51f034f, serial capture)

- 204 two-second report windows. `[sched] device_overrun` records:
  `usb-host` 609 (three tasks, 203 each), `usb-hid`, `thinkpad-volume`,
  `storage-writer`, `optical-media`, `net-input`, `device-work`, `cpustat`
  203 each, `audio-refill` 195 (the directive counts 192 of 201): every
  device task demoted together, the aggregate reading of f1-87 amendment 4.
- `[cpu] top=`: `audio-refill` 33-69 permille of the window (median 41)
  in the 101 windows where it is among the top five, i.e. 0.33-0.69 ms per
  10 ms; `device-work` 23-89 permille (median 78, 202 windows); class
  `device_pm` 73-220 (median 164). `stretch_max_ms=2 task=audio-refill` 4
  times. R3 allows one 2 ms refill service (`AUDIO_SERVICE_US`).

## Policy implemented

`src/kernel/core/task.c`:

- The shared aggregate (4 of the last 10 PIT tick samples, or 4 ms of
  device CPU in the sliding 10-tick window) still decides exhaustion.
- A 32-entry consumer table keeps every consumer's own share of the same
  window: CPU per tick slot and tick samples, lazily advanced, keyed by
  task pointer and id (never dereferenced). A full table leaves a charge
  unattributed; it still counts in the aggregate and narrows the
  attributed fair share.
- On exhaustion the max-min fair share (water level) of each exhausted
  measure is computed over the attributed consumers. Only a P_DEVICE task
  at or above it is demoted; a task under it, or with no share, is not.
  Example: shares 0.3/1.8/2.0 ms give level 1.9 ms, only the 2.0 ms task
  yields; 0.3/2.0/2.0 ms demote both 2.0 ms tasks. Urgent service
  deadlines still bypass demotion.
- Audio class (`task_set_audio_class`, set by `audio_init` on the refill
  worker): its CPU and tick samples never enter the shared aggregate; its
  own budget is 2 ms per 10 ms (`AUDIO_CLASS_BUDGET_US`, about three times
  the trial's highest 2 s average and one R3 maximum service). Exceeding
  it demotes the refill alone until its window drains, and is recorded as
  `audio_refill_overrun`, never as `device_overrun`.
- `device_attribution=false` restores the pre-f1-90 aggregate policy. It
  exists only for the replay's negative control; production never clears
  it.

`src/kernel/core/sync.c`: `kwait_wake_irq` wakes a queue's only waiter in
the top half when that waiter is audio class and the wake is not an I/O
completion (no predicate, no walk, O(1)). On the trial image the AC'97
interrupt's wake of the refill was deferred to the `device-work` worker,
the very task whose 2.1 ms sections exhaust the budget; under f1-90 that
worker is demoted as the consumer, so the deferred wake would still have
coupled the refill to it. Any other waiter, a shared queue or an I/O
completion keeps the bounded deferred path (f0-46).

Records (`src/kernel/core/cpustat.c`), all in the existing 2 s window:

- `[sched] device_overrun task=<name>#<id> count=<n> consumed_us=<us>`:
  the demoted consumer and the largest own window consumption at its
  demotions.
- `[sched] audio_refill_overrun task=<name>#<id> count=<n> consumed_us=<us>`.
- `[cpu] device_top task=<name>#<id> us=<us>`: the largest consumer of the
  shared device budget in the report window (`none#0` when nobody used it).

## Host evidence

The f1-87 continuous replay (`tests/host/t23_audio_replay_test.c`, 400 s,
production scheduler, sampler, refill, mixer, block queue and VFS
admission; the trial's 2110 us device-work sections every 251 ms with the
hold distribution) gains `--old-device-budget`:

| Device budget | VFS policy | Trial device work | audio-refill `device_overrun` | windows | device-work overruns | innocent overruns | `audio_refill_overrun` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| aggregate (trial image) | old | yes | 1230 | 171/171 | 1098 | 2350 | 0 |
| aggregate (trial image) | new | yes | 1249 | 200/200 | 1124 | 2394 | 0 |
| attributed (f1-90) | old | yes | 0 | 0/171 | 793 | 0 | 0 |
| attributed (f1-90) | new | yes | 0 | 0/200 | 871 | 0 | 0 |
| attributed (f1-90) | block, new, old | no | 0 | 0 | 0 | 0 | 0 |

"Innocent" counts demotions of any task other than the device-work peer
(under the aggregate policy these include the refill itself). Every f1-90
demotion carries `consumed_us` > 0 (maximum 3009 us), and `device_top`
names `device-work` in every emitted window (199/199 and 171/171). The
aggregate rows reproduce amendment 4 exactly. The final negative control
(a 5 ms refill without blocking) produces `audio_refill_overrun` under
f1-90 and `device_overrun` under the aggregate policy.

Sensitivity (scratch copies of the replay with other section sizes at the
same 251 ms period; refill `device_overrun` and its windows, then f1-90
device-work demotions; innocent demotions and `audio_refill_overrun` are 0
in every f1-90 run):

| Section | aggregate, old VFS | aggregate, new VFS | f1-90, old VFS | f1-90, new VFS |
| --- | --- | --- | --- | --- |
| 900 us | 80 in 55/171 | 150 in 92/200 | 0 (34 device-work) | 0 (47 device-work) |
| 1500 us | 433 in 141/171 | 421 in 151/200 | 0 (223) | 0 (189) |
| 2110 us | 1230 in 171/171 | 1249 in 200/200 | 0 (793) | 0 (871) |
| 3000 us | 3114 in 171/171 | 3342 in 200/200 | 0 (1865) | 0 (1941) |

Below the trial's 2110 us the aggregate control no longer reaches the 90%
prevalence (the replay asserts it only at the trial's value), as in
amendment 4.

`tests/host/kernel_latency_test.c` (`attribution_test`) checks the
production functions directly: one consumer beside a light device task and
an audio-class refill is demoted alone and named with `consumed_us=4000`
(aggregate negative control: all three, with 4000/300/500); the max-min
cases above; the refill's own budget (demotion, separate record, shared
budget untouched, recovery after the window drains).
`tests/host/kernel_sync_test.c` (`test_audio_irq_wake`) checks the top-half
wake and the deferred path for the other three cases.

## Limits

- The replay runs deferred work inline, so it does not model the refill's
  wake through the worker; the top-half wake is covered by the sync test.
- The refill's 4 ms safety-net timer and the DOS VM's virtual PIT/SB16
  clock (`sched_vm_tick_work`, `dosvm_clock_work`) still run on the shared
  `device-work` worker. They are no longer demoted by other device tasks'
  consumption, but they wait while the worker itself is demoted for its
  own items (on the trial, the 2.1 ms sections inside `timer_work`). The
  IRQ wake keeps the native refill on the period cadence; the VM clock
  needs a lead decision (see `build/codex/f1-90-report.md`).
- The 2.1 ms sections themselves stay the subject of the T23 `usbscan=0`
  experiment (f1-87 G). No physical evidence yet.
