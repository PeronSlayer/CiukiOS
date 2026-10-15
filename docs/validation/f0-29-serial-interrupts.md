# F0-29 interrupt-driven serial transmission

Research checked on 2026-10-15, before implementation:

- [TI PC16550D, sections 8.4.1 and 8.6, vendor datasheet mirrored by DigiKey](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/PC16550D.pdf).
  Confirms the interrupt identification/acknowledgement, FIFO refill limit,
  and THRE/TEMT distinction used below.
- [TI TL16C550D datasheet, FIFO interrupt operation and register descriptions,
  pp. 33–39](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf).
  IER bit 1 enables THRE; IIR low nibble 2 identifies it and reading IIR
  acknowledges it. FCR bit 0 enables both FIFOs; a verified transmitter FIFO
  accepts at most 16 bytes per empty interrupt. LSR THRE means FIFO empty,
  whereas TEMT additionally requires an empty shift register. Use THRE for
  refills and TEMT only for the bounded final panic flush. Retain the existing
  reversible scratch/FIFO probe and divisor 3, 38400 8N1 default.
- [QEMU UART source](https://github.com/qemu/qemu/blob/master/hw/char/serial.c),
  `serial_update_irq`, `serial_ioport_read`, `serial_ioport_write`, `serial_xmit`.
  IIR acknowledges the transmit interrupt; a rising IER THRE-enable edge can
  restart an idle transmitter. Immediate chardev forwarding needs no quota,
  pacing or CPUID exception. Each IRQ services one FIFO, then returns.
- [Linux wait-queue documentation](https://docs.kernel.org/driver-api/basics.html#wait-queues-and-wake-events)
  and this tree's `core/sync.c`: register the waiter and recheck capacity under
  the IRQ lock before blocking. Wake evidence producers after the IRQ frees
  queue space; allocate record sequence numbers only after any wait.
- [Linux 8250 upstream driver](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_port.c),
  startup and IRQ probing: PC-style IRQ operation asserts MCR OUT2 along with
  DTR/RTS. The previous polling setup used only DTR/RTS; enable OUT2.

Repository checks: `arch/trap.c` installs a single handler per legacy IRQ
with `irq_set_handler`; the dispatcher alone issues PIC EOI, also for V86
and DPMI frames. `arch/pic_pit.c` starts with every PIC line masked. UART
initialization precedes PIC/registry/scheduler initialization. Register COM1/3
on IRQ4 and COM2/4 on IRQ3 at the existing `serial_writer_init` startup hook,
reserve the corresponding exclusive IRQ in the resource registry, then
unmask it. Unsupported port-to-IRQ mappings disable the optional serial sink.
No receive interrupt is enabled and no receive data register is consumed.

Decision: preserve the 64 KiB byte ring and 1024 descriptors. Reserve 8192
wire bytes and 64 descriptors for protected evidence; ordinary/periodic
publications drop whole at that boundary. Evidence can use the entire queue,
and preemptible task producers wait on the existing `kwait` service when full.
Early/IRQ/held-lock producers cannot sleep and count whole evidence drops.
No record already accepted is evicted or retried.
Remove generated drop-notice records along with their quota machinery:
the hardware page and existing kernel summaries expose counters without
introducing formatting or extra publications in the TX interrupt.

Producers perform no UART I/O. The TX handler disables IER THRE when idle.
The PIT calls `serial_tick` only to invoke the handler's idle-rearm entry when
queued data needs a new IER edge: this performs one control write, no readiness
read and no data transfer. Actual FIFO loads occur only on identified UART
THRE interrupts. Active transmission has no timer pacing or writer task.
Initialization and panic are the only exceptions to normal IRQ-owned UART I/O.
Panic disables UART interrupts and drains the persistent unsent cursor with
finite polling budgets, then waits boundedly for TEMT; it never resets a FIFO.

The read-only a395ffa8 capture contains duplicated/truncated serial chunks
(including `serial recorcord=` and `serierial`) and a 4229 us maximum at
`c0122ff5` in the directive's matching kernel. It cannot establish whether
capture loss or physical UART behavior caused those bytes. The new `resent`
statistic counts software retries (zero by construction), not remote receipt;
the page continues to distinguish FIFO submission from wire delivery.

Host results and verbatim command output are in `build/codex/f0-29-report.md`.
The cost model charges 1 us per port access and 1 us per 64 wire bytes copied;
it is deterministic latency evidence for the bounded code paths, not a T23
measurement. Existing production timing attribution is unchanged.
The full kernel host suite passed. The model's maximum queue section was
65 us and its maximum TX IRQ was 19 us; the actual H-page function submitted
all 100 records with zero whole-record drops or resent bytes in both UART modes, followed
by an exact comparison of every physical-model wire line. The kernel built
successfully and passed the FPU/SIMD audit.
QEMU f0-smoke/f1-usb/f3-vm and the next T23 capture remain lead validation.
