# Directive f1-00: kernel services for drivers

- **Step:** F1. **Contracts:** `docs/design/f1-acceptance.md` (activation,
  input, disk), `execution-abi.md` (threads, locking, deferred work),
  `device-firmware-ownership.md` (registry lifecycle, quarantine, latency
  budgets).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh` (kernel core).
- **Worktree:** `wt/f1-kernel-services`. Files: `src/kernel/core/sync.c`,
  `src/kernel/core/work.c`, `src/kernel/core/registry.c` (extend),
  `src/kernel/include/ciuki/sync.h`, `work.h`, `registry.h` (extend),
  `tests/host/kernel_sync_test.c`, `scripts/test/host_kernel_tests.sh`
  (add the new test). Do not touch probes, loader, runner or contracts.

## What to build

1. **Sleeping mutex and wait queue** (`sync.h`): `kmutex_init/lock/unlock`,
   `kwait_init/wait_until(cond, deadline_ticks)/wake_all`. A mutex is never
   taken in interrupt context; a thread blocking on a wait queue calls
   `schedule()` with the existing `T_BLOCKED` state and `wake_tick` for
   the deadline; waking is done from interrupt context by marking tasks
   ready (`rq_push` under `irq_save`). Lock ordering is documented in the
   header. Owner death while holding a mutex is a kernel bug (panic).
2. **Deferred work** (`work.h`): one kernel worker thread at `P_DEVICE`,
   `kwork_queue(fn, arg)` callable from interrupt handlers (bounded ring,
   loss is counted and reported), executed with interrupts enabled; the
   worker yields at least every 1 ms of work (contract budget).
3. **Generation handles**: `gen_t` (u32) allocated monotonically; helpers to
   validate a (slot, generation) pair, used by the registry and drivers to
   reject stale completions.
4. **Registry lifecycle** (`registry.h`): states
   `discovered/firmware-reserved → claimed → active → quiescing →
   released` plus `quarantined`; `registry_activate(handle, gen)`,
   `registry_quiesce(handle, gen, idle_proof_fn)`,
   `registry_release` only from `quiescing` with a successful idle proof;
   a failed proof moves to `quarantined` and keeps resources; conflicts
   refuse without side effects and name both owners; counters for claims,
   releases, conflicts and quarantines readable by probes. Shared
   PCI-IRQ chains: `irq_chain_add(irq, handler, owner)` where every handler
   returns `handled`/`not-mine`; the dispatcher visits all participants,
   masks the line while servicing, EOIs after the chain; a line that stays
   asserted with no owner claiming it after 1,000 passes is masked and
   quarantined with a counter (device contract, "Shared PCI INTx").
5. **Elapsed-time deadlines**: `deadline_after_ms(ms)` and
   `deadline_passed(d)` on `g_ticks`, plus a bounded `udelay(us)` based on
   the calibrated TSC (`timing.c`) for port-I/O pacing; `udelay` must never
   be called with interrupts disabled for more than 50 µs.

## Interfaces that must not change

`task.h` (states, priorities, `schedule`, `task_sleep_ms`), `cpu.h`
(`irq_save`/`irq_restore`), `timing.h`, the six F0 syscalls, the record
grammar. The registry's existing claims and the PCI inventory stay as
they are; new functions are added beside them.

## Host tests (mandatory)

`tests/host/kernel_sync_test.c` compiled with the existing
`scripts/test/host_kernel_tests.sh` under ASan/UBSan: the registry state
machine (every transition, conflict refusal without writes, stale
generation rejection, quarantine retention, 100 claim/release cycles
restoring counters), the IRQ chain model with a fake PIC (two owners,
removal of one, stuck source quarantine after 1,000 passes), the work
ring (overflow counted), and deadline arithmetic across the `g_ticks`
wrap. Scheduler-dependent parts (mutex sleeping, wake from IRQ) are
exercised by a new embedded probe `kservices` that Claude adds later; keep
their logic in small functions testable with a fake `schedule`.

## Acceptance by the lead

Diff review; host tests pass; kernel still builds with `-Werror` and the
FPU audit; the F0 `core` run on QEMU still passes; no change to the 240-byte
record discipline. Reply with: files, interfaces added (signatures), test
results, and any contract problem found.
