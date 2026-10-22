# f1-101: audio producer priority and the Music player's prefill

Directive: `docs/directives/f1-101-audio-producer-priority.md`. Worktree
`wt/f1-101` from `main` 296234dd, implementer Claude agent (Opus), written
2026-10-22. Host evidence only: no QEMU, no image, no hardware run.

## Trial evidence (image a4d9454e, disk log `t23-a4d9454e-BOOT.LOG`)

- `[audio] transport ... underruns=0` in all 70 transport records, no
  `[sched] audio_refill_overrun`, no `device_overrun task=audio-refill`: the
  kernel refill never failed.
- `[music] level` records: `reservoir=0/1289/4096`, `0/1327/4096`,
  `0/1374/4096` with 27-37 starvations per 5 s, `reader_fps=35204-40960`;
  `[music] close starvations=119`.
- `[cpu]` windows during playback: `idle_pm=0`, `device_pm` 154-181; the
  decoder thread `music#46` (normal class, base 7) 185-388 permille; the
  compositor `desktop#28` (interactive, 9) 103-502; `desktop#30` 76-197;
  `desktop#31` 64-82; the Music UI `music#44` (focused, 9) 58-76.
- Derived decoder cost: 343 permille for 40116 frames/s, 8.55 us of CPU
  per 48 kHz stereo output frame, i.e. 41 % of the T23's CPU in real time.
  The f4-36 host benchmark measured 24.6 us per 1152-frame MPEG frame at -O2
  on the workstation (0.021 us per output frame): the T23 cost is about 400
  times higher, far beyond a Pentium III / host clock ratio. This is a
  finding for a separate directive (see "Remaining"), not addressed here.

## Research

- Windows 98 Resource Kit (`build/rskt`, read-only): ch. 28 "Virtual Machine
  Manager" (`wrkc28_4.htm`): the Process Scheduler preempts Win32 threads,
  each thread gets time slices; ch. 12 "Overview of Multimedia"
  (`wrkc12_1.htm`) and ch. 30 (`wrkc30_1.htm`): WDM audio mixes every
  client in a kernel-mode system-wide mixer (KMixer) transporting
  "latency-sensitive data streams". The Resource Kit gives no thread
  priority constants for audio feeders (as P34 already found for boosts).
- Microsoft Learn, "Scheduling Priorities"
  (https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities):
  `THREAD_PRIORITY_TIME_CRITICAL` gives base priority 15 in every
  non-realtime class; "a high-priority thread should execute for a brief
  time, and only when it has time-critical work to perform". CiukiOS maps
  Win32 TIME_CRITICAL to level 15 (`src/kernel/proc/priority.c`); the
  Win32 audio runtime's mixer thread already uses it
  (`sdk/win32/audio/runtime.c`). Native programs have no such request.
- Microsoft Learn, "Multimedia Class Scheduler Service"
  (https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service):
  audio threads that register with MMCSS are raised (Pro Audio 23-26,
  foreground 16-22) while `SystemResponsiveness` (default 20) guarantees a
  share of the CPU to everything else; threads that used their quota drop
  to 1-7. This is the precedent for a boost bounded by a CPU budget.
- Microsoft Learn, `waveOutWrite`
  (https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutwrite)
  and `IDirectSoundBuffer8::GetCurrentPosition`
  (https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418062(v=vs.85)):
  the device plays queued blocks and returns them as done; the data
  between the play and write cursors is the lead the mixer already owns.
  Here that lead is the stream's queued frames in the kernel mixer.

Difference from Windows, deliberate: in Windows the application asks for
time-critical priority (or joins an MMCSS task) and keeps it. The directive
requires the kernel to decide from the mixer's real lead, never from a flag
an application sets; the application only names its threads by using the
stream (write, refill read).

## Decisions

