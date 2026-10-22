# f1-101 — An application feeding audio must not starve: producer priority and prefill

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-101` from `main`.
- Evidence: T23 trial of image a4d9454e… (2026-10-22 ~17:40–17:47), disk log `build/lead-scratch/t23-a4d9454e-BOOT.LOG` (5,644 lines, f0-56 protected classes) and `legacy/local/physical/a4d9454e/NETSURF.LOG`; the serial capture did not cover this boot.
- Records: `[user 45] [music] level ms=5003 reservoir=0/15444/41984 … starvations=8`, `starve ms=26 reservoir_frames=0`, `close starvations=119` while `[sched] audio_refill_overrun` is 0 and `[audio] transport … underruns=0`: the kernel refill never fails; the user-space decoder of the Music player (normal class) is not scheduled in time while the desktop (interactive class with the f1-98 25 % share) composites, so its reservoir empties and the user hears gaps. Windows 9x runs wave-out feeding threads at time-critical priority within the audio period; CiukiOS has no equivalent for applications.

## Required

1. A task that owns an open audio stream and is behind its target lead (reservoir below one period) runs at the device band's lower edge (above the desktop's interactive level) until the reservoir is refilled, with a bounded budget (at most 20 % of the CPU over 100 ms) and a record `[sched] audio_producer task=… boosts=… over_budget=…`; the mixer exposes the lead to the kernel so the boost follows the real reservoir, not a flag an application can set.
2. The Music player's reservoir target becomes 500 ms with a 1 s prefill before playback starts, and its level record gains the boost state.
3. Host tests: a decoder thread under a 50 % interactive load keeps the reservoir above one period on the T23 replay model; the budget bound; `test_kernel_latency` with zero refill overruns; the player's level record.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-101-report.md`.
