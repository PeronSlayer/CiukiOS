# F2-23: channel receive signal injection on hardware

Directive: [f2-23](../directives/f2-23-channel-eintr-hardware.md).
Date: 2026-10-11. Qualification: host evidence only; QEMU/hardware pending.

## Research and repository comparison

[Linux v6.12 sched.h](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/sched.h)
describes task state as runnability and shows that wakeup changes a waiting
task's state before its condition loop resumes. This supports distinguishing
an outstanding blocking invocation from its instantaneous scheduler state;
it does not define CiukiOS states or its ABI. The binding interruption and
completed-EAX requirements remain [execution-abi.md](../design/execution-abi.md),
the [POSIX subset](../design/posix-subset.md) and
[F2 acceptance](../design/f2-acceptance.md).

In this repository, `channel_recv()` waits with deadline `g_ticks + 1`.
`kwait_wait_until()` sets `T_BLOCKED`; `sched_tick()` changes an expired
waiter to `T_READY` before dispatch. The controller also sleeps one tick.
It can therefore sample only `T_READY` while receive remains outstanding.
The original predicate accepted only `T_BLOCKED`. The T23 capture for sweep
77777777 shows stage-dependent injection never completed, zero handler
entries/returns and survivor progress 382 -> 3403. That capture contains no
task-state snapshot: the precise hardware interleaving is inferred from the
production wait/scheduler paths, not measured in that historical log.

The payload writes TID at startup, before channel-pair creation and the
stage-2 store. The non-preemptible UP kernel does not switch tasks during
`ua_read()`. Neither reordered publication nor failed channel creation is
needed to explain the failure; an error counter must still reject failed
payload preconditions.

## Implementation rules

Mode 15 may inject only at stage 2 with zero payload errors and an existing
thread/task that is inside its syscall and owns a pending operation. Both
`T_BLOCKED` and `T_READY` are eligible; `T_RUNNING`, `T_ZOMBIE` and `T_DEAD`
are rejected. On this UP kernel another task cannot be running while the
controller executes. The stage-2 payload's pending operation belongs to
receive; the syscall/operation guards reject a sample before receive entry.
The payload and production channel wait timing remain unchanged.

Modes 15-17 record the actual injection target, mask, signal and return code,
with `attempted=0` and `observed=-ESRCH` if no call was made. Close/dup2
capture this information at their existing final-release injection boundary.
When the original 2000-tick controller deadline expires without successful
injection, one `part=wait` record reports the last sampled stage, TID, task
state and ticks waited. Missing thread/task is state -1. Records are emitted
after the polling loop, preserving f2-22's serial-output timing rule.
The original 1000-tick zombie wait and all result/handler assertions remain.

## Verification

Host verification:

- `ASAN_OPTIONS=detect_leaks=0 ... make test-host`: C harnesses and loader
  statics passed. Initial Python discovery found two absent prerequisites:
  `build/f0/files-payload.elf` and the SDK's extracted newlib source. These
  were supplied by the requested kernel build and reuse of the pinned SDK;
  no test or production workaround was introduced.
- Python discovery after restoring those inputs: 225 tests, OK, 10 skips.
- Focused `test_signal_fault_records.py`: 8 tests, OK, with ASan/UBSan.
  Production controller/predicate, `sched_tick()` and release injection are
  compiled unchanged into the harness; payload/syscall completion is modeled.
  Every emitted sample record fits the 240-byte limit and is parsed by the
  normal evidence parser. Missing/failed injections retain FAIL and timeout
  diagnostics for all three modes.
- `scripts/build_kernel.py`: PASS, ELF link and FPU/SIMD audit;
  `VMM.ELF` 1,571,912 bytes, build identity `74774528a448-dirty`.
- Lua/luac build with `--jobs 2` (make `-j2`), and sequential desktop/demo
  build: PASS, existing ELF/SDK and integer-compositor/portrait checks.
- The NASM signal payload and its C host harness are unchanged. The harness
  reports 5709 signal checks, zero pages/threads/zombies at teardown, and
  successful ELF validation/loading; it does not execute the ring-3 payload.

No QEMU, systemd scope, image build or hardware run was performed. The lead
must still qualify `signals-fault` on qemu-t23, qemu-e500 and qemu-min128,
then obtain T23 evidence with entries=1, returns=1, result=-4 and saved/resumed
EAX=fffffffc on the exact integrated image.
