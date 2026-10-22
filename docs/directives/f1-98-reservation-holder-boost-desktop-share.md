# f1-98 — Scheduler: boost the holder of a drive reservation; desktop workers keep a minimum share while a DOS VM runs

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-98` from `main`.
- **Evidence (f1-93b report, T23 trial b453bc92):** the 1.4–1.9 s drive-C holds of the desktop folder worker were the holding thread preempted while a DOS VM at priority 9 used 62–74 % of the CPU; hold lengths clustered at multiples of the 100 ms starvation-boost timer (`task.c` `task_ready_timer`); desktop workers (priority 7) got under 3 %. f1-93b removed the preemption points inside holds, but while a DOS game runs the desktop's launches, folder refreshes and settings reads stay slow: the owner's "windows lag" whenever a game is open.

## Required

1. **Reservation-holder boost** (Windows 9x boosts the owner of a critical section): a task holding a drive reservation or the cache lock runs at least at the priority of the highest waiter until it releases, with the inheritance recorded (`[sched] inherit from=<task> to=<task> level=<n> reason=drive|cache`); bounded to the hold, never beyond release.
2. **Desktop minimum share while a VM runs:** the desktop process and its workers (the window server, shell, folder/launch workers) keep a guaranteed share (propose 25 % of the CPU over a 100 ms window, justify against the Windows 98 foreground/background quantum rules in the Resource Kit) while a DOS VM or Win32 game runs at a higher level; the game keeps the rest and audio keeps its budget (f1-90). Measure on the T23 replay model: a folder compare and a window open complete within 300 ms while a VM at priority 9 runs a busy loop.
3. The focused DOS window must not lose its own responsiveness: measure the VM's frame cadence with the share in place (no more than 5 % slower on the model).
4. Host tests: inheritance on a drive hold with a priority-9 waiter, the share accounting, the VM cadence bound, `test_kernel_latency`, the strict FS gate, `host_kernel_tests.sh`; validation document `docs/validation/f1-98-reservation-boost-desktop-share.md`.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (Windows 98 Resource Kit scheduling chapter in build/rskt, Win95 DDK Adjust_Exec_Priority as used by f1-90).

## Amendment 1 (lead, 2026-10-22 12:20, decisions on the delivery)

Accepted with the 25 % share (window open 97 ms on the replay; 15 % would
give 157 ms); the 5.42 % cadence cost for a vsync game needing 62 % of the
CPU per frame is accepted for the 1.0.0 alpha and recorded; deferring the
desktop's turns to the end of the game's frame is a follow-up if the T23
shows it. R2 of the resource contract is amended by the lead (share, lending,
holder boost). Scope against any higher-priority program and the boot task
is accepted.
