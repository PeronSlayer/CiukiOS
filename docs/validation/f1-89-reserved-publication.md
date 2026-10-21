# F1-89 reserved publication

The cache must admit metadata space before the FAT publication guard. A
bounded reservation pins sector descriptors and accounts for clean sectors
as dirty credits, both globally and per device. Reads may use pinned data;
eviction, layout conversion, reclamation and ordinary writes may not replace
it. Invalidation cancels the reservation without freeing its descriptors.

Publication uses a try-lock and prevalidates the entire set. Contention,
an active barrier, an in-flight write or an undurable predecessor returns
`EAGAIN` without changing any bytes. The caller leaves its logical guard
before waiting or retrying. A successful publication assigns a group to
each image. Writeback submits that image's group, and does not merge across
groups. The next version of a reserved sector is admitted only after the
previous publication's accepted frontier is durable. This preserves the
detached CHAIN image even when LINK changes the same FAT sector. Release
returns unused credits and pins; published dirty bytes retain their normal
writeback/error ownership.

FAT integration must retain DATA, detached CHAIN, LINK, DIRECTORY and FSInfo
boundaries, and the existing special delete/rename boundaries. Cold planning
and cache admission happen outside the namespace guard. Only validation,
cluster reservation and cached publication belong inside it. Borrowed file
and parent identities need pins and generation checks across those waits;
simply clearing `mutating` in the existing VFS_IO macro is unsafe.

## Research (2026-10-21)

Microsoft's *Windows 98 Resource Kit*, chapter 28, sections “Installable File
System Manager”, “VFAT File System” and “Block I/O Subsystem”, distinguishes
IFS access arbitration, reentrant/multithreaded VFAT, and IOS request queues
and completion services. Chapter 10, after Table 10.8, describes VFAT lazy
writes through VCACHE and FSD submission of low-level work to IOS. These
chapters were read in the owner's local extraction (`wrkc28_5.htm` and
`wrkc10_7.htm`); the [chapter 28 public mirror](https://library.thedatadungeon.com/msdn-2000-04/rk98book/html/wrkc28_5.content.htm)
was inaccessible to the web tool. Neither chapter specifies the CiukiOS
20 ms bound or VFAT's exact internal lock algorithm.

The Microsoft DDK's [request routing](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F6E.HTM),
[IOS services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM),
[driver registration](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM)
and [Ring0 file API](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
were checked online as primary-text mirrors. They establish handle/context
and request lifetimes, not a requirement to retain a namespace lock through
device completion. This is Windows 95 DDK ancestry, not a Windows 98 source
listing.

Linux's [JBD2 documentation](https://docs.kernel.org/filesystems/journalling.html)
is an engineering precedent for reserving credits before updates, treating
admission as a blocking operation in lock ordering, and delaying freed-block
reuse until commit. CiukiOS remains nonjournaled FAT; its binding C2/C3
contract is `docs/design/win98-class-resource-management.md`. The directive's
`resource-management-win98.md` filename does not exist in this checkout.

## Concurrency completion (2026-10-21 19:28 CEST, second implementer session)

The first session's transactions validated every unguarded phase against
one volume-wide `mutation_sequence` and returned `EAGAIN` to the public
caller on any foreign publication. Under real concurrency that surfaced as
spurious failures: an unrelated `create` made a running `vfs_write` fail,
and a path lookup failed whenever any file was written meanwhile. The
completion keeps the reserve/transfer/publish steps and changes only who
may publish and how readers validate:

- **Writer turn** (`vfs_turn_*`, `struct vfs`): one FAT-mutating operation
  per drive letter owns the turn for its whole life, including unguarded
  transfer and durability waits. It is taken before volume admission, with
  namespace only (order turn -> namespace -> volumes -> cache); a waiter
  holds no ticket or reservation; queued writers are not overtaken by a
  newcomer. All public DOS/native mutating entry points take it up front;
  node-based mutations reached from close paths take it inside
  `vfs_fat_transaction_locked`. Readers (open-file reads, lookups, stat,
  find) never take or wait for it. `fat_set_metadata` callers became
  `FAT_TX_SET_METADATA` transactions.
- **Reader validation by value**: a read-only lookup records every sector it
  reads (up to 32, with the bytes seen; a repeated read sees the same copy)
  and, at retirement under the guard, compares them with the resident cache
  bytes. Unrelated publications (file data, other directories, other FAT
  entries) no longer invalidate it. Each component also rereads the entry
  that names its directory, so renaming, emptying or removing an ancestor
  during the walk restarts the walk from the root. The third attempt runs
  under the short guard. `chain_stamp` became an atomic counter: the old
  guard step per decoded directory entry was the main restart source.
- **Publication steps** wait for metadata holders and queued tickets only,
  not for private open-file readers of other identities (the target's
  readers are refused by its writer flag and node pin and drained at the
  operation's admission). The in-flight cluster set uses a namespace-only
  claim that excludes legacy `VFS_IO` mutators.

Research for these decisions:

- Microsoft, *Windows 98 Resource Kit*, chapter 28, "VFAT File System":
  "Because it is reentrant and multithreaded, it provides smoother
  multitasking performance"; IFS Manager "is responsible for arbitrating
  access to different file system components"; IOS "is responsible for the
  queuing of file service requests" ([mirror](https://library.thedatadungeon.com/msdn-2000-04/rk98book/html/wrkc28_5.content.htm),
  checked against the owner's local extraction). Chapter 10: VFAT lazy
  writes through VCACHE.
- Stan Mitchell, *Inside the Windows 95 File System*, O'Reilly 1997,
  chapter 9 ([scan](https://bitsavers.mirrorservice.org/pdf/microsoft/windows_95/Mitchell_-_Inside_the_Windows_95_File_System_1997.pdf)):
  pseudocode recovered from VFAT shows `FS_OpenFile` claiming a VFAT
  critical section (`_Claim_Level1`, `_Claim_Level2` for mount) at entry
  and releasing it at exit, so the claim spans the operation's own block
  I/O; directory cache blocks are pinned with `VCache_Hold`/`VCache_Unhold`
  while in use. This is Windows 95 VFAT read from disassembly, not a
  Windows 98 specification; it supports an operation-long writer claim and
  cache pins, and says nothing that requires readers to wait for it.
- Linux pathname lookup ([docs.kernel.org](https://docs.kernel.org/filesystems/path-lookup.html)):
  RCU-walk samples a sequence count, validates it later, and falls back to
  the locking REF-walk when validation fails; `lookup_slow` takes the
  directory's `i_rwsem` shared while modifications take it exclusively.
  Engineering precedent for the optimistic lookup with a locked third
  attempt; CiukiOS validates by value instead of a per-dentry sequence.
- Linux JBD2 ([docs.kernel.org](https://docs.kernel.org/filesystems/journalling.html)):
  credits are reserved at `jbd2_journal_start()`, which must be ordered
  like a semaphore. Precedent for reserving cache credits before the guard.
- McKusick and Ganger, *Soft Updates*, USENIX 1999
  ([paper](https://www.usenix.org/legacy/publications/library/proceedings/usenix99/full_papers/mckusick/mckusick.pdf)):
  ordered metadata updates with explicit dependencies; precedent for the
  per-group `order` carried by each published cache image.

Decision recorded for the lead: writers of one drive are serialized for
the operation's life (as VFAT's claim is), readers are not. Concurrent
writers on one drive would need value-validated, mergeable FAT/directory
publication (byte-level merge of disjoint entries, delta-applied free
counts); that is not implemented here.
