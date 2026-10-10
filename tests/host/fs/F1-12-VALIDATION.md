# f1-12 integration research and validation

Primary sources checked against the production code on 2026-10-10:

- Linux kernel [input event codes](https://docs.kernel.org/input/event-codes.html),
  EV_KEY and SYN_DROPPED: repeated makes have value 2, and loss requires an
  explicit state-recovery boundary. Ciuki uses the frozen F2 policy rather than
  evdev's ioctl recovery: the producer discards stale records, queues one RESYNC
  carrying cumulative loss and the current button bitmap, then preserves fresh
  events. The bridge forwards the event's snapshot, not later queue statistics.
- [SeaBIOS 1.16.3 keyboard source](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c):
  firmware characters and observed scan transitions are separate. Native repeats
  emit a repeat KEY and normal TEXT; firmware repeat KEYs do not generate a
  second TEXT. Set-2 to public set-1 normalization is already integrated.
- Microsoft [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
  directory and clean-shutdown rules: use the existing FAT directory creation,
  cache and durability paths under the namespace lock, after the write gate.
  The canonical image supplies SYSTEM; mount creates LOGS if absent and rejects
  a regular file in its place. The logger writes C:/SYSTEM/LOGS/BOOT.LOG with
  a 128 KiB truncation limit through the existing VFS implementation.

The binding, queue capacity, log limit and probe order are Ciuki contract
decisions, not thresholds derived from these upstream sources. CBI1's accepted
F1 disk-0/partition-1 binding still reports
`qualified=0 reason=loader_fingerprints_absent`; loader fingerprints remain a
boot-info v2 requirement.

Host tests cover exact overflow boundaries, multiple loss episodes, retained
fresh events, sequence/source/generation/timestamp, event-time button snapshots,
native and firmware repeats, F2 forwarding, log directory gating and durable
128 KiB truncation/reopen/append. Host execution does not qualify IRQ latency,
ring-3 payload execution, QEMU or physical hardware.

The complete command output, final linker-map order and remaining contract
problems are recorded in build/f1-12/REPORT.md.
