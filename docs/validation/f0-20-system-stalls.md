# F0-20: storage sync and wallpaper changes integrated with f0-21

Directives: `docs/directives/f0-20-system-stalls-attribution.md` and
`docs/directives/f0-21-bounded-kernel-latency.md`. The merge policy retains
f0-21's latency mechanisms and takes only f0-20's bounded storage sync,
wallpaper sampler, applicable tests and any missing desktop-stall transport.
This document describes the resulting integration. Historical branch-only
measurements remain in the f0-20 worktree's implementation report.

## Research and merge decisions

- [Linux workqueue documentation](https://docs.kernel.org/core-api/workqueue.html)
  describes deferred work running on worker threads and sleeping workers
  releasing execution capacity. Repository audit: `mount.c` already has a
  storage task draining the logger at 250 ms intervals. A USB controller
  worker must request a captured-byte watermark and sleep, rather than
  drain or commit the log on its own stack. The storage task owns runtime
  log writes and commits; shutdown keeps its existing final flush.
- [Linux v6.12 softirq implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/softirq.c)
  combines elapsed-time and iteration bounds because the clock can stop
  advancing. Keep f0-20's cancellation plus independent 2000 ms deadline and
  2000-turn escape. This is a behavioral reference; Ciuki's sync duration
  and batch size are the directive's values, not Linux's softirq values.
  Source references were revisited for this merge.
- f0-21 already provides the serial log-writer task, cpustat CPU accounting,
  normal-priority console repaint at 10 Hz, lock-aware `kpreempt_point`,
  device overrun demotion, 64 KiB ELF copy/zero preemption points and one
  `usb-host` worker per controller. Preserve these mechanisms, task fields,
  record formats, mapping transaction reservations and deadline sleeps.
  Do not include the competing `attribution.c` or `attribution.h`, their
  scheduler/syscall hooks, deferred formatter or PIT-drained async API.
- cpustat already has `cpu_stall_record` and immediate reporter wakeup.
  Its original caller is the console device stream, whereas f2-61's
  desktop observer uses `ciuki_raw_debug_write`. Port f0-20's missing
  debug-write bridge to `cpu_stall_record`, retaining the complete prefix
  and display-owner checks. No second sampler or direct synchronous
  emission is added; existing console-stream detection is preserved.
- [ELF program-header specification](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html)
  requires zero bytes after `p_filesz`. Preserve f0-21's loader and reserved
  preparing extents. Port the f0-20 large-BSS and allocation-failure tests
  to `kpreempt_point`; remove assertions tied to its private-image-only
  sleep implementation and its early STOPPING rollback behavior. Retain
  f0-21's own bounded mapping/protection and cancelled thread-creation tests.
- The wallpaper scaler already runs in a preemptible user-space worker.
  Keep the auto-merged sampler optimization: hoist coordinate divisions
  per axis and use 32-bit channel sums within the decoder's 4096 by 4096
  bounds. Preserve bilinear/area-average sampling, rounded alpha, fallback
  calculations, cache identity and approved portrait assets.

## Storage synchronization semantics

`bootlog_sync` captures the current byte count in one of 16 bounded slots,
then waits using the caller's pause and cancellation callbacks. It performs
no VFS call, holds no controller/storage reservation and never takes drain
ownership. Each waiter retains its own completion point; newer waiters or
producers cannot extend older waits. Full capacity returns `-FS_EBUSY`.
Cancellation, the 2 s deadline or the stalled-clock iteration bound return
`-FS_EAGAIN` without disabling the logger. Timed-out work remains pending
for an eventual storage-task commit.

Each storage turn copies/writes at most 4096 bytes, using at most 128 bytes
per IRQ-off copy section. Contended VFS work defers to a later storage turn.
Once retired bytes reach the earliest outstanding watermark, that task
commits and publishes completion. Runtime-ring overwrites count as retired,
consistent with the existing lossy ring; the protected prefix is retained.
A one-byte `queued` token makes commit-only work visible to the unchanged
storage timer. f0-21's `CPU_BOOTLOG` accounting encloses the write/commit,
and `kpreempt_point` runs after drain ownership is released.

The 250 ms storage cadence means a large protected backlog or slow media
may outlast a waiter's 2 s bound. Timeout remains recoverable. This merge
changes neither memory limits nor `usb_storage.c`/`mount.c`.

## Evidence and limits

The supplied 66843edc T23 records establish delayed USB scans, desktop
startup, wallpaper and frames. They do not identify a CPU hog or distinguish
CPU execution from scheduling, locks and disk waits. The resulting CPU
record is f0-21's existing format, with its existing verbosity/threshold
policy. f0-20's competing CPU record and original attribution-only tests
are dropped. Its useful scheduler replay is ported to production cpustat
and `kpreempt_point`; the authorized debug-write test uses that same API.

Host merge results are recorded in `build/codex/f0-20-merge-report.md`.
QEMU and T23 validation remain with the lead on one canonical image and
its recorded SHA-256. Host replay timings cannot qualify physical latency.
