# f1-80 — after f1-79 the DOS VM spins 151–160 ms per quantum in `fs_read` and the full-screen return takes 8 s and never completes

- **Phase:** F1/F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f1-80` from main (after `ffeca72b`).
- **Authority:** `src/kernel/fs/fs_port.h` (FIFO `fs_lock_take`), `vfs.c`/`mount.c` (f1-79 admission and `VFS_IO`), `src/kernel/core/sync.c` (`kwait_wait_until`), `src/kernel/core/display.c` (`fullscreen_log`, `display_fullscreen_checkpoint`, return budget), `src/kernel/vm/dosvm_fullscreen.c`, `src/kernel/core/bootlog.c`; host tests `tests/host/fs`, `tests/host/kernel_latency_test.c`, `system_stalls_test.c`, display tests.

## Evidence (T23, image 041b221f, `legacy/local/physical/041b221f/BOOT.LOG` tail, Normal boot, read-only; the boot had IRQ 11 quarantined earlier, see f0-39)

- Owner: quitting DOOM while in native full screen leaves the system unusable; forced power-off.
- Return sequence: `fullscreen_watchdog reason=return-or-fault` → `watchdog-return tick=116344` → `guest-revoke tick=118495` (2.1 s later) → `vga-capture 120653` → `desktop-restore 120860` → `native-restore-streams 121023`; then no `native_supersavage phase=restore result=…`, no `mode=280`, no `desktop-restored`; the log ends. `[display] fullscreen_log error=-11 step=watchdog-return` and `step=guest-revoke` (EAGAIN from the durable checkpoint log). Desktop: `stall stage=render … no_frame_ms=4528`, `7904`.
- `[kernel] stretch_max_ms=151…160 task=dosvm eip=fs_read+0xd` in seven consecutive stat windows (the dosvm task runs its whole quantum inside the first instructions of `fs_read`), `stretch_max_ms=133 task=display-watchdog eip=native_read+0x10`, `stretch_max_ms=109 task=dosbox eip=memset+0x22`. `irqoff_max_us=2130` is the known SMI; these stretches are not IRQ-off.
- Hypothesis to verify first: the FIFO `fs_lock_take` (`fs_port.h:101-108`) spins when `kwait_wait_until` returns without blocking (caller under a preemption hold, IF-off, or a condition evaluated against a stale `serving`), so a task waiting for a volume lock held by a task blocked in device I/O (here a quarantined USB device) burns its quantum; f1-79's admission (`vfs_io_begin_locked`, reservation tickets) adds such waits in `fs_read`/`native_read`.

## Required

1. Reproduce on the host with the production `kwait`/scheduler model (the F0-20 cpustat replay and `kernel_latency_test`): a holder blocked in a slow or failing device read (25 ms per read, then errors from a quarantined device) while a second task takes the same volume lock and a third reads another volume: the waiters must block with zero CPU, `stretch_max_ms ≤ 4` for every task; fix the spin at its root (block properly; if the caller's context forbids blocking, change the caller). Cover `fs_lock_take`, `fs_event_wait` (f1-78) and the f1-79 admission waits.
2. The durable full-screen checkpoint log must not return EAGAIN on the return path: the write is queued or waits boundedly (≤ 50 ms) and never stalls the sequence; `fullscreen_log error` is logged once and the return continues.
3. The full-screen return completes within its 2 s budget even when the VM task is slow or stuck: each step has a bounded wait, a VM that does not park within the budget is forced out (quiesced) and the desktop is restored anyway; host display tests measure `watchdog-return → desktop-restored`.
4. Host suites: `tests/host/fs`, `tests/host/proc`, `tests/host/usb/run.sh`, display/VM/latency/stalls tests, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
5. Report `build/codex/f1-80-report.md` with the root cause named (file:line) and the before/after replay numbers.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Keep the f1-79 lock split and the f1-78 cache transport; FAT durability unchanged.
