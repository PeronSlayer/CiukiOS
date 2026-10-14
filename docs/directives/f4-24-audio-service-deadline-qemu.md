# f4-24 — Audio service misses its 1 ms bound 350 times per run on QEMU (deterministic): find what the per-period path gained since f4-15/f4-18

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-24` from `main`. Host-testable; the lead runs the `audio-native-qemu-t23` case on QEMU.

## Evidence (QEMU `qemu-t23`, image 78df33ef of main b76e976d, runs `build/test-runs/zz-audio/261fc277` and `fa615308`, host idle)

Two runs, identical outcome: `case=clock output=202496 fetched=202496 fifo=0 halts=0 deadlines=350 quarantine=0`, `case=service max_service_us=1106|1103 max_irq_us=0 max_service_gap_us=152222|154349 max_control_us=18884|18890`, both sources accepted/consumed 192000 frames with no starvation, cleanup pages equal, then `END status=FAIL` because `src/kernel/probes/f4_probes_audio.c:144` requires `deadlines == 0`. `audio_dev.c:324` counts a deadline when a non-control service call exceeds 1000 µs (`ktime_elapsed_us(start, 1000)`), and the maximum service is 1.1 ms: a systematic, slightly-too-slow service, exactly 350 times in a 4 s playback of 750 periods of 256 frames at 48 kHz. The reset stage is fine since f4-23 (`reset=qemu_model`). `f4-audio` was not run on QEMU between f4-15 (ICH3 CIV/LVI/PICB semantics, silence on underrun, resync after halt) and f4-18 (linear SB16 mixer, VM master follows the system master, software master gain, boot preference), so either may have added the cost. The 150 ms service gap also deserves an explanation (what blocks the audio task for 150 ms on an otherwise idle guest: the ELF spawn of the two payloads, the disk log drain of f1-59, or a scheduler change of f3-17).

## Task

1. Measure the per-period service cost on the host harness (`tests/host/audio`) with a cycle budget comparable to TCG (count instructions or use `clock_gettime` on an `-O2` build; the harness already models the transport), and break it down: period fetch, mixing of two sources (f4-18's Q16 gains), silence fill, resync, descriptor writes, record emission. Identify the step that crosses 1 ms and the reason exactly 350 of 750 periods miss (a pattern of every other period, or every period after a certain point).
2. Make the service O(frames) with cheap integer arithmetic, no redundant memsets or double mixing, no record/log work in the service, so that it stays well under 1 ms on TCG; keep the f4-15 correctness rules.
3. Explain and fix the 150 ms service gap if it is in our code (the service must run at its period cadence while a spawn or a disk-log drain happens).
4. Host test with a per-period cost assertion and the gap assertion.

## Acceptance

`tests/host/audio/run.sh` and `test_audio.py` green; the lead runs `audio-native-qemu-t23` on QEMU: `deadlines=0`, `max_service_gap_us` bounded, then the T23 boot tone and UI sounds unchanged. Files: `src/kernel/audio/*`, `src/kernel/drivers/ac97.c`, `tests/host/audio`. Do not touch the probe's bound.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-24-report.md`.
