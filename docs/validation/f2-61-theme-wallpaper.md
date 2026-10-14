# F2-61 theme and wallpaper changes

Research and repository comparison, 2026-10-14. Host implementation only;
the lead owns QEMU and T23 qualification.

- [POSIX mutex locking](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)
  specifies that trylock returns immediately on contention. The wallpaper
  consumer already uses it; settings reload still calls `settings_load` and
  `scan_programs` synchronously in `shell_service`. Move the settings read to
  the existing writer, discard obsolete read completions, and retain pending
  work when a mailbox is contended. Do not rescan programs for display settings.
- [POSIX condition waits](https://pubs.opengroup.org/onlinepubs/7908799/xsh/pthread_cond_wait.html)
  release the mutex during waiting and require rechecking the predicate.
  Existing workers follow this rule and perform I/O outside their mailbox
  locks. Keep these ownership rules; never wait for a worker on a render turn.
- [SDL event draining](https://wiki.libsdl.org/SDL3/SDL_PollEvent) supports
  servicing input before rendering. Retain that ordering and observe render
  progress from a separate watchdog thread, so a blocked main loop can report
  its last stage independently of the settings/storage worker.
- [Clang AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
  detects invalid accesses and lifetime errors. Use the existing production
  desktop harness with ASan/UBSan, real worker threads and bounded subprocess
  timeouts. These checks do not prove absence of every native scheduling race.

The wallpaper request includes the effective desktop colour. Platinum to
Ciuki Blue changes that colour, so `wallpaper_update` calls `wallpaper_load`,
which paints an empty image into the currently displayed frame. Retain that
frame while the worker prepares its replacement, including theme recolouring;
publish only the latest request. Icon preparation already occurs only in
`desk_shell_reset`, not in theme application; measure icon-cache counters and
theme application rather than changing the approved icon family.

The supplied ce2e6ce9 evidence establishes a desktop frame-production stall
while kernel workers remain alive, and the owner's report establishes the
visible wallpaper loss. It does not establish a particular native deadlock.
It cannot qualify the new implementation on the T23.

Native logging comparison: `sdk/libciuki/posix.c` maps `write(stderr, ...)`
to the ordinary file syscall. `src/kernel/proc/syscalls_file.c:byte_io` takes
the VFS lock even for console writes, so it cannot reliably diagnose a stall
behind that same lock. The existing syscall 2 in
[the execution ABI](../design/execution-abi.md),
`src/kernel/core/syscall.c:sys_debug_write`, accepts at most 240 bytes and calls
`klog` directly. `src/kernel/core/output.c:log_line` captures that line in the
bootlog, serial and console without the VFS lock. Use that bounded diagnostic
call for the native watchdog; keep stderr as the host capture seam. Strip the
trailing newline before the diagnostic call because it sanitises control
characters and supplies its own log newline. No kernel/API change is needed.

## Implementation and host evidence

The existing settings worker now handles explicit Control Panel reloads as
well as polling. Read generations discard ready/in-flight older snapshots.
The poller waits for its outstanding read rather than superseding a slow read
every two seconds. Explicit desktop Refresh still refreshes the program
inventory; display settings avoid that scan. Transient read errors retain the
active settings/theme. Pending reloads defer saving the pre-reload snapshot.
Desktop mailbox operations use trylock with retained work for retry. The new
`cfg_writer_try_submit` is used by the renderer; `cfg_writer_submit` preserves
the shared Control Panel writer's existing submission contract.

Absolute-file wallpaper requests retain the complete displayed frame until
the latest worker result transfers ownership. This covers both theme colour
changes and later path/mode changes, including contended submission. It does
not retain obsolete geometry after a display resize. No theme colours, icon
preparation or approved image assets change.

The observer starts before the first frame and uses 32-bit atomic stage,
frame-time, input-pending and loop-heartbeat values. It polls at 100 ms and
rate-limits records to one per two seconds. Worker status is sampled with
trylock; logging is independent of the storage worker. A lost loop heartbeat
also emits a record with the actual input-pending flag, because a blocked loop
cannot discover later input in its kernel grant. Healthy idle loops keep their
heartbeat and do not trigger the observer. Millisecond wraparound is covered.

The new sequence in `tests/host/desktop/theme_wallpaper_sequence_test.h` runs
inside the production launcher harness under ASan/UBSan, with a 20-second
alarm and 25-second subprocess timeout. Real wallpaper, settings and sound
threads stay alive; durable mixer saves run on the settings thread, matching
production. It exercises Control Panel Platinum to Ciuki Blue followed by
Ciuk3.png/Fit, delayed mixer persistence, an in-flight wallpaper render, latest
result publication, exact final pixels, continued input/presents and locked
mailbox retry. The independent observer is captured while the mixer is blocked.
`settings_io_test.c` also tests contended producer/consumer operations directly.
The deterministic gate harness gives background observers real sleeping;
only its main thread advances the atomic simulated clock and owns longjmp
checkpoints.

Before the fix the retained-pixels assertion failed:
`switch_us=665 icon_cache_before=204 icon_cache_after=204 retained=0`.
The full desktop run after the change recorded `switch_us=4`, `frame_us=452`,
204 icon entries before/after and `retained=1`. Assertions bound state
application to 100 ms and the following host turn to 500 ms. The real observer
record was `[desktop] stall stage=render worker=idle/mixer no_frame_ms=2002 input_pending=1`.
Existing desktop and sound allocation guards still reported zero allocations
after the first frame. These host costs are not measurements on the T23.

Validation ran sequentially under `nice -n 19`:

- `python3 -m unittest discover -s tests/host/desktop -t . -v`: 96 tests,
  91 passed and five pre-existing desktop-payload checks skipped. The pinned
  Lua archive required by the Terminal harness was verified and reused from
  the available cache. The skips require built native desktop payloads:
  Accessories/Tasks inventory, FAT payload readback, build provenance and
  stale-output rejection.
- `python3 -m unittest discover -s tests/host -p test_desktop_gate.py -v`:
  all ten tests passed, including normal first frames and background observers.
- Native i686 SDK compilation of `desktop.c`, `input.c`, `settings_io.c` and
  `wallpaper_io.c` passed with `-O2 -Wall -Wextra -Werror`. The desktop object
  references the diagnostic syscall and nonblocking submit; no atomic runtime
  helper references are present. Temporary objects were removed.
- The final focused footprint/sequence and wallpaper startup run passed all
  five tests in 17.436 s, recording a 7 microsecond state update and a
  605 microsecond following turn, unchanged icon count, retained pixels and
  the same 2,002 ms observer record.

## Lead qualification

The host establishes the wallpaper-loss defect and liveness with deliberately
blocked workers/mailboxes; it does not identify the exact ce2e6ce9 native
deadlock. Build one canonical image, record its SHA-256, run
`desktop-shell-qemu-t23` and the earlier gates, then repeat theme and wallpaper
changes on the T23. Check retention, final image/mode, input/frame continuity
and switch/full-repaint latency while storage/audio work is active. Confirm
the native `[user <tid>] [desktop] stall ...` record reaches the disk log, with
no healthy-idle false positives, and qualify the initial observer thread and
memory in the process ledgers/hardware budget. No QEMU or hardware run was
performed here.
