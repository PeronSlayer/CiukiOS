# F0-20: whole-system stall attribution

Directive: `docs/directives/f0-20-system-stalls-attribution.md`. Host evidence
only; QEMU and physical T23 qualification belong to the lead.

## Research and implementation decisions

- Linux's [voluntary rescheduling implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/core.c)
  (`__cond_resched`) supplies explicit scheduling points. Ciuki's
  ring-0 kernel is non-preemptible: long private ELF load loops need explicit
  scheduling points with interrupts enabled. Zero/copy at most 64 KiB between
  points. Yield only provisional ELF image mappings, before publication;
  yielding inside a shared live address-space transaction would introduce a
  race with another thread mapping the same range.
- The [ELF program-header specification](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html)
  requires the memory beyond `p_filesz` to be zero. Keep eager page zeroing,
  permissions, failure rollback and the unpublished-child boundary.
- Linux's [workqueue documentation](https://docs.kernel.org/core-api/workqueue.html)
  describes sleeping workers releasing execution capacity. In this tree the
  storage writer already drains one 4 KiB logger batch per turn, but
  `bootlog_sync` also performs VFS work directly on the USB task. Replace that
  caller drain with a captured-byte watermark request and cancellable sleep.
  The existing storage task writes one batch and commits a satisfied
  watermark. Bound each sync wait independently of its caller's bind deadline;
  pending commits must remain visible even with an empty byte queue. No
  changes to `usb_storage.c` or `mount.c` are needed.
- Linux's [CPU accounting documentation](https://docs.kernel.org/filesystems/proc.html)
  separates running-task CPU time and idle time. Use Ciuki's calibrated TSC
  at context switches for the diagnostic deltas: PIT ticks alone miss time
  with interrupts disabled. Attribute interrupted kernel/IRQ execution to
  the task owning that CPU interval; this is elapsed CPU ownership, not an
  instruction counter. Fall back to PIT time if TSC calibration is unavailable.
  Path totals exclude sleeps and preempted intervals. The longest single
  path stretch ends at a context switch or a path boundary.
- `console.c` already honors the display grant for ordinary LFB records;
  preserve that rule and prove verbose scrolling does no framebuffer work.
  Explicit operator/evidence overlays and panic reclaim retain their purpose.
- Linux's [printk documentation](https://docs.kernel.org/core-api/printk-basics.html)
  warns about hot-path logging through slow serial consoles. Ciuki's ordinary
  `serial_write` can synchronously poll a UART, including in verbose mode.
  Attribution records use a bounded queue-only formatter: serial drains on
  the existing PIT hook, disk logging on the storage task, and no console
  painting. The observer must not create another long ring-0 interval.
- `desk_wall_prepare` already decodes and scales in a ring-3 pthread, without
  a scaling syscall. Preserve this boundary. Hoist invariant resampling
  coordinates and use bounded channel sums, preserving pixel output and cache
  identities. A user-space CPU loop remains timer-preemptible.

The directive requests a two-second verbose record cadence and also no
records when nothing exceeds 20 ms. Apply the latter as the periodic emission
gate: examine each two-second window, emit a pair only after a known path
stretch exceeds 20 ms. The authorized desktop's stall debug record forces
an immediate pair even below that threshold. A delayed report states the
actual `window_ms`; it never labels a nine-second interval as two seconds.
Quiet mode performs no attribution collection or output. Normal records
use the runtime log ring, not the protected boot prefix.

## Evidence and limits

The supplied 66843edc T23 records prove delayed USB scans, delayed desktop
startup/wallpaper stages and missing frames. They do not identify a CPU hog
or distinguish CPU execution from I/O waiting. The implementation paths
above are an audit finding, not a retrospective attribution of that image.

Host results and the lead's remaining checks are recorded below after testing.

## Record semantics

`top` lists at most five copied task names and kernel task IDs, ordered by
CPU delta (`desktop#42:300`, `usb-2474#9:150`, etc.). Task IDs disambiguate
threads of the same native process. Reaped tasks remain in the current
window; more than 319 distinct non-idle task identities are aggregated as
`other`. The bounded table occupies 10 KiB on the i686 target. There is no
allocation, formatting or I/O in the scheduler hook.

Path totals are exclusive CPU ownership for console output, logger draining
and ELF mapping/copying; sleeps and work by other tasks are excluded. Nested
path labels do not double-count totals or restart a continuous kernel stretch.
The maximum also observes syscall dispatch and ordinary synchronous UART
polling (`site=serial_drain`). `site=ua_map_at` covers ELF page zeroing and
permission installation; `site=scroll` covers console history repainting.
These are source function names. A symbol identifies the measured active
path, not proof that firmware/SMI pauses within it were software execution.

