# f0-56 — The disk boot log drops most records (169,761 dropped) — evidence loss

- **Phase/model:** F0, Claude agent (Sonnet). **Worktree:** `wt/f0-56` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records: `[kernel] bootlog writes=235 bytes=7700480 metadata=62 rotations=3 dropped=169761`, `dropped_protected=169761 … records_protected=1484`; BOOT.LOG on disk has 1,975 lines while the serial has 13,588 for the same boot. The periodic `[cpu]`/`[mem]`/`[storage]` statistics flood the log (hundreds of lines per 2 s window) and the application lines (NetSurf stderr before its exit, the Control Panel) are lost.

## Required

1. Classify records: application stderr/`[user N]` lines, `[dosvm] lifecycle`, `[accel]`, `[ata]`, `[storage] readonly/failure`, `[entropy]`, `[kernel] irqoff` are protected and never dropped before the periodic statistics; periodic statistics get a per-window budget (for example 40 lines per 2 s) and are the first to be dropped, with one summary line saying how many of which kind were dropped.
2. Capacity: the log rotation keeps at least the last 8 MB on disk across the three rotations (document the current sizes) and the writer never blocks the producer for more than 1 ms (keep the existing bounded queue).
3. Host tests: a flood of periodic lines never drops an application or lifecycle line; the drop summary format; rotation sizes.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f0-56-report.md`.
