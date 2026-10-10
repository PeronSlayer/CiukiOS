# Directive f2-03: file descriptors, the POSIX path view and the file syscalls

- **Step:** F2. **Contracts:** `posix-subset.md` ("POSIX paths over the
  drive VFS", "Files and departures from full POSIX", `/dev/null`,
  `/dev/console`), `execution-abi.md` F2 extension (syscalls 29–49 and 50–51:
  `open`, `read`, `write`, `pread`, `pwrite`, `lseek64`, `close`, `fstat`,
  `stat`, `getdents`, `mkdir`, `rmdir`, `rename`, `unlink`, `dup`, `dup2`,
  `fcntl`, `fsync`, `ftruncate`, `getcwd`, `chdir`, `clock_gettime`,
  `nanosleep`; `stat`/`dirent` records; blocking classes; validation
  order), `vfs-storage-contract.md` (open descriptions, share modes,
  durability), `f2-acceptance.md` (`fd-table`, clock cases).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh` (namespace
  atomicity and the FAT departures).
- **Prerequisites on `main`:** f2-02 (process object, fd table storage,
  cwd reference, syscall layer), f1-09 (mounted volumes, boot log, RTC
  provider).
- **Worktree:** `wt/f2-files`. Files: new `src/kernel/proc/fdtable.c`,
  `src/kernel/proc/posixpath.c`, `src/kernel/proc/syscalls_file.c`,
  `src/kernel/proc/devnodes.c`, `src/kernel/proc/clock.c` (complete the
  f2-02 hook: REALTIME seed from the RTC provider or the build epoch,
  CPU time accounting, `nanosleep`), headers under `ciuki/`,
  `src/kernel/fs/vfs.c` only for integration fixes with a note each,
  `src/kernel/probes/f2_probes_files.c` (`fd-table` probe and the clock
  cases), `tests/host/proc/*`, `scripts/test/host_kernel_tests.sh`.

## What to build

1. **Path view**: `/` = `C:\`, `/mnt/<letter>` for other volumes, synthetic
   `/mnt` and `/dev` with `null` and `console` taking precedence; parser
   rules (separators, `.`/`..` with lookup before `..`, trailing slash,
   backslash/colon → EINVAL, empty → ENOENT, UTF-8 validation → EILSEQ,
   259 UCS-2 units, `PATH_MAX` 1040, `NAME_MAX` 765); cwd snapshot at
   syscall entry; `getcwd` reconstruction under namespace locks; pinning
   (cwd and open directories make `rmdir` fail EBUSY); case-preserving,
   case-insensitive comparison through the VFS key.
2. **Descriptors**: fd table of 128 with lowest-free allocation, open
   descriptions shared by `dup`/`dup2`/inheritance (position and status
   flags), `FD_CLOEXEC` per fd, `fcntl` subset, object kinds (file,
   directory, null, console; surfaces/channels come from f2-05 through the
   same table), `close` releasing exactly once even on EIO.
3. **File syscalls** per the table and the departures: 1 MiB per call,
   short counts, append placement under the node lock, `pread`/`pwrite`
   position rules, 64-bit seek without allocation, EFBIG above 0xFFFFFFFF,
   zero-filled growth, unlink-while-open retention with `st_nlink` 0,
   rename semantics (replacement rules, EISDIR/ENOTDIR/EINVAL, identical
   entry no-op, case-only spelling update, descendant refusal, EXDEV),
   `getdents` fixed 792-byte records with cookies, `fsync` with the F1
   barrier, `ftruncate`, `stat`/`fstat` field synthesis (`st_dev`, `st_ino`
   stable per mount, modes, blocks, UTC timestamps, ctime tracking).
4. **Devices**: `/dev/null` and `/dev/console` semantics (console write
   through the serialized F1 console/serial sink preserving newline/tab/
   UTF-8, no record forgery, ESPIPE on seek/pread/pwrite, EACCES on
   read-capable console open).
5. **Clocks**: MONOTONIC from the timer, REALTIME seeded from the RTC
   provider when qualified else the build epoch (`realtime_source`),
   `CLOCK_PROCESS_CPUTIME_ID` with 1 ms accounting across the process's
   threads, `clock_getres` 1 ms, `nanosleep` rounding up with EINTR
   remainder.
6. **Probe** `fd-table` exactly as the acceptance row, plus the clock
   cases of the acceptance (10,000 monotonic reads, REALTIME − MONOTONIC
   equals the seed within a tick, CPU time behaviour, 20 ms sleep, EINTR
   at ~10 ms via f2-04's `thread_kill` if present — else the case reports
   `not_run` for that subcase).

## Host tests (mandatory)

The path resolver matrix of `f2-acceptance.md` (every listed case), the fd
table (allocation, dup/dup2/self, CLOEXEC, EMFILE, close-once), rename
decision table, `getdents` record packing and cookies, stat synthesis,
clock arithmetic (seed, rounding, overflow), all against the F1 fs host
harness with fake block devices.

## Acceptance by the lead

Diff review; host tests; kernel build with audit; on QEMU `fd-table`
passes on the three profiles with the interim payloads (SDK payloads once
f2-06 lands), F0/F1 suites unchanged. Reply with: files, interfaces, test
output, and any contract problem found.
