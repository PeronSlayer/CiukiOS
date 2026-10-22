# f1-101 — An application feeding audio must not starve: producer priority and prefill

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-101` from `main`.
- Evidence: T23 trial of image a4d9454e… (2026-10-22 ~17:40–17:47), disk log `build/lead-scratch/t23-a4d9454e-BOOT.LOG` (5,644 lines, f0-56 protected classes) and `legacy/local/physical/a4d9454e/NETSURF.LOG`; the serial capture did not cover this boot.
- Records: `[user 45] [music] level ms=5003 reservoir=0/15444/41984 … starvations=8`, `starve ms=26 reservoir_frames=0`, `close starvations=119` while `[sched] audio_refill_overrun` is 0 and `[audio] transport … underruns=0`: the kernel refill never fails; the user-space decoder of the Music player (normal class) is not scheduled in time while the desktop (interactive class with the f1-98 25 % share) composites, so its reservoir empties and the user hears gaps. Windows 9x runs wave-out feeding threads at time-critical priority within the audio period; CiukiOS has no equivalent for applications.

## Required

1. A task that owns an open audio stream and is behind its target lead (reservoir below one period) runs at the device band's lower edge (above the desktop's interactive level) until the reservoir is refilled, with a bounded budget (at most 20 % of the CPU over 100 ms) and a record `[sched] audio_producer task=… boosts=… over_budget=…`; the mixer exposes the lead to the kernel so the boost follows the real reservoir, not a flag an application can set.
2. The Music player's reservoir target becomes 500 ms with a 1 s prefill before playback starts, and its level record gains the boost state.
3. Host tests: a decoder thread under a 50 % interactive load keeps the reservoir above one period on the T23 replay model; the budget bound; `test_kernel_latency` with zero refill overruns; the player's level record.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-101-report.md`.

## Amendment 1 (lead, 2026-10-22 19:13, decisions on the delivery)

1. Threshold: "behind" means more than one period below the 1792-frame
   target (behind below 1536, clear at 1792), as delivered.
2. Budget stays at 20 ms per 100 ms: the gaps that remain at the trial's
   decoder cost (140 instead of 413 starvations) are a decoder-cost problem,
   handled by directive f4-37; the budget is re-measured after it.
3. Prefill 1 s with a 500 ms reservoir target is accepted: the f1-87 2 s
   requirement was set against 1.5 s volume holds that f1-93/f1-93b removed
   (longest hold in the last trial 294 ms, replay gate 20 ms).
4. A program that keeps a short lead on purpose can trigger the boost; the
   shared 20 % budget bounds it. Per-stream fairness is a follow-up.
5. R2/R3 of the resource contract are amended by the lead.
