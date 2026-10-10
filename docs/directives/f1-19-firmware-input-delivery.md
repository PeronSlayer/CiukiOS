# Directive f1-19: firmware-first input delivers no events on QEMU

- **Step:** F1. **Contracts:** `f1-acceptance.md` (`input` on the
  firmware-first profile: 100 text characters, 200 key transitions, 20
  button transitions, x=200 y=-100), `device-firmware-ownership.md`
  (firmware IRQ reflection through virtual vectors, the lease), the f1-07,
  f1-07b, f1-15 and f1-18 reports.
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`.
- **Worktree:** `wt/f1-fwdelivery`. Files: `src/kernel/vm/*.c`, `*.asm`,
  their headers, `src/kernel/drivers/fwinput_adapter.c`,
  `src/kernel/drivers/i8042_probe.c` (the `input` probe's firmware branch
  only), `src/kernel/arch/trap.c` only for IRQ reflection accounting,
  `tests/host/v86_test.c`, `tests/host/fwqueue_test.c`,
  `scripts/test/host_kernel_tests.sh`.

## Observed on image `aee7f539…` (commit `408d056`, `f1-input` case `input-qemu-e500`)

Setup succeeds (`[biosvm] step=ready leases=4 qemu_pmtimer=1`, `[fwinput]
step=setup_done keyboard=1 mouse=1`), the probe emits `READY backend=firmware`
and `ARM keys=100 moves=100 buttons=10`, the runner sends 220 QMP
`input-send-event` batches, and nothing follows: no DATA record, no klog, the
host deadline (120 s) expires. The native profile passes the same case. The
`safe` probe on the firmware-first profile passes (console and backend state
only; no stimulus).

## What to do

1. Instrument the delivery path with bounded klog counters printed on a
   10 s timer while the `input` probe waits: physical IRQ1/IRQ12 arrivals
   on the host dispatcher, reflections queued, VM entries for reflection,
   firmware ISR completions (IRET to the sentinel), BDA head/tail values,
   observed port-60h bytes, mouse ring producer index, adapter wakes and
   drains, events queued. The first run on QEMU must show where the chain
   breaks.
2. Reproduce on the host as far as the fakes allow: a fake physical IRQ1
   with a scripted firmware ISR that reads port 60h through the trapped
   `IN` and writes the BDA; prove that the adapter observes it and queues a
   key event.
3. Likely suspects to check first: the physical IRQ1/IRQ12 handlers under
   the firmware lease (are they installed, is the PIC line unmasked, is the
   EOI issued by the dispatcher), the virtual PIC's mask bits as left by
   the SeaBIOS ISR (`OUT 21h`), the adapter's pending check (f1-18) reading
   the BDA at the right physical addresses (0x41A/0x41C) and the mouse ring
   in the scratch page, and the reflection of IRQ12 to vector 0x74 through
   the slave virtual PIC.
4. Fix the cause; keep every f1-18 guarantee (no VM entry without pending
   data, no P_DEVICE spin).

## Acceptance by the lead

Host tests; kernel build; on QEMU `f1-input`'s `input-qemu-e500` passes
with the QMP stimulus (`backend=firmware`, counts as the contract), and
`input-fault`'s firmware subcases report their real outcome. Reply with:
root cause, files, test output, and any contract problem found.
