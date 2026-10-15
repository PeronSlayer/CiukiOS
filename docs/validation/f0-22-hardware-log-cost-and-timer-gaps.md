# F0-22 log cost and interrupt timing

Research checked on 2026-10-15 before implementation:

- [FatFs preallocation](https://elm-chan.org/fsw/ff/doc/expand.html) allocates
  storage before timed writes. CiukiOS `fat_write` instead invokes `replace`
  for every write, copying the entire file, replacing its chain and issuing
  metadata barriers. The boot log needs an explicit reserved-chain writer;
  ordinary file writes retain their existing semantics.
- [FatFs synchronization](https://elm-chan.org/fsw/ff/doc/sync.html) describes
  periodic synchronization for persistent log files. Reserve the 1 MiB log
  at activation, publish its logical length every five seconds and at shutdown.
  Explicit durability watermarks also publish before completing. Publish data
  before its directory length; release unused reservation clusters at shutdown.
  A crash may retain an allocated tail beyond the last published length. Both
  repository ownership scanners follow that tail; external checker behaviour
  still needs qualification by the lead.
- [Ring buffer writer ordering](https://docs.kernel.org/trace/ring-buffer-design.html)
  requires publication and consumption to respect nested writers. Every UART
  producer uses the existing whole-record queue. A user console envelope must
  also be assembled before publication, including its terminating newline.
- [TI TL16C550D](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf) specifies a
  16-byte transmit FIFO and distinguishes THRE from TEMT. Keep the existing
  verified FIFO gate and panic drain. Normal producers never write UART data.
- [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html) provides
  the existing test-selector transport. A separate diagnostic boot flag is
  enabled by an accepted fw_cfg selector. The screen-log menu choice does not
  enable high-volume diagnostics. Selector text and suite predicates stay intact.
- [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  define IF as the maskable-interrupt gate. Disk operations require IF set;
  accounting snapshots must copy bounded state with IF clear and sort/format
  with IF set. Record the longest section's label and address for attribution.

The T23 evidence is the read-only a738204a capture, disk lines 2812–3090.
It reports 247–269 ms of drain CPU time per approximately two-second window,
storage shares of 125–146 permille, and a cumulative 4230 us critical maximum.
That capture has no critical-section address, so it cannot identify which
section produced that maximum. ATA PIO data transfers and console rasterization
already run with IF set; the sampler currently sorts all tasks under one IRQ
lock. Shorten that lock and retain address attribution for every other section.

The same capture reports 523260 KiB usable, 2442 free DMA pages and 126816
free normal pages at boot. Increasing the runtime queue from 8 to 64 KiB and
the staging buffer from 4 to 32 KiB costs 84 KiB of static storage. No physical
memory limit or cache budget changes; small counters and reservation fields
are additional. Normal drains write aligned 32 KiB blocks,
preserving the preceding bytes and clearing the unwritten block suffix. The
logical length counts captured bytes only. Prefix and runtime blocks never
overlap. Ordinary drains run at most once per second; high water, explicit
durability requests and shutdown may run sooner.

The storage task checks work every 10 ms instead of every 1 ms. The existing
CPU/path/drop/gap counters remain, with additional log write, physical byte and
metadata totals. Timing snapshots include a static section name and return
address; sorting task samples runs with IF set. Shutdown owns the drain token
through final publication and close, so the worker cannot race tail release.
User console writes assemble one bounded 2048-byte envelope before entering the
serial queue; input with many line endings may return a positive short count.

Host checks and their exact output are recorded in `build/codex/f0-22-report.md`.
They do not qualify the image or prove hardware timing targets. The lead must
run the named QEMU suites and repeat the graphics workload on the T23 with the
same counters, inspect section addresses, verify shutdown and crash recovery,
and check the complete serial records.
