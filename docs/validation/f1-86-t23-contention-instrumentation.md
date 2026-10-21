# T23 contention instrumentation (f1-86)

This change adds diagnostic counters and records; it does not change admission,
transfer, scheduling or quarantine policy. Hardware evidence is still required.

Research and implementation decisions:

- [T13 ATA/ATAPI-6 draft 1410D revision 3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
  sections 8.25–8.26 and 9.7: count a DMA data command only after successful
  completion, not when UDMA mode is selected. FLUSH is a non-data command
  (8.12); it is not a PIO data completion.
- [Intel 82801CAM ICH3-M datasheet, 290716-001](https://datasheet.lcsc.com/datasheet/pdf/7d520883a0f21f1845e4892c5243848a.pdf?productCode=C3247608),
  section 5.15.2 and table 5-54: normal DMA completion requires the interrupt
  indication and an inactive bus master. The existing `command_data` path also
  checks ATA status and proves master stop before publishing success. Counters
  and first-completion evidence use that path; no new register access or retry
  is introduced.
- `src/kernel/core/output.c`, `log_line`: ordinary lines have 254 bytes of
  payload capacity. Periodic publications use reserved UART headroom and the
  protected disk ring. VM summaries and deferred IRQ-off reports use
  `klog_periodic`; no formatting or logging is added inside scheduler phases.

The implementer's exact commands and results are in
`build/codex/f1-86-report.md` (local build output).

## Reading the records

- `[storage] vfs_hold drive=C ms=N task=name#id op=write waiters=N` is
  emitted on release at 50 ms or longer. The hold includes every filesystem
  wait and repeated `VFS_IO` phase, until the mutation guard retires. `waiters`
  is the number of outstanding volume admissions/per-file waits at release.
  Tags: `write`, `truncate`, `create`, `rename`, `unlink`, `mkdir`, `log_write`,
  `log_reserve`, `commit`, `mount`, `other` (including metadata/read reservations).
- `[storage] vfs_wait` retains its 100 ms threshold and original fields, adding
  `task=name#id holder_task=name#id op=tag`. Volume holder snapshots are copied
  under namespace before sleeping; namespace waits use an atomic best-effort
  snapshot, which can become stale. Missing owners are `none#0`/`other`.
  Names are sanitized and bounded to 15 characters; request identity follows
  worker calldowns and uncancellable legacy reservation helpers.
- The existing `[storage] queue name=ata0 depth=...` record is followed by
  `[storage] queue name=ata0 unit=0 dma=N pio=N svc=A/B/C/D svc_max_ms=N`
  for each observed master/slave. Mode counts are successful ATA data commands
  (PIO includes ATAPI packets with data), not sectors or userspace bounces.
  FLUSH/failed commands do not increment mode counts. Service buckets cover
  dispatched lower callbacks, including failures/flushes, at `<2`, `2..<8`,
  `8..<32`, `>=32` ms; admission wait is excluded. Counts/maxima are cumulative
  per device lifetime. Pre-dispatch cancellations contribute no service sample.
  Queue snapshots retain values, not removable-device pointers. Only ATA
  supplies mode proof; zero counts on another adapter do not establish its mode.
- `[ata0] dma first-completion unit=0 elapsed_us=N` appears once per channel
  per boot after validated DMA success; it uses calibrated TSC microseconds,
  or milliseconds converted to microseconds without calibration. The existing
  quarantine record stays. `[ata0] dma fallback ... mode=pio` confirms that
  stop/idle proof permits future PIO commands; a blocked channel makes no such
  claim. The failed command is never replayed.
- Each due two-second VM sample emits `[dosvm] perf vm= gen= exits= gp= pf=
  db= timer= other= pm= busy_pm= idle_2f= idle_28= idle_16= idle_hlt=`, up to
  three `perf_port vm= port= exits= us=` lines ranked by cost, and one
  `perf_cost vm= cause= exits= us=` line for the costliest cause. All are deltas
  from the same underlying counters as disk records, without a probe/verbose flag.
  Serial windows are two seconds; the original disk sample/record families
  retain their independent five-second windows and port banks.
  `exits` sums GP/PF/DB/timer/other; `pm` overlaps them. `busy_pm` is CPU/wall
  permille, capped at 1000; idle values count hints. Costs exclude descheduled
  time. Empty windows emit a zero-cost cause and no stale ports. Publication
  runs with IF restored, guarded against reentry; the maximum-width summary
  is 239 bytes, within the logger's 254-byte payload budget.
- `[kernel] irqoff_long site= us= phase= phase_us= from= to=` publishes the
  longest section of at least 1 ms per cpustat window (about 2 s), including
  ordinary boots. Scheduler phases: `pick`, `fpu`, `address_space`, `ldt/debug`,
  `vm-enter`, `other`. `site` is the original IRQ-disable address; `phase_us`
  is the largest phase. If phases cover less than half the section, `phase=gap`
  reports unmeasured time instead. Seven nonserializing RDTSC marks per switch
  (three for a same-task dispatch) reuse the existing section boundary stamps.
  Capture does no division, formatting, allocation or logging; names are copied
  before task reclamation, and the cpustat task publishes the record. `gap`
  is a heuristic, not SMI proof: an unmaskable event inside a marked phase is
  charged to that phase, and a PIII has no SMI counter.

## Host coverage and limits

The real FAT/cache/VFS fixture uses a stalled fake device and two named owners
to check both records, exact 49/50 ms hold threshold, bounded names and calldown
identity. Block tests cover 2/8/32 ms boundaries, clock wrap, failures and unit
isolation. ATA tests cover completion proof and fallback; VM tests cover deltas,
ranking, empty windows, line size and bounded IRQ snapshot work; cpustat tests
exercise production scheduler marks, the 1 ms boundary and one report/window.
The lead owns full suites, image builds and hardware evidence. LeakSanitizer
cannot finish the ATA fixture in the ptrace-restricted implementer sandbox;
its settings were not changed. Other fixtures retain their repository runners'
explicit sanitizer settings. Host results do not qualify T23 throughput/firmware.

## Amendment 1

Hold and wait snapshots include the event's measured duration and bounded
owner fields. Admission waits follow their caller across reservation sleeps
and `VFS_IO` phases. Formatting, logging and observers run after namespace and
volume unlock and waiter wakeup, including failed admission. The existing
identity lock protects the diagnostic budget only; it is released before any
publication. No diagnostic lock or allocation is added.

Each drive admits at most eight holds and eight waits per two-second budget
window. At the next unlocked VFS boundary after rollover, a window with
suppression emits one protected summary:

```text
[storage] vfs_contention drive=C hold_suppressed=N wait_suppressed=N hold_max_ms=N wait_max_ms=N
```

Maxima include suppressed records. Counts saturate, rollover handles the
32-bit clock wrap, and an unknown drive (`-`) has its own bounded budget.
`vfs_wait` again uses `klog_diag`: it remains opt-in and verbose on disk,
preserving the pre-f1-86 protected-slot policy. Holds and suppression summaries
use `klog_periodic`. The real output/bootlog fixture, with the drain stalled
for five seconds, records four VMs plus contention in 249/256 slots and
29,079/65,536 bytes: 233 protected and 16 verbose slots, zero drops. This
finite test is not a guarantee for an indefinitely stalled sink or all drives
simultaneously contending. No slot/byte capacity was increased. Independent
serial port banks add 8 KiB per VM plus fixed snapshot baselines; no allocation
is performed by the reporter.

`schedule()` starts timing when entered with IF clear and no active section,
retains an existing outer section, and retires only its own section on a
same-task return. The switch path retains the existing suspend/resume timing
boundaries. Phase marks store only raw TSC values; sections below 1 ms do no
phase subtraction or ranking. Sections without marks do not clear the phase
array. Up to eight marks are retained; overflow leaves its tail unmeasured
rather than assigning it to a stale phase. The ATA first-completion timestamp
is read only for DMA-enabled devices before their channel's first completion.

Both queue-report paths publish dirty completion counters at the next normal
five-second report even if the queue has become idle. A clean idle queue does
not repeat the snapshot. The target-object check retains the kernel's exact
CFLAGS and 4 KiB frame guard (VFS unlock 1,440 bytes, diagnostic publication
932 bytes, VM reporter 1,480 bytes); it does not link or build an image.

Additional host evidence covers namespace/volume timeout publication, logger
reentry, per-drive budget isolation and clock wrap, independent two/five-second
VM cadence with concurrent producers, real scheduler/timing/cpustat hooks,
raw phase overflow, calibrated PIO timestamp avoidance and idle queue reporting.
Optional P3 namespace generation validation is deferred: namespace owner fields
retain the documented atomic best-effort snapshot semantics.

Amendment 1 item 7 preserves the cumulative idle API counters: neither reporter
resets blocks, reasons or per-reason counts. UART and disk deltas use their own
previous snapshots. The periodic fixture checks both cadences and empty windows;
the existing `dosvm_speed_test.inc` idle-accounting assertion stays unchanged.
