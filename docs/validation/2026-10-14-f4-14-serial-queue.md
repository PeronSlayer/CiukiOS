# F4-14 serial record integrity

Research checked before implementation, 2026-10-14. This change implements
`docs/directives/f4-14-serial-queue-integrity.md`; T23 qualification belongs
to the lead.

## Sources and implementation decision

- [TI TL16C550D datasheet, FIFO description and LSR bits 5–6](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf):
  a 16550-compatible transmit FIFO holds 16 bytes. THRE indicates an empty
  transmit FIFO, whereas TEMT includes the shift register. Preserve the
  detected 16-byte/one-byte burst sizes and the timer's single readiness
  check. Serialize the readiness check, FIFO writes and queue-head updates.
- [Linux ring buffer design, writer/reader preemption](https://docs.kernel.org/trace/ring-buffer-design.html):
  interrupt writers can nest inside an ordinary writer; publication and
  consumption must account for this ordering. CiukiOS uses a uniprocessor
  IRQ lock rather than adopting Linux's lockless algorithm. Publish each
  complete CRLF-expanded record under that lock, then let every sender
  consume only the published queue head under the same lock.

The existing `serial_write` already locks queue appends, but selects direct
polling whenever the caller's physical IF is clear. That also includes
ordinary IRQ/critical-section logs during buffered playback. Its direct
producer-buffer writes happen after restoring IF, allowing a second
producer or a timer burst to splice into the current record. The original
host test exercises only sequential producers and cannot detect this.

Use the fixed 8 KiB queue for synchronous records too. Polling drains release
the IRQ lock between readiness checks; each successful check and burst is
atomic. Playback producers enqueue without UART polls regardless of IF.
An explicit panic entry disables buffering and drains older records and the
pending drop notice before panic records. Panic does not infer its identity
from IF alone. Whole-record overflow retains the existing drop counter and
`[serial] result=log_dropped` notice. No allocation or timer formatting is
introduced. Emit the operator prompt and its newline in one serial call.

## Validation

The host UART model tests deterministic interrupt preemption and mode
transitions, concurrent producers with a burst sender, queue wrap/overflow,
panic ordering and absent/busy UARTs for both FIFO and non-FIFO devices.
Commands, before/after reproduction and results are recorded in
`build/codex/f4-14-report.md`. No QEMU or physical qualification is performed
in this worktree. The next T23 capture remains an acceptance requirement.
