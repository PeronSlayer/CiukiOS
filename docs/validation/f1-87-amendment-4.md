# F1-87 Amendment 4: conflicting admission and DOS focus transfer

Research checked on 2026-10-21:

- Microsoft's [IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
  distinguishes handle-specific requests and thread contexts. Admission and
  diagnostics retain the operation's file identity and copied task/site tags.
- Microsoft's [IOS Services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM)
  describes request packets and device associations. Neither it nor the
  [FSD routing description](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F6E.HTM)
  specifies CiukiOS's admission times; C3 in
  [the resource contract](../design/win98-class-resource-management.md#c3-same-volume-reader-fairness-and-asynchronous-requests-s01s06)
  remains the policy authority.
- Microsoft's [Priority Inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm)
  describes Windows 95 dependency boosting; the
  [DDK Deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM)
  distinguishes ring-0 scheduling from Kernel32 waits. A forwarded input
  boost must follow the authoritative DOS input focus, including transfers
  arriving before an old window's focus-loss message.

Only a conflicting serving ticket that can progress on all its volumes can
close urgent admission. An unrelated active file operation, a known
different-file ticket and compatible metadata cannot close it. Unknown path
mutations conservatively conflict. Handle reservations carry their retained
file identity. Once a conflicting ticket reaches 100 ms, the volume shares
10 ms drain turns with 10 ms urgent-reader turns. Successive aged tickets do
not restart the clock. Actual conflicting mutations remain excluded during
both turns; this admission rule cannot bound their duration (B is f1-89).
The 10 ms turns are CiukiOS policy, not historical Windows constants.

Active readers publish stack-lifetime owner records under the existing
namespace lock, indexed by the shared description's busy flag. A record is
built from the reader's own request only (task, `read`, its site); the
namespace owner's site is never borrowed, and a reader without a site stays
fallback-attributed. A waiting reader records that actual owner, a blocking
mutation, or the serving ticket; an empty/fallback snapshot never becomes
valid, so a wait whose only blocker is fallback-attributed publishes no
`vfs_wait` record (the blocker's own `vfs_hold` record still shows its
`tagged=` bits). The first complete snapshot survives subsequent retries.
Existing diagnostic thresholds, suppression and unlocked publication remain
in force.

Gate limits: a queued ticket that does not conflict with the urgent read
(known different file, read-compatible operation) does not close admission,
but its admission still waits for `readers[drive]==0`. Its progress against
overlapping urgent readers therefore relies on the bounded urgency of
`audio_refill_reader` (byte budget, expiry, stream at most 3/4 full), not on
the gate. For conflicting tickets the 10 ms drain turn ends on time, not on
drain: a reader whose transfer outlasts the drain turn can overlap the next
open turn.

## Item 4: the audio-refill overrun metric

`[sched] device_overrun task=X` is emitted by `schedule()` and `sched_tick()`
(`src/kernel/core/task.c`) for every non-urgent P_DEVICE task demoted when
the aggregate device budget runs out (`device_throttled()`: 4 of the last 10
PIT ticks with a service-priority task current, or 4 ms of device CPU in
10 ms). The record names the demoted task, not the task that consumed the
budget; `audio-refill` sleeps with a 4 ms service deadline, so it is listed
at almost every exhaustion. A P_NORMAL task can consume the budget only
through donation (effective priority 16 or more).

`tests/host/t23_audio_replay_test.c` results (production scheduler, sampler,
mixer, refill, block queue and VFS admission):

| Mode | audio-refill overruns | overrun windows |
| --- | --- | --- |
| block boundary, holds only | 0 | 0/196 |
| new policy (`--vfs`), holds only | 0 | 0/201 |
| old policy (`--old-vfs`), holds only | 0 | 0/171 |
| old policy, `--trial-device-work` | 1230 | 171/171 |
| new policy, `--trial-device-work` | 1249 | 200/200 |

The trial's hold distribution changes the decoder's admission delay (old:
89 admission misses, 38 PCM starvations; new: none) but no device-class
CPU, so it cannot produce the overruns under either policy. Adding only the
trial's measured device-work sections (fact 6: 2110 us at 3.98 per second,
`tests/host/t23_trial.h`) reproduces the trial's prevalence (192/201) under
both policies with equal counts. Old-policy windows are fewer because a
decoder blocked inside old admission defers the 2 s collection.
Sensitivity to the section length at the same rate (new/old overrun
windows): 900 us 92/200 and 55/171, 1200 us 106/200 and 123/171, 1500 us
151/200 and 141/171, 2110 us 200/200 and 171/171.

The old-policy negative control (`--old-vfs --trial-device-work`) asserts
overruns in at least 90% of the windows. The new policy asserts zero with
the holds alone; with the trial's device work it prints
`F1-87 C NOT ACCEPTED`, which becomes fatal with `CIUKI_F187_REQUIRE_C=1`
(the precedent of `CIUKI_F187_REQUIRE_BOUNDED` for B). The C acceptance as
written cannot be met by VFS admission changes in this model; the lead
decides how C is closed (amendment 4 report, decisions needed).

No image or hardware qualification is claimed by these host models.
