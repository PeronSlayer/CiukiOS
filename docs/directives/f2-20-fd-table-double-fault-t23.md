# Directive f2-20: `fd-table` double-faults on the ThinkPad T23 (kernel stack exhausted in the probe task)

- **Step:** F2. **Contracts:** `f2-acceptance.md` (`fd-table` row), the
  F0 kernel-core contract (8 KiB kernel stack per task with a guard page
  below it), directive f2-17 (frame-size guard `-Wframe-larger-than`
  4 KiB kernel-wide, 1 KiB for the app probe and the supervisor; guarded
  8 KiB stack host regression), `hardware-baseline.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-stack-t23`, from `main` at or after the f1-29
  merge. Files: `src/kernel/core/task.c` and the task/stack headers,
  `src/kernel/arch/*.asm` only for an interrupt-stack change if chosen,
  `src/kernel/core/kheap.c` or `mm` only for the allocation of larger
  stacks, `src/kernel/probes/f2_probes_files.c` and `probes.c` (stack
  high-water record), `scripts/build_kernel.py` (guard bound only if the
  stack size changes), `tests/host/proc/*`, `tests/host/test_*.py`,
  `tests/host/record_scope_test.py`, one paragraph in the kernel-core
  contract if the stack size or an interrupt stack is introduced.

## Observed on the ThinkPad T23 (image `cb33aec3…`, commit `f7286ca`, unattended sweep `44444444`, boot 33, 2026-10-11)

The `fd-table` probe runs its kernel cases up to `inherit-cloexec` and
the machine double-faults with the stack pointer at the bottom of the
probe task's kernel stack:

```
… probe=fd-table event=DATA operation=inherit expected=0 observed=0
… probe=fd-table event=DATA operation=inherit-cloexec expected=-9 observed=-9
Ciuki VMM double fault: halting.
… probe=panic event=PANIC vector=8 error=00000000 eip=c01139a0 esp=f0006ff4 cr2=f0006ff0 why=double_fault
```

`ESP=f0006ff4` is four bytes above the guard page of the stack whose base
is `f0007000` (the same stack the f2-17 overflow hit at `f0007000`). On
QEMU (five profiles, `-icount`) the same probe passes through the whole
case list. The next operations after `inherit-cloexec` in the probe are
the spawn-based user cases (process creation with inherited fds, ELF
load from the FAT volume through the ATA PIO driver). On real hardware
the ATA and timer interrupts arrive while the probe task is deep in the
FAT/VFS/spawn call chain, and the interrupt frame plus the handler's
frames are pushed on the same 8 KiB stack; under `-icount` TCG the
interrupt timing differs and the depth never coincides. The frame guard
bounds single frames, not the sum of a deep chain plus an interrupt.

## What to do

1. Measure before changing: add a kernel stack high-water facility
   (pattern-fill at task creation, scan on demand) and emit
   `case=stack task=<name> size=<bytes> high_water=<bytes>` in the
   `fd-table` probe (and in the sweep's `SWEEP` records for every step if
   cheap), on the host harness and on QEMU, so hardware captures show
   the headroom. Report the deepest call chain you can identify from the
   fault address (`c01139a0`) in the current kernel map.
2. Fix with the smallest structural change that gives real headroom:
   either a larger kernel stack for the tasks that run probes and
   spawn/VFS paths (for example 16 KiB with the guard page kept), or a
   separate interrupt stack so handlers never consume task stacks. State
   the choice, its cost in pages, and keep the double-fault handler and
   the guard pages as they are.
3. Host tests: the high-water facility; a test that runs the `fd-table`
   kernel cases on a guarded stack of the chosen size with a simulated
   interrupt frame at the deepest point.

## Acceptance by the lead

Host tests; kernel build with the guard; QEMU `fd-table` on three
profiles and the `f2-all` batch unchanged; on the T23 the next sweep
passes `fd-table` and reports its stack high-water. Reply with: files,
the measured depth, the choice and its cost, test output.
