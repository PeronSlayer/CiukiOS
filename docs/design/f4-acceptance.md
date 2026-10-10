# F4 implementation and acceptance evidence

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
