# Disk Defragmenter and Drive Converter (G29b)

## G29D implementation contract (2026-10-24)

Research: the read-only Resource Kit `wrkc10_3.htm`, “Disk
Defragmenter”, describes recording application startup accesses and placing
program data in access order. `wrkc10_2.htm`, “Checks Made by FAT32 Drive
Converter Wizard” and the CVT switches, requires offline conversion or an
exclusive level-zero lock, checking and backup/compatibility warnings.
Microsoft's [Windows 98 SE General.txt, Q234856](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/234/856.HTM)
also ties launch optimisation to FAT32. The FAT specification linked below
defines the logical chain: physical placement may follow observed read order
while FAT links must continue to return the original logical file bytes.

Use a bounded, checksummed, volume-serial-bound private startup log. Record
actual executable/PE library snapshot reads, their first distinct logical
clusters, and use counts; do not infer an access trace from filenames.
The optional placement pass visits recorded files by use count and recent
observation, placing observed clusters first and preserving logical FAT
order. Unrecorded files retain ordinary defragmentation. Validate each log
identity against its directory entry before using it; corrupt/stale logs
are hints to discard, never disk addresses to trust. Logging failure must
not fail application launch. A full-file free extent remains necessary.

Boot-volume conversion must be requested durably while FAT16 is still
usable and run before publishing C: on the next boot. The native MBR's
loader lives outside the partition. Its FAT reader must accept both FAT16
and converted FAT32 geometries. During a committed converter transaction,
the loader must read the checksummed redo images as a read-only overlay;
only the kernel performs durable replay. Never enable this request for an
unrecognised loader. Invalid requests/journals fail closed without a panic.
A pre-commit refusal clears the validated request and continues with intact
FAT16; a committed locator remains mandatory recovery state. Conversion
does not create a foreign DOS/Windows boot sector.

## References and required behaviour

Microsoft's [FAT specification 1.03](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf),
sections “FAT Type Determination”, “FAT Data Structure”, “FAT Directory
Structure”, “FAT Volume Initialization”, and “FAT32 FSInfo Sector”, defines
cluster numbering, packed FAT12 entries, the FAT32 upper nibble, the
65,525-cluster boundary, both FATs, and the cluster-size table (page 20).
FAT32 uses 1 sector/cluster through 532,480 sectors, then 8 through
16,777,216 sectors. The table is a policy; cluster count still determines
the filesystem type. A geometry below the FAT32 boundary must be refused.

The owner's read-only Windows 98 Resource Kit, chapter 10,
`build/rskt/wrkc10_5.htm`, sections “Converting an Existing FAT16 Drive to a
FAT32 Drive”, “Defragmenting Disks”, and “Performance with Other Programs”,
requires one explicit drive, backup/compatibility warnings, error checking,
Start/Pause/Stop, summary/details and restart after competing writes.
Its application-start optimisation uses Task Monitor's actual startup I/O
profiles and usage counts. Merely sorting EXE names is not that feature.
There is no FAT32-to-FAT16 converter. Older DOS/Windows boot environments
and dual-boot installations may lose access after conversion.

## Defragmentation publication

Move an entire regular file to a free contiguous run. Do not modify its old
chain in place. Copy data and flush before allocating the replacement chain
in both FATs, flush that chain before publishing its first cluster in the
existing directory entry, flush the directory before reclaiming the old
chain. This follows the directive's copy / flush / FAT / flush / directory
order. A crash before directory publication leaves the original owned
chain; a crash afterwards leaves the complete replacement. The ordinary FAT
ownership check can reclaim the unowned chain and reconcile its partial
mirror writes. The operation never moves directories, including FAT32's
root; doing so also requires publishing dot/dot-dot and descendant identity
changes. Packed FAT12 writes preserve adjacent entries.

