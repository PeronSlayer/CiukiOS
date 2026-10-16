# F2-78 AutoPlay frame-loop isolation

Research and implementation decision, 2026-10-16. Host evidence only;
QEMU and T23 qualification remain with the lead and owner.

POSIX [mutex locking](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)
specifies that `pthread_mutex_trylock` returns immediately on contention.
The [newlib manual](https://sourceware.org/newlib/libc.html) documents the
library's retargetable locks and reentrant stdio routines. Neither a bounded
directory entry count nor a small registry file bounds the time spent in
filesystem calls or libc locks.

Repository check: `sdk/libciuki/posix.c:readdir` synchronously invokes
GETDENTS; the Files VFS adapter also stats each entry and may open files to
classify executables. The SDK's `sdk/libpthread/pthread.c:acquire` returns
EBUSY on a contended trylock before reaching WAIT_WORD. This supports the
worker/mailbox approach already used by settings and wallpaper, without a
libc, kernel, filesystem or USB-driver change.

The existing `desk_autoplay_service` runs `ap_probe_workspace` (directory
enumeration, stat and file reads), then `ap_preference_get` (registry recovery
and file reads), on the frame thread. Confirmation can also persist a choice
on that thread. Input-ownership guards only return; they contain no waiting
loop. The wallpaper and settings mailboxes already use trylock, and AutoPlay
does not call either mailbox. In the read-only a79f840c T23 log, wallpaper was
applied at 2.292 s before the persistent `wait=autoplay-service` reports;
the wallpaper worker was subsequently waiting for another request. The log
does not identify the syscall or lock inside the synchronous AutoPlay path.
It cannot establish a particular physical deadlock or its lock owner.

Reserve one AutoPlay worker before the first frame, with one request/result
slot and the shell's existing reusable discovery workspace. Run media probing
and preference reads/writes there, outside its mailbox lock. Service submits
or polls with trylock and advances at most one media item per turn. Keep all
window/focus changes on the frame thread, wait for a successful first desktop
presentation, and retain arrivals through menus, foreground windows and a
client’s first presentation. Validate the observed drive generation before
publishing an asynchronous probe. A stale result cannot launch a replacement
medium. Selected applications are queued for the desktop's existing launch
stage; process creation does not run in AutoPlay service. Shutdown polling
waits for I/O completion without joining in service.
The worker is idle during setup and is never created by a later media arrival.

Host reproduction holds the filesystem discovery callback until the test
permits the frame loop to advance, while wallpaper decoding and a settings
read are also held. This reproduces the synchronous frame-thread dependency
without claiming to emulate the T23's unknown internal I/O wait. New worker
activity diagnostics distinguish media scanning and preference I/O from the
frame thread's bounded AutoPlay service on a future hardware run.

Host validation completed serially with `nice -n 19`:

- `make CAP= desktop` built and inspected the native applications using a
  verified local copy of the installed SDK; no SDK or canonical image rebuild.
- All 102 desktop tests passed. The new held-I/O case measured a maximum
  service turn of 105 microseconds; its 50 ms assertion includes host scheduling
  margin and is not a claim about T23 timing. The footprint audit still reports
  zero allocations after the first frame. Existing frame goldens are unchanged.
- All 11 gate tests passed, including the unchanged three `f1-usb` cases'
  exact restored/final ledger predicates.
- The original HEAD AutoPlay implementation fails the regression at its
  frame-thread preference callback before readiness. A second baseline run,
  omitting only those two early service checks in a temporary harness, fails
  at the frame-thread media-enumeration callback after the first frame, while
  wallpaper decode and a settings read are held. Link-only compatibility stubs
  leave the original service unchanged. Temporary baseline sources were deleted.
- `git diff --check` passed. No QEMU, systemd scope, commit or diary edit.

Commands, application hashes and logs are indexed in
`build/codex/f2-78-report.md`. Desktop/media QEMU gates and the owner's T23 boot
with the stick already inserted remain unqualified by this host evidence.
