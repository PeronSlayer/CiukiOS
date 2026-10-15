# f4-29 — ThinkPad volume keys move the master in steps of 5 (image a738204a)

- **Model/effort:** `gpt-6-luna`, `medium`. **Worktree:** `../CiukiOS-wt-f4-29` from `main`.

## Evidence

`[ec] thinkpad volume=4 mute=0` records on the T23: the embedded controller reports its own 0–14 level and the kernel maps that level to the master percentage, so one key press moves the master by an uneven amount. The owner wants every press to move the system master by exactly 5 points on the 0–100 scale (boot default 20 unless a preference is saved), mute toggling, and the tray showing the step.

## Task

Use the EC level only as the key source (up, down, mute): each change of the EC level by one moves the master by 5, clamped to 0–100; the EC's absolute level never sets the master directly. Save through the existing preference path (f4-2x, `master_preference_save`); the tray and the control panel show the resulting percentage. Host test for the mapping (sequence of EC levels → master values, wrap and clamp cases).

## Acceptance

Audio host tests green; QEMU `f4-audio` cases green. Files: `src/kernel/drivers/thinkpad_ec.c`, the master audio service, the tray.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f4-29-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