The UART queue has its existing whole-record overflow policy and drop notice.
Attribution stays in the disk runtime ring and never polls the UART or paints
the display, including before the desktop's grant. The ordinary boot/evidence
console and serial policies remain in force for their existing callers.

`bootlog_sync` snapshots the captured-byte count, requests a commit and then
sleeps through the caller's callback. It performs no VFS operation and never
owns `draining`. Each wait observes caller cancellation plus its own 2 s /
2000-turn bound. A timeout leaves the storage request pending and does not
poison the logger. The storage task drains one 4096-byte batch per turn;
committed older watermarks can complete while newer producers/waiters remain.
Runtime-ring bytes already overwritten count as retired, matching the existing
lossy ring policy; protected boot bytes cannot be overwritten. The `queued`
statistic includes a one-byte scheduling token when only a commit is pending.
The unchanged storage task's 250 ms cadence still applies.

Private ELF image zeroing sleeps after each 64 KiB, including an exact final
chunk, so exact 64 KiB segments also reach a scheduling point. Live
anonymous mappings and `mprotect` keep their transaction intact. File payload
copying also sleeps after each 64 KiB. Allocation failure or `PROC_STOPPING`
after a zeroing yield rolls back all provisional frames/page tables before
the child can be published. No memory admission limit changes.

The wallpaper worker keeps its original area-average/bilinear sampling and
alpha rounding. Precomputed horizontal coordinates use 12 bytes per output
column (24 KiB at the worker's 2048-pixel limit); allocation failure computes
the same coordinates inline. Decoder limits of 4096 by 4096 make the rounded
channel sums fit 32 bits, removing four software 64-bit divisions per output
pixel on i686. No cache-version or image/portrait changes.

The completion tracker has 16 bounded watermark slots. The earliest unsatisfied
active waiter determines the next commit point; newer callers cannot extend
an earlier wait. Cancelled/timed-out targets still receive an eventual commit.

## Completed host validation

All builds/tests used `nice -n 19`; suites ran sequentially.

- `ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_kernel_tests.sh`:
  PASS, including the new attribution/scheduler replay and all USB/HID,
  storage/hostile-bind, process, signal and DOS VM host suites.
- `nice -n 19 python3 -B -m unittest discover -s tests/host/desktop -t tests/host -v`:
  PASS, 97 tests and five native-build-dependent skips. Wallpaper mode goldens,
  default-cache hashes/FAT readback, worker responsiveness, mailbox ownership,
  desktop allocation/fallback checks, compositor and application replays pass.
- `nice -n 19 python3 -B -m unittest discover -s tests/host -p test_boot_modes.py`:
  PASS, ten tests, including 5000 verbose wrapping records with the desktop's
  framebuffer unchanged and explicit panic reclaim still painting.
- `nice -n 19 python3 scripts/build_kernel.py`: PASS, final 4,671,724-byte ELF,
  successful i686 link, frame-size guards and FPU/SIMD audit. The wallpaper
  source also compiles for `-m32 -march=pentiumpro` with x87/MMX/SSE disabled.
- Original/updated `wallpaper_bench` at `-O2`, ten iterations each: scaling
  1024x704 fill averages **10.513 ms before / 4.103 ms after** on this x86-64
  host. Decode averages 25.732 / 25.897 ms. All five 1024x768 output-frame
  SHA-256 values match the original renderer. These timings do not qualify
  the Pentium III; the reduction in software 64-bit divisions needs T23 timing.

The production scheduler replay models a fixed 4 MiB zeroing job at 1 ms per
page. Three P_DEVICE scan workers and a P_NORMAL desktop run for 3000 ms:

| Metric | Uninterrupted reference | Cooperative 64 KiB chunks |
| --- | ---: | ---: |
| Scans per worker | 9, 9, 9 | 12, 12, 12 |
| Maximum scan lateness | 774 ms | 5 ms |
| Desktop frames | 125 | 184 |
| Maximum frame gap | 1024 ms | 17 ms |

This is a deterministic scheduling mechanism test, not measured T23 execution
speed. Separate production ELF tests check all BSS bytes, 64 zeroing yields,
copy yields, and complete rollback on allocation failure or STOPPING after a
yield. Logger tests replay three simultaneous watermark waits and verify
commits at the first 4 KiB, 8 KiB and final batch, without binder VFS calls.
UART tests prove queue-only diagnostics do not poll even with buffering off.

No QEMU, full image build, hardware run, commit or diary entry was performed.
Guest and T23 gates, including normal/verbose cold/warm boot and slow USB bind
with the receiver in use, remain with the lead. Inspect the unchanged 250 ms
storage cadence and the 2 s sync bound alongside f1-62/f1-63. Full evidence and
commands are in `build/codex/f0-20-report.md`; raw comparison/review captures
were removed, retaining textual results and baseline frame hashes.
