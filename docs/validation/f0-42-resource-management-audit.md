# F0-42 resource-management research and gap audit

Research/audit date: 2026-10-19. Source baseline:
`21eec37245463bd9676ac7025c93745c702b3b31`, branch `wt/f0-42`.
Directive: [f0-42](../directives/f0-42-windows-98-class-resource-management-audit-and-contract.md).
Target: [proposed contract](../design/win98-class-resource-management.md).
Status: lead cross-review is recorded in the contract; f0-42b adds the
licensed 1998 Resource Kit reading on 2026-10-19. Historical-source gaps
still listed below prevent a claim of exact Windows 98/ME policy equivalence.
This is a documentation delivery, not an implementation or qualification.

## Source register and provenance

The following register records what was actually accessible. Microsoft text
on an archive host remains a primary document; the host is not its author.
Windows 95 documentation establishes ancestry, not an unverified numerical
claim about every Windows 98 SE/ME build. Modern NT documentation and tuning
forums were not used as evidence of 9x kernel policy. Search-index excerpts
are distinguished from full topic access. No Microsoft implementation or
book text is imported into the repository.

| ID | Document, URL and access | Facts used / limit |
| --- | --- | --- |
| P01 | Microsoft, Windows 98 Resource Kit, ch. 26, Performance Tuning / optimizing Windows 98: [original TechNet address](https://technet.microsoft.com/en-us/library/cc768196.aspx); [archived chapter excerpt](https://www.oocities.org/~budallen/98performance.html) | Original endpoint unavailable; indexed excerpt read, full chapter retrieval timed out. Dynamic cache and swap sizing respond to application demand; MapCache avoids a second executable-page copy. Does not establish internal thresholds. |
| P02 | Microsoft Windows 98 DDK, Related Driver Documentation → Windows 95 Documentation, Kernel Services / VMM / IOS / IFSMgr; [distribution mirror](https://www.mdgx.com/spx/98DDK.RAR), [original documentation root](http://www.microsoft.com/ddk/ddkdocs/win98ddk/) | Full 98 DDK help bundle could not be retrieved. Accessible inherited topics P06–P11 were read separately. No claim to have read the complete 98 VMM service reference or its service list. |
| P03 | Microsoft [KB Q108079, 32-Bit File Access Maximum Cache Size](https://jeffpar.github.io/kbarchive/kb/108/Q108079/) | `MinFileCache` and `MaxFileCache` are KiB-valued SYSTEM.INI bounds. Article applies to WfW/95; its old tested sizes are not 98 defaults. |
| P04 | Microsoft [KB Q253912, large-RAM out-of-memory errors](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/253912) | Article text read through search index; direct request returned 403. Cache maximum is determined at boot and reserves system-arena virtual addresses, potentially preventing DOS VM creation. Internal ceiling 800 MiB; workaround ≤524288 KiB. Applies to 95/98/SE/ME. |
| P05 | Microsoft [KB Q223294, PageFile_Call_Async_Manager](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/223294) | Article text read. Windows 98 asynchronously prewrites swap through VFAT's idle callback after pending VFAT writes drain. `ConservativeSwapfileUsage=1` disables this path; defaults: 95=1, 98=0. This does not mean “never swap until all RAM is used.” |
| P06 | Microsoft DDK, [IOS Services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM) | Full topic read. IOP/DCB allocation, calldown insertion, device arrival/removal, association and removal queries are explicit services with packet/result ownership. |
| P07 | Microsoft DDK, [load groups](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM), [DRP](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S302E.HTM), [port-driver registration](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F87.HTM) | Full topics read. IOS initializes lower layers first; volume tracker, type-specific, vendor and port layers have distinct registrations; configuration/timeouts are asynchronous events. |
| P08 | Microsoft DDK, [Virtual Devices](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S22EB.HTM) | Full topic read. VPICD dispatches registered interrupt/return callbacks; VxDs virtualize ports, memory and BIOS services for V86 clients. |
| P09 | Microsoft DDK, [Deadlocks](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S31A7.HTM) | Full topic read. Ring-0 and ring-3 readiness both matter. Blocking in an event/timeout or while owning the global critical section can halt multitasking; event pumping is not a substitute for blocking. |
| P10 | Microsoft DDK, [IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM) | Full topic read. Context-sensitive/global handles, LFN operations, record locks and distinct no-cache/swapper flags; swap I/O requires special deadlock prevention. |
| P11 | Ruediger Asche / Microsoft, [What's New in Windows 95 for VxD Writers?](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm), updated 1995-10-16 | Full article read. Thread-specific events/lifecycle callbacks extend VM services. Dynamically loadable VxDs do not gain access to initialization-only services. |
| P12 | Microsoft [KB Q196550, ESDI_506 IDE passthrough](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/196550) | Indexed article text read. IOP/IOR, register payload and buffers must be locked; IOS submission has explicit completion status and synchronous/high-priority flags. Not evidence of a universal queue-latency guarantee. |
| P13 | Microsoft [KB Q192607, VCache.exe](https://helparchive.huntertur.net/document/108084) | Full KB read; linked executable unavailable. Identifies the 111 KiB VCache.doc supplement to KERNEL.DOC / VCACHE.H. Its internal policy details were not retrieved. |
| P14 | Microsoft [KB Q192602, IOSFAQ.exe](https://helparchive.huntertur.net/document/108080) | Full KB read; linked executable unavailable. Identifies IOS_Port_DriverFAQ.doc (85 KiB). Its contents are not treated as read. |
| P15 | Microsoft [KB Q169592, timeout services are asynchronous](https://helparchive.huntertur.net/document/37467) | Full KB read. `Set_VM_Time_Out`, `Set_Thread_Time_Out`, `Set_Async_Time_Out`, `Set_Global_Time_Out`, `Cancel_Time_Out` have asynchronous-service semantics. |
| P16 | Historical Microsoft SDK, [timeGetTime](https://techshelps.github.io/MSDN/MULTIMED/devdoc/live/pdref/mmfunc_2q3p.htm) | Full topic read. Windows 95 reports 1 ms default precision; DWORD time wraps after approximately 49.71 days. Precision is not callback latency or scheduler quantum. |
| P17 | Historical Microsoft SDK, [Priority Inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm) | Full topic read. Windows 95 boosts an identified low-priority dependency owner to the waiting high-priority thread's priority. This differs from the NT heuristic described alongside it. |
| P18 | Matt Pietrek, Windows 95 System Programming Secrets, pp. 124–142, [book text](https://manualzz.com/doc/46388115/microsoft-windows-system-programming-secrets-manual) | Relevant text read. VMM schedules threads, not processes; 32 priority levels, classes and foreground distinction; nominal 20 ms slice attributed to Microsoft; temporary I/O-completion boost. Win95 observations, not measured 98/ME timings. |
| P19 | Walter Oney, Systems Programming for Windows 95 (Microsoft Press, 1996), [catalogue/scan](https://archive.org/details/systemsprogrammi00oney); [author's MSJ driver article](https://jacobfilipp.com/MSJ/1995/1995-12.pdf) | Catalogue and indexed author article read. Book scan is access-restricted and full text unavailable. No VMM/IOS numerical policy attributed to the unread book. Must still be consulted for the named literature requirement. |
| P20 | Andrew Schulman, Unauthorized Windows 95 (1994), [book](https://archive.org/details/unauthorizedwind00schu_1), [author's update](https://ftp.st.ryukoku.ac.jp/pub/published/oreilly/windows/win95.update/schulman.html) | Author's update read; full book text unavailable. Its pre-release findings require version checks; no 98 timings inferred from the book. Full relevant chapters remain required. |
| P21 | Adrian King / Microsoft Press, Inside Windows 95, physical/virtual-memory discussion, [scan](https://ftpmirror.your.org/pub/misc/bitsavers/pdf/microsoft/windows_95/King_Inside_Windows_95_1994.pdf) | Indexed original excerpts read; full PDF unavailable. Describes global competition for RAM and LRU-style replacement based on access history, rather than NT per-process working-set guarantees. |
| P22 | Intel, [Bus Master IDE programming interface 1.0](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/IDE-BusMaster.pdf), §§1–3 | Full specification read. Eight-byte PRDs, even addresses/counts, zero count=64 KiB; neither a region nor its dword-aligned descriptor table crosses 64 KiB; stop/completion ordering matters. |
| P23 | Microsoft, [FAT specification, 2005 SDA contribution](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf), FAT/FSInfo/directory sections | Relevant specification text read. Cluster chains, advisory free/next-free hints and LFN entries. This is an on-disk specification, not proof of VFAT lazy-write crash ordering. |
| P24 | Intel, [IA-32 SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), Vol. 3A paging | Official manual catalogue checked. Architectural reference for present/write/accessed/dirty bits and TLB invalidation; exact exception/locking design must be checked against the relevant manual sections in f0-47. |
| P25 | Mark Russinovich / Bryce Cogswell, [Examining VxD Service Hooking](https://jacobfilipp.com/DrDobbs/articles/DDJ/1996/9605/9605c/9605c.htm), May 1996 | Full original investigation read. VCMon observes cache demand, hit/miss/free-memory/page-fault feedback used to resize VCACHE. No fixed Win98 sampling period established. |
| P26 | Stan Mitchell, Inside the Windows 95 File System (1997), ch. 10–11, [book text](https://pdfcoffee.com/inside-the-windows95-file-system-3-pdf-free.html) | Relevant original investigation read. Page-based VCACHE reservations/commit, held blocks and memory-manager cooperation; VFAT is a client, not simply a raw cache synonym. Win95 reverse-engineered constants are not transplanted into 98. |
| P27 | Creative, [Sound Blaster hardware programming guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf), DMA chapters | Relevant hardware reference read. ISA DMA channel/address/count constraints differ from PCI IDE PRDs; audio buffers need stable DMA ownership. |
| P28 | Microsoft Windows 98 Resource Kit Book Online, 1998 CD, ch. 26, **Performance Tuning Overview** (`build/rskt/wrkc26_1.htm`) and **Optimizing File System Performance** (`wrkc26_4.htm`); ch. 28, **Installable File Systems → File System Drivers → Virtual File Allocation Table** (`wrkc28_5.htm`) | Full relevant topics read locally. Cache adapts to demand/free memory, shrinks during heavy paging; growth with network use and shrink as applications start are described. No numerical growth/reclaim thresholds, cadence, or chunk increment. `MinFileCache`/`MaxFileCache` are not documented in these extracted books; P03 remains the version-limited source for their units. |
| P29 | Same edition, ch. 26, **Optimizing File System Performance → Optimizing File System Performance with Profiles / Using File System Troubleshooting Options** (`build/rskt/wrkc26_4.htm`); ch. 10, **File System Management** (`wrkc10_7.htm`) | VFAT lazy writes; `BufferIdleTimeout`, `BufferAgeTimeout`, `VolumeIdleTimeout` are named without values. Commit ordinarily writes to disk before returning; disabling synchronous commits weakens this. `DriveWriteBehind=0` disables write-behind for all drives. The spelling `AsyncFileCommit` does not occur; no registry encoding inferred for the commit checkbox. |
| P30 | Same edition, ch. 26, **Optimizing File System Performance → Optimizing Removable Disk Drive Performance** (`build/rskt/wrkc26_4.htm`) | Documents enabling/disabling removable write-behind (ZIP/JAZ examples). Does not explicitly state the installed checkbox default or SE/ME exceptions. |
| P31 | Same edition, ch. 10, **Technical Notes on Disk Device Support → VCACHE and CDFS Supplemental Cache** (`build/rskt/wrkc10_6.htm`); ch. 26, **Optimizing File System Performance → Optimizing CD-ROM File System Performance** (`wrkc26_4.htm`) | File-system read-ahead and separate, pageable CDFS cache established. CDFS table values are not disk `ReadAheadThreshold` values. That setting's name, units, default, range and request-generation rule are not in the extracted book/tools help. |
| P32 | Same edition, ch. 26, **WinAlign and MapCache** (`build/rskt/wrkc26_10.htm`) and **Optimizing File System Performance** (`wrkc26_4.htm`); ch. 28, **Installable File Systems** (`wrkc28_5.htm`) | Aligned executable sections use 4 KB boundaries; mapping from cache avoids duplicate copies of many pages. Mapped files/executables use cache, ordinary swap-file I/O bypasses it. No writable-alias/truncate/raw-write coherency protocol specified. |
| P33 | Same edition, ch. 26, **Optimizing the Swap File** (`build/rskt/wrkc26_3.htm`), **Optimizing File System Performance** (`wrkc26_4.htm`); ch. 28, **Virtual Machine Manager → Memory Paging** (`wrkc28_4.htm`); ch. 31, **Administrating with the Registry → System.ini entries retained and supported in the user interface** (`wrkc31_3.htm`) | Dynamic growth/shrink, idle dirty-memory prewrite and demand paging confirmed. UI minimum/maximum values are in kilobytes. `MinPagingFileSize`/`MaxPagingFileSize` retained under `[386Enh]`; no defaults/increments given. `ConservativeSwapfileUsage` absent; P05 supplies its documented 98 default, not the book. |
| P34 | Same edition, ch. 28, **Virtual Machine Manager → Process Scheduling and Multitasking** (`build/rskt/wrkc28_4.htm`), **Core System Components → User / Kernel** (`wrkc28_8.htm`) | Win32 preemptive threads, cooperative Win16 and asynchronous input queues described without boost magnitudes/decay, starvation policy or quantum. These numbers are not in the Resource Kit. |
| P35 | Same edition, ch. 28, **Understanding Device Drivers → The Virtualization Driver** (`build/rskt/wrkc28_3.htm`), **Core System Components → User** (`wrkc28_8.htm`) | VTD identified as a timer virtualization driver; input interrupts become queued messages. No VPICD service contract, timer resolution, interrupt/wakeup latency bound or VTD scheduling guarantee. |
| P36 | Same edition, ch. 26, **Tracking Performance with System Monitor**, tables 26.7–26.10 (`build/rskt/wrkc26_9.htm`); ch. 26, **Understanding System Performance** (`wrkc26_2.htm`) | Cache recycling, filesystem, CPU/thread/VM, memory and swap counters read in full. LRU recycle searches oldest buffers for new cache data or memory-manager borrowing; random and failed recycles also counted. Resource capacity is distinct from free physical RAM. |
| P37 | Microsoft Windows 98 Resource Kit Tools Help, same CD, `build/rskt2/{winalign,tlist,usbview,filewise,fileinfo,filever,windiff,compreg,reg,srchreg,sredit,task_registry_scanreg,task_registry_using_scanreg,using_system_monitor}.htm`; CD `README.DOC` and `TMC/{PERFORM,CONFIG,FILETOOL,DXTSHOOT,REGTOOL}.HTM` | Tool functions and CD category layout read, not executed. System Monitor/Registry Checker are Windows components documented by the kit, not executables in PERFTOOL. SYSFILES is supporting DLLs/controls, not a separate counter suite. HLP inspection limits below. |
| P38 | Same book, ch. 27, **Troubleshooting Tools → Using Registry Checker** (`build/rskt/wrkc27_2.htm`); ch. 2, **Preparing to Run Setup**, table listing System Tools (`wrkc02_3.htm`) | Registry integrity scan, daily successful-start backup and restore/repair roles; System Resource Meter listed as a component. The book does not supply its detailed counter definitions. |
| P39 | Microsoft [KB300059, How to Use the System Resource Meter Tool in Windows 98 and Windows Millennium Edition](https://www.betaarchive.com/wiki/index.php/Microsoft_KB_Archive/300059) | Supplemental internet corroboration, not CD content: search-index text read; direct retrieval 403. Meter shows free system, USER and GDI resource percentages. Covers 98/SE/ME; does not turn those values into free physical RAM. |

### Research closure: Resource Kit findings (f0-42b, 2026-10-19)

Edition/provenance for every P28–P38 finding: **Microsoft Windows 98
Resource Kit Book Online / Tools Help, English, 1998 CD**, with tool dates
in March–April 1998 and both CHMs dated 1998-04-30. The book's `#SYSTEM`
reports **HHA Version 4.72.8033**, a help-compiler version, **not a Windows
kernel build**. No explicit book revision or tested OS build was found in
the introduction/metadata; do not substitute RTM build 1998 or extend this
edition's claims to SE/ME. Identity is reproducible from the local originals:

| Local input | Bytes | SHA-256 |
| --- | --- | --- |
| `third_party/mswin98rskt/HELP/RK98BOOK.CHM` | 3255721 | `85cbf8e3a82a07009f7f4340912af68629b0f38aba9347343fef6d47c1a5bf17` |
| `third_party/mswin98rskt/HELP/WIN98RK.CHM` | 311429 | `436b33f09bb52bce1bc97855c496b955abea7c00c7a3c4d863064dfb1be88751` |

Inputs were read in the neighboring main checkout
`/home/peronslayer/Desktop/CiukiOS/`; the paths above and P28–P38 paths are
relative to that checkout. No media or extracted text files were copied into
this worktree. `git check-ignore` confirms both media and extraction paths
remain ignored. The book TOC is `build/rskt/rk98book.hhc`.

The following resolves what this edition actually establishes. Short quotes
are literal; other facts are paraphrased. **D** = documented default or
normal behavior, **T** = tunable (not necessarily a stated default), **N** =
description without policy numbers, **U** = not specified in this edition.
An absence closes this media lookup, not the underlying historical question.

| Question | Fact and classification | Source: chapter/topic/file | Effect on contract / remaining evidence |
| --- | --- | --- | --- |
| 1. Cache growth/reclaim thresholds, cadence, bounds, chunks | **N:** “The cache grows and shrinks as needed.” Cache shrinks with heavy paging; network demand can grow it. **N:** oldest-first LRU recycle and random recycle are exposed separately. **U:** growth/shrink thresholds, sample interval and chunk increment; no cache-growth chunk specification found. `MinFileCache`/`MaxFileCache` absent; KiB bounds remain P03 (WfW/95), not a new 98 default. | P28, ch. 26 **Optimizing File System Performance**, `wrkc26_4.htm`; ch. 28 **Installable File Systems**, `wrkc28_5.htm`; P36, ch. 26 **Tracking Performance with System Monitor**, `wrkc26_9.htm`, table 26.7 | C4/M3 dynamic resizing and pressure reclaim **confirmed in role**. Keep 100 ms windows, two-window trigger, 256 KiB steps and 2/8/32 MiB bounds as CiukiOS policy. P02/P13 needed for internal constants. |
| 2. Dirty age, idle delay, removable default, eviction, lazy-write ordering and profiles | **T:** `BufferIdleTimeout`, `BufferAgeTimeout`, `VolumeIdleTimeout` control buffered-write delay, with no numerical values. **D:** ordinary commit writes to disk before returning; async-commit troubleshooting option weakens integrity. **T:** `DriveWriteBehind=0` disables all write-behind; removable write-behind can be enabled/disabled, default unstated. **T:** profile PathCache/NameCache values below. **N:** oldest-first/random cache recycling; no dirty-victim or FAT publication-order specification. `AsyncFileCommit` spelling/value absent. | P29/P30, ch. 26 **Optimizing File System Performance**, `wrkc26_4.htm`, profiles/removable/troubleshooting topics; P36 table 26.7 | C1's 12.5%/1 s cannot be called a Win98 default; retain accepted policy. C2 finite synchronous commit behavior **confirmed in role**; do not enable weak commits. Write-through removable policy retained because no explicit historical default was found. Exact age/delay, dirty eviction and crash ordering still need P02/P13/P14. |
| 3. `ReadAheadThreshold`: units, default, range, generation | **N:** VCACHE supports filesystem read-ahead; CDFS has a distinct cache and parallel reads. **U:** `ReadAheadThreshold` name and all four requested semantics absent from book/tools help. CDFS size table is a different control. | P31, ch. 10 **Technical Notes on Disk Device Support**, `wrkc10_6.htm`, **VCACHE and CDFS Supplemental Cache**; P29, ch. 26 `wrkc26_4.htm`, CD-ROM topic | C4's two adjacent reads, 16–64 KiB stream window and C/8 speculation cap remain CiukiOS policy. Do not label them an implementation of the registry setting or adopt tweak-site values. P02/original registry reference still needed. |
| 4. Mapped files and ordinary I/O coherency | **N:** “memory-mapped files and executable files do” use cache (in contrast to swap-file I/O); MapCache shares many pages instead of duplicating them. **T/mechanism:** WinAlign aligns sections to 4 KB boundaries. **U:** ordering/visibility across ordinary writes, writable mappings, truncate and raw external writes. | P32, ch. 26 **Optimizing File System Performance**, `wrkc26_4.htm`; **WinAlign and MapCache**, `wrkc26_10.htm` | C5 single identity/shared frames **confirmed in role** only. No claim that MapCache alone proves full writable coherence. f1-84 and f0-47 must specify/test aliases and invalidation using P02/P13. |
| 5. Page replacement, swap growth, `ConservativeSwapfileUsage`, `MinPagingFileSize` | **N:** demand paging, idle dirty-memory prewrite, dynamic swap growth/shrink confirmed; no replacement scan, trimming period or growth increment. **T:** UI min/max in kilobytes; `[386Enh]` `MinPagingFileSize` retained, no numerical default. **U:** `ConservativeSwapfileUsage` absent in the book; P05 separately documents 98 default 0 (95 default 1) and 1 disabling idle async prewrite. | P33, ch. 26 **Optimizing the Swap File**, `wrkc26_3.htm`, and `wrkc26_4.htm`; ch. 28 **Virtual Machine Manager**, `wrkc28_4.htm`; ch. 31 **Administrating with the Registry**, `wrkc31_3.htm` | M3 global second chance/64-page slices remain CiukiOS policy. M4 dynamic backing and idle prewrite **confirmed in role**; 16 MiB extents and 30 s shrink remain CiukiOS choices. P05's VFAT-idle ordering retained. Full P02 pager services still needed. |
| 6. Foreground/input/I/O/starvation boosts, decay, 98/ME quantum | **N:** “preemptive multitasking” for Win32, cooperative Win16 and per-thread slices/input queues. **U:** requested boost magnitudes, expiry/decay and quantum **not in the Resource Kit**. | P34, ch. 28 **Virtual Machine Manager → Process Scheduling and Multitasking**, `wrkc28_4.htm`; **Core System Components → User / Kernel**, `wrkc28_8.htm` | R2 stays CiukiOS policy: 10/2 ms quanta; +2/20 ms input, +1/one quantum I/O, 100 ms starvation threshold. P18's 20 ms is Win95 evidence only. Need P02/Oney; no NT values substituted. |
| 7. VTD/VPICD timing | **N:** “a virtualization driver for a timer device is a VTD”; input IRQs feed queued processing. **U:** no VPICD service timing, VTD resolution or interrupt/wakeup guarantee **in the Resource Kit**. | P35, ch. 28 **Understanding Device Drivers → The Virtualization Driver**, `wrkc28_3.htm`; **Core System Components → User**, `wrkc28_8.htm` | R1/R3 100 µs IRQ-off work, 1 kHz PIT and service deadlines remain CiukiOS policy. P15/P16 keep their narrower timeout/Win95 precision meanings. Need P02 timer/interrupt service contexts, not an inferred latency guarantee. |

Profile details (P29, table 26.3 and following bullets; **registry tunables**):

| Typical role | PathCache paths | NameCache names / storage | Other stated behavior |
| --- | --- | --- | --- |
| Desktop computer | 32 | about 677 / 8 KB | Assumes more than minimum RAM and mains power. |
| Mobile or docking system | 16 | about 337 / 4 KB | Assumes limited RAM/battery; frequent flushing described without an interval. |
| Network server | 64 | about 2729 / 16 KB | Assumes adequate RAM and frequent disk activity. |

Templates live under `HKLM\Software\Microsoft\Windows\CurrentVersion\FS Templates`;
active values under `HKLM\System\CurrentControlSet\Control\FileSystem`.
Path/name caches use the general system heap; these are not VCACHE data
budgets. P29 also states a default contiguous-allocation search of **0.5 MB**
(`ContigFileAllocSize`, rendered once with a space in the book), then fallback
to first-available allocation. This is not a dirty threshold, prefetch window
or swap growth increment; any adoption belongs to reviewed allocation work.

Search coverage: relevant chapters 10/26/28/31 and their topic headings were
read; case-insensitive searches of both extracted books' HTML and TOC/index
covered the named settings, chunks, read-ahead, replacement/LRU, priorities,
quantum, VTD and VPICD. `chunk` occurs only in unrelated hypertext/clipboard
descriptions. The directive's suggested chunk-growth description was not
found; neither 4 KB executable alignment nor 4096-byte swap slots proves a
cache growth increment. The removable enabling instructions do not explicitly
establish the installed default. These are limits of this edition, not proof
that the settings or mechanisms do not exist in Windows 98.

Internet corroboration before edits: P03 full Microsoft KB text was read
again; P05 full article text was available through the search index (direct
access still 403), confirming its default/idle-order statements. The original
P01 TechNet endpoint remained inaccessible. No tuning forum or modern NT
documentation supplies missing values. P01 now has the local full-chapter
supplement P28–P36; P02, P13/P14 and P19/P20 reading gaps remain.

### Native diagnostic functions to mirror (P36–P39)

This is input to the H page and Task Manager, not a new implementation
directive or an approval to run/ship Microsoft tools. Counter names below
identify documented measurements; the CiukiOS presentation/units must avoid
inheriting misleading overlaps in the old memory counters.

| Function / original location | Documented measurements or output | CiukiOS use |
| --- | --- | --- |
| System Monitor: Disk Cache, book `wrkc26_9.htm`, table 26.7 | Active buffers (including compressed), hits, misses, current/min/max pages; failed, oldest-first LRU and random recycles | H cache demand/reclaim history; label unsupported compression/random recycle unavailable rather than fabricating zero. |
| System Monitor: File System, table 26.8 | Read/write bytes per second, read/write operations per second, dirty bytes (cache-block-rounded, possibly exceeding actual changed bytes) | H per-device and aggregate throughput/debt; Task Manager owner attribution is a CiukiOS extension. |
| System Monitor: Kernel, table 26.9 | Approximate CPU busy %, thread count, VM count | Task Manager activity and H totals; account observer overhead. |
| System Monitor: Memory Manager VMM32, table 26.10 | Allocated/other/swappable/locked bytes, locked non-cache pages; unused physical bytes; page/instance faults, discards, page-ins, page-outs per second; current/min/mid/max disk cache; pages mapped from cache | H ledger and fault/reclaim rates, Task Manager residency/pins. Page-ins include executable/mapped-file reads, not only swap. Old allocated/locked/swappable figures overlap cache; do not sum them as disjoint physical ownership. |
| System Monitor: swap, table 26.10 | Swap file size, bytes in use, defective bytes; defect granularity 4096 bytes even for one bad sector | H backing capacity/used/bad slots, paging rates; distinguish file size, commitment and live data. |
| System Monitor chart/log function, `wrkc26_9.htm`; tools help `using_system_monitor.htm` | Line/bar/numeric charts, logging, configurable update interval; remote counters via appropriate agent | Local time-series export for trial evidence; remote monitoring remains F6. |
| System Resource Meter, book `wrkc02_3.htm` / resource discussion `wrkc26_2.htm`; supplemental P39 | CD confirms component/resource-capacity distinction. P39 separately identifies free system, USER and GDI resource percentages; their exact calculation is not supplied by this CD | Offer native UI resource/handle pressure and exhaustion diagnostics; clearly label counts/limits and free percentages. Do not equate free RAM with free UI resources or reproduce historical heap limits. |
| Registry Checker (Windows component), `wrkc27_2.htm`; tools help `task_registry_scanreg.htm`, `task_registry_using_scanreg.htm`; `CONFIG/SREDIT.EXE` documented in `sredit.htm` | Consistency scan, successful-start daily backup, restore/repair; SREdit selects backup directory/count, additional files and compaction | Native configuration integrity/recovery status; expose last check/result, last good backup, retained count and restore result as **proposed CiukiOS status fields**, not historical performance counters. |
| `REGISTRY/{REG,COMPREG,SRCHREG}.EXE`, tools help `reg.htm`, `compreg.htm`, `srchreg.htm` / TMC `REGTOOL.HTM` | Registry query/search/export/backup/restore and comparison of keys | Native effective-policy inspection and before/after configuration differences; no periodic performance counters. |
| `PERFTOOL/WINALIGN.EXE`, tools help `winalign.htm`, book `wrkc26_10.htm` | 4 KB executable section alignment; effects observed with mapped-cache pages and swap-in-use counters | Loader/layout diagnostics and mapping reuse counters; no automatic rewriting of installed binaries. |
| `DIAGNOSE/TLIST.EXE`, tools help `tlist.htm`; `USBVIEW.EXE`, `usbview.htm` | PID/name/window title; USB descriptors, connection status and power information from hub drivers | Task Manager process identity, H device/transport context; these are inventories, not latency counters. |
| `DIAGNOSE/{FILEWISE,FILEINFO}.EXE`, tools help `filewise.htm`, `fileinfo.htm`; `FILE/FILEVER.EXE`, `filever.htm` | CRC32/change report and version/language/flags; system-file sizes/dates/install and CAB locations; file version inspection | Native integrity/version manifest and change reports; use CiukiOS files/metadata, never Microsoft's file database. |
| `FILE/WINDIFF.EXE`, tools help `windiff.htm`; `DIAGNOSE/DEPENDS.EXE`, TMC `DXTSHOOT.HTM` | File/directory comparison; module dependency tree | Native comparison/dependency diagnostics; no resource-policy constants derived from these tools. |

CD layout check (`README.DOC`, TMC pages): PERFTOOL contains WinAlign;
SYSFILES contains runtime libraries/controls, not System Monitor binaries.
`7z l` could not open `DIAGNOSE/DEPENDS.HLP` or `FILE/WINDIFF.HLP`; `strings`
produced fragmented compressed text, insufficient for substantive claims.
Their functions above come from readable TMC/Tools Help, not guessed HLP
content. No Windows executable, script in the TMC, or bundled DLL was run.

Contract reconciliation: mechanism-level confirmations above are recorded
beside C1/C2/C4/C5/M3/M4/R2/R3. No contradictory historical dirty/read-ahead
number was found and none was invented. Keep the lead's accepted policies;
adoption candidates and owners are in `build/codex/f0-42b-report.md`.

## Physical evidence register

The diary describes trials of the *previous* image named in each entry,
then the next image produced. The source below is the trial image, not the
subsequent delivery. These existing records were read; no physical test was
performed in this worktree.

| ID | Trial and durable reference | Resource-management observation |
| --- | --- | --- |
| E17 | [2026-10-17-02](../../dev_diary/2026-10-17-02-prova-t23-d598bf4c-chiavetta-schermo-intero-avvio-normale-immagine-43d72a5c.md), trial `d598bf4c`, BOOT1 1765–1955 / BOOT2 371–417 | Desktop no-frame 8715/8884 ms; USB worker 8704 ms; approximately 28 ms per one-sector command. Diary records f1-78 cache transport unlocking and pending f1-79 lock split. Historical defect, not proof that those locks remain global today. |
| E18a | [2026-10-18-01](../../dev_diary/2026-10-18-01-prova-t23-041b221f-irq11-chiusura-doom-isr-seriale-immagine-eca4d773.md), trial `041b221f`, BOOT 391–401 and tail | IRQ11 overrun 2123 µs cascaded into shared-line quarantine; fullscreen-return stretches 151–160 ms and 133 ms; guest IRQ average 11354 µs. Later fixes recorded in that entry are not automatically physical qualification. |
| E18b | [2026-10-18-02](../../dev_diary/2026-10-18-02-prova-t23-eca4d773-acceleratore-lettore-chiavetta-interfaccia-immagine-9af46936.md), trial `eca4d773`, BOOT/BOOT1 | USB 1 MiB in 3073 ms; MP3 open model 7.4 s / 291 reads; guest IRQ average 808–1322 µs after earlier fixes. These identify transport batching, startup reads and guest cadence as separate costs. |
| E19a | [2026-10-19-01](../../dev_diary/2026-10-19-01-prova-t23-9af46936-doom-irq-trasporto-audio-f4-32-immagine-c2d2a237.md), trial `9af46936` | Audio stop/refill-gap analysis, f4-31/32 nominal/adaptive lead conflict; first 4-period replay failed, 8-period replay retained samples. Historical QEMU results here are read-only evidence, not new gates. |
| E19b | [2026-10-19-02](../../dev_diary/2026-10-19-02-prova-t23-c2d2a237-seriale-doom-irqret-stallo-vm-copia-immagine-82fadce3.md), trial `c2d2a237` | 110 ms VM-creation stretch, 113127 µs refill gap, lead 28; IRQ11 2129 µs; Files UI synchronous copy load. f0-41/f2-84 are now in the audited source. |
| E82 | [f1-82 evidence](../directives/f1-82-cache-writeback-reader-writer-concurrency.md), trial `82fadce3`, serial.log | 28 volume waits: 24 C:, four E:. C: maximum 740 ms; overall maximum is **E: 1092 ms**, not 740 ms. Source-copy MP3 playback starved despite a running DMA transport. |

E19b and E82 serial files were additionally inspected read-only in the
neighboring main checkout, under `legacy/local/physical/<image>/serial.log`.
Evidence coordinates use LF-delimited lines, as `rg -n` does; an embedded CR
in the loader output makes generic `splitlines()` coordinates differ.
No raw logs or private filenames are copied into tracked files.

| Trial image SHA-256 | Serial SHA-256 | Checked coordinates |
| --- | --- | --- |
| `c2d2a2372b2f74f44d5bbf3e041cf8b874ca10561a4bf8ca54765969e731253f` | `a4c04ed298cb0fcd3f503621c7af060b360b68b827eb434d80ef93a92b23d24d` | 21: early RAM; 1441: stretch 110; 2594: IRQ11 2129 / 171 violations; 2623–2626: gap 113127 / lead 28 / recovery |
| `82fadce383d2c2d4041ef413762165ceeed4eb50a9bb33f2e2776eec6d27312c` | `d2d0d528df86221fc8a6099720e7290d57ea89865af9104ede59e7c4b32b2ed0` | 21: early RAM; 1785–1788: C: waits 740/467/345/200; 1890: E: 1092 |

Both early RAM records say usable 523260 KiB, free DMA 1676 pages, free
NORMAL 126816 pages. At 4 KiB/page that is 513968 KiB free and a 9292 KiB
difference within reported usable RAM at that instant. This is **not** a
complete post-desktop reservation ledger: it precedes later allocations and
does not classify all firmware/MMIO/heap/DMA/VM commitments. No 128/256 MiB
physical ledger is present in these records. No memory cap is changed here.

## Gap tables

P0 = observed service/integrity blocker or unbounded path directly relevant
to the current trial gate; P1 = required mechanism/correctness work before
complete resource-management parity; P2 = subsequent precision/efficiency
work. A risk inferred from source is explicitly described as unmeasured.
References Pxx resolve to the source register above; E references resolve to
the evidence register. Code paths in table cells are relative to
`src/kernel/`. Row IDs are stable acceptance/report identifiers.

### Storage, cache and filesystem

| ID | Windows model / source | Current CiukiOS file and function | Gap and trial risk | Priority |
| --- | --- | --- | --- | --- |
| S01 | IOS queues and completes requests independently of unrelated file work (P06/P07) | `fs/vfs.c::vfs_reserve_locked`, `vfs_io_begin_locked`; per-letter FIFO reservation survives device I/O | f1-79 separates volumes, but a whole same-volume operation still excludes other files. E82 C: 740 ms / E: 1092 ms. A sleeping FIFO lock is fair but does not bound convoy latency. | P0 |
| S02 | Cache demand/reclaim and deferred write mechanisms (P25/P26); exact 98 dirty thresholds unresolved | `fs/cache.c::get` selects an eligible oldest slot including dirty; `cache_write` / `cache_writeback_tick` use 5000 ms; `fs/mount.c::storage_writeback` | No dirty-byte budget, clean-victim preference or writer credits; miss caller synchronously repays dirty debt while its VFS volume reservation remains held. E82 playback starvation. See correction below. | P0 |
| S03 | Dynamic cache competes for RAM, bounded virtual reservations (P01/P03/P04) | `fs/cache.h::CACHE_DEFAULT_BYTES=8 MiB`; `cache.c::cache_init` allocates once, accepts 8 KiB–32 MiB, counts workspace/descriptors | Runtime use cannot expand/shrink with demand or pressure. 8 MiB is total allocation, not 8 MiB of file payload. No measured OOM from these trials; contention E82 is not proof that simply enlarging cache fixes it. | P1 |
| S04 | Read-ahead and shared backing; exact threshold policy unresolved (P01/P26) | `cache.c::cache_read_run`, `cache_read_ahead`; `fat.c::readsec` performs bounded metadata prefetch | Existing coalesced reads are useful; no general per-stream adaptive data prefetch/cancellation. E17/E18b command overhead; MP3 eager-open cost was also an application issue. | P1 |
| S05 | IFSMgr/VFAT delayed writes, explicit flush/locking; FAT layout is not a crash-order guarantee (P10/P23) | `fat.c::fat_write` barriers at data, detached-chain, tail-link stages; `barrier`, `update`, `fat_commit` | Even without eviction, an append can drain device-wide dirty work at several publication boundaries while holding the volume reservation. Preserve existing crash guarantees; a budget-only fix cannot prove all stalls solved. E82, source-level attribution not a per-barrier physical trace. | P0 |
| S06 | Layered IOS, IOR completions and locked buffers; ESDI_506; PRDs (P06/P07/P12/P22) | `fs/blkdev.h` synchronous callbacks; `drivers/ata_dma.c::ata_dma_prd`, `ata_dma_prepare`, `ata_dma_irq`, `ata_dma_finish` | BM DMA and boundary splitting already exist. Missing common async request queue, deadlines/fairness and page-vector mapping; current noncontiguous buffers bounce. E82 shows queueing need, not a demonstrated PRD corruption. | P1 |
| S07 | Explicit media/removal lifecycle (P06); exact 98 removable write-behind default unresolved | `fs/mount.c::storage_shutdown`, `storage_sync`; `cache.c::cache_invalidate`, device epoch/generation/error | Safe removal and sticky per-device errors exist; proposed dirty budgeting/queues must integrate cancellation and media generations. E18a shared failure cascade corrected; E82 source removal/error remains a correctness risk, not observed data corruption. | P1 |
| S08 | IFSMgr context, sharing/record locks; common cache clients (P10/P26) | `fs/vfs.c` shared node/description claims; `proc/fdtable.c`; `proc/uaddr.c::ua_mmap` rejects file-backed and MAP_SHARED | Preserve existing LFN/share checks. No file-mapped/ordinary-I/O coherence because file mappings are unsupported; byte-range/DOS bridge semantics still need completion. No physical mapped-file defect claimed. | P1 |

**Correction to the f1-82 diagnosis.** At this baseline the helper is named
`get`, not `cache_get`. `cache.c::write_block` marks the block busy, drops
`c->lock`, calls `d->write`, then reacquires it. `barrier` likewise drops the
lock for `d->flush`; `read_transport` does so for reads. Thus the directive's
claim that the mutex stays held across those transports does not describe
this checkout. The five-second whole-device barrier and synchronous dirty
victim eviction are present; the VFS volume reservation still covers the
caller. Cache scans/reacquisition, block-busy waits and channel serialization
can add contention. The logs prove waiting, not which of these accounts for
each millisecond. f1-82 must retain the f1-78 unlocking and instrument these
causes separately. Waiting for dirty credits in `cache_write` while holding
that volume reservation would preserve a reader convoy; the contract proposes
admission outside it and staged publication for lead review.

### Memory

| ID | Windows model / source | Current CiukiOS file and function | Gap and trial risk | Priority |
| --- | --- | --- | --- | --- |
| M01 | VMM coordinates cache/application residency; reserve and resident differ (P01/P04/P26) | `core/pmm.c::pmm_init`, `pmm_free_count`, `pmm_reservations`; `include/ciuki/mm.h` | E820 bitmap and zones, 768 MiB direct-map ceiling, partial ledgers; no unified committed/resident/reclaimable/pinned/backed accounting or pressure controller. Early E19b/E82 snapshot above cannot justify raising limits. | P1 |
| M02 | Demand-paged private backing and protected sharing (P21/P26); precise 98 COW rules need P02 | `core/vmm.c::as_create/as_map/as_replace_pages`; `proc/uaddr.c::reserve_pages`, `ua_map_at`, `ua_mmap`; `arch/trap.c::trap_dispatch` | Present mappings and eager zeroing; PROT_NONE / Win32 reservations do exist. No general demand-zero pager or refcounted private COW backing. Win32 guard faults are special cases. E19b eager-pool cost; no general pager fault measured. | P1 |
| M03 | Dynamic swap and 98 VFAT-idle asynchronous page-out (P01/P05/P10) | No disk pager in `core/vmm.c`, `proc/uaddr.c` or `fs/mount.c`; private data stays resident | Missing slot/extent map, writeback/error protocol and nonrecursive fault I/O. No swapping-induced defect is observed, since swapping is absent; current memory admission refuses instead. | P1 |
| M04 | Global access-history replacement; do not import NT working-set trimming (P21/P25) | `core/pmm.c::pmm_alloc*` fail on exhaustion; `core/vmm.c` has no reclaim state; `ua_extent` describes mappings | No resident-page aging, clean discard, modified queue or per-owner resident-set telemetry/fairness. Allocation pressure cannot reclaim cache or cold processes. Physical thrashing/OOM unmeasured. | P1 |
| M05 | DMA memory must stay locked with hardware constraints (P12/P22/P27) | `pmm.c::pmm_alloc_dma_run/alloc_run`; `drivers/ata_dma.c`; SB16/audio pinned rings | Existing DMA allocation and bounce safety retained; no shared pin-count API or measured low-zone reserve against pageable demand. Exhaustion/fragmentation risk inferred; no trial proves a DMA boundary defect. | P1 |
| M06 | Long allocation/destruction work must not exclude unrelated service (P09) | `core/vmm.c::as_destroy`; `proc/uaddr.c::free_pages/protect_pages`; `vm/dosvm_xms.c::dosvm_memory_init` | f0-41 added points every 16 pool pages; failed-create teardown and other large walks still need complete bounds. E19b 110 ms is historical creation evidence; teardown latency is an identified unqualified dependency, not a new physical result. | P0 |

### Scheduling, timers and real-time service

| ID | Windows model / source | Current CiukiOS file and function | Gap and trial risk | Priority |
| --- | --- | --- | --- | --- |
| R01 | Short deferred work; event waits cannot stall multitasking (P08/P09/P11) | `core/task.c::kpreempt_point`, `sched_tick`; `fat.c::chain_at`; f0-41 VM paths | Ring-3 preemptive, ring-0 voluntary; safe points improve creation but are not full kernel preemption. E18a 133–160 ms, E19b 110 ms; host f0-41 pool 110592 → 288.2 µs. Remaining chain/VGA/cleanup paths not physically bounded. | P0 |
| R02 | VPICD interrupt callbacks and bounded deferred handling (P08/P15) | `core/irq_budget.c::irq_budget_stamp/elapsed/fault`; `arch/trap.c`; shared IRQ registry | E19b IRQ11 raw 2129 µs, AC97 2104 µs. 2000–2300 µs no-work double-TSC stalls are heuristically SMI-attributed; attribution is not proof that all such latency is firmware. Need raw/work/deferred separation and enforceable software bounds. | P0 |
| R03 | Prioritized device service, not a hard real-time OS guarantee (P09/P18) | `audio/audio_dev.c::audio_worker/audio_service`, `include/ciuki/audio.h`; `task.c` P_DEVICE | Existing 1 ms continuation wait, 2 ms service cap, 256-frame periods, nominal eight-period lead. E19b gap 113127 µs grows lead to 28 (~149 ms transport). More buffering cannot meet the nominal ≤50 ms VM target at the same time. | P0 |
| R04 | Thread classes, foreground and I/O boosts; exact 98 decay unresolved (P18) | `core/task.c::rq_pop/schedule/sched_tick`; four fixed classes, 10 ms default quantum, focused DOS 8/10 ticks | Desktop initial thread is interactive; general foreground-native input and I/O boosts absent. One-second normal starvation guard exists (temporarily queued at P_DEVICE). Existing focus policy must not be silently erased. E19b UI contention motivated workers; not proof of specific missing boost size. | P1 |
| R05 | Dependency-owner priority boost (P17) | `core/sync.c::kmutex_lock/unlock`; `fs/fs_port.h::fs_lock_take` | Wait queues sleep; no priority donation. kmutex also suppresses voluntary preemption while held. E82 convoy is demonstrated; priority inversion contribution remains unmeasured. Need inheritance plus bounded lock ownership, not spinning or boosting every writer. | P1 |
| R06 | Asynchronous timers and distinction between precision/quantum (P15/P16/P18) | `arch/pic_pit.c::pit_init` divisor 1193 (~1 kHz); `core/sync.c::deadline_after_ms`; `task.c::sched_tick` scans tasks | Keep current 1 ms tick. Timer/readiness cost scales with task count; no unified timer-deadline queue. No evidence that raising tick rate fixes stretches or firmware pauses. Exact historical 98/ME timer defaults remain a source gap. | P2 |

### DOS box integration

| ID | Windows model / source | Current CiukiOS file and function | Gap and trial risk | Priority |
| --- | --- | --- | --- | --- |
| D01 | V86 client memory services are part of VMM management (P08/P11) | `vm/dosvm.c::dosvm_admit/create`; `dosvm_xms.c::dosvm_memory_init`; `include/ciuki/dosvm.h` | Four VMs, 32 MiB admission cap, 32 MiB host headroom, 8 MiB low/accounting cap and up to 24 MiB eager pool. Not an assertion that Windows had a 32 MiB limit. E19b eager pool caused stretch; quota changes need a measured ledger and later directive. | P1 |
| D02 | DOS file/block requests share protected I/O infrastructure (P06/P10) | `vm/dosvm_disk.c::dosvm_disk_read`, disk gate; FAT16 private image via VFS, two 64 KiB immutable-base read windows, file-backed overlay indexed by 1024 rows | Outer files use the shared block cache, with additional fixed VM read copies. Seek+read and nested FAT/volume waits can delay VM progress. Overlay bytes are already file-backed, not a resident 512 KiB dirty cache. E82 native reader contention also threatens VM disk clients; no per-VM physical wait histogram yet. | P0 |
| D03 | VPICD virtual IRQ delivery and V86/PM state (P08) | `vm/dosvm.c`, `dpmi_dispatch.c::dpmi_irq_resync/pmint_return`; `dosvm_perf.inc` | Preserve f3-45 exact saved-context returns, VIF/shadow rules and f3-40 DMA cadence. E18a guest IRQ 11.4 ms average improved in E18b; E19b IRQRET failure has a separate corrected cause. Scheduling cannot fix a guest legitimately holding IF clear. | P1 |
| D04 | VM lifecycle and locked callbacks integrate with VMM (P08/P11) | `dosvm.c::dosvm_create` failure unwinding; `dosvm_xms.c` pool aliases; `vmm.c::as_destroy` | Future paging/COW must handle XMS/EMS/DPMI aliases, pins, destruction and pending I/O by generation. Existing eager ownership is simpler; unsafe generalization could free an in-flight DMA page. Risk inferred, no such corruption measured. | P1 |

Counts (24 rows, each counted once):

| Subsystem | P0 | P1 | P2 | Total |
| --- | ---: | ---: | ---: | ---: |
| Storage/cache/filesystem | 3 | 5 | 0 | 8 |
| Memory | 1 | 5 | 0 | 6 |
| Scheduler/real-time | 3 | 2 | 1 | 6 |
| DOS integration | 1 | 3 | 0 | 4 |
| **Total** | **8** | **15** | **1** | **24** |

## Verification boundary

No tests, source/header/ABI changes or builds are required by f0-42; its
deliverables are these documents. Documentation checks and evidence
extraction are reported in `build/codex/f0-42-report.md`. The implementation
sequence and its future host/physical acceptance live in the contract.
Earlier passing host models cited above were read as existing evidence,
not rerun or relabeled as this delivery's tests. No QEMU, systemd scope,
kernel/image build, commit or diary edit was performed.
