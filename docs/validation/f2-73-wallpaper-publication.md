# F2-73 wallpaper publication

Research and source comparison, 2026-10-15; host evidence only.

[POSIX fsync](https://pubs.opengroup.org/onlinepubs/009695399/functions/fsync.html)
permits waiting for storage completion. The
[upstream filesystem locking documentation](https://www.kernel.org/doc/html/v5.14/filesystems/locking.html)
also identifies writes and fsync as potentially blocking operations.
[POSIX condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html)
release the mailbox mutex during sleep; publication must depend on the ready
predicate, rather than unrelated storage progress.

In the supplied a395ffa8 disk-BOOT.LOG, the current boot reaches
wallpaper_scaled at 1,777,000 us but never records wallpaper_applied.
Earlier blackbox records must be distinguished from that boot. Its broad
shell/wallpaper/settings labels cannot identify an individual blocked call.

Source comparison finds that wall_prepare writes the full raw frame and
promotes the disk journal before wall_worker sets ready. At 1024x768 that
is 3,145,728 bytes. fat_write calls full-file COW replace on each write.
The native byte_io syscall holds the volume-wide VFS lock across
file_io_locked and fat_write; writes transfer at most PAGE_SIZE (4 KiB).
Appending a 3 MiB raw file therefore replaces 768 growing prefixes: about
1.21 GB of payload writes alone, before allocation, metadata and barriers.
This is a calculation from the source, not a measured hardware write count.
The shell polls the Desktop directory before wallpaper_poll, so a worker
doing nominally asynchronous cache persistence
can stall both publication and the shell through the shared filesystem.
The logs do not prove which individual write was executing.

Decision: the asynchronous runtime worker reads existing raw caches and
retains its bounded memory caches, but does not create raw files or update
the recent journal. Build-seeded disk frames remain usable. The explicit
synchronous desk_wall_prepare API retains cache generation for tooling and
its cache-format tests. Changing FAT transaction semantics, adding a cache
writer thread, or changing hardware registers is outside this fix. Slow
optional persistence must not gate a visible frame or contend with shell
directory reads. A later runtime cache writer needs a qualified bounded
storage path first.

The identical-photo upload shortcut also needs a valid displayed scene and
background, not just an allocated wallpaper buffer. Watchdog records will
identify the active shell call and worker phase at the 500 ms threshold;
normal damage-free idle must remain quiet.

The diagnostic distinguishes input_pending from work_pending: wallpaper and
settings publication can need a frame when no input was consumed. It reports
the shell operation, wallpaper I/O/render/mailbox operation, ready mailbox
state, and settings read/write/log/mixer activity. A lost loop heartbeat also
reports without inventing input. Messages retain bounded raw-write lengths.

No hardware programming decision is made. The supplied driver package and
DOS drivers do not define application cache publication or FAT transaction
ownership. QEMU, the H page and T23 qualification remain with the lead.

## Final host evidence

All tests and builders ran sequentially under `nice -n 19`.

- Desktop suite: 101 tests in 318.104 s, OK, five existing absent-native-payload
  skips. The initial run exposed an unrelated missing formatter link in the
  Savage host harness; the final run includes its repair.
- Desktop gate, compositor/settings performance and stall tests: 14 tests in
  54.203 s, OK. The performance fixture now uses the pinned cursor footprint
  and current font layout rather than obsolete cursor/clock constants.
- Both changed production C units compiled as i686 objects with the SDK ABI
  flags, current overlay/ABI headers, `-O2 -Wall -Wextra -Werror` and the 4 KiB
  frame limit. This is not a desktop link or SDK qualification.
- Kernel build: link and FPU/SIMD audit passed. DOS payload build: fsck and
  T1 header/payload readback passed. No canonical OS image was built or run.

The actual normal shell setup and first placeholder frame precede worker
submission in the new replay. Its idle service adds no artificial input or
damage to trigger the photograph. Native-like cache writes would be short
and delayed; read-only mutations would return EROFS. The fixed worker makes
zero raw/journal mutation attempts in every case.

| Cold cache | Access | Scaled to visible, host upper bound |
| --- | --- | --- |
| absent | writable | 29,502 us |
| present | writable | 27,529 us |
| absent | read-only | 29,762 us |
| present | read-only | 27,193 us |

Two additional matching disk-seed cases require actual cache hits with
decode/scale skipped. Theme and photograph changes present their completed
replacement in the next eligible frame; invalid scanout/background prevents
an identical buffer from suppressing the upload. Independent direct rendering,
an uncached desktop painter and a separate scanout verify the frames. Nine
PNG captures and their hashes are under `build/host/f2-73/`; SHA-256 goldens
are enforced by the host test. Existing wallpaper and UI goldens remain valid.

The original production implementation fails the new writable cold-cache
replay's one-second publication bound with one injected 1.2 s cache write.
The existing three-window transition/settling, worker supersession, failed
present and zero-allocation replays also pass. Observer checks cover 500/501
ms, quiet idle, work without input, lost heartbeat, wraparound, mailbox
contention and raw-message length bounds.

Full verbatim output, source references, artifact hashes and the H/T23
handoff are in `build/codex/f2-73-report.md`. Actual T23 latency, normal boot
visibility and `desktop-shell-qemu-t23` remain unqualified by host evidence.