1. **Lead and thresholds** (`audio.h`): the lead is the stream's queued
   source frames converted to output frames (32-bit arithmetic, at most
   2048 x 48000). A stream is behind its target lead once the lead is more
   than one period below the target (`AUDIO_PRODUCER_BEHIND` = 1536 frames,
   32 ms; the same mark as the `AUDIO_REFILL_READ` demand) and refilled at
   the target (`AUDIO_PRODUCER_TARGET` = 1792 frames, the f4-36 refill mark
   one period under the ring). Draining, stopped, failed and closed streams
   are never behind. The literal reading, "lead below one period" (256
   frames, 5.3 ms), was replayed as a control: a decoder whose next batch
   costs more CPU than 5.3 ms cannot be rescued that late (results below).
2. **Producers** (`task.c` producer table, keyed by the stream): the thread
   that writes the stream (`audio_write_user`) and the thread that issues
   `AUDIO_REFILL_READ` on it, one per role, replaced by the latest caller,
   cleared at task exit, stream reset and close. Device- and idle-class
   tasks are never boosted.
3. **Level**: `SCHED_AUDIO_PRODUCER` = 16, the device band's lower edge,
   above the desktop (9, 11 with input, 15 with the starvation boost or
   TIME_CRITICAL) and below device service (24). Its CPU is ordinary CPU in
   every account: not the shared device budget, not the refill's own
   budget (f1-90), not a device sample, `normal` in `[cpu]` class records;
   the 2-tick slice of the device band applies.
4. **Budget**: one budget for all producers, 20 ms of boosted CPU in any
   sliding 100 ms (100 one-tick slots); at the limit every boost is
   withheld until a tick of budget is free again. Measured maximum on the
   replays: 20.99 ms per sliding 100 ms window, the 1 ms PIT granularity of
   the charge (CPU is charged at the next tick) and of the enforcement.
5. **Inversion**: a boosted producer that waits for a lock or a drive
   reservation lends its owner at most level 15 (`task_donor_priority`,
   used by the sync.c donation walk), so a lock holder never becomes device
   service through a producer.
6. **Record**: `[sched] audio_producer task=<name>#<id> boosts=<n>
   over_budget=<n> boost_us=<n>` per task and 2 s window (`cpustat.c`).
7. **Observation ABI**: `AUDIO_GET_PRODUCER` (0x4108,
   `struct ciuki_audio_producer`, 32 bytes): flags (behind, boosted,
   withheld), boost episodes, budget withholdings, producer count, boosted
   CPU since open/reset. Output only; nothing in it can be set.
