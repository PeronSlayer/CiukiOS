# f2-10 implementation and host validation

This report covers the authorized worktree implementation. Guest and hardware
qualification belong to the lead; no QEMU, full image build, systemd scope or
Git command was run by the implementer.

## Research and decisions

- [POSIX uname](https://pubs.opengroup.org/onlinepubs/9799919799/functions/uname.html)
  describes copied system identity. CiukiOS's exact strings, 336-byte wire
  layout, build ID and clock-source extension follow execution-abi.md, row 56.
  The kernel copies a completely initialized record through copy_to_user.
- [POSIX nanosleep](https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html)
  specifies EINTR and a remaining duration. The existing CiukiOS clock backend
  retains its pin until that output is written; its cancellation point now uses
  the existing I-class helper. Console is write-only and null returns EOF, so
  the additional genuinely blocking guest case uses channel_recv.
- [POSIX open](https://pubs.opengroup.org/onlinepubs/9799919799/functions/open.html)
  specifies EROFS when creating on a read-only filesystem. Repository inspection
  found that storage_probe_readonly returns true for every test selector and
  storage_init skips automatic storage_enable_write in that case. The original
  libc controller did not enable writing. Consequently tmpfile and mkstemp
  exercise a read-only mount. The fix belongs in controller setup, using the
  existing qualified write gate, rather than weakening SDK error handling.
- [Newlib manual](https://sourceware.org/newlib/libc.html) and the pinned local
  source were checked for temporary files, dynamic reentrancy and retargeted
  syscalls. Native tests compile the actual smoke main, SDK wrappers, pthread
  runtime and newlib. Only architectural startup/TCB access and the syscall
  boundary are substituted; this does not qualify target execution or timing.
- [Microsoft FAT specification](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf)
  was compared with fat_mount and FAT COW replace. The disposable sparse FAT32
  fixture has 65,525 clusters and valid BPB, FSInfo and reserved FAT entries.
  FAT replace assigns its owning entry after its durable update barrier. The
  fixture fails the first device write after that assignment, then checks the
  production fd byte count and sticky cache/fsync error. It is independent of
  the real controller and canonical volume. Its temporary 1 MiB cache supplies
  16 workspace pages for mount and is fully freed; system limits are unchanged.

## The three libc failures and fixes

The original image's aggregate count does not identify names. Dispatch/mount
inspection supplies the diagnosis below; the native harness reproduces exactly
these three named failures with the original read-only/missing-uname setup.
They are host-reproduced, not yet confirmed by a newly instrumented guest run.

| Failing check / call-3 name | Host observation | Fix |
| --- | --- | --- |
| `tmpfile()` result `f != NULL` / `f__NULL` | NULL, errno=30 (EROFS), line=70 | libc controller calls `storage_enable_write(storage_get(), 2)` before launch. A refused F1 write gate fails setup and never spawns the payload. |
| `mkstemp()` result `fd >= 0` / `fd__0` | negative fd, errno=30 (EROFS), line=71 | Same qualified write-gate setup; preserve the SDK's error propagation. |
| `!uname(&u) && !strcmp(u.sysname, "CiukiOS")` / `_uname__u____strcmp_u_sysname__CiukiOS__` | -1, errno=38 (ENOSYS), line=99 | Implement row 56 and route it through file_syscall; copy initialized identity/build/clock-source fields. |

Healthy native execution exits 0 with 10,116 total checks, failures=0 and callback
orders 2/3/4. The original-setup regression exits 1 with exactly three failure
records and failures=3 in all summaries. The check count differs from the guest's
11,240 because its busy-loop assertions depend on clock-read progress. Native
TCB/newlib sizes and fake-clock values are not guest resource/timing evidence.
No SDK production runtime fix was warranted by these failures.

Each failed CHECK now submits one printable ASCII record of at most 240 bytes,
with a sanitized expression name, expected=1, observed=0, source line and saved
errno. Call 3 retains supervisor PID association and hex framing. The controller
requires the three ordered main/atexit/destructor summaries, zero failures,
valid fields and exit status zero; a failed CHECK remains sticky even if a
payload later exits zero.

## Other implemented points

- uname host coverage checks 336-byte size, extension offsets 328/332, identity,
  build fallback, RTC and build clock-source agreement, zero padding, surrounding
  canaries, read-only/wrapped/page-straddling output errors. The ring-3 file
  payload also calls row 56 and the controller compares its clock seed source.
- The signal payload/controller adds channel-receive EINTR and close/dup2
  deferral at a yielding final-release boundary. Together with the existing
  thread_kill/nanosleep case, it records actual result, handler-saved EAX and
  resumed EAX; nanosleep injection requires a blocked target and its poisoned
  remainder output must already hold the final value at handler entry.
  Close/replacement must
  finish before handler entry and release exactly once. These are target
  payloads, assembled and loaded by host tests, not host-executed ring-3 tests.
- The production interruption helper is wired into nanosleep and the next-read
  cancellation point. T0 checks all 48 decision combinations and the same 48
  through the real pending-signal helper with a fake scheduler, plus handler and
  mask suppression, result priority and wakeup. The guest controller separately
  labels all 48 expected/observed rows `layer=decision`; it does not claim issued
  ATA I/O.
- The fresh FAT32 block fixture fails the first post-commit write, observes
  `count=3 error=-5 fsync=-5 sticky=-5 committed=1 quarantined=1`, and destroys
  only its own cache. The exact production helper is linked into ASan/UBSan T0
  tests; pages and heap return to the preceding baseline.
- fd-table writes, fsyncs and reopens `/tmp/f2-durable.bin`, records its path,
  byte count and SHA-256, cleanly syncs storage and emits a durable-shutdown ARM.
  The runner requires one ARM and both checker/digest declarations, verifies
  stopped QEMU, then uses the existing F1 read-only overlay export, fsck.fat -n,
  mtools listing and independent file digest path. The three fd-table profile
  entries declare those checks without removing existing acceptance predicates.
  Fake-QEMU tests reject a missing ARM, mismatched digest or missing declarations.

## Files

| Files | Change |
| --- | --- |
| `src/kernel/proc/syscalls_file.c`, `src/kernel/core/syscall.c`, `src/kernel/proc/clock.c` | uname dispatch/record, call-3 observation, existing interruption helper wiring. |
| `src/kernel/probes/f2_probes_signals.c`, `tests/host/proc/signal_payload.asm` | Blocking channel, close/dup2 release boundary, remainder/EAX observations and decision rows. |
| `src/kernel/probes/f2_probes_files.c`, `tests/host/proc/files_payload.asm` | Private post-commit block fault, durable workload and guest uname check. |
| `src/kernel/probes/f2_probes_desktop.c` | Qualified libc write gate and PID-bound summary/failure checks. |
| `sdk/tests/libc_smoke.c` | Named bounded failure records with preserved errno. |
| `sdk/tests/libc_host.c`, `sdk/tests/libc_start64.S`, `sdk/tests/build_libc_host.py` | Native pinned-newlib reference and modeled syscall boundary, all build artifacts inside the SDK's build directory. |
| `scripts/test/run.py`, `tests/suites/f2-process.json`, `tests/host/test_runner.py` | Durable checker routing, declarations, signed errno encodings and fake-QEMU regression. |
| `tests/host/proc/files_test.c`, `tests/host/proc/signal_test.c`, `tests/host/proc/signal_legacy.c`, `tests/host/proc/desktop_test.c`, `scripts/test/host_kernel_tests.sh` | Layout/fault/helper/controller tests and production-helper linkage. |
| `tests/host/test_sdk_smoke.py` | Native healthy run, isolated named failure and exact three-failure original-setup regression. |
| `tests/host/proc/F2-10-REPORT.md` | Research, decisions, test evidence and remaining acceptance gaps. |

## Validation

All logs are in `build/f2-10/` in this worktree.

| Command | Final result / output |
| --- | --- |
| Initial `bash scripts/build_sdk.sh --archive /home/peronslayer/Desktop/CiukiOS/build/downloads/newlib/newlib-4.5.0.20241231.tar.gz --jobs 2` | PASS; 17.659 s. `sdk-initial.log`. |
| Same SDK command after final SDK source changes | PASS; 17.927 s; 6,762,628 installed bytes; all 53 F2 raw stubs, errno/layout, native pthread and x87/SSE/MMX audits. `sdk-rebuild.log`. |
| `build/tools/ciuki-sdk/bin/ciuki-cc --self-test` | PASS; hello compile/link/ELF inspection. `sdk-self-test.log`. The tool correctly states runtime qualification not_run. |
| `ASAN_OPTIONS=detect_leaks=0 scripts/test/host_kernel_tests.sh` | PASS; 48 decision/helper rows; signal final checks=4,687; file final checks=14,433, descriptors=0, pins=0, pages=0; production fault result above; controller write-gate/refusal and report checks. `host-kernel.log`. |
| `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -v` | PASS; 97 tests in 35.261 s; 4 skips for unbuilt desktop/Lua artifacts. Runner checker and three native libc tests pass. `python-host.log`. No lock-contention retry was needed in the final run. |
| `python3 scripts/build_kernel.py` | PASS; FPU/SIMD audit; VMM.ELF 1,413,924 bytes. `kernel-build.log`. |
| Native reference / `original` mode | Exit 0 / expected exit 1, failures=0 / exactly three named failures. `libc-native.log`, `libc-original.log`. |

The kernel script normally invokes Git for provenance. To honor the explicit
no-Git instruction, a build-local executable named git returned failure through
PATH; the script used its existing build-ID fallback (`unknown`, dirty=1).
This is a compile/host check, not canonical-image provenance. The lead must build
the integrated image with its normal build ID. Temporary investigation copies
and the build-local shim are removed after validation.

## Contract and acceptance problems still requiring the lead

1. f2-acceptance.md lines 185–199 requires a full pre/post-issue read/pread
   interruption matrix, failed/time-out drain/quarantine, partial transfer,
   D-class commit and close/dup2 precommit waits with command/buffer/offset/pin
   evidence. The directive's real sleep/channel cases, completion deferral and
   pure decision table do not provide that entire guest matrix. Existing file
   T0 tests cover drained getdents, and helper tests cover result priority; they
   do not establish those guest disk schedules. Backend changes beyond wiring
   the existing helper were not authorized here. This work does not certify
   that larger normative requirement.
2. f2-process.json already requires `group=fd-table` and `group=resources`
   aggregates, including directory/identity/link/flags/checker fields and full
   ledgers. The current controller emits individual operation/case records and
   a narrower file ledger. Those original predicates remain intact, so the full
   suite will reject incomplete evidence even if the single probe ends PASS.
   The host durable-checker test proves wiring after successful evidence;
   it does not claim the current full-suite evidence passes.
3. f2-runtime.json requires signal summary fields (including ordering_errors)
   and a combined syscall-interruption disk summary that current case records
   do not provide. libc-smoke also requires `tests`, `missing_cases=0`, supported/
   excluded syscall coverage, hashes/clocks and a full resource ledger. The
   smoke program does not yet exercise every 16–68 syscall success/negative
   pair; the new summaries intentionally do not invent a coverage bitmap or
   missing_cases=0. The runtime suite is outside the authorized file list and
   was not relaxed. Different syscall result records also need proper grouping
   before that suite's `combine` predicates can accept them.
4. The host original-setup run is strong evidence for the three-failure
   diagnosis but is not the requested named guest rerun. The lead still needs
   the rebuilt SDK payload in the canonical image, the three single-probe
   boots, then suite evidence tied to that image SHA-256. No guest or hardware
   pass is claimed by this report.
