# f3-61 — Unified DOS: a game in full screen trips the watchdog and the VM stops

- **Phase/model:** F3, Claude agent (Opus). **Worktree:** `wt/f3-61` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records: `[console] CiukiDOS unified (default)`, `[dosvm] fullscreen_request vm=1 mode=19`, `fullscreen=1`, `fullscreen_first_frame … source=aperture-write`, then `[dosvm] fullscreen_watchdog vm=1 generation=5469 reason=return-or-fault`, `fullscreen=0 … result=0`, `[dosvm] lifecycle stage=stop vm=1 reason=1 vector=0 eip=00000000`, `cleanup … data_loss=1`; perf before the stop: `exits=840 gp=840 … idle_28=86`. With `DOS kernel=freedos-1.3` the same game runs (owner). So under the fork kernel with the native IFS the game reaches mode 13h, draws a first frame, then stops drawing or returns; the watchdog then ends the session with data loss. The lead reproduces on QEMU with DOOM shareware under `kernel=unified`; this directive prepares the reproduction and fixes the cause.

## Required

1. Add a runner case (suite `f3-vm` or the title battery) that launches a bundled DOS game (DOOM shareware, `-nosound` not allowed: use the default) with `kernel=unified` through the real DOS Box path in full screen, and records the VM console text (the kernel's DOS console capture of the VM screen, text and mode-13h first frames) plus the `[dosvm]` lifecycle; the case must distinguish "game running" (frame progress) from "watchdog".
2. From the fork side: list what differs for a game between `kernel=freedos-1.3` and `kernel=unified` at runtime (IFS redirector on C:, DOS=LOW, CIUKIDF.SYS resident size, INT 21h paths used by DOOM's DMX/WAD loader: open/read/lseek of a 4 MB WAD, int 2Fh/1A00h, memory), and the likely trap: a 2 s budget or an INT 24h path that returns failure to the game, or the PENDING path stalling the VM so the watchdog fires. Fix what the host harness can show (an INT 21h trace of DOOM's startup on the fork with the live backend, compared with the reference kernel), and add the regression to the h57 suite.
3. The watchdog must not lose data: on `return-or-fault` the VM returns to the window with its state and a message, not `stop … data_loss=1`, unless the guest really faulted (vector≠0).

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. (the lead runs the QEMU case and sends you its serial and console capture). Report `build/codex/f3-61-report.md`.

## Amendment 1 (lead, 2026-10-22 09:59, decision on the delivery)

Accepted. The delivery shows that on every image so far the unified kernel
ran unbound (the VM refused the DFV2 requests before checking
`kernel=unified`; once bound, the native mount read sector 0 instead of the
partition at sector 63), so no previous "unified" evidence covered the
native IFS. The f3-48 comparison is re-run by the lead on QEMU with the
bound fork. The `return-or-fault` record was an Alt+Enter/Alt+Tab return;
the stop was the window close. Follow-up: the Quake VM creation error
`-16` seen in the same trial (f3-62).
