# Boot log publication and throughput (f0-34)

## Research and decision, 2026-10-16

Linux's [printk documentation](https://docs.kernel.org/core-api/printk-basics.html)
distinguishes message priority from console visibility and recommends bounded
or rate-limited diagnostics. Its [dev-kmsg ABI](https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/dev-kmsg)
overwrites complete records, retaining sequence numbers to expose loss.
CiukiOS currently overwrites individual bytes irrespective of their producer;
its one-block drain leaves a backlog below high water waiting another second.
The T23 e8dfd0cd second boot reports 24,212 dropped bytes and no surviving
five-second vm_perf CPU/share/exits/port/vector/service rows. The counters
previously counted bytes, despite the directive's description as records.

Decision: classify publications explicitly at the output boundary, evict whole
bounded records, count loss by class, and continue pressure-driven draining in
a bounded storage turn. This is CiukiOS policy, not a claim that Linux protects
these same classes. Console/UART admission and the raw black box keep their
existing independent rules.

## Contract

- `bootlog_capture`: protected boot/event record; uses the immutable prefix
  while room remains. `bootlog_capture_ring`: protected recurring evidence,
  bypassing the prefix. This includes ordinary kernel two-second summaries,
  hardware-check output, storage repairs, and five-second disk-only vm_perf
  families (`rec_diag_disk` / `rec_diag_disk_bootlog`).
- `bootlog_capture_verbose`: droppable diagnostic, always in the runtime queue.
  All `rec_diag` rows, including the verbose VM stop-time dump, use this class.
  `klog_diag` is DIAG_VERBOSE-only and droppable. Event/failure `klog` calls
  retain their protected class; USB controller mode, port/device, quarantine,
  storage stages, CSW and reset output retain their content and gating.
- The runtime payload budget remains 64 KiB, in 256 fixed 256-byte slots.
  Short records consume one slot; descriptor pressure also counts as high
  water. A formatted record fits one slot. Bulk API input uses bounded
  256-byte slices. No allocation, VFS, queue compaction or unbounded search
  occurs with interrupts disabled. Intrusive chronological and diagnostic
  lists provide constant-time eviction. A partially drained record is pinned
  until its remainder is copied, so overflow never tears a formatted record.
- On pressure evict the oldest droppable record first. A droppable incoming
  record is rejected when only protected records remain. Protected-only
  saturation rejects the incoming record and is observable: finite RAM cannot
  guarantee lossless storage through an indefinitely stalled device.
  `dropped` remains bytes for compatibility, with `dropped_protected` and
  `dropped_verbose` byte counters and separate record counters for eviction
  and refusal. Existing prefix trimming when black-box replay is attached
  remains byte-counted (there are no descriptors for that prefix).
- A normal sparse drain keeps the one-second cadence. Reaching high water
  latches pressure until the backlog is empty; the storage task drains up to
  four aligned 32 KiB blocks or 10 ms elapsed per call, checked between blocks.
  One synchronous device operation may exceed that elapsed budget. Each block
  retains preemption opportunities and the next storage turn retries pressure
  without a one-second delay. Raw black-box flush precedes FAT logging and
  cache writeback follows it, as in f0-25. No storage work inherits an IRQ lock.
- Sync requests snapshot both prefix and runtime stream watermarks. Removing
  newer diagnostics cannot satisfy an older protected record's watermark.
  A completed request publishes/commits before servicing a newer watermark;
  later producers cannot extend a caller's captured target.
- USB schedule/poll/latency and worker-rate details are DIAG_VERBOSE-only and
  droppable. VM audio periodic diagnostics follow the same rule; discovery,
  state transitions and IRQ-wait events keep their existing publication.
  The periodic producer is `vm/dosvm_sb16.c`; `audio/vm_source.c` only supplies
  diagnostic snapshots and has no `[vm-audio]` output to throttle.

Host models exercise both 4,000,000 and 300,000 bytes/s devices, complete
protected records under diagnostic pressure, watermark ordering and bounded
storage turns. Host evidence does not qualify throughput on physical hardware.
The FAT layout remains three 320 KiB files with a 128 KiB prefix each and the
existing 1 MiB total reservation budget. Runtime disk rotation can retire older
persisted evidence; this contract protects admission through the RAM queue.
