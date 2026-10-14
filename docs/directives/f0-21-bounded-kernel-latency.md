# f0-21 — Bounded kernel latency: serial never spins, wake preemption, CPU accounting, preemption points (research block of 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-21` from `main`. Host-testable; the lead runs the QEMU gates and the T23.

## Input

Read `docs/design/research/kernel-latency-2026-10-14.md` in full (research with primary sources, audit of our paths with file:line, ordered changes C1-C11, the 2 s `[cpu]` record definition). Its central finding: `serial_write` drains the UART synchronously in the caller at 38400 baud (0.26 ms per byte, about 40 ms per record, up to 267 ms per console write) inside the non-preemptible kernel, on every boot with a COM port; the UHCI schedule dump alone cost 3 s per controller on the T23; a timer wake never requests a reschedule (`task.c:343-347`); P_DEVICE tasks can hold the CPU indefinitely; there is no per-task CPU accounting; the desktop runs at P_NORMAL although the ABI says interactive. The T23 symptoms (USB workers idle 9 s, desktop stalls 2-6 s, wallpaper thread starved 14.5 s) follow from these. f0-20 (running in parallel) adds the attribution record and touches console/bootlog/usb_core; coordinate by keeping your changes in the files listed below and resolving conflicts at merge by keeping both.

## Task (one block; each item with its host test)

1. **C1 — serial never spins.** All records are queued; a dedicated writer kernel task drains the queue to the UART with the verified FIFO sender of f4-16; the caller never waits except on panic (synchronous flush path reserved for panic/oops). The disk log keeps its own path. Bounded queue (drop with a counter and one `[serial] dropped=` record, never block). Test: `tests/host/serial_audio_test.c` with the clocked 38400 UART model: a burst of 100 records costs the caller under 1 ms.
2. **C2 — wake preemption.** `sched_tick` sets `g_need_resched` when it readies a task of higher priority than the running one. Test: extracted `sched_tick` (`tests/host/test_signal_fault_records.py`) and `tests/host/proc`.
3. **C3 — per-task CPU accounting and the `[cpu]` record** every 2 s (fields per section 5 of the research: window, idle permille, top five tasks name:permille, longest non-preemptible stretch in ms with its site EIP), emitted through the serial queue; the desktop stall record of f2-61 triggers an immediate one. Test: scheduler replay with a hog task.
4. **C4 — console repaint coalesced** to at most 10 per second before the desktop takes the display; scrolling paints only new rows. Test: console model row counter.
5. **C5 — `kpreempt_point()`** in every ring-0 loop that can exceed 1 ms (ELF load copy/zero per 64 KiB, memset/memcpy of pages, bootlog drain batches, VFS/FAT scans, schedule dumps) and P_DEVICE overrun demotion: a device task running 10 ms without blocking is demoted for the rest of its window. Test: scheduler replay.
6. **C7 — one `usb-host` worker per controller** so a slow bind on one controller never stalls input on another (keep the per-host reservation; the HID input service stays). Test: `tests/host/usb` concurrency replay (bind on controller 1 while HID polls on controller 0 keep their 1 ms cadence).
7. **C9 — desktop UI thread at the interactive priority** per `execution-abi.md`. **C10 —** 1 ms polling loops become deadline sleeps where the research lists them. C6 (bind without `bootlog_sync`) belongs to f0-20/f1-62: do not edit `usb_storage.c`.

## Acceptance

Kernel host suite, `tests/host/proc`, USB, audio and desktop suites green; the lead runs `f0-smoke`, `f1-usb`, `desktop-shell`, `vm-window` on QEMU, then the T23: USB worker scans at 4/s throughout a verbose boot, `[cpu]` records with no stretch above 10 ms in normal use, desktop first frame and wallpaper within 2 s of entry. Files: `src/kernel/core/serial.c`, `console.c`, `task.c`, `task.h`, `trap.c`, loader/`ua_map`, `src/kernel/usb/usb_core.c` (worker split only), tests. Not `usb_storage.c` (f1-62), not `mount.c` (f1-63).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record decisions in `docs/design/` (amend `execution-abi.md` or the test architecture where behaviour changes). Report `build/codex/f0-21-report.md`.
