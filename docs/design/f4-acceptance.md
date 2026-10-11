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
