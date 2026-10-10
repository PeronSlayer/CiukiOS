# Directive f2-14: `signals-fault` subcases `fault-repair` and `nanosleep-eintr` fail on QEMU after f2-10

- **Step:** F2. **Contracts:** `posix-subset.md` (signals: delivery frames,
  FNSAVE ucontext, `sigreturn`, fault mapping, EINTR), `execution-abi.md`
  F2 extension, `f2-acceptance.md` (`signals-fault` row), QEMU TCG
  deviations recorded in `docs/validation/2026-10-09-f0/README.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-signals-fix`, from `main` at or after `c4b10dc`.
  Files: `src/kernel/proc/signal.c`, `sigframe.c`, `syscalls_signal.c`,
  `src/kernel/probes/f2_probes_signals.c`, `tests/host/proc/signal_payload.asm`,
  `signal_test.c`, `signal_legacy.c`, `scripts/test/host_kernel_tests.sh`,
  `docs/validation/2026-10-09-f0/README.md` only if a new TCG deviation is
  proven. Do not touch the suites (directive f2-13 owns them).

## Observed on image `a9550f04…` (commit `3619267`, single boot on `qemu-t23`, 2026-10-11)

```
probe=signals-fault event=DATA case=fault-repair part=status expected=0 observed=512 raw_vector=16 corruption=2
probe=signals-fault event=DATA case=nanosleep-eintr part=status expected=0 observed=512 raw_vector=0 corruption=2
probe=signals-fault event=END status=FAIL reason=signal_contract
```

`observed=512` is the wait status of exit code 2 and `corruption=2` is the
payload's own error counter (`r.errors`): in both subcases the user payload
counted two failed checks and exited 2 instead of 0. Every other
`signals-fault` subcase passes, including `syscall-interruption` (48 decision
rows). The full record set is `build/f2records/signals-fault.records` in the
worktree. On the ninth image (`f76ab59e…`, before f2-10) the probe's only
failure was the `syscall-interruption` placeholder, so `fault-repair` and
`nanosleep-eintr` passed then; f2-10 (commit `a8b3810`) changed
`f2_probes_signals.c` and added 64 lines to `signal_payload.asm`. The host
signal tests pass in the ASan harness.

## What to do

1. Identify the two checks the payload fails in each subcase (the payload
   source is `tests/host/proc/signal_payload.asm`; the probe scores
   `r.errors` and the exit status). Reproduce on the host if the harness
   can express it; otherwise reason from the frame layout and say so.
2. Decide and prove which of these it is:
   - a kernel defect in the signal frame (FNSAVE/FRSTOR area, saved EAX,
     `sigreturn` restoration, EINTR result propagation) introduced or
     exposed by f2-10 → fix the kernel, with a host test that fails before
     and passes after;
   - a payload check wrong under the ABI (for example comparing fields the
     contract leaves unspecified) → fix the payload, with the contract
     reference;
   - a QEMU TCG deviation from the i686 behaviour (x87 state after `#MF`,
     precision control, status word bits) → make the check robust to the
     documented deviation without weakening the contract on hardware, and
     record the deviation with its evidence in the F0 TCG deviation list.
3. Keep the records' names and fields; do not lower `expected` values.

## Acceptance by the lead

Host tests; kernel build; on QEMU `f2:signals-fault` ends `END status=PASS`
on `qemu-t23`, `qemu-e500` and `qemu-min128` (the lead boots it). Reply
with: files, the two failing checks per subcase and their cause, the proof
(host output or TCG evidence), test output.