Analysis walks the actual directory tree and FAT, counts nonempty regular
files and files with nonconsecutive links, and reports the integer percentage
of fragmented files. The map aggregates free, allocated, fragmented,
directory and bad-cluster bits over bounded drive ranges. It is a summary,
not a false claim that one square is always one cluster.

A maintenance step reserves the selected VFS volume and refuses active file,
identity, pagefile and working-directory dependencies before moving data.
No open file can retain a stale first cluster. A mutation sequence carried
between steps detects other writers; the UI reports “Drive contents
changed... restarting” and repeats analysis. Pause/Stop occur between files;
no partially published move is cancelled by a UI action.

G29D enables the application-start setting. The bounded log records actual
image snapshot read clusters and per-image use counts; unrecorded files
retain the ordinary defragmentation path.

## Converter transaction

Conversion keeps file data sectors in place where possible and splits old
clusters according to Microsoft's FAT32 cluster-size table. Allocated old
clusters displaced by the larger FATs are copied into old free space. The
fixed root becomes a real cluster chain, every short directory entry and
`..` reference is translated, and both FAT32 copies, FSInfo and backup boot
records are generated. File slack beyond the last new cluster is released.

Before publishing any changed metadata, write a complete redo journal into
an old free contiguous extent and flush it. Each record contains a bounded
sector target, sequence and CRC; the journal excludes every target sector.
A checksummed locator in the old boot sector is then flushed, with an
invalid sector-size field to prevent ordinary/foreign FAT mounting. From that point
normal mounting is prohibited until idempotent replay finishes. Replay checks
all records before writing any, writes and flushes all metadata including the
partition type, and publishes the primary FAT32 boot sector last. The final
boot sector removes the locator. A restart before locator publication sees
the intact FAT16 filesystem; a restart afterwards replays the same journal.
Journal space becomes ordinary free space only after final publication.
Recovery must precede FAT parsing and automatic write activation. Primary
and extended/logical partition table entries are supported; all other table
bytes are checked against the live MBR/EBR before replay can change its type.

This assumes atomic 512-byte sector writes and truthful flush completion,
the same device contract as existing FAT publication. Torn-sector recovery
and devices that acknowledge flush without durability are outside that
contract. Refuse insufficient journal space, malformed/dirty ownership,
removable media and unsupported cluster geometry. The live boot volume uses
the deferred G29D path and is never converted during the requesting session.


## Verification and current limits

`nice -n 19 bash scripts/test/host_fs_tests.sh --defrag` checks real
FAT12/16/32 media images and nested files, every move write/flush under four
persistence modes, packed FAT12 entries across sector boundaries, read-only
refusal, insufficient contiguous space, sequence revalidation and independent
ownership/fsck checks. `--convert` checks every sector-write/flush boundary,
additional volatile-cache stage cuts, corrupt-journal refusal before any
write, MBR/EBR types, cluster table cutovers, file bytes/long names/empty
files/dot-dot links, FAT slack trimming, FSInfo and backup records. It also
runs `--maintenance-mount` for the production storage gates and exclusive
read-only/read-write remount. These tests use only image files under
`build/host`, never device files.

The Files controller scenario exercises worker-only calls, analysis,
Settings, Show Details/legend, Start/Pause/Resume/Stop, competing-write
restart, busy errors, conversion warnings/cancel/completion and removable/
system refusals. Fx-15 row 37 appends those states to the existing model.

The defragmenter requires a free run large enough for the entire file;
it does not compact free space or relocate directories. An unscanned/fast-mounted volume always gets an ownership proof before
source-chain reclamation, even when the optional early check is off. The error check
uses the existing checker and refuses corruption rather than inventing a
successful repair. Settings apply to the current invocation. The Start/System Tools launcher
remains open. G29D adds bounded persistent startup image profiles and their
placement, plus deferred boot conversion under the recognised native MBR
and recovery-aware loader. The existing FAT32 system image needs no
conversion; FAT32 input is refused as already converted. It requires a free contiguous
journal extent plus destination space for the root and displaced clusters.
Failure leaves the volume detached; reboot resumes a committed locator.
Neither ST-10 nor ST-12 is fully complete.


