# Directive f1-18: the firmware input queue thread starves ring-3 tasks

- **Step:** F1. **Contracts:** `execution-abi.md` (fixed priorities,
  `P_DEVICE` reserved for short device work; anti-starvation boost),
  `device-firmware-ownership.md` (latency budgets; firmware calls are not
  real-time), `f1-acceptance.md` (`input` on the firmware-first profile),
  the f1-07 directive ("after each reflected IRQ1 and on a 10 ms poll").
- **Implementer:** Codex, `gpt-6.1-sol`, effort `high`.
- **Worktree:** `wt/f1-fwqueue`. Files: `src/kernel/drivers/fwinput_adapter.c`,
  `src/kernel/vm/fwinput.c` and `biosvm.c` (only to expose a cheap "data
  pending" check and the IRQ1/IRQ12 reflection count), their headers,
  `tests/host/runtime_init_test.c` or a new `tests/host/fwqueue_test.c`,
  `scripts/test/host_kernel_tests.sh`.

## Observed on image `f76ab59e…` (commit `5ccc80fd`, f1-15 merged)

On `qemu-e500` the firmware backend now initialises (`[fwinput] step=setup_done
… keyboard=1 mouse=1`) and the F0 `preempt` probe fails:
`switches=0 timer_switches=0 dispatch_a=0 dispatch_b=0 … ticks=120020` —
the two ring-3 probes were never dispatched in 120 s. `fwinput_adapter.c`
runs a `P_DEVICE` thread that loops `task_sleep_ms(1)` and polls the
firmware every millisecond; each poll enters the BIOS VM (INT 16h), which
under TCG costs more than a tick, so the top-priority thread owns the CPU
and `P_NORMAL` tasks starve. The native profile (no firmware thread) passes.

## What to do

1. The adapter must not enter the VM unless there is work: check the BDA
   keyboard buffer head/tail (mapped page, no VM entry) and the mouse
   callback ring's producer index, and the count of reflected IRQ1/IRQ12
   since the last drain; poll at 10 ms at most and only when one of those
   says data is pending; after a reflected IRQ wake immediately (`kwait`
   on a queue the reflection path signals).
2. Run the adapter on the device worker (`kwork`) or as a `P_INTERACTIVE`
   thread that blocks between polls (`kwait_wait_until` with the 10 ms
   deadline), never as a spinning `P_DEVICE` loop; keep each step bounded
   (≤16 events, ≤1 ms budget, counted violations).
3. Host test: a fake time source and fake fwinput; prove that with no
   pending data the adapter enters the VM zero times in 1 s of fake time,
   that a reflected IRQ wakes it within one poll period, and that a
   `P_NORMAL` task gets dispatched between adapter steps in the fake
   scheduler; keep the input probe counts unchanged.

## Acceptance by the lead

Host tests; kernel build; on QEMU `qemu-e500`: `f0-core` passes
(including `preempt`), `f1:input … platform=e500` passes with the QMP
stimulus and `backend=firmware`. Reply with: files, test output, and any
contract problem found.