8. **Music player** (`apps/music/player.h`): prefill 1 s (was 2 s); the
   player defends a 500 ms reservoir target: below it the pump keeps the
   device stream at `MUSIC_DEVICE_BEHIND` (1280 frames, one period under
   the kernel's mark), so the mixer's lead reports the deficit and the
   kernel raises the decoder and the pump; at the target, or at the end of
   the file, the pump fills the stream as before (2048). The reader still
   fills the 2.7 s ring when the CPU allows, so the f4-34 storage-stall
   tolerance is unchanged. The level record gains
   `boost=<flags> boosts=<n> boost_ms=<n> over=<n>` (window deltas from
   `AUDIO_GET_PRODUCER`).

## Host replay (`tests/host/t23_audio_producer_test.c`, in `test_kernel_latency`)

Production run queues, priorities, starvation boost, f1-98 share, f1-90
device budget, timers, cpustat, sync.c, the production mixer and stream;
the Music pump and reader follow `player.h`. Assumptions: loads periodic
over 20 ms (desktop) or 50 ms (UI), the trial windows' obtained CPU taken
as demand (a lower bound: every window had `idle_pm=0`), device class 18 %
as in the f1-98 model, no storage waits for the reader. 46 s per run (the
23 trial windows), starvation = a mixed period that found the stream short.

Trial windows, decoder at the trial cost (8550 ns per frame, 41 %):

| policy | starvations | decoder CPU | compositor | desktop workers | boosted CPU |
| --- | --- | --- | --- | --- | --- |
| pre-f1-101 (negative control) | 413 | 36.8 % | 21.5 % | 16.2 % | 0 |
| player change only | 645 | 37.0 % | 21.5 % | 16.1 % | 0 |
| boost, pre-f1-101 player | 207 | 37.8 % | 20.3 % | 16.3 % | 1.3 s |
| literal threshold (below one period) | 180 | 39.4 % | 19.7 % | 15.3 % | 2.5 s |
| **f1-101** | **140** | 39.5 % | 19.8 % | 15.1 % | 4.0 s, 531 withholdings |

The CPU is oversubscribed in most trial windows (device 18 % + desktop
30-64 % + decoder 41 %); within 20 % the boost cannot remove every gap.
Informational sweeps (scratch builds, not in the tree): with the same
windows and decoder cost a 30 ms budget leaves 6 starvations and 40 ms none
(lead >= 768 frames, 13 withholdings);
with the 20 % budget a decoder of 7000 ns per frame (34 %) has none (gated:
control 1, f1-101 0, lead >= 512 frames) and 6000 ns or less needs no boost.

50 % interactive load (compositor 10 ms of every 20 ms, a desktop worker
15 %, UI 6 %, device 18 %):

| decoder ns/frame | pre-f1-101 starvations | f1-101 starvations | f1-101 lead min | compositor with f1-101 |
| --- | --- | --- | --- | --- |
| 2000 (10 %) | 6 | 0 | 1280 | 50.0 % |
| 4000 (19 %) | 310 | 0 | 1024 | 48.0 % |
| 5000 (24 %) | 1781 | 0 | 512 | 43.2 % |
| 8550 (41 %) | 4050 | 1713 | 0 | 38.0 % |

Gates: 2000-5000 ns per frame keep the mixer's lead above one period with
no gap, the negative control starves at 4000-5000; at 8550 there are fewer
gaps. The boost takes its CPU from the interactive level: up to 12 points
of the compositor at the trial cost.

Every scenario: zero `audio_refill_overrun` and no refill device overrun,
the refill's longest service gap equal to the control's (6.3 ms), boosted
CPU at most 20.99 ms per sliding 100 ms (a decoder that never catches up
included: 487 boosts, 464 withholdings). The f1-87/f1-90 replays in
`test_kernel_latency` keep `audio_refill_overruns=0` under the attributed
budget.

Other host tests: `tests/host/audio/run.sh` (writer and reader
registration, the mixer-reported lead with rate conversion, hysteresis,
`AUDIO_GET_PRODUCER`, reset/failure/close), `tests/host/desktop/test_music.py`
(mirror fill, EOF, level record deltas, real-codec storage stalls with the
1 s prefill), `scripts/test/host_kernel_tests.sh` (ABI layout, task.c
fixtures).

## Remaining

- The decoder's T23 cost (8.55 us per output frame) is the dominant cause;
  with it a 20 % producer budget cannot keep a 48 kHz MP3 under the trial's
  desktop load. Candidates for a separate directive: x87 denormal assists
  in minimp3's float synthesis on the Pentium III (no FTZ/DAZ without SSE),
  or a fixed-point decoder.
- f1-87 C asked for a decoded buffer of at least 2 s against 1.5 s holds.
  The ring still fills to 2.7 s in steady state, but playback now starts
  after 1 s: a hold longer than 1 s in the first second, with the reader's
  256 KiB read-ahead also exhausted, can gap. The trial's longest drive C
  hold was 294 ms.
- The budget is global: one stream behind for a long time can use it all
  before another; per-stream fairness is not implemented.
- Physical confirmation on the T23: `[sched] audio_producer` records while
  Music plays, the level record's `boost=`, no `[music] starve`. `[sched]`
  lines are f0-56 statistic records (window budget, first evicted); the
  `[user N] [music] level` record is protected, so the boost state reaches
  the disk log even when the statistic budget drops `[sched]` lines.
