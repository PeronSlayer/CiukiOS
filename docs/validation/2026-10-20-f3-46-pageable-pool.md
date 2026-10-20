# F3-46: DOS VM pageable pool and protected I/O (host validation)

Contract D1 of [Windows 98/ME-class resource management](../design/win98-class-resource-management.md),
directive `docs/directives/f3-46-dos-vm-pageable-objects-protected-io.md`.
Design text: [DOS/DPMI contract, "F3-46 pageable pool"](../design/dos-dpmi-contract.md).
Implementation report: `build/codex/f3-46-report.md`.

## Research (2026-10-20, before implementation)

| Source | What it establishes | Decision |
| --- | --- | --- |
| Microsoft Windows 98 Resource Kit (1998, owner's media in `build/rskt/`, not redistributed), ch. 28 "Virtual Machine Manager" (`wrkc28_4.htm`) | Each MS-DOS-based application runs in its own VM; the VMM's Memory Pager provides demand-paged virtual memory backed by a paging file. | The VM's XMS/EMS/DPMI pool is pageable memory of the global pager, not a fixed allocation. |
| Same, ch. 25 "Configuring Applications" (`wrkc25_3.htm`), table 25.2 (Apps.inf `[PIF95]` `Enable=`/`Disable=`) | Per-program switches `eml` (EMS memory locked), `xml` (XMS memory locked), `lml` (low memory locked), next to `ems`, `hma`, `gmp`. | A per-game "memory locked" compatibility option exists in the historical model: CiukiOS keeps the eager, resident pool as `launch.memory = 1`. |
| Same, ch. 26 "Optimizing the Swap File" (`wrkc26_3.htm`) | Dynamic swap file (already adopted by f0-49). | Pool commitment may be backed by the published pagefile capacity. |
| [DPMI 0.9, 0600h Lock Linear Region](https://www.delorie.com/djgpp/doc/dpmi/api/310600.html) | Lock counts per page; failure locks nothing; hosts with virtual memory "must be able to handle page faults transparently at arbitrary points", including interrupt and exception handlers. | 0600h pins (all or nothing), 0601h releases; guest faults anywhere (ISRs included) are resolved transparently at the trap entry. |
| [DPMI 0.9, 0702h](https://www.delorie.com/djgpp/doc/dpmi/api/310702.html), [0703h](https://www.delorie.com/djgpp/doc/dpmi/api/310703.html) | 0702h is advisory, contents preserved, partial pages not marked; 0703h makes contents undefined, partial and locked pages are not discarded. | 0702h drops the pages' access history; 0703h returns whole unlocked pages to demand-zero (previously it zeroed locked pages too). |
| Windows 95/98 DDK VMM page-commit flags, as transcribed from the DDK headers in [Wine's `vmm.vxd`](https://goma.googlesource.com/wine/+/be728f10bf20b0b080529f49f72d14f78254dd70%5E%21) (the full 98 DDK, P02, is still not retrievable) | `PD_ZEROINIT` "swappable zero-initialized pages" versus `PD_FIXEDZERO` "fixed zero-initialized pages"; `PC_LOCKED` "pages are made present and locked"; `PC_FIXED` "permanently locked". | Two pool modes: paged (swappable, zero on first touch) by default; locked (present and locked) for the compatibility option. No service names or semantics beyond these are claimed. |
| [OS/2 Museum, "VDS: Borne out of Necessity"](https://www.os2museum.com/wp/vds-borne-out-of-necessity/) (secondary, on VDMAD/VDS) | A multitasking VMM locks a DMA buffer region (VDMAD_Lock_DMA_Region / VDS lock) so its pages cannot move or be paged out until the transfer ends. | An unmasked virtual DMA channel's window is pinned until masked or terminal count; the clock work never pages in. |
| [Intel SDM vol. 3A](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf), page-fault exception and A/D flags (already cited by f0-48) | #PF is a fault: CR2 holds the linear address, the instruction restarts after the handler; A/D bits are set by the processor per translation. | Guest faults are resolved at the trap entry and the instruction restarts; every alias's A/D bits are folded into one PFN history. |
| [XMS 3.0](https://www.phatcode.net/res/219/files/xms30.txt), [LIM EMS 4.0](https://www.phatcode.net/res/218/files/limems40.txt) | XMS lock prevents moving and returns a linear address; EMS maps logical pages into frame windows. | XMS locks keep blocks in place (unchanged); they do not pin pages, as under Windows' `xml` default. EMS windows are aliases of pool pages. |

Repository comparison before coding: the pool was eagerly allocated, zeroed and
mapped at creation (`dosvm_memory_init`, about 110 ms per default VM on the
T23 model) and pinned whole by seven ledger tokens; every guest-memory
access used `as_lookup`; the f0-48 sampler cleared PTE dirty bits that the
VBE buffer and VGA window harvest relied on; the f3-47 cache had no reclaim
path; DPMI 0703h zeroed locked pages.

## Deliberate differences and open decisions

- Low memory (conventional, UMB, HMA, ROM, VGA window frames) stays resident;
  Windows 98 pages V86 memory. The monitor, VGA model and DPMI real-mode
  stacks address it directly.
- VM admission keeps the f3-47 RAM formula (`request + headroom <= free -
  outstanding`); the pool commitment additionally goes through
  `ledger_commit` (RAM or pagefile). Admitting VMs against pagefile capacity
  is a lead decision after the physical ledger (contract D1).
- The read cache stays in the VM's `MM_VM_PRIVATE` commitment but is
  reclaimable through the f0-48 provider; C5 page identities cannot yet
  replace it (no data, 256 entries).
- A DMA window inside the EMS frame (E0000h-EFFFFh) is not pinned; its reads
  wait for residency like an unanswered DREQ.

## Host evidence

Run on this worktree, host model only (not P-III timings). See the report for
commands and complete logs.

| Evidence | Result |
| --- | --- |
| Creation work model (f0-41 costs) | locked 134.3 ms total, paged 23.6 ms; longest span with points 0.29 / 0.08 ms |
| Maximum teardown (paged, all 6144 pages touched) | 73.2 ms total, 0.32 ms longest span with points (main: 72.9 ms / 0.13 ms; close slices of 16 pages, F0-44 bound 2 ms) |
| Paging through aliases (production reclaim.c and pager.c) | XMS, EMS-window and DPMI pages paged out and back with data intact; window-only dirt forces a write; clean copies discarded without I/O; window remap folds dirt; DPMI lock/unlock/discard; hold keeps pages; controller writes during a worker transaction |
| Virtual DMA | window pinned at the settle point (16 pages); reads before it are DREQ waits, not faults; released when masked |
| Failures and termination | failed write keeps the dirty page; failed read contained; second task coalesced on the in-flight page; failing guest fault stops the VM; destroy during an in-flight page-out with an IRQ pending: frames orphaned and freed once, every counter at baseline |
| Commitment | pagefile grown before a pool commitment beyond RAM; refusal before partial state; locked pool never paged |
| VM cache | 32 blocks reclaimed without I/O, overlay rows kept, regrowth only out of pressure |
| Idle prewrite | 32 cold pool pages written while mapped, later discarded without I/O |
| Per-profile admission (production `pmm.c`/`ledger.c`) | 128 MiB: second default VM refused (unchanged); 256/512 MiB admitted; identical margins whether the pool is untouched or fully resident |
| Combined audio replay | paged creation, DOS/4GW start, 140 Hz ISR, disk trace, page-outs/page-ins: no late-grow, lead 8 periods, max gap 1.77 ms |
| Sampler policy | VM-private and pinned translations keep A/D; pool pages folded once |
| f0-46 IRQ cost model (`tests/host/irq_work_replay.py`) | `DOS-audio-clock-per-VM max_us=248 budget_us=250` (main 246): the clock work's DMA read goes through `dosvm_read_resident`; present translations keep the `as_lookup` fast path |

## Remaining (lead)

QEMU (if the owner resumes it) and T23: DOOM, Wolf3D and Quake creation,
play and close for at least ten minutes with C3, recording `vm_perf_page`
rows (page-ins, waits, IRQ delay split), guest IRQ delivery and IF-clear time,
host refill and input/storage maxima; the same with the locked option; a
memory-pressure run on the 128/256/512 MiB profiles with the pagefile active.
