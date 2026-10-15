# F0-24 serial records and hardware latency

Research checked on 2026-10-15 before implementation:

- [TI TL16C550D, sections on FIFO operation and LSR bits 5–6](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf)
  specifies 16 transmit FIFO bytes, THRE for an empty FIFO, and TEMT for
  an empty FIFO and shift register. At 38400 baud, 8N1 carries 3840 bytes/s.
  Keep the verified FIFO probe, but use one normal consumer, a persistent
  record cursor and a conservative byte quota. A FIFO refill must never
  advance into another record or abandon an already started record.
- [Intel architecture manuals, interrupt handling and IF](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  describe the maskable interrupt gate. A normal UART readiness check and
  repeated refills must not share a long interrupts-off interval. Keep only
  a single output byte and its cursor update atomic against panic takeover.
- [Kernel delay documentation](https://www.kernel.org/doc/html/latest/timers/delay_sleep_functions.html)
  distinguishes atomic busy waits from sleeping waits. The existing PS/2
  data pacing samples a calibrated TSC for 7 us with a 256-sample escape;
  status reads do not wait. Do not change that controller timing on the
  basis of a UART address.

The read-only image 5e018550 serial capture has two boots and missing chunks
of records. The matching kernel ELF in the lead's build directory embeds
build ID `763eefa66b76`, also present in the disk log. `addr2line` resolves
`c011e445` to `serial_writer`, after its inner-loop `crit_begin`;
`c013533d` resolves to `read_port`. The recurring 2120 us maximum is attributed
to the UART writer. Both the task and timer currently refill the UART, with
up to sixteen port writes under one IRQ lock. The capture alone cannot
distinguish transmit loss from receive/capture loss. Do not claim to have
proved either cause of the missing wire bytes from this evidence.

The disk text between USB ticks 7205 and 7870 contains 153 records and
13196 CRLF-expanded wire bytes; between ticks 8058 and 8309, 105 records
and 10501 wire bytes. Those require at least 3.44 s and 2.73 s respectively
on a 38400 8N1 line. The reported tick intervals are 665 ms and 251 ms;
timer loss means they are not precise wall-clock measurements, but these
are substantial bursts above the line's capacity.

The normal log-writer is now the only drain context. Its creation is
idempotent; the timer compatibility entry does no I/O. Panic takes over
with IF clear. A 1024-entry length ring accompanies the existing byte ring;
each refill stops at its current record's boundary. At divisor 3, admission
allows 2816 expanded data bytes and one notice of at most 64 bytes per
1000 PIT ticks. Idle windows do not bank credit. The wire is paced to at
most three bytes per tick and still requires THRE. Oversize, quota, byte
ring and descriptor exhaustion drop whole records and increment
`serial_drop`. Panic bypasses quota/pacing while retaining FIFO gating.
The additional descriptor storage is 2 KiB; the evidence reports 523260 KiB
usable RAM. No memory limit or existing queue size changes.

Selected evidence waits for space/quota only in preemptible task context,
before allocating its sequence number. Bounded formatting and queue
publication then run under one IRQ lock; console work runs after restoring
IF. IRQ and held-lock producers never wait. This keeps serial sequences
increasing even if a quota wait dispatches another evidence producer.
Normal UART readiness reads run with IF set. A separate short IRQ lock
protects each data byte and its cursor update against panic takeover.

The second-boot text log has `boot_flags=00001a08`: both verbose bits are
set. The loader's L menu choice currently enables `CBI_F_DIAG_VERBOSE`, so
the accelerator and USB guards are working as written. F3-26 explicitly
enabled this one-boot diagnostic selection for hardware VM cost attribution;
retain it. Accepted fw_cfg tests also enable high-volume diagnostics.
Kernel summaries are not gated on the diagnostic bit. They use
`bootlog_capture_ring`, whose disk region starts at 128 KiB,
after a potentially zero-filled gap. The supplied text contains no CPU,
kernel or USB periodic summaries. It cannot establish their absence from
the raw runtime region; inspect the entire file by its directory length,
including bytes after the first NUL.

Implementation and host results are recorded in `build/codex/f0-24-report.md`.
Hardware acceptance remains with the lead: readable complete serial lines,
`timer_gaps=0/…` and `irqoff_max_us` below 500 in the raw disk log. Resolve
any remaining attribution address against the exact ELF used by that image.
