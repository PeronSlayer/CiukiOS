# F4 implementation and acceptance evidence

## f4-01

Directive: [native audio](../directives/f4-01-native-audio-ac97.md).
Scope: native streams, integer mixer, Intel ICH AC97 and QEMU WAV evidence.
QEMU and physical qualification belong to the lead; host/build results below
never qualify analog sound or latency.

### Research and implementation decisions (2026-10-11)

- Intel [ICH3 datasheet](https://www.intel.co.kr/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf),
  sections 5.18/13.2: CAS serializes posted codec access; RUN clear precedes
  reset, and CIV/PIV plus status identify immutable descriptors. Fetched data
  is not audible completion. Use 32 slots, eight published 256-frame periods,
  stable cursor reads and absolute retirement; fail closed on discontinuity.
- Intel [AC97 2.3](https://hands.com/~lkcl/ac97_r23.pdf): keep output at 48 kHz,
  check playback readiness and rate readback, apply software gain and verified
  unity codec paths. Headphone is used only when the capability advertises it.
  Section 5.7.5/Table 22 specifies PCM unity as 0808h (zero is +12 dB), with
  master/headphone at zero. QEMU 9.2 `update_combined_volume_out` instead maps
  zero to full scale linearly. Select PCM zero only for the validated QEMU
  8086:2415 twin; physical codecs use 0808h. Both paths verify readback and
  explicit mute. This preserves one logical software master mapping.
- [Linux v6.12 intel8x0](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c)
  and [QEMU v9.2 AC97](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/ac97.c)
  were compared with the retained session_audio and the new registry/worker/fd
  interfaces. No upstream implementation is imported. The QEMU model clears
  CAS on mixer access and uses 16-bit sample lengths, independently of frames.
  Unlike Linux's recovery access after a semaphore timeout, Ciuki refuses it.
- Validate Intel IDs 2415/2425/2445/2485/24c5, I/O BAR documented extents
  (physical NAM 256, NABM 64; QEMU's `ac97_realize` exposes 1024/256), quiet
  existing engines, and QEMU PIIX or physical ICH
  PIRQ routing/ELCR. Unknown routing or an AC-link modem dependency prevents
  activation. No firmware reset or bridge/BAR relocation is attempted.
- Rational linear interpolation retains phase/history across writes. A bounded
  final held sample completes the resampled tail; starvation does not move
  source counters or phase. Source rings own decoded stereo S16 frames and
  allocate two pages at admission; DMA BDL/payload pages are pinned in DMA.
- DMA segments have a 32-bit address mask, page-aligned allocation and
  eight-byte-aligned descriptors; each payload is a contiguous 1024-byte
  period. ZONE_DMA allocations satisfy the mask without asserting an ISA
  boundary restriction. RES_MMIO claims describe the pinned physical pages;
  RES_DMA is reserved for 8237 channels in the existing registry. The backend
  owns nine DMA pages and four worker-stack pages; each stream adds two pages.
  Static scratch/settings tables are part of the reserved kernel image.
- Prime with eight silence periods before consuming source frames, allowing
  independently scheduled clients to fill their rings. Refill runs in P_DEVICE,
  wakes on the existing kwait queue/IRQs, observes progress at least every
  millisecond, and publishes one bounded quantum per invocation. A codec
  transaction includes posted-write readback in its single 100 ms deadline;
  activation and stop share a 500 ms deadline across their stages.
- Generated frames and observed physical fetches are separate monotonic counts.
  Stopping may discard queued silence; it never credits those frames as fetched.
  Drain uses the generated-frame cursor of the final client tail. A new engine
  run starts a new cursor base while preserving the actual fetched count.
  Status includes an explicit current transport queue in addition to the source
  queue; discarded silence from previous runs cannot inflate that queue.
  A quarantined engine can be asked to clear RUN, but cannot reset, republish
  or free its pinned buffers. Preactivation refusals also unwind registry claims.
- A 20 ms drain allowance follows final fetched output, a provisional digital
  allowance requiring T23 measurement before analog qualification. The output
  worker services one quantum then yields; no codec/file work in IRQ sections.
- AUDIO.CFG contains two 48-byte checksummed slots. Write the inactive slot
  and commit through VFS, retaining the previous valid record. Defaults and
  read/save failures are explicit. ABI is frozen in both owning amendments.
- PID 1 installs the non-inheritable mixer control handle while the trusted
  desktop is still private and supplies `CIUKI_MIXER_FD` in its environment.
  Public opens remain read-only; dup shares authority, and clearing CLOEXEC
  cannot make it inheritable. This adds no desktop volume interface.
- WAV checks use standard-library Python, trim boot/trailing silence, measure
  whole-tone duration/channel RMS, DFT peak and silence gaps in declared
  windows. Tests must include negative waveform controls. WAV success proves
  digital output only. Captures are removed only after complete PASS.
- The controller uses READY/ERROR with a checked `not_run` reason for an absent,
  safe-mode or unsupported backend. Activation/allocation/quarantine failures
  terminate FAIL. The runner requires matching backend readiness and reason;
  neither outcome can satisfy the AC97 acceptance cases. Required source,
  device, memory, service-budget and cleanup records have independent predicates.

### Lead policy after the QEMU evidence (2026-10-11)

The service owns the lead policy: eight periods while a stream is active,
a 28-period idle tail for 500 ms after the last writer, then a controlled
stop (an underrun inside the tail is not a failure); the driver only refuses
more than 31 outstanding periods. The refill thread runs at `P_DEVICE` and
must block between services (f1-18 rule): the IRQ and the stream writers
wake it, and its deadline is a safety net only (1 ms while the first periods
are published, 4 ms while running, 50 ms idle). The earlier 1 ms poll
starved ring-3 tasks and failed the F0 `preempt` probe on the integrated
image 7ab10f64 (zero task switches in two minutes).

### Validation

The ASan/UBSan production harness passes 10,481,598 checks, including every
integer rate 8000..48000, signed-extreme interpolation, all four format/channel
combinations, sum/master/clipping, split phase, allocation failure, nonblocking
and interrupted/partial writes, drain/tail cancellation, reset generations,
fd sharing and final close, settings/delegation and read-only-save restoration.
Mock registers cover posted readback/combined 100 ms timeout, activation/stop
deadlines, three BDL wraps, publication/current/prefetch canaries, ambiguous
progress, W1C and permanent retention of all nine DMA pages after failed stop.

WAV host controls pass at 44100 and 48000 Hz; swapped channels, wrong frequency,
gain, duration, channel-specific gaps and silence are rejected. Fake runner
tests verify recorded measurements, finalized captures, failure retention and
PASS deletion without running QEMU. The full host kernel test script and loader
statics pass. Final Python/build/image results are appended below.

Final validation on the implementation worktree:

- Full Python host discovery: 252 tests, PASS, eight existing skips. The six
  F4 waveform/runner tests also pass independently. Loader statics PASS.
- Full kernel host script PASS with ASan/UBSan stopping on errors; final
  production audio harness: 10,481,598 checks PASS.
- Kernel: 1,666,224-byte VMM.ELF, FPU/SIMD audit PASS. SDK/newlib build with
  `--jobs 2`: PASS, 6,775,889 installed bytes; SDK layouts, pthread checks,
  ELF inspections and complete runtime/compiler archive audits PASS.
- Lua/luac (`--jobs 2`) and desktop/demo builds and ELF inspections PASS.
- Canonical 512 MiB image: T1 geometry, MBR, fsck, loader CRC/size and all
  55 payload size/SHA-256 readbacks PASS. Build identity `798a92da51e3-dirty`.
  Kernel SHA-256:
  `aca583d21fe397263e2d1da2475747ba937f35eea808811dd19cdcc8246a5392`.
  Image SHA-256:
  `4e19177c9f5ed35aee35a0d2edd1079b5f13a4f0ecf5f018df6c678e4ddcddb9`.

Lead acceptance:
run f4-audio on qemu-t23 and qemu-desktop-2002, then f2-all and f0-smoke on the
same image hash; preserve compact waveform measurements and runtime counters.
Physical analog sound, drain allowance and the 60 ms target remain unmeasured.

## f4-02

Directive: [f4-02](../directives/f4-02-atapi-iso9660.md). Host implementation
evidence only; QEMU and physical qualification belong to the lead.

Research before implementation (2026-10-11):

- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  §§5, 7 and 10: PACKET command phase CoD/IO=1/0, data-in=0/1,
  completion=1/1; only the final data phase may have an odd byte count.
  IDENTIFY PACKET DEVICE establishes CD-ROM type and 12/16-byte packets.
  Decision: polling PIO with nIEN set, one channel mutex through packet
  completion and REQUEST SENSE, no overlap, DMA, retry or firmware calls.
  Ordinary packets have a 2 s elapsed deadline, explicit start/eject 10 s.
  Timeout/malformed phase quarantines BOTH units on the channel, including
  an ATA peer; ordinary CHECK CONDITION uses sense without quarantining a
  proved completed transport. Unit attention/media disappearance expires
  the media generation before another mount transaction.
- [QEMU v9.2 ATAPI source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/ide/atapi.c):
  READ CAPACITY reports last LBA and 2048-byte blocks; TOC format 1 is
  encoded in byte 9 bits 7:6 and gives the last data session. Decision:
  select the data session through TOC; audio-only media is unsupported.
- [ECMA-119 sixth edition](https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf),
  §§7–9: descriptors, directory bounds and ordered sections of a file.
  [Microsoft Joliet specification](https://pismotec.com/cfs/jolspec.html):
  `%/@`, `%/C`, `%/E` and UCS-2 big endian. Decision: validate both-endian
  fields, terminator, records and extents; prefer the strongest advertised
  Joliet hierarchy, refuse malformed advertised Joliet. No Rock Ridge/UDF.
- [QEMU QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html):
  eject/change removable media with explicit ARM synchronization. Decision:
  CD-ROM uses IDE index 3 (secondary slave), with ATA fixture peer at index
  2 when declared. Refuse any collision with FAT fixture indices 1–3;
  optical cases can declare at most two FAT fixtures.

Repository comparison: `ata.c` formerly skipped ATAPI after reset signature;
`cache.c` stored only 512-byte blocks; the two VFS views assumed FAT volumes.
The shared cache now keys device, generation, block size and LBA. Each 4096-byte
allocation includes metadata, one 2048-byte slot and three 512-byte slots;
512-byte requests may use either capacity. This keeps the same byte budget
and preserves healthy-device progress when a failed dirty FAT block remains
pinned in the minimum 8192-byte cache. A uniform 2048-byte layout failed that
existing host regression because only one slot remained after workspace.
The generic `fs_ro.h`
interface keeps 64-bit identities/sizes and a bounded opaque cursor; it
requires no FAT or ISO layout. VFS owns bounded per-directory name/alias
views, constructed in on-media enumeration order before lookup. This makes
aliases independent of lookup order. Exhaustion/case collisions refuse the
directory visibly. Media loss invalidates cache, aliases and mount identities,
resets legacy cwd, expires native cwd identities, makes handles return EIO,
and removes the letter before reuse. Old references never acquire the new
mount generation. Read-only sync/unmount never invokes write/flush callbacks.

The VFS bounds each directory view to 256 entries and a mount to 32 views;
each allocation holds four metadata nodes. The parser bounds descriptor sets
and multi-extent chains to 128 records. Exhaustion fails visibly. Generation
checks occur on namespace/handle operations and explicit storage refresh;
readiness is queried before serving cached names or blocks. Reinsertion uses
a new mount transaction; stale legacy searches, handles and native cwd/file
identities retain the expired VFS generation. No background media retry is
introduced. The probe bounds its iterative fixture walk to 16 directory
levels and records names separately from file hashes to respect the existing
240-byte record envelope.

Host evidence (2026-10-11, `wt/f4-optical`):

- `make test-host`: PASS, including production packet/ATA peer mocks, native
  and legacy read-only lifetime/mutation checks, loader statics, independent
  ISO comparisons and 250 Python tests (7 existing skips: six absent local
  physical captures and one f2-15 evidence-policy case). All six Level/Joliet
  combinations are deterministic; synthetic multi-extent and malformed
  descriptor/extent cases pass. Host block writes/flushes are zero and image
  before/after hashes agree. ASan/UBSan enabled; LSan disabled because ptrace
  is unavailable in the sandbox. i686 layouts are compiled/extracted, but
  Linux cannot execute the native `-m32` test binary in this sandbox.
- `scripts/test/host_fs_tests.sh`: PASS, 14 groups, three fsck checks and
  three independent mtools comparisons. Its production cache regression
  proves healthy-device progress after a different device's sticky write
  error. Crash classification covers 2122 cuts with no unknown diagnostics.
  The helper now extracts the builder's constant formatted frame-limit flag
  without executing/importing the builder.
- SDK and Lua builds use two jobs, desktop/kernel/image builds run
  sequentially. SDK compile/link/audits, Lua/luac and desktop/demo inspections
  pass. Final kernel: 1,690,876 bytes, build `798a92da51e3-dirty`, 4096-byte
  frame checks and FPU/SIMD audit PASS. Image T1 passes MBR/FAT32 geometry,
  fsck, loader size/CRC32 and independent read-back of all 55 payloads.

Canonical image `build/f0/ciukios.img`, 512 MiB, SHA-256:

```text
ed2af39ae4f177ca16bf25343df5119653a34fa5ba536d9f9f4424cf48296283
```

No QEMU or systemd scopes were run by the implementer. The lead must review
and integrate the diff, build the integrated image and run `f4-media`,
`f2-all` and the earlier applicable gates on that exact image. Actual packet
device, QMP tray behavior and hardware optical qualification remain unproved
by host evidence. Complete commands/output and handoff are retained in
`build/codex/f4-02-report.md` and the referenced logs.

## f4-03

Directive: [NTFS read-only library](../directives/f4-03-ntfs-ro-library.md).
This is a freestanding read parser, not a mounted VFS service. QEMU and
hardware qualification remain the lead's work.

### Research and implementation decisions (2026-10-11)

The approved F4 S9 reference is Linux v6.12:
[ntfs.h](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/ntfs.h),
[run.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/run.c),
[super.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/super.c).
Additional behavior references are its
[fslog.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/fslog.c)
restart-page layout and ntfs-3g's
[volume.c](https://raw.githubusercontent.com/tuxera/ntfs-3g/edge/libntfs-3g/volume.c)
hibernation check. These were read as specifications; no code is copied.
The repository currently has FAT/VFS/cache layers, no NTFS parser, and
build_kernel.py automatically compiles fs/*.c. Keep that build behavior:
ntfs_ro.c compiles and links in the canonical kernel with no caller or boot
registration. It has no global workspace or initialized hardware state.
This verifies the real i686 flags/audit without modifying the build recipe.

Read callbacks use volume-relative logical sectors, with the caller supplying
the sector size and containing partition length. Boot geometry must agree;
all subsequent reads are bounded by both supplied and advertised lengths.
NTFS fixup strides and index VCNs for an index smaller than a cluster use
512-byte NTFS blocks, independently of the callback sector size; this is
confirmed by the reference `ntfs.h` and `mkntfs.c:add_attr_index_root`.
There is no write, flush, allocation or cache callback. The caller reserves
and charges the workspace before mount, serializes calls, and invalidates it
on media replacement. Retained run maps and the 128-KiB UpCase table are
decoded metadata, not a private block cache; sector/record buffers are scratch.

As explicitly requested by f4-03, uncompressed sparse runs are supported as
logical zeroes, including the >4-GiB fixture. This narrows the F4 draft's
general sparse refusal: compressed/encrypted streams, reparse points and
alternate stream syntax remain EOPNOTSUPP. Initialized length is an independent
upper bound on physical data exposure, including ordinary allocated tails.
Missing run coverage, malformed metadata and stale sequence references are
EIO; bounded-workspace exhaustion is EOPNOTSUPP. Mount exposes separate
dirty, recovery-required and hibernated refusal reasons. No salvage or replay.

Fixtures use the installed ntfs-3g tools without privileges. References:
[mkntfs.c](https://raw.githubusercontent.com/tuxera/ntfs-3g/edge/ntfsprogs/mkntfs.c),
[ntfscp.c](https://raw.githubusercontent.com/tuxera/ntfs-3g/edge/ntfsprogs/ntfscp.c),
[ntfstruncate.c](https://raw.githubusercontent.com/tuxera/ntfs-3g/edge/ntfsprogs/ntfstruncate.c).
Tool versions and executable hashes belong to each fixture manifest. Tool
output listings and streamed ntfscat hashes are the independent oracle;
patched structural cases are separately identified and checked by those tools
when valid. Fixtures are nonboot input volumes, never system-image variants.
`mkntfs -T` supplies zero time/GUID; post-generation normalization fixes the
serial in both boot copies, timestamps in FILE/INDX metadata, unused metadata
slack, and the order of tied FILE_NAME attributes by their unchanged IDs.
Two complete generations must produce identical image SHA-256 values. These
changes do not alter the independent content/listing oracle. Corrupted cases
retain the independent source file hashes before structural patching; those
patches leave their data extents unchanged. The hibernation marker has its own
independent ntfscat hash. All before/after test image hashes must match.

### Bounds and refusal policy

| Resource | Limit |
| --- | --- |
| Caller workspace | 233,536 bytes under the actual i686 flags; 233,568 bytes on the x86-64 test host, obtained with `ntfs_ro_workspace_size()` |
| Sector / cluster | 512–4096-byte logical sectors, power of two; clusters up to 64 KiB |
| FILE / INDX | 4 KiB / 16 KiB |
| Stream runs / attributes per record | 256 / 256 |
| Attribute-list bytes / entries / selected extents | 16 KiB / 512 / 128 |
| Directory index blocks / depth / visited entries | 4096 / 16 / 65,536 |
| One pread / blank-log validation | 1 MiB / 64 MiB |
| Name | 255 UTF-16 units; strict UTF-8 input and surrogate-pair conversion |

Exhaustion returns `-95` (EOPNOTSUPP); malformed records/maps, stale MFT
references and failed/short block reads return `-5` (EIO). Invalid arguments
return `-22`, too-small workspace `-12`, malformed UTF-8 `-84`, oversized
components `-36`, a missing component `-2`, and file/directory type mismatches
`-20`/`-21`. DOS namespace entries have `name_space=2` and the exact same
reference as their long name; listing clients can present them as aliases.
The library emits actual index names; synthetic dot entries and path/mount
generation policy belong to the future VFS wrapper.

Mount keeps its output null until all required checks succeed. It validates
the primary MFT map and first mirror record, volume version/flags, log state,
UpCase and the root index. It never falls back to a damaged mirror or repairs
anything. Dirty is reason 3, recovery-required 4, hibernated 5; invalid metadata
is 1 and unsupported geometry/limits 2. Dirty takes priority when multiple
refusal conditions coexist. Nonzero volume maintenance flags except the NT4
marker require recovery. Blank logs must be entirely FF, not just have an FF
first page. Otherwise both bounded version-1 restart pages must prove clean;
no newer damaged/active page is ignored. Version-2.0 logs conservatively report
hibernated/cached metadata ownership. A present hiberfil.sys shorter than 4 KiB
or with any nonzero header byte is conservatively refused as hibernated.

MFT bootstrap extensions are read only through the already proved prefix;
each listed extent extends that prefix before the next record is read. Nested
attribute lists, missing references, duplicate/overlapping VCN coverage and
index cycles/duplicate children fail closed. The callback must return a full
read or an error; it cannot be used to write or flush media. The caller must
discard partial buffers/listings on an error and serialize callbacks/context
use. No ACL enforcement or Windows mount compatibility beyond this subset
is claimed.

### Validation

The host set contains 27 generated 32-MiB NTFS volumes, including 512-byte
clusters and 4096-byte sectors/16-KiB clusters. The large-directory fixture
has 2048 independently hashed files. Interleaved 240-cluster writes and a
blocker deletion produce fragmented DATA with a nonresident ATTRIBUTE_LIST
and extension records. Separate lawful layout patches exercise negative
relative LCNs and a fragmented MFT, preserving allocations and independently
verified file bytes. ntfstruncate creates a 4,295,032,833-byte sparse file;
streamed ntfscat and library hashes agree without a whole-file allocation.
An allocated file with initialized length 13 proves physical slack stays
inaccessible. Unsupported and corrupted variants cover compression,
encryption, reparse flags, FILE/INDX fixups, runlists/VCNs, reference sequences,
bitmaps, index cycles, attribute lists and log/volume/hibernation refusal.

- Focused T0: 14 tests PASS, including a second complete deterministic
  generation and ASan/UBSan traversal of every fixture. Every fixture retains
  its image SHA-256 after reads, refusal, failed I/O and unmount. Instrumented
  callbacks check the advertised volume bound; no write callback exists.
- Existing C host suite: PASS, including ABI, driver, storage, process,
  signal, file and desktop harnesses. Loader statics: 6 tests PASS.
- Full Python host regression: 260 tests PASS, 8 existing conditional skips;
  the 14 NTFS tests all ran. Final command:
  `PYTHONDONTWRITEBYTECODE=1 ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests/host`.
- Kernel: canonical `scripts/build_kernel.py` PASS, i686 compilation/link,
  stack-frame checks and FPU/SIMD audit; the NTFS object is linked but has no
  runtime caller. No VFS mount, SDK capability, syscall or boot registration
  was added.
- SDK/newlib (`--jobs 2`), Lua/luac (`--jobs 2`) and desktop/demo builds:
  PASS with the existing pinned archives and their host/static checks.
- Canonical image: 536,870,912 bytes, T1 MBR/FAT32/fsck/loader/CRC PASS;
  55 payloads read back with matching sizes/hashes. SHA-256:
  `1d60e6851bbba02f6f11532b652c2c5fb4fdad6be24ea45602eed63f5b0404b6`.

No QEMU, f2-all or hardware evidence was run by Codex. The lead still owns
code review, the integration diary entry, earlier QEMU gates on the integrated
image and the later fs_ro/VFS directive. NTFS 3.0 parsing is admitted by the
version check; generated-tool evidence here covers NTFS 3.1 only.

## f4-04

Directive: [NTFS read-only mounts through `fs_ro` and optical cache slots on
demand](../directives/f4-04-ntfs-vfs-cache-slots.md). Host implementation
evidence only; QEMU and hardware qualification belong to the lead.

### Research and implementation decisions (2026-10-11)

Sources read before the change, in addition to those of f4-02 and f4-03:

- [Linux v6.12 ntfs3 `ntfs.h`](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ntfs3/ntfs.h):
  `FILE_ATTRIBUTE_*` (read-only 1, hidden 2, system 4, archive 0x20, directory
  0x10000000), file-name types (POSIX 0, Win32 1, DOS 2) and `MFT_REC_USER = 24`.
  Decision: MFT records 0-23 are NTFS system files and are never exposed
  through the VFS (not listed, not resolvable); the FILE_NAME flags of an index
  entry become the DOS attribute bits (read-only is implied by the mount).
- [Linux-NTFS `$FILE_NAME`](https://flatcap.github.io/linux-ntfs/ntfs/attributes/file_name.html):
  flags at 0x38; size/time fields of the name "are only updated when the
  filename is changed", `$STANDARD_INFORMATION` and the data stream stay
  current. Decision: the size reported by listings and `stat` is the node's
  `$DATA` size read at enumeration (the library's `ntfs_ro_open`), never the
  index copy.
- [Microsoft disk partition types](https://learn.microsoft.com/en-us/windows/win32/fileio/disk-partition-types):
  0x07 is the IFS partition type shared by NTFS, HPFS and exFAT. Decision: an
  MBR type 07h partition is mounted only if its boot sector carries the NTFS
  signature; other type 07h content (exFAT, HPFS) still gets no drive letter,
  as before f4-04.
- The QMP `query-blockstats` fields (`wr_operations`, `wr_bytes`,
  `flush_operations`) the runner already uses for panic evidence, now also for
  the NTFS fixture drive (`id=ciuki-ntfs`). The runner fails closed when the
  device name is missing from the statistics, so a mismatch is visible on the
  first QEMU run rather than silently passing.

Repository comparison before the change: `ntfs_ro.c` (f4-03) had no caller;
`storage_add_disk` mounted only FAT partition types; the cache had a fixed
one-2048 plus three-512 slot split per page and the VFS views held only
FAT-like or ISO names. The library file keeps its name: the adapter is the new
`src/kernel/fs/ntfs_fs.c` (the directive said `ntfs_ro.c`; that file is the
f4-03 library and stays free of fs_ro, VFS and cache types).

Decisions:

1. **Mount path.** `storage_add_disk` offers each type 07h partition to
   `ntfs_add`: a counted read-only view (`struct storage_ntfs.io`) of the
   partition is created and probed through the shared cache. The view forwards
   reads; `write` and `flush` are refused with `EROFS` and counted, it is never
   durable (`write_cache_state` unknown), so the cache cannot even issue a
   dirty write. A volume that fails the NTFS probe leaves no state and no
   letter; an NTFS volume takes the next drive letter in partition order,
   including a refused one (as a FAT volume with a mount error does), and is
   mounted with `fs_ro_mount(&ntfs_ops, ...)` and `vfs_attach_ro`. Slots:
   `STORAGE_NTFS` = 8. `storage_ntfs_mount` / `storage_ntfs_unmount` are the
   lifecycle (probe remount, tests); unmount refuses with `EBUSY` while files
   are open and issues no write. Boot logs `[storage] ntfs slot=... result=...
   reason=...`; no ACTIVATION ledger entry is added, so the F1 boot/safe
   records and predicates are unchanged.
2. **Refusals.** The library's reasons become `ro.diagnostic` and the error:
   dirty `ntfs_dirty`, recovery-required `ntfs_recovery_required`, hibernated
   `ntfs_hibernated` (all `-EUCLEAN`, 117); malformed metadata `ntfs_malformed`
   (`-EIO`); unsupported geometry/limits `ntfs_unsupported` (`-EOPNOTSUPP`);
   workspace exhausted `ntfs_workspace` (`-ENOMEM`). Nothing is replayed,
   cleared or repaired; a refused mount holds no workspace and no letter.
3. **Workspace.** The 233,540-byte i686 library workspace (233,576 on the x86-64
   host; it grew by one appended `file_attributes` field, below) plus 52,596
   bytes of adapter header and bounded directory snapshot are one zeroed
   contiguous allocation of 70 pages per mounted volume, returned at unmount.
   Nothing in the kernel provided a multi-page contiguous allocation, so
   `pmm_alloc_contig(pages)` (NORMAL zone first, then DMA, never LOW, freed page
   by page with `pmm_free`) and `fs_contig_alloc/free` in `fs_port.h` were added;
   the DMA-only wording of boot-memory.md concerns device DMA, not kernel
   workspaces. Mounting happens early in `storage_init`, when a run exists.
4. **Library change (additive).** `ntfs_ro_entry` gained a trailing
   `file_attributes` (the index entry's FILE_NAME flags); nothing else in
   `ntfs_ro.c/.h` changed and all 14 f4-03 tests pass unchanged.
5. **Names.** The VFS name policy is BMP-only (`path_decode` rejects 4-byte
   UTF-8, `path_validate_name` rejects reserved characters and a trailing dot or
   space). An NTFS name that fails it is not shown wrongly: when the node has a
   valid DOS alias the alias becomes its name, otherwise the entry is omitted
   and counted in `fs_ro_volume.names_omitted` (once per directory per mount;
   the probe records it). DOS-namespace index entries are merged as the stored
   alias of exactly one long name with the same MFT reference (an orphan alias
   is `EIO`, an alias that fits two names of one directory is `EOPNOTSUPP`);
   an alias the view's 8.3 syntax would refuse is dropped and the VFS generates
   one. `.`/`..` entries and system files are hidden. Lookup compares with the
   VFS folding first and, on a miss, with the volume's own `$UpCase` collation
   through the new `FS_RO_DRIVER_COLLATION` flag (the two tables differ in 27
   characters NTFS folds and 222 the VFS folds beyond NTFS; the first
   direction is covered by the Greek `U+1F80` fixture, the second can make two
   distinct NTFS names collide in the view, which refuses that directory with
   `EIO`).
6. **Unsupported and damaged content.** A node the library refuses
   (compressed, encrypted, reparse point: `EOPNOTSUPP`; damaged record or run
   map: `EIO`) is still listed (`fs_ro_node.status`, new) with size 0, and every
   lookup of its name returns that status, so `stat`, open and the native
   namespace fail explicitly while its neighbours keep working. A `:` in a
   component is stream syntax (`FS_RO_STREAM_SYNTAX`): `EOPNOTSUPP`. A device
   error while a directory is enumerated is returned as that error and never
   recorded as a per-file status. Ordinary sparse files read as zeros,
   compressed ones are refused: this is the f4-03 reconciliation of the F4
   contract's blanket "sparse is unsupported", recorded here as the final F4
   behavior (the 4-GiB+1 sparse fixture reads zeros at 64-bit offsets; the
   32-bit legacy API returns `EFBIG` for it, the native API is 64-bit).
7. **Enumeration.** The adapter rebuilds one bounded snapshot of a directory at
   cursor zero (514 raw index entries, 256 shown entries, 32 KiB of names) and
   serves the opaque cursor from it; the VFS keeps the existing 256-entry and
   32-view-per-mount bounds, so the 2048-entry fixture directory fails with
   `ENOSPC` rather than a truncated listing. Limits worth a later directive:
   compact views with eviction would lift both.
8. **Sharing and mutation.** Unchanged generic read-only rules apply (deny
   modes in both directions, `EROFS` for every mutation, zero free space);
   the host harness repeats the conflict matrix on an NTFS file, and the
   probe's eleven mutation attempts must all return `-EROFS`.
9. **Cache slots on demand.** A data page is two 2048-byte halves; a half serves
   four 512-byte slots or one 2048-byte slot. Descriptors (44 bytes on i686)
   are packed 11 page headers per descriptor page, carved from the same byte
   budget (a static assertion fixes the i686 density; the first header lives in
   `struct block_cache`, so the 8192-byte minimum still has a data page).
   `cache_init` allocates exactly `bytes/4096` pages (default 8 MiB: 128
   workspace, 1760 data and 160 descriptor pages on i686; 1680 and 240 on the
   64-bit host) and starts every page as eight 512-byte slots: 14,080 slots on
   i686 against F1's 13,440 (seven per page; the host packs fewer headers and
   gets exactly 13,440). A system without an optical device keeps all of them.
   The first 2048-byte device (`cache_attach`, called implicitly by the first
   read) converts `max(1, data_pages/8)` pages, two halves each, writing healthy
   dirty blocks back first: on i686 220 pages become 440 slots of 2048 bytes and
   12,320 slots of 512 bytes remain, with the same page budget. A half pinned by a failed dirty
   block stays 512-byte, so with a failed FAT block and an optical device at the
   8192-byte minimum the other half still serves the optical device; if every
   half is pinned the attach fails with the sticky error. 512-byte requests may
   use a 2048-byte slot (f4-02 rule). Two LRU lists (one per slot capacity) keep
   a 2048-byte miss from walking the 512-byte slots. F1 `cache` probe: the loop
   writes `blocks + 1` sectors; at the 8192-byte minimum `blocks` is now 8 (it
   was 4) and the arithmetic still yields `issued = 2 >= 2` (one earlier
   write-back plus one eviction), so its records and predicates are unchanged;
   `ata_test` and the F2 resource ledger (`cache_blocks` before == after) hold
   because conversion happens only at attach.
10. **Test infrastructure.** `tests/host/fs/kernel_compile.py` rejected
    `__udivdi3` since f4-03 (the library divides 64-bit values) although
    `lib/divdi3.c` provides the helper; the four division helpers are now
    allowed explicitly and any other `__` helper still fails. The strict
    evidence parser rejected `END status=NOT_RUN` (the form f4-02's optical
    probe already used), which would have turned a missing fixture into a parse
    failure instead of `not_run`; it now accepts exactly `optical`/`device_absent`
    and `ntfs-ro`/`fixtures_absent` as a `not_run` outcome and nothing else.

### Probe, records and cases

`f4:ntfs-ro run=<8 hex>` (selector, loader menu, loader model): with no NTFS
partition mounted it records `not_run reason=fixtures_absent`. Otherwise
`device`, `refusal` (letter absent, zero writes), and for a mounted volume two
cycles of `entry` (kind, attributes, status, name length, SHA-256 of the path:
the record envelope cannot carry a 255-unit name), `file` (size, SHA-256),
`status` (open and stat of a refused node), `large` (>4 GiB, `EFBIG` through the
legacy API) and `names`; plus `mutation` 0-10, `ads` (-95), `peer` before/after,
`unmount` busy (open handle, -16) and clean (letter gone, generation advanced,
cwd expired), `gone`, `remount`, and `counters` (device reads, view
writes/flushes and cache writes, all zero). `scripts/test/ntfs_fixtures.py`
`verify_records` compares both cycles with the ntfs-3g oracle (ntfsls/ntfscat)
through the VFS name model, and the runner additionally requires an unchanged
fixture image hash and no QMP block write or flush. `tests/suites/f4-media.json`
gains `ntfs-{clean,fragmented,names,compressed,dirty,hibernated}-{qemu-t23,
qemu-desktop-2002}` (FAT16 peer at IDE index 1, NTFS at index 2; refusal cases
have no peer).

### Validation (host)

The lead runs everything on QEMU. Commands and results in this worktree:

- `make kernel`: PASS (i686 clang `-Werror`, frame limit 4096, FPU/SIMD audit).
- `bash scripts/test/host_fs_tests.sh`: PASS, 15 groups including
  `test_cache` (20,581 checks: budget for seven sizes, on-demand attach,
  20,000 randomized mixed 512/2048 reads, write-back on conversion, the
  minimum-cache rule in four attach orders) and the kernel compile of all 13
  `fs/*.c` units.
- `bash scripts/test/host_kernel_tests.sh`: PASS, including
  `pmm_contig_test` (zone order, exact accounting, fragmentation, zone edges)
  and the F1/F2 harnesses linked with the new sources.
- `python3 -m unittest tests.host.test_ntfs_vfs tests.host.test_ntfs_runner`:
  PASS (17 tests). The production path from MBR disk to VFS is compiled under
  ASan/UBSan: tree and hash equality with the oracle for eight fixture kinds
  and two cycles, every mount refusal (12 kinds) with its reason, unsupported
  and damaged files, a 4 GiB sparse hole, the large-directory bound,
  directories/aliases/collation/attributes, sharing, stale handles
  (quarantine), unmount/remount, and failure injection at all 4401 mount
  reads, all 44 reads of a directory view and the workspace allocation, with
  zero disk and view writes everywhere.
- `python3 -m unittest discover -s tests/host` and
  `python3 tests/loader_stub/test_static.py`: 303 tests PASS, 13 skips (absent
  local desktop/SDK builds and private hardware captures); the 17 new tests all
  ran. The f4-03 library tests (14) pass unchanged, including the second
  complete fixture generation (identical hashes) and the sanitized walk of every
  fixture, now 29 images (`names` and `attributes` are new).

No QEMU, `f2-all` or hardware evidence was run by the implementer. For the
lead on the integrated image: `f4-media` NTFS cases (twelve, above) with hashes
equal to the manifest, zero writes and an unchanged fixture; the F1 `cache` and
`f1-fat32` cases; `f2-all` (the cache layout changed for every system, the NTFS
mount code runs only with a type 07h NTFS partition); and the existing optical
cases (the first 2048-byte read now converts pages). On first run, check that
QMP `query-blockstats` names the drive `ciuki-ntfs` and that the mount's
4401 sector reads of the blank `$LogFile` (2 MiB; the library reads a blank
log up to 64 MiB) fit the 120 s case timeout under TCG.

## f4-05

Directive: [DOS VM audio output](../directives/f4-05-vm-audio-output.md).
Scope: each DOS VM's f3-05 SB16 PCM, direct DAC and OPL3 output rendered into
the f4-01 mixer. Implementation and host evidence only; the QEMU `f4-audio`
vm-audio cases, physical sound and latency belong to the lead.

### Research checked before implementation (2026-10-11)

- Creative, [Sound Blaster Series Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf)
  (contract S5), section "Auto-initialize DMA Mode" (3-5/3-6): the DSP
  interrupts after every block; with double buffering the application refills
  "that half of the DMA buffer whose data has just been transferred"; exit by
  a single-cycle command at the end of the current block or by the exit
  command. F4 therefore never renders past a block whose successor has not
  been produced, and it never fetches across a segment boundary. Chapter 4,
  CT1745: 30h/31h master, 32h/33h voice, 34h/35h MIDI (FM) are 5-bit levels
  "0 to 31 ⇒ -62 dB to 0 dB, in 2 dB steps", default 24 (-14 dB); 41h/42h
  output gain "0 to 3 ⇒ 0 dB to 18 dB, in 6 dB steps", default 0 dB; the
  logical schematic places the output gain after the master. DSP 4.xx D1h/D3h
  only set the speaker latch (f3-05, unchanged).
- DOSBox DBOPL, the contract's reuse candidate, pinned unmodified in
  `third_party/vsbhda` (VSBHDA `75fa4bb`, `src/DBOPL.CPP`/`DBOPL.H`, header
  "Copyright (C) 2002-2021 The DOSBox Team", GPL version 2 or any later
  version). Its own comment: "Except for the table generation it's all integer
  math". Floating point appears only in `InitTables` (MulTable, sine and
  exponential waves) and `Chip::Setup(rate)` (LFO/noise adds, frequency scale,
  linear and attack rates). Synthesis is per channel in blocks between LFO
  steps and skips channels whose carrier envelope is silent.
- Nuked OPL3 1.8, [opl3.c](https://raw.githubusercontent.com/nukeykt/Nuked-OPL3/master/opl3.c):
  "GNU Lesser General Public License ... either version 2.1 ... or (at your
  option) any later version", Copyright (C) 2013-2020 Nuke.YKT; integer core
  (floating point only in the optional stereo extension); one native sample at
  49716 Hz processes all 36 slots and 18 channels with no silent-channel skip;
  resampling through `rateratio = (samplerate << RSM_FRAC) / 49716`. Die-shot
  verified, bit accurate.
- [libADLMIDI README](https://raw.githubusercontent.com/Wohlstand/libADLMIDI/master/README.md),
  which ships both cores: Nuked OPL3 "Very-accurate, needs more CPU power",
  DOSBox "Well-accurate and fast", "Please use DosBox emulator on mobile
  devices because it requires small CPU power"; its 1.6.0 changelog kept the
  Nuked cores disabled by default "because they are too heavy for ordinary
  processors". VOGONS threads on the DOSBox `oplemu` options
  ([nukeykt's patch post](https://vogons.org/viewtopic.php?p=517858)) give
  listening opinions only, no measurements.
- Yamaha [YMF262 catalog](https://map.grauw.nl/resources/sound/yamaha_ymf262.pdf)
  (contract S5): NEW (105h), four-operator connection (104h), rhythm (BDh),
  C0h bits 4-7 select outputs A-D. The SB16 wires A and B to left and right.
- Licences: the repository is GPL-2.0 (`LICENSE`). DBOPL's
  "version 2 or later" is compatible; the combined kernel is distributed
  under GPL-2.0 with complete source, as `CVSESSION.DLL` already was (see
  `third_party/vsbhda/README.md`). LGPL-2.1-or-later code may also be
  conveyed under GPL-2.0 (LGPL 2.1 section 3), so Nuked would be legally
  usable too; licence does not decide between them.

### OPL3 decision: integer C port of DBOPL in the refill worker

`src/kernel/audio/opl3.c` is a C17 port of the pinned DBOPL (WAVE_TABLEMUL,
the configuration VSBHDA and DOSBox build), keeping the DOSBox Team notice and
GPL-2.0-or-later (SPDX) with the list of changes in its header; one
`struct opl3_chip` (3.4 KiB) per VM. Its floating-point tables are integer
constants for 48000 Hz in `opl3_tables.h`, generated by
`tests/host/test_opl3_tables.py`, which recomputes them with the same IEEE
double formulas and fails on any difference; the kernel file contains no FPU
code and passes the existing FPU/SIMD audit. Synthesis runs at the mixer rate
(48000 Hz, DBOPL's `Setup(48000)`), so OPL output needs no second resampler.
Host evidence compares the port sample for sample with the unmodified C++
(`tests/host/opl3_ref_test.cpp`: OPL2/OPL3 random scripts, every waveform,
four-operator pairs, rhythm, LFO, feedback, mode switches; the C++ int16
accumulation compared modulo 2^16).

Reasons, weighed against Nuked: CPU on the targets (a 1998 i686 must render
up to four VMs and native streams inside the 1 ms service budget; DBOPL skips
silent channels and processes channel blocks, Nuked evaluates 36 slots per
49716 Hz sample and needs a resampler, its own distributors call it too heavy
for ordinary processors); determinism (both are integer and deterministic
once DBOPL's tables are constants; the port is checked bit for bit against the
pinned source, so its output equals the 0.8 line's DBOPL sound); reuse (the
contract names DBOPL; no new third-party code enters the tree). Nuked's
higher accuracy is not needed for the pre-alpha and can be revisited behind
the same `opl3_write`/`opl3_generate` interface.

**Contract deviation for the lead.** The contract says DBOPL "setup/synthesis
runs in a scheduled ring-3 worker with ordinary FPU ownership, replacing the
singleton ring-0/x87 adapter", fed by a bootstrap-delegated channel. Here the
chip runs in the f4-01 P_DEVICE refill worker. The reasons for the ring-3
worker were the x87 table setup and the singleton adapter; both are gone (no
FPU instruction, one chip per VM, inputs are validated register bytes, no
guest memory, PIC or DMA access). A ring-3 worker needs an ABI not yet
specified (`execution-abi.md` channel, scheduling without priority elevation)
and would add a message hop to the 60 ms budget. Fault isolation is the
remaining argument for ring 3; the port's bounds are host-tested under ASan/
UBSan. This is recorded as a decision to confirm or reverse; moving the chip
later only changes `vm_source.c`'s two calls.

### Output path decisions

- Attach and release: `dosvm_create` attaches once the VM is complete and a
  backend is ready (weak hook; no backend leaves f3-05 discarding);
  `dosvm_destroy` revokes before `dosvm_audio_close`, so no fetch can follow
  the page release. Source state is static, four slots in kernel BSS (no
  allocation, the VM ledger and the f3 `devices`/page pins are unchanged).
  attach/detach change only control fields under `irq_save`; the worker owns
  the render state and resets it when the attach epoch changes (the OPL chip
  from a template built once in thread context, a copy instead of 1,500
  register writes in the service). Another consumer's attach (the f3 probe)
  makes the source's next fetch fail and it ends with `revoked`.
- Timeline: the VM timeline and the mixer are both 48 kHz frame clocks, so a
  source maps timeline frame t to output frame t + offset. The offset is set
  at the first quantum: latency = VM clock + queued output - next rendered
  frame = eight periods + 640 frames = 2688 frames (56 ms), inside the 60 ms
  provisional budget. A quantum must start at least 304 frames (one quantum
  and one VM tick) behind the VM clock so f3-05 has produced it. Below that
  floor, or more than 1,024 frames under the target, the source holds:
  output silence without consuming timeline (OPL voices keep sounding).
  More than 1,536 frames over the target it skips timeline forward: events
  apply without synthesis, PCM behind the new position is dropped as late.
  A quantum not rendered for the source (transport restarted) re-anchors,
  never moving back. Holds, skips, re-anchors, lead and lag minima/maxima are
  counters. The worker's catch-up or the backend never changes the VM
  timeline, which f3-05 advances from `dosvm_tick` alone.
- PCM: f3-05 already decodes every advertised layout to S16 stereo at the
  programmed rate; F4 resamples with the f4-01 interpolation
  (`a + (b - a) * frac / 48000`, frac in 1/48000) and keeps frac across
  fetches, quanta and contiguous segments (same rate, start within two frames
  of the stream's next frame position). F4 peeks at the queue head before
  adopting a segment and never fetches from one that does not continue the
  stream: a pause, DMA stall or new transfer starts a new stream at its
  stamped start, after silence. At the end of the available data the last
  frame is held for its fraction; when data ran out inside an open segment or
  while the DSP plays, the output frames are counted `starved`, and frames
  that arrive after their position are dropped and counted `late_frames`
  (the block log marks them). f3-05's `LATE`, `STALE`, `OVERRUN` and
  `UNSUPPORTED` segment flags are counted per source; ADPCM plays f3-05's
  silence and is counted as unsupported.
- Direct DAC (DSP 10h): the 8-bit level is held from its event frame for
  100 ms (4,800 frames), then released to zero, so a stopped program leaves no
  DC offset on the shared output; the speaker latch never gates it.
- OPL events apply in write order at their timeline frame before the affected
  samples (the render splits at each event); an event behind the rendered
  position applies at once and counts `late_events`; a `GAP` reloads the chip
  from `dosvm_audio_opl_registers` (NEW, pairs, operators, channels, rhythm,
  then key-on registers) and skips queued writes already in that snapshot;
  events of another VM generation are refused and counted. OPL2 mode is mono
  on both channels; OPL3 routes C0h bits 4/5 to left/right (C/D outputs are not
  claimed).
- Gains: PCM and DAC use master x voice, OPL master x FM, each times the
  output gain (all Q16 from the guide's dB law), then the source gain/mute
  (default 100, unmuted; focus never mutes), then the f4-01 sum, software
  master and the final saturation. A muted source still consumes. Open point:
  f3-05 resets the CT1745 to the Creative defaults (level 24), so a program
  that never writes the mixer plays at -28 dB relative to a native stream;
  DOSBox resets to 31. A boot-time mixer initialisation in CIUKIDOS (as
  CTMIX/SBMIXER did in AUTOEXEC) or an F3 default change is a lead decision.
- Transport: a source with DSP activity, queued segments or events, a
  sounding OPL voice, a held DAC level or a pending hold keeps the transport
  running; an idle VM lets f4-01's idle tail stop it. When the worker is idle,
  the next VM tick with queued audio wakes it once (`audio_vm_kick` from
  `dosvm_tick`); the worker still blocks between services (f1-18).
- Idle tail with a VM attached (fix after QEMU campaign 12, image e0af9210):
  the 28-period tail burst queued ~150 ms of silence; a VM transfer starting
  inside the tail (VMPLAY stage 3, 150 ms after stage 2) made the service stop
  refilling until the queue drained to eight periods, and the source then
  skipped that timeline and dropped the PCM as late (QEMU: 2 of 8 blocks,
  594 frames; the capture holds only the end of block 5 and blocks 6-7).
  While any VM source is attached the tail keeps eight periods, one per
  service (`audio_refill_lead` in `ciuki/audio.h`, shared by `audio_service`
  and the host model); an underrun inside it is the normal tail stop. The
  native-only tail is unchanged. Host evidence: `test_vm_idle_tail` in
  `tests/host/dosvm_vmaudio_test.inc` (AC'97 DMA modelled per period, the
  production refill policy, f3-05 devices and source): with the old policy
  0 of 8 blocks rendered (512 late frames, one 4,560-frame skip, 28 periods
  queued); with the fix 8 of 8, 2,048 frames, no late frame, skip or hold.
- Evidence: per-source counters, a 64-entry block log (sequence, first frame,
  frames, flags), PCM and OPL path taps after the CT1745/source gains (non-zero
  frames, FNV-1a over int16 L/R, sums of squares, peaks, negative-to-non-negative
  crossings), a digest of the first 4,096 OPL frames of a note, and a ring tap of
  the final frames handed to the AC'97 ring.

### Probe `f4:vm-audio`

Selector-only F4 probe (selector, loader menu, loader model, record scope and
the runner's not-run rule updated). Without a ready backend it ends `READY
case=backend status=not_run` and `ERROR status=not_run` (`device_absent`,
`safe_mode`, ...), as `audio-native`. Otherwise it boots one VM (records
`vm-audio`), checks the attached source and types `\VMPLAY.COM`
(`src/probes/dos/vm_play.asm`, in `DOSBOOT.IMG`; DSP/DMA/ISR routines copied
unchanged from `VMAUDIO.COM`). Stages through the mailbox at 0000:0600:
1 detection, CT1745 master/voice/FM at 31; 2 the regression, 8-bit unsigned
mono at 8000 Hz, 64-unit auto-init double buffer over 2048 units with ISR
refill (block k holds 20h+k) and DAh from the ISR; 3 16-bit signed stereo at
12000 Hz, 8 blocks, voice at 25 (-12 dB); 4 OPL2 440 Hz (AM pair, modulator
TL 63) held 11 BIOS ticks; 5 OPL3 NEW, the same channel at 880 Hz on output A
only; 6 restore and exit. Records: `memory`, `identity`, `transport`
(`latency_frames=2688`), `attach`, `detect`, `pcm8` (32 fresh blocks in
sequence, no repeated first frame, 32 services/boundaries, 12,288 rendered
frames = 2048 x 6, peak 24576), `pcm8_digest`, `pcm16` (2,048 frames; peaks
454/455 = 1809/-1810 x Q16 16462), `opl2`/`opl3` (frames, crossings frequency
x10, RMS, peaks; OPL3 right channel zero), `opl2_digest`/`opl3_digest` (head
digest, events 15/7), `pacing`, `accounting`, `exit`, `clock`, `cleanup`
(VM ledger restored, source detached, native pages unchanged). Every record is
below 256 bytes. Suite `f4-audio.json`: `vm-audio-qemu-t23`,
`vm-audio-qemu-e500`, `vm-audio-qemu-min128`, AC97 by case device exception
(the e500 twin has no audio device of its own; this is a protocol twin, not
ESS evidence), timeout 300 s, no WAV expectations (the capture is deleted on
pass); the OPL RMS values, lead/lag extremes, holds, the deadline count and
`max_service_us` are `PIN-FROM-FIRST-RUN`; digests and peaks come from the
pre-QEMU run recorded below.

### Host and static verification (2026-10-11, this worktree)

- `tests/host/dosvm_vmaudio_test.inc` (in the production DOS VM harness; the
  VM sources, the OPL3 port and the f4-01 mixer linked as their own units
  through `tests/host/vm_audio_shim.h`; ASan/UBSan): attach only with a backend,
  revocation by a second consumer, release before the queue pages are freed;
  all 16 PCM layouts (8/16-bit, signed/unsigned, mono/stereo, single/auto-init)
  exact at 24000 Hz against the f4-01 interpolation; DSP 10h held 4,800
  frames; ADPCM counted, silent; the regression through the whole output path
  (8-bit, 64-unit auto-init double buffer over 2048 units: 32 logged blocks in
  sequence with fresh first frames, 32 services, one stream, no late or
  starved frame, the output equal frame for frame to the reference) and a
  16-bit ramp over 20 blocks checking the fractional phase across fetches,
  quanta and contiguous segments; a 6 ms pause shorter than the render lag
  keeps its gap (two streams, no starvation); a guest that never services its
  IRQ beside a native stream and a second VM (native consumed every frame, the
  second VM's ramp exact, the stalled VM counted late/stale and silent); hold
  with a frozen VM clock, skip after a 120 ms worker stall, re-anchor after
  missed quanta, and a rising ramp that never goes back; CT1745 master/voice/
  output gain, source gain and mute; OPL3 events against a reference chip
  frame for frame, 440/880 Hz from crossings, OPL3 left-only routing, late
  events after a re-anchor, GAP resynchronisation. Mutations that reset the
  phase at a segment, prefetch across a non-contiguous segment, apply events
  256 frames early or disable the hold each fail these cases.
- `tests/host/opl3_ref_test.cpp`: the port equals the unmodified DBOPL C++ on
  1,157,007 frames (1,133,551 sounding) of OPL2/OPL3 scripts.
  `tests/host/test_opl3_tables.py`: the generated header equals its formulas.
- `bash scripts/test/host_kernel_tests.sh`: rc=0 (dosvm harness 1,629,144
  checks, max record 212 bytes; f4-01 audio 10,481,668 checks; OPL3 port PASS),
  with the f3-04 libx86emu guest runner skipped. With `DOSBOOT.IMG` present
  that runner does not link on the base `b5325f0` (it was written before f3-02
  and f3-05 and lacks their units and the desktop fakes), independently of
  f4-05.
- `python3 -m unittest discover -s tests/host`: 330 tests, one error that
  also fails on a clean archive of `b5325f0`
  (`test_dosvm.test_suite_needs_every_cycle_and_cleanup`, `devices_before eq
  4`); the f4-05 changes to `test_audio.py` pass.
- `make kernel`: green, FPU/SIMD audit passed. Static additions: 4 x 7,796
  bytes of source slots, a 3,276-byte chip template, the 8 KiB wave table,
  4.5 KiB of render buffers; about 15 KiB of code.
- `python3 scripts/build_dos_image.py`: T1 DOS PASS, 22 payloads,
  `VMPLAY.COM` 1,744 bytes.
- Pre-QEMU end-to-end check (scratch copy of the f3-04 libx86emu runner with
  the missing VM units, not committed): the real `DOSBOOT.IMG` booted, the
  shell ran `\VMPLAY.COM` on the production DOS VM and f3-05 devices, and the
  f4-05 sources rendered it through the mixer with a fake transport: detect
  `aa/0405/f8/f8/f8`, pcm8 32 blocks `-24576` to `-16640` in sequence, 12,288
  frames, digest `99159afd`, ring `f5ca336d` (master 50); pcm16 peaks 454/455,
  digest `0a2f6943`, ring `5a0f7acb`; OPL2 258 crossings on 28,188 frames
  (439 Hz), peak 4090, head `f679c41d`; OPL3 509/0 crossings, right silent,
  head `257cbe85`; no late, starved or skipped frame. Two runs with different
  note lengths gave the same digests. These values are pinned in the suite;
  a QEMU mismatch means a timing dependence to investigate, not a re-pin.
- Cost of the port, host measurement only (Ryzen 7 7800X3D, i686
  `-march=pentiumpro -O2`, 256-frame quanta): silent chip 0.6 ns/frame, one
  voice 5.7, nine OPL2 voices 47.7, eighteen OPL3 voices 98.6. Physical cost on
  the 1998-2004 targets is not measured; it belongs to the T4 service-time
  records (`max_service_us`).

### For the lead

Build the integrated image, then run `python3 scripts/test/run.py f4-audio
--image build/f0/ciukios.img` (the two `audio-native` and the three
`vm-audio-*` cases; a scratch `zz-` suite holding only the latter also works),
one QEMU at a time and capped as usual, and the `f3-vm` audio-state cases on
the same image (on `qemu-t23` the AC97 is present by default, so every f3 VM now gets
a source; the f3 probe's own attach revokes it). Values to pin from the first
run: `opl2`/`opl3` `rms_l`, `pacing` `lead_min`/`lead_max`/`lag_min`/`holds`,
`clock` `deadlines`/`max_service_us`.
