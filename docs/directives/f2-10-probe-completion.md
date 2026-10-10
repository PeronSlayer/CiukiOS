# Directive f2-10: complete the F2 probes — `uname`, interruption subcase, fd-table checkers, libc-smoke failures

- **Step:** F2. **Contracts:** `execution-abi.md` F2 extension (syscall 56
  `uname`, blocking classes and interruption), `f2-acceptance.md`
  (`signals-fault` incl. `syscall-interruption`, `fd-table` incl. the
  post-commit fault and the durable checker, `libc-smoke` coverage and
  `missing_cases=0`), `posix-subset.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-probes`. Files: `src/kernel/proc/syscalls_file.c` or a
  new `syscalls_info.c` (uname), `src/kernel/core/syscall.c` (row),
  `src/kernel/probes/f2_probes_signals.c`, `f2_probes_files.c`,
  `f2_probes_desktop.c` (libc-smoke controller), `src/kernel/proc/signal.c`
  and the f2-03 blocking points only to wire the existing interruption
  helper, `sdk/tests/libc_smoke.c`, `sdk/libciuki/*` and `sdk/libpthread/*`
  for the fixes the failures reveal, `scripts/test/run.py` (durable checker
  after the fd-table case: read-only export + `fsck.fat -n` + mtools listing,
  reusing the F1 machinery), `tests/suites/f2-process.json`,
  `tests/host/*`, `scripts/test/host_kernel_tests.sh`, `sdk/tests/*`.

## Observed on image `f76ab59e…` / `0086f20…` (manual single-probe boots, `qemu-t23`)

- `elf-load`, `spawn-wait`, `mmap`, `threads-wait`, `crash-isolation`
  (stand-in): PASS.
- `signals-fault`: every case passes except `syscall-interruption`
  reported `not_run reason=f2_03_backend_required` → END FAIL
  `signal_contract`. f2-03 is merged now: wire the subcase to the real
  `nanosleep`/`read` interruption paths.
- `fd-table`: `post-commit-fault not_run guest_fault_controller_not_wired`,
  `clock-uname not_run uname_not_implemented`, `durable-checker not_run
  runner_checker_not_wired` → END FAIL `files_contract`.
- `libc-smoke`: the SDK program ran (first newlib program in the guest):
  11,240 checks, **3 failures**, exit status 1; stdout
  `CiukiOS libc smoke: 4294967297 1.25`; the report lines do not say which
  checks failed.

## What to do

1. **`uname` (56)**: fill `ciuki_utsname` per the ABI (sysname CiukiOS,
   nodename ciuki, machine i686, release/version from the build id,
   `abi_version=1`, `realtime_source` from the clock seed) with the row's
   errors; host test for the layout.
2. **`syscall-interruption`**: implement the subcase with the merged
   backends: a thread blocked in `nanosleep` (I class) interrupted by
   `thread_kill` SIGUSR1 → EINTR with remainder; a `read` on `/dev/console`?
   (not readable) — use the contract's console/null semantics or a channel
   receive if present; `close`/`dup2` deferral observed; record the
   decision table outcomes; no `not_run` left when the backend exists.
3. **fd-table checkers**: (a) the post-commit fault: a supervisor-only
   boundary that fails the next sector write after commit (reuse the F1
   ATA register boundary or the cache fault hook from f1-09) and the
   probe's expectations (positive count, sticky fsync error); (b) the
   durable checker: after the fd-table case the runner stops QEMU, exports
   the overlay read-only and runs `fsck.fat -n` and the mtools listing
   against the probe's recorded digests (same code path as `fat-write`);
   the probe emits `ARM` before the durable shutdown so the runner knows.
4. **libc-smoke**: make `sdk/tests/libc_smoke.c` report each failing check
   as its own bounded line (`case=<name> expected=… observed=…`) through
   call 3 so the controller frames it; rebuild the SDK program, run it on
   the host-side harness where possible, identify the three failures on
   the guest from their names and fix them (in the SDK runtime or in the
   kernel syscall they exercise); the probe passes only with
   `failures=0` and exit status 0.

## Host tests (mandatory)

uname layout; interruption decision outcomes with the fake scheduler;
the fault boundary and checker wiring (runner test with fake QEMU);
libc_smoke's report format; SDK build and `--self-test`.

## Acceptance by the lead

Host tests; kernel and SDK build; on QEMU `f2:signals-fault`, `f2:fd-table`
and `f2:libc-smoke` pass on `qemu-t23` (single boots), then the `f2-process`
and `f2-runtime` suites. Reply with: the three libc-smoke failures and their
fixes, files, test output, and any contract problem found.