## G29D bounds and qualification

`CIUKIST.LOG` in the volume root is a private hidden/system, serial-bound,
CRC32-checked hint file, at most 24 images and 16 distinct observed logical
clusters per image. Each successful FAT-backed native/PE image snapshot
records its actual storage reads; this includes PE library image snapshots.
Use counts saturate and the bounded eviction clock ages at overflow.
The log's open-file/identity sharing is checked before writing it under the
volume reservation. A log write failure never fails a launch. This does
not collect arbitrary post-start data reads, DOS VM startup reads or a
Windows-compatible Applog file format. Placement is ordered by image use
count/recency, not by an application/dependency graph. No launch-time
improvement percentage is claimed without hardware measurements.

Boot requests preserve FAT16 and open file handles until restart. The
request is bound to serial, partition size and start, checksummed and
flushed. On the next storage start, conversion runs before the C: VFS
attachment and before raw boot-status/log binding. The loader validates all
redo headers, payload CRCs, target bounds, partition-table preservation and
the final boot record before enabling its read-only virtual FAT32 view.
It never writes a BIOS sector during recovery. The kernel rechecks and
replays the journal with its usual flush barriers.

The supported boot layout is the native first active primary partition at
LBA 2048, the 48-sector CRC-checked loader at LBA 1, source clusters of at
most 32 KiB and at most 8192 redo records. The native MBR carries `CKD1`
at byte 436; the compatible loader carries `G29DCVT1` at byte 24536.
Older/foreign stages, larger clusters and bigger journals are refused.
This extends one universal loader; it adds no per-machine image/build.
Installing this loader on an existing foreign FAT16 system is a separate
installation operation; the converter does not silently replace foreign
boot code. Backup and older-DOS/dual-boot warnings remain mandatory.

Focused verification commands: `nice -n 19 bash scripts/test/host_fs_tests.sh
--startup`, `--startup-image`, `--boot-convert`, `--maintenance-mount`, `--defrag`, `--convert`.
`--startup-image` exercises the actual native image snapshot hook and open-log
sharing (including the FAT32 root-cluster/zero alias). The startup suite checks persisted use order, logical bytes versus physical
permutation, corrupt/stale hints and every write/flush boundary under four
persistence modes on FAT12/16/32. Boot tests execute assembled loader
instructions with raw disk reads supplied from a production converter
fixture: native FAT16, committed redo, partial replay, corrupt-record
refusal, kernel replay and FAT32 file seeking. The assembled IEEE CRC is
checked at each used input length; bulk journal scans substitute the same
host CRC polynomial to bound interpreter time. These are host evidence;
firmware/physical boot qualification remains the lead's responsibility.

The loader reserves `[0x20000,0x38000)` (96 KiB) until kernel handoff: a
32 KiB cluster buffer covers Windows FAT16 cluster sizes, and a separate
GS segment holds the bounded journal index. The boot-info loader extent
includes both. No physical-memory or cache admission limit is changed.

## Integration correction: startup logging and short holds (2026-10-24)

The standalone host filesystem suite reproduced the exec-image hold exactly:
20 device commands at 106 ms each, or 2,120 ms, tagged `metadata/vfs`.
The G29D hook used direct `fat_lookup`/`fat_startup_record` under one volume
reservation after the executable snapshot. This was independent of fx-23's
publication gate and of host scheduling load.

