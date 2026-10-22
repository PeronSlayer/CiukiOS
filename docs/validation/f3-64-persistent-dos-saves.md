# F3-64 persistent DOS game data

The implementation follows the unified DOS contract's kernel/base/content
namespaces and resource contract C2/D1. The previous provider created a
generation-specific scratch file, kept its only index in RAM, and unlinked it
at every close. It could not preserve a saved game across launches.

Primary sources consulted before implementation:

- [Microsoft Windows 95 DDK, IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM):
  protected file I/O and explicit offsets belong to the filesystem manager;
  DOS calls and raw physical-disk access are distinct interfaces.
- [Microsoft MS-DOS 3.3 Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/),
  file handles, close and disk reset: buffers and directory metadata must be
  drained; a successful buffered write alone is not a durability guarantee.
- Owner-provided Windows 98 Resource Kit, `wrkc10_9.htm`, table 10.10,
  and `wrkc28_5.htm`: disabling synchronous commits weakens reliability;
  protected filesystem ownership is retained. This research-only media is
  read from the lead's `build/rskt`; none of its text is redistributed.

Decision: retain the existing logical FAT/provider and shared native cache.
For title sessions, append immutable sector records and checksummed commit
records. Flush data before publishing a commit, then flush the commit. Replay
only complete validated commit frontiers; never overwrite a committed sector.
An explicit close seals the frontier. Identity changes preserve the old file
under a date-based, collision-checked archive name before creating a fresh one.
The 64 MiB quota is a bound on the actual overlay file, including metadata and
history; reaching it refuses further writes while reads and sealing remain
available. It is CiukiOS policy, not a Windows constant.

Physical T23 free-space measurement and power-cut qualification belong to the
lead. This worktree supplies host evidence and does not claim those results.

## Runtime format and lifetime

`C:\SYSTEM\DOS\SAVES\G<title>K<kernel><base-sha256>.COW` is the active
namespace. The full base hash covers the guest kernel, filesystem identity,
and installed content; it is computed through the existing deny-write open
and version-keyed hash cache, with the existing unified sidecar check.
The header additionally freezes geometry and the filesystem adapter identity
`DFV2-FAT16-1`. Unmatched namespaces and incompatible headers become
`G<title>-YYYYMMDD-HHMMSS-<collision>.BAK`; archives are never reattached or
silently deleted. A failed preservation refuses writable reuse.

| Record | Size | Validation |
| --- | --- | --- |
| `CSAVE01` header | 512 bytes | Version, title, kernel, geometry, filesystem identity, full content hash; SHA-256 at byte 480 |
| `CSDAT01` metadata + sector | 1024 bytes | Record offset, LBA, sector SHA-256 and metadata checksum |
| `CSCOM01` commit/seal | 512 bytes | Record offset, preceding committed end, seal flag, cumulative chain SHA-256 and metadata checksum |

All integers are little endian. Record boundaries are sector aligned. Existing
committed bytes remain immutable. Recovery validates the chain, then rebuilds
the index through the last valid commit, then trims only the uncommitted tail.
Read/transport errors propagate without treating unreadable data as a valid
empty overlay. A read-only system can replay existing data without truncating.

The existing disk gate serializes the append, index publication and finite
flush frontier. Orderly VM teardown first quiesces its logical FAT mount,
then seals. Forced teardown suppresses logical-provider commit publication;
its outer close may flush an uncommitted tail, which recovery ignores.
`forced-session-end` records include pending records or recovered tail bytes,
the retained committed frontier, and `volatile_loss=unknown` for application
buffers that never reached the provider.

The index allocates 4 KiB pages only for populated LBA ranges. VM pages are
charged to `MM_VM_PRIVATE`, its existing commitment/residency budget and
overlay-row ledger, and freed during teardown. Neither VM memory caps nor
the shared file-cache policy changes. Interactive shell and explicit probe
fixtures retain their existing transient lifetime.

## Controls and quota

Right-click a title in **Programs > Games**, or select it there and press
**Shift+F10**, to open its Properties. The data panel shows active file bytes
and the 64 MiB limit. It adds no DOS kernel, memory or network settings.
Requests run on the desktop's existing sleeping discovery worker. Reset
requires a separate confirmation whose default action is Cancel. Reset and
export hold the existing title-removal reservation, excluding active games,
launches and uninstall; query remains available while a game runs.

Export mounts the committed merged view and its immutable base read-only
through the existing FAT parser. It copies only existing files whose bytes
or metadata differ, preserving their relative paths in a new
`C:\Documents\Saves\G<title>-<number>` folder, without overwriting previous
exports. It includes new files; deleted files have no bytes to copy. A
failed export is reported as incomplete and leaves the source intact.

Quota counts the actual active file, including superseded record history;
there is no automatic compaction. Two marker slots are reserved before each
sector append so committing and sealing a full session remain possible.
Quota rejection issues no sector write. It returns ENOSPC once, then EROFS;
reads remain available. Native disk exhaustion also reports full. The native
client displays a clear notice and returns from full screen to show it.
Reset removes active overlays; identity archives remain preserved. Uninstall
cleans active persistent overlays as well as old transient overlays.

## Host evidence and limits

`tests/host/dos/run_persistent_saves.sh` runs the production journal,
namespace, management API and FAT/VFS composition with ASan/UBSan and default
leak detection. It covers 54 journal-level creation/write/flush cuts and 60
cuts at every physical write of a two-sector transaction on the outer FAT
volume (30 writes, lost/torn variants). Recovery admits both old sectors or
both new sectors. It also covers more than 1024 indexed sectors, the final
LBA, save/close/reboot/reopen, forced tails, base/kernel/filesystem mismatch
preservation, archive-name collisions, unchanged-file exclusion from export,
title isolation, active-title reservations, quota and reset.

The VM harness additionally exercises the production provider, cache merge
and invalidation, forced flush suppression, close sealing and zero index-page
teardown ledgers. Desktop tests exercise keyboard access, query, size display,
confirmation/cancel, busy refusal, export status and full notices in both
windowed and full-screen sessions. The changed kernel units compile with
canonical i686 flags, and changed app units compile against the native SDK
with this branch's ABI header.

The complete filesystem suite passed, including 4678 generic crash cuts,
fsck and independent mtools comparisons. DOS Python tests passed (79 tests,
4 skips), and H49/H50 file/continuation/admission tests passed. The ABI oracle
passed 1790 checks before LeakSanitizer's exit inspection was blocked by the
sandbox. Required sanitizer-enabled runs retain their leak settings; their
nonzero exit status is not reported as a clean sanitizer pass.

Two unrelated baseline failures were reproduced with unchanged `HEAD`
sources: DOS window loading measures 6500 ms against its existing 3000 ms
assertion; the complete VM harness fails the existing 35/30-frame cadence
assertion in `dosvm_speed_test.inc`. Neither assertion was changed. H56/H57
runtime qualification is blocked because the sandbox refuses execution of
the pinned 32-bit Open Watcom tool (`Bad system call`). Full kernel-host
qualification stops on the existing ACPICA LeakSanitizer inspection blocks.
Exact commands, results and remaining lead checks are in
`build/codex/f3-64-report.md`.
