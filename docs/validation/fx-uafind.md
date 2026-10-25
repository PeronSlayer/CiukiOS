# User-address extent index

Research and implementation decision, 2026-10-25 (directive `fx-uafind`).

Primary sources:

- Microsoft's [Windows Research Kernel address support](https://github.com/mic101/windows/blob/master/WRK-v1.2/base/ntos/mm/addrsup.c)
  describes an AVL tree specialized for VADs, with caller-owned nodes and hints.
  This is a structural precedent only; no source is copied or vendored.
- Microsoft's [!vad documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-vad)
  exposes the per-process range tree and its depths.
- [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)
  describes forward regions grouped by allocation, state and protection, including
  free holes and COW views retaining their mapped/image type.
- [VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect)
  requires validation of the complete committed range within one reservation;
  guards are one-shot alarms.
- The locally readable Windows 98 Resource Kit, chapter 28, `wrkc28_4.htm`,
  “Memory Paging”, describes Win98 demand paging and private/shared address
  spaces. It does not specify a VAD tree. NT's index is a data-structure precedent,
  not a replacement of the existing Win98-style memory policy.

Repository inspection found a sorted singly linked list, a head-to-address walk
in every `ua_find`, head scans for overlap and free-hole queries, and repeated
predecessor scans on removal. All per-page copy/residency/fault paths call
`ua_find`. Protection and backing operations already pin their ranges across
service points.

Decision: retain the ordered list and existing range/object ownership; add an
intrusive AVL index, predecessor links and one last-hit pointer per address
space. Point and successor searches and rotations are bounded logarithmic work
without allocation or service points on this cooperative UP kernel. Range
mutation walks seek to the first affected extent, then follow ordered links
under their existing pins; point validation uses the same logarithmic index.
All insertion, split, removal, rollback and coalescing sites maintain both
representations and invalidate the cache before retiring a node. First-fit
placement and genuine whole-space enumeration remain ordered walks; neither is
a per-page address lookup. No ABI, protection policy, backing policy, lock order,
mapping limit or panic path is added.

Target-layout compilation with the canonical i686 flags measures
`sizeof(ua_extent)` as 96 -> 116 bytes and `sizeof(uaddr)` as 72 -> 80 bytes.
Both extent sizes still fit the existing 128-byte heap class including its
header. The index adds no separate allocation and has no fixed extent array.
An ordered-list/tree differential test checks parent links, heights, balance,
ordering, predecessor/successor consistency, count, bounds and cache membership
after every mutation. Independent native and Win32 page-state oracles cover
splits, overlaps, holes, decommit/recommit and coalescing.

Median host wall times of three runs of the same optimized production replay:

| Extents | 200,000 random hit/miss lookups, before -> after | 48 MiB `copy_from_user`, 64 KiB chunks, before -> after |
| --- | --- | --- |
| 500 | 137.317 -> 6.377 ms | 80.276 -> 1.218 ms |
| 2,000 | 746.559 -> 9.747 ms | 316.505 -> 1.214 ms |

The profile includes actual retained shared views, guards, reservations,
image/heap/stack metadata and a committed 48 MiB archive split over 24 extents.
The existing copy fixture transfers through production uaddr backing rather
than executing the architecture's user-copy assembly. The Win32 core's five
tests passed; the loader's 137-image corpus passed, and its sole missing-cache
test passed after restoring SHA-256-verified qualified PEs. Native proc, file,
signal, desktop, reclaim and new index assertions passed, but their canonical
LeakSanitizer exit checks cannot run in this sandbox (ptrace restriction).
The extra pager test stalls after its concurrent-fault race marker with both
the original and indexed implementation; it is not a passing qualification.

Host evidence and before/after replay results are recorded in
`build/codex/fx-uafind-report.md`. The replay uses the existing 32x host-time T23
estimate; it does not establish hardware speed or identify the trial's actual
kernel CPU attribution. Runtime integration and hardware qualification belong
to the lead.
