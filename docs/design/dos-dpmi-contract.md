# DOS virtual machines and DPMI under Ciuki VMM

Author: Codex. Reviewer and integrator: Claude (lead). Status: approved
after the Claude–Codex cross-review, 2026-10-09.

## Decision

Implements D7 of the
[foundations decision](../../dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md):
Ciuki VMM owns protection, scheduling and DPMI; CIUKIDOS supplies each VM's
DOS personality. Every VM boots cleanly on a private FAT16 disk. Windows
95/98's division between VMM services and per-VM DOS, including non-file
INT 21h services, is a reference, not a requirement to reproduce its memory
layout. This is distinct from Windows NT's NTVDM.
([Microsoft: DOS's role](https://devblogs.microsoft.com/oldnewthing/20071224-00/?p=24063),
[DPMI ownership](https://devblogs.microsoft.com/oldnewthing/20230829-00/?p=108661/),
[NTVDM](https://learn.microsoft.com/en-us/windows/compatibility/ntvdm-and-16-bit-app-support).)

## Sources

Primary specifications, including archival copies, checked on 2026-10-09:

- [Intel IA-32 SDM, volume 3](https://kib.kiev.ua/x86docs/Intel/SDMs/245472-004.pdf),
  chapter 15: V86, instruction trapping and virtual interrupts.
- [DPMI Committee 0.9](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs),
  with [1.0 appendix C](https://sudleyplace.com/dpmione/dpmispec1.0.pdf) for
  corrections and the boundary between versions.
- [XMS 3.0](https://www.phatcode.net/res/219/files/xms30.txt),
  [LIM EMS 4.0](https://www.phatcode.net/res/218/files/limems40.txt), and
  [VESA VBE 3.0](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf).
- [Open Watcom DOS/4GW guide](https://open-watcom.github.io/open-watcom-1.9/pguide.html)
  and [FreeDOS file services](https://github.com/FDOS/kernel/blob/master/kernel/dosfns.c):
  client behavior and a reference for DOS handle/redirector boundaries.

Requirements below are project policy informed by these sources, not claims
that the existing implementation already satisfies them.

## Phase boundary and migration

The following DOS deliverables are proposed for alignment with
`foundations-transition.md`; the binding F0 acceptance gate is unchanged.
Phase tags identify when a requirement becomes mandatory and remain binding
thereafter.

| Phase | Mandatory DOS-related scope |
| --- | --- |
| F0 | Supervisor isolation, denied user I/O, resource accounting and fault cleanup foundations; no runnable DOS VM or advertised DPMI host. |
| F1 | Preserve DOS sources/assets and qualify VFS prerequisites; DOS execution remains outside this gate. |
| F2 | Native processes and desktop only (decision D10); DOS sources stay buildable but no DOS VM runs. |
| F3 | Clean V86 DOS boot, private disk, virtual devices, A20/HMA/XMS/UMB and lifecycle isolation; then complete DPMI 0.9, EMS, SB16/OPL device state and reference DOS workloads (audible output is verified in F4 with the native audio backends). |
| F4 | DOS-to-VFS bridge for shared volumes and cross-VM file semantics; audible SB16/OPL output through AC97 and ESS Maestro-2E. |

Phase allocation integrated by the lead on 2026-10-09 to match decision D10
(F2 native processes and desktop, F3 DOS VMs then DPMI): the requirements
first drafted here as F2 now carry the F3 tag.

**[F0]** Reuse MUST be explicit and license-reviewed under
`foundations-transition.md`, including GPLv2 compatibility. Existing code
MUST remain available until migrated or explicitly retired. The new runtime
MUST NOT depend on Jemm, VCPI or HDPMI. These are concrete migration boundaries:

| Existing implementation | Required replacement |
| --- | --- |
| `src/vm/session_vmm.inc`, `vmm_create`, `bios_ram_fork`, `vmm_switch`: copies parent low memory, swaps PTEs and Jemm interrupt state, coordinates DOS/FAT activity. | Separate address spaces, clean personality boot, kernel scheduler and virtual PIC. |
| `src/com/vmfork.asm`, `start`, `child`, `reparent_retained_ancestors`: clones then frees/reparents inherited DOS allocations. | Launch descriptor with executable, arguments, environment and explicit mounts; no inherited MCB/PSP/IVT snapshot. |
| `src/vm/hdpmi_session_adapter.asm`, `cvdpmi_cli_enter`, `cv_if_disable`, `cv_if_enable`: HDPMI hooks, stepping and physical PIC mask writes. | Kernel DPMI dispatch and exclusively virtual guest interrupt state. |
| `src/boot/floppy_stage1.asm`, `bios_disk_interrupt`, `disk_sector_lba32`, `fat16_ensure_sector`: BIOS-sector I/O and guest FAT cache. | Retain for private disk compatibility; bypass completely for shared volumes. |

`src/vm/session_disk.inc:disk_interrupt` currently forwards some INT 13h
requests through Jemm nested execution; `src/com/vmfork_disk.inc:disk_prepare_parent`
binds the physical boot disk. Neither is a private virtual-disk boundary.

## VM lifecycle and containment

**[F3]** A VM MUST transition through creating, booting, runnable/blocked,
stopping and dead states, with a generation identifier on asynchronous work.
Creation MUST allocate zeroed private memory, synthetic IVT/BDA/ROM services,
reset devices and a private writable overlay on a clean FAT16 DOS image.
Its boot sector MUST load `CIUKIDOS.SYS` and the DOS shell using virtual
INT 13h. Personality changes needed to boot MUST be recorded; initially
retain existing DOS behavior where possible. Failed creation MUST unwind
every completed allocation before publishing a runnable VM.
DOS reentrancy guards MUST be VM-local; a blocked DOS call MUST NOT prevent
another VM from being scheduled.

**[F3]** Normal shell exit, explicit termination and unhandled guest faults
MUST converge on one idempotent cleanup path: stop scheduling, revoke
callbacks/input, cancel or drain pending I/O, detach audio, close handles and
release locks, then release selectors, memory, page tables and disk-overlay
references. Late completions MUST fail generation validation. A guest reset
request MUST reset/terminate only that VM. Crash cleanup MUST NOT run guest
code or depend on guest DOS data remaining valid. Normal exit flushes private
storage; forced termination reports possible unflushed private data loss.

**[F3]** Each VM MUST own linear `0–0x0010FFEF`; DPMI allocations begin at
`0x00110000`, below `0xC0000000`. UMA policy qualifies what is RAM there.
Kernel mappings remain shared, supervisor-only, with CR0.WP set. Page zero
contains this VM's IVT; the native-process page-zero prohibition remains.
Guest faults MUST leave another VM and a native heartbeat runnable.

## V86 monitor and BIOS boundary

**[F3]** V86 execution MUST use IOPL=0 and a deny-all TSS I/O bitmap. The
monitor MUST decode prefixes, operand/address sizes and segment limits for
CLI/STI, PUSHF/POPF, INT/IRET and IN/OUT/INS/OUTS. String I/O MUST be bounded,
restartable and validate each accessed range. HLT blocks the VM; forbidden
control-register/table changes fault locally. Unknown ports MUST follow a
documented absent-device response or fault, never physical passthrough.
([Intel, §§15.2.7–15.3](https://kib.kiev.ua/x86docs/Intel/SDMs/245472-004.pdf).)

**[F3–F4]** CR4.VME and CR4.PVI MUST remain disabled even when CPUID reports
support; VIF/VIP MUST NOT be treated as authoritative state. Software
virtual IF is the qualification baseline. VME acceleration MAY be proposed
later only with CPUID gating and equivalent instruction/IRQ tests. This
choice avoids two monitor paths during foundations work.

**[F3]** Guest CLI MUST suppress only guest IRQ injection, never host
preemption. Each VM MUST have one authoritative virtual PIC with IRR, ISR,
IMR, priority, cascade, initialization and EOI behavior. Default guest bases
are `08h/70h`; host bases remain `20h/28h`. Guest reprogramming MUST NOT alter
host vectors or masks. Delivery requires virtual IF, PIC eligibility and no
STI/MOV-SS interrupt shadow; instruction-boundary tracking MUST cover a
shadow crossing a trap. Faults consumed for emulation MUST NOT be reflected;
architectural guest exceptions use the guest handler or terminate the VM.
([DPMI 0.9, interrupt environment](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs).)

**[F3]** BIOS dispatch MUST obey this table and
`device-firmware-ownership.md`. Guest ROM entry stubs MUST resolve to these
services, never execute copied physical firmware unchecked.

| Guest service | Ownership and behavior |
| --- | --- |
| INT 10h VGA; INT 16h keyboard; INT 1Ah clock | Virtual VGA/input/time state; no physical mode change. |
| INT 11h/12h; INT 15h memory, A20 and waits | Synthetic equipment/memory values, VM A20, scheduler waits; never expose host E820 or allocator pages. |
| INT 13h CHS/EDD | Bounded private-disk requests, geometry/status and validated buffers; unknown drives fail. |
| INT 14h/17h, PCI BIOS, APM/ACPI control | Absent/unsupported until individually implemented; no device/power ownership transfer. |
| INT 19h, reset ports, keyboard-controller reset | VM-local reset/termination. |

**[F3–F4]** No guest BIOS operation is directly forwarded to firmware.
A kernel storage backend MAY internally request an allowlisted, serialized
BIOS fallback after quiescing native ownership, exactly as the device
contract specifies. DPMI real-mode simulation does not bypass this rule.
Guest VBE MUST report unavailable through F4; host boot VBE capability is
not guest capability. Failure MUST use the
[VBE status convention](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf),
without exporting framebuffer addresses or firmware entry points.

## Memory services and admission at 128 MiB

**[F3]** The physical A20 line MUST stay enabled. Virtual A20-off aliases
`0x100000–0x10FFFF` to the VM's first 64 KiB; A20-on restores private HMA
backing. The final 16 bytes above `0x10FFEF` are padding, not allocatable HMA.
XMS, port 92h, virtual 8042 and INT 15h A20 operations MUST agree. HMA ownership
and XMS local/global enable counts MUST be per VM; protected-mode client
execution MUST have A20 enabled, saving/restoring the V86 A20 state across
transitions without affecting other VMs.

**[F3]** XMS MUST implement discovery `INT 2Fh/4300h,4310h`, functions
`00h–12h`, and 3.0 extensions `88h,89h,8Eh,8Fh`: HMA/A20, allocation,
move, locks, handle information, resize and UMB management. Lock addresses
MUST describe VM guest memory, never host physical frames. Moves MUST check
handles, bounds, overflow and conventional-memory endpoints. Errors and
available-memory answers MUST follow [XMS 3.0](https://www.phatcode.net/res/219/files/xms30.txt).

**[F3]** The synthetic layout MUST reserve `A0000–BFFFF` for virtual VGA,
`C0000–C7FFF` and `F0000–FFFFF` for virtual ROM/stubs, offer at most
`C8000–DFFFF` as UMB RAM, and reserve `E0000–EFFFF` for an EMS frame.
This is a guest design, not a statement about T23/E500 firmware holes.
Conventional free memory MUST be measured after CIUKIDOS boot, not reported
as an unconditional 640 KiB.

**[F3]** EMS MUST provide the LIM 4.0 interface, handle ownership and
four 16 KiB frame windows, including saved mappings and move/exchange
validation; version advertisement requires the complete declared function
matrix to pass. Until then EMS discovery MUST report absent. EMS MUST NOT
provide VCPI through INT 67h/DE00h.
([LIM specification](https://www.phatcode.net/res/218/files/limems40.txt).)

**[F3] Budget proposal for boot-memory cross-review:** a VM MUST have a
32 MiB total resident cap: at most 24 MiB shared by XMS, EMS and DPMI
allocations, with the remaining 8 MiB covering low memory, page tables,
stacks, LDT, devices and charged cache. EMS has an 8 MiB sub-cap inside that
24 MiB pool. Disk capacity is not RAM; private overlays MUST be file-backed.
There is no swap or memory overcommit through F4.

**[F3]** Admission MUST reserve the VM commitment only after subtracting
measured firmware/kernel/driver/framebuffer/native commitments, and leave
32 MiB uncommitted host headroom. Reports MUST distinguish reservation from
actual resident use. Two admitted VMs consume at most 64 MiB; their admission
therefore requires at least 96 MiB available after those deductions.
128 MiB installed RAM does not establish that condition. Allocation and
resize failures MUST leave old objects intact; aliases MUST be charged once.
The runner MAY select smaller commitments at runtime for isolation probes;
it MUST log the resulting quotas without changing the canonical build.

## Kernel-owned DPMI 0.9

**[F3]** Public discovery MUST appear only when the complete contract passes.
`INT 2Fh/1687h` MUST return AX=0, BX bit 0 set, the processor class, version
`DH=0, DL=90` decimal, SI=0 and a VM-local ES:DI entry. Initial entry MUST
support 16-bit and 32-bit clients, the specified initial selectors/registers,
and failure without a partial transition. Clients MUST execute at ring 3,
IOPL=0, with denied physical I/O. `1686h` MUST report execution mode.
([DPMI entry specification](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs),
[version correction](https://sudleyplace.com/dpmione/dpmispec1.0.pdf).)

**[F3]** Every VM MUST have its own kernel-managed LDT and virtual interrupt/
exception tables. Clients within that VM share the 0.9 tables; the first
client fixes their 16/32-bit convention until all clients exit. Opposite-width
nested entry fails cleanly. Selector ownership MUST track client lifetimes;
0002h segment aliases remain VM-owned. Clients MUST NOT install privileged
descriptors, call gates or kernel mappings. Flat descriptors MAY span 4 GiB;
supervisor page permissions remain the protection boundary.
([DPMI 1.0 appendix C](https://sudleyplace.com/dpmione/dpmispec1.0.pdf).)

The entire following INT 31h surface is mandatory at **F3**, including
specified refusals. F0–F2 MUST NOT advertise it. Numbering is checked against
the [upstream DJGPP DPMI index](https://www.delorie.com/djgpp/doc/dpmi/ch5.n.html)
and the Committee specifications above.

| AX, hexadecimal | F3 operation/policy |
| --- | --- |
| 0000, 0001, 0002, 0003 | LDT allocation/release, segment alias, selector stride. |
| 0006, 0007, 0008, 0009 | Read/write base, set limit/access; validate ownership and descriptor type. |
| 000A, 000B, 000C, 000D | Alias, read/write descriptor, specific-selector allocation. |
| 0100, 0101, 0102 | DOS arena allocation/release/resize with matching selectors. |
| 0200, 0201, 0202, 0203, 0204, 0205 | Read/write real vectors, exception handlers, protected vectors. |
| 0300, 0301, 0302 | V86 interrupt, RETF-framed call, IRET-framed call. |
| 0303, 0304, 0305, 0306 | Callback allocation/release, state save/restore, raw switch entries. |
| 0400 | Version, capabilities and guest PIC bases; no demand-paging claim. |
| 0500, 0501, 0502, 0503 | Quota-aware memory report and allocation/release/resize. |
| 0600, 0601, 0602, 0603, 0604 | Lock/unlock, real-region pageable/relock, 4096-byte page size. |
| 0702, 0703 | Paging hint; discard validated complete pages, preserving partial edges. |
| 0800 | Recognized, refused with CF set: no physical-device mapping. |
| 0900, 0901, 0902 | Disable/enable/query virtual IF with specified previous-state result. |
| 0A00 | Vendor lookup: CF set when no matching extension exists. |
| 0B00, 0B01, 0B02, 0B03 | VM-local watchpoint allocation/release/query/reset. |
| 0004, 0005, 0700, 0701 | Reserved; fail. |
| 000E–000F, 0210–0213, 0401, 0504–050B, 0801, 0C00–0C01, 0D00–0D03, 0E00–0E01 | 1.0-only; unsupported through F4. |

**[F3]** Failure MUST set CF, preserve unspecified registers and use the
0.9-defined outputs, including DOS allocation errors. Unknown functions
MUST fail without state mutation. Nonpageable memory does not excuse
unvalidated locking/discard calls. Watchpoints MUST switch with VM state
and never target kernel memory. Refusing 0800h is explicitly permitted for
protection by [DPMI 0.9 §16](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs).

**[F3]** Mode transitions MUST stay under kernel paging/IDT ownership.
Raw switches MUST implement the published register and state-save contracts;
0300h–0302h MUST use the VM's vectors and validated register/stack buffers.
The host MUST provide guarded, resident transition stacks, at least 16
callback slots per client and eight tested nested transitions. Reentry MUST
preserve each continuation; exhaustion returns failure where possible,
otherwise terminates only the VM. No VFS or allocator lock may survive guest
execution. Callbacks use the specified IRET return; exception handlers use
the specified far-return frame. Guest-edited return frames MUST be validated.
([DPMI 0.9, stacks and translation services](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs).)

**[F3]** Protected-mode CLI/STI and 090xh MUST share virtual IF. PUSHF can
expose physical IF; POPF at IOPL=0 does not reliably trap or restore virtual
IF. The monitor MUST NOT infer otherwise. Interrupt/exception return
trampolines MUST restore the saved virtual state, honor chaining and validate
16/32-bit frames. Guest IF=0 MUST never acquire physical PIC ownership or
be forcibly enabled by a timeout.
([DPMI virtual IF](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs),
[Intel §15.4](https://kib.kiev.ua/x86docs/Intel/SDMs/245472-004.pdf).)

**[F3]** DOS termination MUST free client allocations, callbacks, locks and
selectors while preserving surviving parent state. Both V86 and protected
INT 21h/AH=4Ch MUST reach this cleanup path. A stale handler pointing
into freed storage MUST terminate the VM rather than execute reused memory.
Fault termination unwinds all nesting without guest assistance. DPMI calls
MUST be reentrant within these resource limits.

**[F3]** Virtual IRQs MUST reach the active client's protected handler or its
default V86 reflection path; unhandled software interrupts use the VM's real
vectors. Exceptions MUST first undergo host fault resolution, then use the
client's validated locked-stack handler or terminate that VM. Dispatch MUST
distinguish guest IRQ vectors from identically numbered CPU exceptions.
Reflection MUST preserve registers and honor the virtual PIC's in-service
and EOI rules. DOS API pointer translation belongs to the
extender/personality boundary, not an assumption that selectors are real
segments. ([Open Watcom DPMI services](https://open-watcom.github.io/open-watcom-1.9/pguide.html).)

**[F3]** DOS/4GW MUST select Ciuki VMM as its host; no external HDPMI or
VCPI installation is allowed. Unsupported extenders MUST receive truthful
discovery failure; attempted privileged takeover faults locally, with a
diagnostic identifying the executable and unsupported operation.
([Open Watcom](https://open-watcom.github.io/open-watcom-1.9/pguide.html).)

## DOS personality and VFS bridge

**[F4]** Shared-drive routing MUST live inside CIUKIDOS before FAT/LFN
resolution. Non-file INT 21h remains personality-owned. Each PSP MUST have a
JFT mapping to reference-counted SFT entries backed by opaque kernel VFS
handles. Duplication/inheritance MUST preserve shared offsets and
no-inherit flags; close/EXEC failure/exit MUST release exactly one reference.
Sharing modes and byte-range locks MUST apply across VMs and native clients.
Compatibility mode MUST use the DOS matrix agreed in the VFS contract;
silently accepting unsupported sharing flags is forbidden.
([FreeDOS `DosOpen`, `DosOpenSft`, `DosCloseSft`](https://github.com/FDOS/kernel/blob/master/kernel/dosfns.c).)

**[F4]** Searches MUST associate validated continuation tokens with each
DTA/PSP, supporting interleaved searches without kernel pointers in guest
memory. FCB open/close, sequential/random I/O, find, rename/delete and record
counts MUST use the same file objects and sharing rules. EXEC, overlays,
environment paths, drive-relative directories and 8.3/LFN aliases MUST resolve
through that bridge. DOS-size/offset limitations MUST return defined errors
rather than truncate native values. Buffers MUST be copied/validated; no
guest pointer survives blocking I/O unchecked.

**[F4]** Shared volumes MUST have no guest sector device. INT 13h, INT 25h/
26h, raw IOCTLs and LFN sector helpers MUST refuse their raw reads and writes;
private FAT16 sector access remains available. Host VFS owns caches, flush,
mutation visibility and stable aliases. The current
`floppy_stage1.asm:int21_open` masks sharing bits;
`int21_find_first/int21_find_next` use common search state;
`int21_exec_load_to_es` follows FAT chains; `int21_sync_sft_table` mirrors
legacy slots. These require integration, not an outer INT 21h interception.
`src/runtime/ciukidos_lfn_services.inc:kernel_lfn_read_sector`,
`kernel_lfn_flush_fat` and `kernel_lfn_invalidate_fat` MUST be bypassed for
shared drives.

## Network for DOS VMs (F6)

**[F6]** Each VM MAY be given a virtual NE2000-compatible network card
attached to the kernel's network stack, used with selected, qualified DOS
drivers (packet driver and IPX). Guest IPX frames travel over an Ethernet
bridge to the physical network, so DOS LAN games (DOOM through IPXSETUP,
Duke Nukem 3D, Warcraft II, Quake through its network drivers) play against
other machines; native TCP/IP uses lwIP. The exact contract (frame
injection, virtual IRQ, PIO-only model with no guest bus-master DMA, MAC
policy for several VMs on one card) is written with the F6 network
contract; a guest never sees or programs the physical NIC.

## Virtual devices and reuse

**[F3]** VGA text, planar modes, mode 13h/Mode X, keyboard focus, INT 33h
mouse polling/callbacks and PIT/PIC MUST be per VM. The guest PIT defaults to
approximately 18.2 Hz and tracks guest programming independently of the
host's 1000 Hz timer. Focus loss MUST release held keys/buttons.

**[F3]** Each VM MUST have independent SB16 DSP/mixer, OPL and virtual DMA
state. DMA addresses MUST resolve only through the owning VM; invalid
ranges fail locally. Physical audio DMA belongs to the native backend.
Virtual audio IRQ timing MUST continue while guest IF is clear, with delivery
pending until eligible. Relevant reuse candidates are:

| File/functions | Migration requirement |
| --- | --- |
| `src/vm/virtual_vga.c`: `cvga_init`, VRAM access and rendering | Preserve model tests; replace Jemm/HDPMI memory adapters. |
| `src/vm/guest_peripherals.c`: `cvgp_init`, `cvgp_io_read/write`, `cvgp_advance`, `cvgp_render_audio` | Retain models; integrate one authoritative virtual PIC and kernel time. |
| `src/vm/session_audio.c`: `cvaudio_stream_convert`, `cvaudio_dma_slice`, codec helpers | Separate stream conversion from physical AC97 ownership. |
| `src/vm/session_opl.cpp`: `cvop_create/write/generate` | Replace singleton storage with per-VM chips; move x87-dependent synthesis/setup to a scheduled user worker under `execution-abi.md`. |

## Acceptance tests

These are required future suites, not results obtained by writing this
document. **[F0 onward]** Tests MUST follow [test-architecture.md](test-architecture.md):
host model tests, image checks, smoke, then focused runs on the same canonical
image. QEMU uses SeaBIOS/pentium3/128 MiB, fw_cfg selection, serialized capped
runs and qcow2 overlays. T23/E500 use the same probes selected by boot menu or
serial under `f0-acceptance.md`; they do not use fw_cfg/overlays. Results MUST
record image SHA-256, probe/version, measured RAM and explicit failure markers.

| Phase/suite | Required observable result on QEMU, T23 and E500 |
| --- | --- |
| F0 isolation | Native unauthorized I/O/kernel access faults locally; resource counters recover. No DPMI success advertised. |
| F3 lifecycle | 100 clean create/boot/exit and create/fault cycles restore page/handle/device baselines; IVT/disk sentinels never cross VMs; native heartbeat continues. |
| F3 memory | A20 wrap/on, HMA ownership and every XMS call pass; UMB boundaries hold; quota exhaustion/failed resize preserve data. Log admission arithmetic; second-VM refusal is explicit if reservations prevent admission. |
| F3 monitor | CLI loops remain preemptible; STI/MOV-SS shadows defer exactly the required boundary; PIC mask/EOI order, HLT wake, malformed I/O and guest reset remain local. |
| F3 DPMI | Exercise every table row for 16/32-bit clients, success and invalid inputs; eight-level callback/raw-switch nesting, exceptions, stale selectors, watchpoints and cleanup pass; rejected 1.0/VCPI/map requests change nothing. |
| F3 workloads | Pinned DOS/4GW probe and original DOS DOOM complete launch/demo/exit loops; Wolf3D exercises real-mode VGA/input/UMB paths. Background CLI-loop VM cannot starve audio/input; PCM/OPL counters and audible output are checked separately. |
| F4 bridge | Two DOS clients plus native client interleave DTA searches, duplicate/EXEC/FCB access, conflicts, locks, flush and rename; raw shared-volume access fails; reopen sees agreed VFS contents after crash. |

**[F3]** Workload records MUST include executable/extender/data hashes and
redistribution status. Wolf3D is not a DPMI conformance test:
[its memory manager uses XMS/UMBs](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_MM.C).
The [released DOOM source](https://github.com/id-Software/DOOM/blob/master/README.TXT)
does not contain the original DOS sound implementation; native ports do not
substitute for DOS-binary evidence. Private commercial payloads MUST stay
out of release images.

## Open questions

- **128 MiB admission:** T23/E500 usable/reserved totals and resulting
  concurrency are not established here. Close with E820/reservation logs,
  post-driver peaks and two-VM workload measurements on each machine.
- **Compatibility beyond 0.9:** determine whether pinned DOS/4GW/DOOM builds
  need PUSHF/POPF virtual-IF compatibility beyond the standard. Close with
  instruction traces and a bounded emulation design; never relax IOPL.
- **Physical qualification:** supported display modes, input behavior and
  AC97/Maestro output quality are not inferred from chipset names. Close with
  the device contract's enumeration logs and these suites on each laptop.

## Interfaces required from other contracts

- `foundations-transition.md`: phase allocation above, reuse/license inventory
  and retirement of VMFORK/Jemm/HDPMI runtime dependencies.
- `boot-memory.md`: `ciuki_boot_info` v1 reservations, address-space ownership,
  A20 baseline and approval/adjustment of the proposed 32 MiB VM budget.
- `execution-abi.md`: V86/DPMI contexts alongside native tasks, LDT/TSS/IDT
  dispatch, guarded stacks, FPU ownership, cancellation and generation-safe
  service entry/return, including an INT 31h path distinct from native INT 80h.
- `vfs-storage-contract.md`: private image overlays, opaque handles, DOS sharing
  matrix, search tokens, path aliases, atomic operations and flush/error rules.
- `device-firmware-ownership.md`: virtual IRQ inputs, native input/audio/display
  backends, sole physical EOI/DMA ownership and serialized BIOS fallback.
- `f0-acceptance.md`: probe selection/markers, hardware log capture and resource
  baselines; DOS workload gates remain later than F0.
