# f1-09 storage integration — research and validation

Implementation scope: `docs/directives/f1-09-storage-bringup.md`. No QEMU,
image build, systemd scope, commit or push is part of this implementation.
The lead owns integration, the Italian diary entry and guest evidence.

## Primary sources and decisions

Read before implementation on 2026-10-10:

- Linux v6.12, [rtc-mc146818-lib.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c):
  serialize CMOS index/data, sample seconds before UIP, check UIP and seconds
  after reading the fields, bound retries. The new provider implements these
  checks independently, including register-B consistency across the sample.
- Linux, [MC146818 register definitions](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/mc146818rtc.h)
  and [x86 access policy](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/mc146818rtc.h):
  BCD/binary, 12/24-hour and PM decoding. Port 70h's NMI policy comes from
  software, never a readback. Ciuki's sole native accessor keeps the current
  enabled-NMI policy (shadow zero), serializes each complete sample with
  irq_save, never writes port 71h, never reads register C and does not unmask
  IRQ8. The V86 monitor's CMOS registers are virtual, not another native reader.
- Microsoft, [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
  section 6.3 and directory-entry creation-fraction field;
  [DOS date/time packing](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-dosdatetimetofiletime):
  store calendar date and time, two-second write-time granularity, year offset
  from 1980. An odd second contributes 100 to the creation hundredths field.
  No timezone conversion or fractional RTC precision is invented.

The frozen boot handoff does not identify a century register. The provider
does not read an assumed CMOS 32h location; its explicit pivot maps 80–99 to
1980–1999 and 00–79 to 2000–2079. Invalid dates/BCD/VRT, SET, failed pacing,
100 ms timeout or the 1,024-attempt stalled-clock escape yield the existing
1980-01-01 fallback. RTC hardware timing still needs guest/physical evidence.

## Integration fixes

1. `fat_enable_write`: upgrades an already mounted/scanned read-only volume
   without remounting or discarding live VFS references. Only REQUEST and a
   now-resolved DURABILITY reason can be cleared; dirty/error/copy/ownership
   reasons remain prohibitions. FAT16/32 dirty marking is durable before success.
2. Cache refresh callback: consults `blkpart_device` before cache hits,
   admission, delayed writes, eviction, barriers and error checks. A channel's
   sibling quarantine therefore cannot be hidden by a stale scalar snapshot.
   No changes to the frozen `blkdev` or VFS interface were made.
3. Cooperative cache/scan servicing: long task-context loops yield with IF=1;
   storage callbacks use `kwork_yield`. The namespace→cache→device lock order
   remains unchanged. Cache due-time metadata avoids an 8 MiB traversal on
   every timer tick. A one-tick timer submits work only when a deadline/log
   batch is pending. Missing worker/timer allocation keeps ordinary mounts RO.
   Actual five-second deadline latency under guest scheduling is not yet measured.
4. FAT LFN observation counters expose orphan, bad-checksum and malformed-name
   fallback for read evidence. These count read observations, including repeated
   lookups; they are not claimed to be unique on-disk entry counts.
5. The host kernel-object helper permits the already-required stack protector
   symbols and compiles mount, bootlog and probes using the builder's exact flags.
   Its `.su` files retain target stack-frame evidence; host ASan is not kernel
   guard-page qualification.

## Mount and logging behavior

One 8 MiB system cache serves all live partition views. IDE slot numbers remain
stable when devices are missing. Only primary MBR entry 1 of slot 0 becomes C:;
other FAT partitions start at D:, in enumeration order. All begin read-only;
mount, scan, durability and write-gate state and reasons are observable. Invalid
partitions/mounts stay recorded. No protective GPT entry is interpreted as FAT.

All test selections suppress automatic writable mounts, the disk logger and
the background timer. Write probes explicitly open their gate. Ordinary boot
starts its cache writer on the device worker. Storage shutdown preflights every
mount and open node before any clean flag; a delayed error stays visible and
prevents clean unmount. A running logger returns EBUSY instead of racing shutdown.

`bootlog_capture` is an allocation-free RAM producer. It retains 8 KiB, counts
dropped bytes, and does no storage work. `bootlog_activate` requires a qualified
writable C:. A worker drains at most 4 KiB at a time to `C:/SYSTEM/BOOT.LOG`,
truncating durably when the next batch would exceed 64 KiB. Reopen appends;
shutdown drains, commits and closes before volume unmount. Errors stop the sink
without printing, retrying a failed disk or recursively logging. Qualification
and first payload-write sequences belong to the storage/log lifecycle, not the
global CIUKI_TEST record sequence. Panic functions are unchanged.

**Output integration still needs the lead:** the directive excludes
`src/kernel/core/output.c`, whose ordinary `klog` and `rec_emit` paths have no
append callback. The implementer requested authorization for the small hook;
without it, this change exposes the RAM producer but cannot intercept existing
activation records or klog lines. The real `bootlog` probe checks klog capture
and fails explicitly with EOPNOTSUPP when the hook is absent. Host tests supply
the proposed ordinary-output boundary; they do not prove that production output
is connected. The required hook must append only ordinary formatted lines,
never `kputs_raw`, `rec_emit_panicsafe` or panic output.

## Probe/runner handoff

Every probe is registered with `CIUKI_F1_PROBE` and emits BEGIN/DATA/END;
external reboot/cut synchronization uses ARM. No selector grammar is extended.
Missing fixture evidence is reported, never synthesized as an observation.
The runner must aggregate coverage and its independent checks across boots.

- `fat-read`: boot SYSTEM and recursively enumerated roots on every mounted
  FAT partition, including MBR-wrapped FAT12/16 disks at IDE slots 1/2. Missing
  disks are reported. Name records carry `id`, `field=path|alias`, byte offset
  and up to 40 UTF-8 bytes as hex. File records carry size and SHA-256. List
  digest input, in disk order: path NUL, alias NUL, size LE32, attribute byte,
  content SHA-256 (32 zero bytes for directories). Dot entries are excluded.
  Explicit bounds: 32 directory levels, 16,384 entries, existing path limit.
  Cluster-boundary reads, EOF and invalid-name failures are also measured.
- `fat-write`: new `C:/F109` only; never erases an interrupted/existing workload.
  Seed `0x00c1a009`, workload `zero_patch_v1`: zero-extend to 0 bytes, one
  cluster and 4 MiB; read all bytes; overwrite the first min(size,64) bytes
  with `(i*37+seed)&255`; verify; halve size; verify; rename; verify; delete;
  recreate the original-size patched files. This uses bounded RAM with the
  existing whole-file COW implementation. GROW contains colliding long names
  and extends beyond one cluster. DONE.BIN is a versioned manifest of all
  three sizes/digests, committed last. After successful durable unmount ARM
  requests reboot on the same overlay. Its next boot performs read-only
  manifest/content verification without repeating the writes.
- `cache`: isolated small synthetic cache forces eviction and records writes,
  reverse-order persistence and barrier IDs. The timed hook, shared/duplicate
  handles, absent flush with caching enabled, delayed writes/flush failures and
  failed unmount are checked. Additional synthetic ATA channels inject errors
  at the actual production register boundary; quarantine and zero subsequent/
  real commands are measured. No real controller is rearmed or faulted.
- `mount-crash`: runner-prepared corrupted nonboot MBR partitions provide bad
  BPB, dirty/error, mirror and chain cases. The probe reports refusal/reasons,
  ownership-scan outcome and write counters; it never upgrades those fixtures.
  For a crash workload the runner creates empty `C:/F109CUT.ARM` on its clean
  overlay. After ARM the probe creates/patches an 8 KiB `F109CUT.BIN` and renames
  it to `F109END.BIN`. Each completed sector write/flush has a cut index and
  barrier identifier; successful flush records commit the preceding trace
  prefix. Cut at a declared index and reboot the same overlay. The subsequent
  dirty/corrupt mount stays RO and reports its bounded ownership scan. A missed
  cut is an explicit failure; fsck/mtools and durable-sector-set reconstruction
  are the runner's responsibility, not guest assertions.
- `bootlog`: checks zero prequalification writes/calls and ordinary klog capture,
  persists a known line, durably closes, hashes the result and stores a versioned
  digest/size manifest in F109LOG.OK. ARM requests reboot; next boot compares
  the actual log with that manifest, with no log writes. Read-only cases report
  unavailable. F0 panic is a separate required boot; no panic PASS is invented.

## Contract/integration problems for the lead

1. f1-09 says `SYSTEM/BOOT.LOG`, 64 KiB; f1-acceptance says
   `SYSTEM/LOGS/BOOT.LOG`, 128 KiB. The implementation follows f1-09.
2. CBI1 provides no loader sector fingerprints. f1-09's fixed slot-0/partition-1
   mapping is implemented, with `qualified=0 reason=loader_fingerprints_absent`
   evidence. It does not satisfy f1-acceptance's stronger identity binding.
   Loader/handoff work is outside scope; normal writable enablement currently
   follows the directive's fixed mapping, pending the lead's decision.
3. Ordinary output capture is not connected within the allowed files (above).
4. The builder's explicit `f1_order` omits `probes_fat_probes`, so its current
   ELF placement precedes the old F1 entries. The lead must add it after the
   ATA probe object and before safe. Individual QEMU selectors are unaffected.
   Storage write probes end with durable shutdown; a photographed `f1:all`
   sequence also needs orchestration/remount between those probes. The current
   dispatcher/builder are outside f1-09's allowed edits.
5. Existing f1-01 limits remain: impossible universal torn-sector detection,
   additional classified crash states, same-directory long-rename ENOSPC,
   imported FAT12 split directory tails, active-only FAT32 RO, bounded scan
   workspace and whole-file COW spare-space cost. See REPORT.md/VALIDATION.md.

## Evidence location

Final commands, statuses and concise results are recorded in STORAGE-REPORT.md.
Full command output is retained under `build/host/storage/`. The filesystem
suite also retains its pre-existing bounded crash classification evidence.
No guest execution or real RTC/ATA hardware qualification is claimed.
