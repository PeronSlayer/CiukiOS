# f2-88b — Display properties still "not responding" on the T23 after f2-88

- **Phase/model:** F2, Claude agent (Sonnet; Opus if the cause is in the desktop IPC). **Worktree:** `wt/f2-88b` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records: no application line from the applet in BOOT.LOG; serial `[cpu] top=… control#42:174k150s3 … control#44:92k90s3` for about ten 2 s windows and `stretch_max_ms=5 task=control#42`; the desktop still shows "Not responding". f2-88 moved the preview decode to a worker thread and changed the desktop's ping rule; on the T23 the applet's UI thread (#42) still runs 150–200 ms of CPU per 2 s window without answering the ping. Hypotheses to test: the applet's 50 ms poll loop blocks on the worker's mutex while the worker decodes; `pthread` on the target yields no real concurrency between #42 and #44 (both at level 3); the ping arrives on the channel but the applet's event loop reads it only after the decode completes.

## Required

1. Reproduce on QEMU through a scripted run: write `scripts/test/control_display_probe.py` (or a runner case) that boots the image, clicks Ciuki menu → Control Panel → Display with QMP input events (coordinates from the desktop layout, or the keyboard shortcuts), and screendumps every second for 12 s, plus the serial; the lead runs it. The applet must log one line per phase (`[control] display open ms=… decode=… first_frame=… ping_answered=…`) so the T23 log shows where the time goes.
2. Fix the cause the reproduction shows: the UI thread must answer the desktop's ping within 100 ms whatever the worker does (no shared lock held across the decode; event loop first, decode results polled without blocking); if target threads do not run concurrently, make the decode incremental on the UI thread (bounded slices of 10 ms).
3. Host tests: the UI loop answers a ping while the decode worker holds its data lock; the slice bound.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f2-88b-report.md`.