Research: Linux's [iomap design, IOMAP_NOWAIT](https://www.kernel.org/doc/html/latest/filesystems/iomap/design.html)
describes avoiding synchronous metadata I/O for a nonblocking best-effort path;
its [readahead documentation](https://www.kernel.org/doc/html/v6.12/core-api/mm-api.html#readahead)
allows advisory prefetch to fail without failing the demand read. These are
supporting examples, not Windows compatibility claims. The Windows 98
application-access recording requirement and the CiukiOS f1-93 short-hold
contract both still apply.

The hook now opens the private log with an ordinary exclusive read/write VFS
handle. Lookup, create, write, truncate and commit use the existing bounded
FAT transactions; log reads use the open-file reader path. Namespace and
volume reservations are released across device commands. A pure serialization
helper retains the same format, CRC, volume serial, record validation, bounded
use/recency update and observed cluster order. A failed allocation, sharing
check or log I/O remains advisory and never fails the image snapshot.
The exclusive handle also protects the complete update against a log open
while a transaction has temporarily released its volume guard.

Host verification on the integrated tree: the same f1-93 exec-image row
still issues 39 modeled commands but releases the reservation 28 times,
with `logical_hold_max_ms=0`, `vfs_hold_records=0` and `cold_fallbacks=0`.
The startup-image harness also passes persistent logging, native open-log
sharing, launch success and descriptor/identity cleanup. The standalone
filesystem run and its complete results are recorded in
`build/codex/fixd-fs-final.log`.

## Start logging leaves the launch path (2026-10-24)

QEMU runs of image 56e58374: each native program start still wrote and
committed `CIUKIST.LOG` inside exec (`fdtable.c` `image_fat`), so while C:
publication was stuck after a DOS Box close every Run launch waited there.
Research: the Resource Kit, “Disk Management” (`wrkc10_5.htm`), describes
the source of Disk Defragmenter's start order as a separate process: “To
record the disk access patterns of programs during their startup, Windows 98
uses a process called Task Monitor”, which writes its logs to
`\Windows\Applog` and counts program uses. Logging is the monitor's work,
not the launched program's.

- Exec only appends the start (identity, actual read order, the volume's
  attachment generation) to a bounded in-memory queue in the VFS
  (`VFS_STARTUP_QUEUE`, 16 starts, `identity_lock`); it performs no I/O and
  keeps its locks. A full queue drops the oldest start (`startup_dropped`).
- The `startup-log` kernel task (normal priority, created with the storage
  writer) wakes on a queued start, lets a burst settle for up to one second
  (at once when the queue is 3/4 full, `VFS_STARTUP_URGENT`, so a boot burst
  does not drop its first starts) and calls `vfs_startup_flush`: per volume
  one exclusive open of the log, the existing serialization
  (`fat_startup_prepare`) for each start, one write and one commit; no
  reservation spans device I/O. A start whose volume was detached or
  remounted, a refused hint or a log held open by a reader is dropped
  (`startup_failed`): the log stays advisory and never fails a launch.
  Without memory for the flush the whole queue is dropped once and counted,
  so nothing is retried; the task logs an error when it first appears.
- The Disk Defragmenter's analysis for “rearrange program files” flushes the
  queue first, and Shut Down flushes it after the `shutdown requested`
  marker and before the boot log. Neither waits for a flush in progress: that
  flush holds the log open and may be stalled in a C: commit, so both refuse
  with EBUSY at once (Shut Down before any change, as for the boot log; the
  desktop retries it every 250 ms for up to 5 s). The Files defragmenter
  reports an analysis EBUSY as an error and does not retry it. The
  defragmenter keeps reading the same log format.

Host evidence (`--startup-image`): a launch leaves the device write and flush
counters and the volume's mutation sequence unchanged and creates no log;
the log appears with the next flush (use clock +1); a flush while a native
reader holds the log is refused and counted; 19 starts in a burst keep the
newest 16, written by one flush; a flush without memory empties the queue
once; 12 queued starts end the settle delay at once. `--maintenance-mount`:
with a start-log flush stalled in progress, `storage_startup_log_shutdown`
(Shut Down's step), the defragmenter's analysis and a no-wait flush return
EBUSY within 1 s, and succeed once it finished. On 56e58374 the launch check
fails (the start was written inside exec); on the first version of this
change (a64d5839) the stalled-flush case blocks the analysis until the
test's alarm.
