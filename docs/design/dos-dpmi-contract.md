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

F3-33 implementation clarification (2026-10-16): bounded monitor bursts MAY
retire adjacent virtual PIT/PIC byte I/O and flag-preserving MOV setup, with
the same checked scalar device handlers. A pending enabled IRQ, reschedule,
interrupt shadow, TF/watchpoint or owned IF/return proof MUST stop a burst.
Instruction faults, register high halves, latch/read order, PIC EOI and guest
IVT hooks retain scalar behavior. Proof-read windows are local to one monitor
invocation; no saved-image or instruction-boundary evidence is cached across
guest execution. Research and host differential evidence are in
[F3-31/F3-33 validation](../validation/2026-10-15-f3-31-performance.md).

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
The [F3-37 amendment](supersavage-fullscreen.md#f3-37-mode-inventory-and-guest-vbe-amendment-2026-10-16)
allows synthetic linear guest VBE 101h/103h/111h/114h only where the native
provider supports them. Other guest VBE remains unavailable; host boot VBE
capability is not guest capability. DPMI 0800h/0801h may resolve/release only
the private VBE framebuffer token into the VM's admitted RAM pool; arbitrary
physical mappings remain refused. Failure MUST use the
[VBE status convention](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf),
without exporting host framebuffer addresses or firmware entry points.

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
Amendment accepted by the lead at the f3-04 merge (2026-10-11): within the
`C0000–C7FFF` reservation, `C6000–C7FFF` is the DPMI host's real-mode
stack (writable guest pages, `DPMI_RSTACK_PAGE`); `C8000–C8FFF` holds the
read-only XMS entry stub, so UMB RAM starts at `C9000`.
Conventional free memory MUST be measured after CIUKIDOS boot, not reported
as an unconditional 640 KiB.

**[F3]** EMS MUST provide the LIM 4.0 interface, handle ownership and
four 16 KiB frame windows, including saved mappings and move/exchange
validation; version advertisement requires the complete declared function
matrix to pass. Until then EMS discovery MUST report absent. EMS MUST NOT
provide VCPI through INT 67h/DE00h.
([LIM specification](https://www.phatcode.net/res/218/files/limems40.txt).)

**[F3] Budget (f3-47 Amendment 2, lead decision 2026-10-19):** a VM MUST have
a 48 MiB default commitment and total resident cap: at most 24 MiB shared by
XMS, EMS and DPMI allocations, up to 16 MiB of charged disk cache, and at most
8 MiB covering low memory, page tables, stacks, LDT and devices. EMS has an
8 MiB sub-cap inside that 24 MiB pool. Disk capacity is not RAM; private
overlays MUST be file-backed.
~~There is no swap or memory overcommit through F4.~~ Superseded by f3-46
(2026-10-20, contract D1): the 24 MiB pool is a pageable memory object,
committed against RAM or the published pagefile and backed on demand; the
locked per-game mode keeps the former eager, resident pool. The admission
formula below is unchanged; see "F3-46 pageable pool" at the end.

**[F3]** Admission MUST reserve the VM commitment only after subtracting
measured firmware/kernel/driver/framebuffer/native commitments, and leave
32 MiB uncommitted host headroom. Reports MUST distinguish reservation from
actual resident use. Two default admitted VMs promise at most 96 MiB; their admission
therefore requires at least 128 MiB available after those deductions.
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
| 0800, 0801 | F3-37: resolve/release only the private VBE token. Other addresses fail with CF set. |
| 0900, 0901, 0902 | Disable/enable/query virtual IF with specified previous-state result. |
| 0A00 | Vendor lookup: CF set when no matching extension exists. |
| 0B00, 0B01, 0B02, 0B03 | VM-local watchpoint allocation/release/query/reset. |
| 0004, 0005, 0700, 0701 | Reserved; fail. |
| 000E–000F, 0210–0213, 0401, 0504–050B, 0C00–0C01, 0D00–0D03, 0E00–0E01 | 1.0-only; unsupported through F4. |

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

## F3-34 amendment proposal: return-boundary acquisition (2026-10-16)

Contract review completed before implementation against `0dcf11a3`, the
[f3-10 record](../validation/2026-10-13-f3-10-menu.md),
[f3-11 record](../validation/2026-10-13-f3-11-irq-stall.md),
[f3-15 policy](../validation/2026-10-14-f3-15-virtual-if.md) and
[f3-31 optimization](../validation/2026-10-15-f3-31-performance.md).
This is an amendment proposal for lead review, not hardware qualification.

| Guarantee or failure | Existing enforcement; consequence without return tracking | Required replacement |
| --- | --- | --- |
| No kernel/other-VM memory access, even with an arbitrary SS:ESP | Ring-3 paging, supervisor mappings, private address space and checked monitor copies. A flat descriptor can span 4 GiB; descriptor limits alone do **not** enforce memory ownership. The tracker does not police every stack write. | Keep paging and `dpmi_ptr`/`dosvm_range` checks. Do not create physical mappings. |
| No return to privileged code or physical I/O | CPU selector/type/presence/privilege checks, kernel-managed LDT without gates, IOPL=0, TSS bitmap. `dpmi_frame_check` additionally validates every frame used by the kernel's own IRET; guest-edited exception continuations use this path. | Leave these validators and architectural faults authoritative. A rejected acquisition is not a guest fault. |
| Legacy save/CLI/POPF restores virtual IF | Physical IF is independent of virtual IF. The f3-10 tracker supplies actual instruction boundaries after mixed-selector IRET and near returns. Without those observations the forward proof can miss a save/CLI, leaving virtual IF clear. | Retain exact-image matching at CLI and saved-image TF ownership; acquire a real boundary with sparse execute faults, retaining bounded stepping for unsupported decoding or full client DR allocation. |
| No TF leaked by boundary acquisition into PUSHF/POPF or an interrupt frame | f3-10 stops tracing before flags, SS, far/system and interrupt instructions, refuses preexisting TF/NT and confines ownership to client/depth/CS/SS. | Sparse acquisition leaves TF untouched. A bounded fallback may own live TF at checked safe boundaries (including a validated same-ring IRET), but never patches an IRET image; it clears TF before flags saves or nested frame construction. Existing saved-image and pending-IRQ recovery TF remain separate owners. |
| Correct IRQ frame and boundary | CPU traps establish instruction boundaries; `dpmi_deliver` constructs a checked frame before PIC acknowledgement. Nested continuations live in kernel records. The return tracker does not implement this transaction. | Keep frame-before-ack, handler-stack checks, STI shadow, and CPU SS-shadow behavior; an execute #DB is a pre-instruction fault, not retirement. |
| Client debug events | Enabled B0-B3 are filtered separately from BS/BD/BT; allocated watchpoints switch with the VM. f3-06 already excludes ambiguous debugger stepping of armed compatibility images. | Never steal an allocated DR slot or consume client BS/BD/BT; fallback BS belongs only to its recorded client/depth/SS and source/target CS; simultaneous client causes remain visible. Four client slots remain available. |
| Finite monitoring and context isolation | f3-10 has 512 retired steps, 16-instruction/64-byte epilogue admission and a 32-byte forward-proof stack snapshot; client/depth prevents a callback with identical selectors consuming parent TF. | Bound decoded work and checkpoint count; suspend acquisition through nested contexts, cancel stale/faulted contexts, and capture stack proof only at an observed boundary. |

The old 512-step counter, intermediate TF values and a TF bit patched into
the extender's IRET image are implementation details, not DPMI guarantees.
Fixtures asserting them must be replaced with unchanged flags images,
bounded checkpoint work, exact flag-image arming and nested/debug ownership
assertions. Flags/SS/system barriers still terminate acquisition; valid ordinary
instructions retain their CPU execution and fault classes.

Research and decision:

- [Intel SDM Vol. 3B, chapter 18](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/253669.pdf),
  §§18.2–18.3: instruction breakpoints compare linear instruction addresses;
  RF and SS inhibition can suppress them. DR6 causes must be handled
  independently. Therefore a prospective return address is only a hint until
  an actual matching execute fault arrives. No predicted PC may become a
  forward-proof root merely because it was read from the stack.
- [Intel SDM Vol. 2A, IRET](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366621.pdf),
  pp. 3-484 onward, and [Vol. 2B, RET/POPF](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366721.pdf):
  operand and stack sizes differ; selector high halves are discarded.
  Retain same-ring/target validation and leave return execution to the CPU.
- [DPMI virtual interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.1.html)
  and [0B00h watchpoints](https://www.delorie.com/djgpp/doc/dpmi/api/310b00.html):
  virtualization and debugging are distinct services. Use only a spare DR
  slot; a client allocation takes priority. With client TF, decline acquisition.
  With all four slots allocated, use
  bounded TF acquisition without stealing a slot. Valid unsupported encodings
  also use one checked TF step, then resume sparse acquisition.

Chosen design: one temporary execute breakpoint, first at the validated
epilogue's return target, then at the next control-flow or flags/SS barrier
found by a bounded forward decode. Decoded ordinary instructions run directly
when a spare slot is available.
At a control-flow checkpoint, compute a prospective successor from the live
frame; it is not an emulated instruction. Only a later CPU trap establishes
the successor as an actual boundary. A fault, changed descriptor, stack
selector, mode, client or nesting context invalidates or suspends the hint.
The breakpoint cannot be used to establish return retirement or to change
virtual IF. Stale/suppressed hints may be discarded without a new guest
fault. No target bytes are overwritten.

Event-only validation without any new observations was rejected: SS loads,
ESP changes, near returns and POPF need not trap at CPL3, so it cannot recover
the f3-10 missing flag-save boundaries by itself. A breakpoint only at the
first return target is insufficient for the documented result copier's
stores/LEAVE/RET path; sparse subsequent checkpoints cover that path.
Capture independent forward proof at every observed checkpoint, never at a
predicted PC. Replan from the actual resumed boundary after nested execution
or a physical IRQ; stack and code predictions are not assumed stable across
that execution. Direct transfers may be followed while preceding decoded instructions are
provably free of memory writes. Otherwise checkpoint the transfer before
reading its displacement, so earlier stores cannot invalidate a predecoded
transfer. A CALL also counts as a possible write through a stack/code alias.
A hint expires after 100 ms or 64 active physical IRQ observations, even
without hits; expiry releases the slot without forcing virtual IF and allows
fresh admission. The 512 work units cover decoding and fallback steps; the
32-hit cap applies to execute checkpoints. Fallback never steps flags saves,
SS changes or unvalidated context transfers. Client TF/NT prevents ownership.
Intel SDM 18.3.1.4 permits a step of IRET with TF set on entry even if its
image clears TF, allowing full-slot cross-selector acquisition without an
image patch. IRQ entry clears owned live TF before constructing guest frames.

F3-15's separate 4096-step/100-ms pending-IRQ compatibility escape already
contradicts this document's earlier absolute timeout prohibition. F3-34
records that preexisting discrepancy and does not broaden or claim to remove
that policy. Sparse return tracking with bounded fallback does not promise zero #DB:
saved-image restores, guest debuggers and pending-IRQ recovery still use it.
The <2000/s gameplay target requires same-image QEMU/T23 evidence from the
lead; host instruction-clock rates are reported separately.

### F3-35 platform selection (2026-10-16)

The VM captures `CBI_F_SMBIOS_QEMU` once at creation, outside resettable
DPMI client state. Its startup record reports `tracker=tf` on QEMU and
`tracker=breakpoint` on hardware. QEMU uses the checked, bounded TF
acquisition above for every return boundary; it never installs a private
return execute breakpoint. Client-owned DPMI watchpoints retain their API
and debug causes. Hardware retains the F3-34 sparse path and its fallback.
Neither path patches a return flags image or changes virtual IF on expiry.

Research checked against upstream
[QEMU 11.1.1 breakpoint helpers](https://github.com/qemu/qemu/blob/v11.1.1/target/i386/tcg/system/bpt_helper.c#L156)
and [TCG breakpoint checks](https://github.com/qemu/qemu/blob/v11.1.1/target/i386/tcg/tcg-cpu.c#L125):
execute slots are registered at linear addresses, but matching uses EIP;
the execute check lacks SS inhibition. This source audit, together with
the lead's SIGSEGV evidence on image `27b35378`, requires platform selection.
QEMU PASS therefore qualifies TF acquisition only. The T23 must supply
positive private and segmented checkpoint hits and the RF/SS suppression
evidence required above before the hardware path is qualified.

Every five seconds, disk/boot-log `vm_perf_tracker` rows report interval
`armed` (successfully installed private predictions), `hits` (matching
private execute faults), `expired` (100-ms/64-IRQ lifetime exhaustion),
`fallback` (retired owned TF steps, including the QEMU selection), and
`reacquired` (fresh forward-proof roots at owned checkpoints or after
replanning). `segmented` retains the nonzero-CS-base subset of hits.
The retained serial history uses the same fields. These are observations,
not predictions counted as proof. Host replay separates return TF and
private breakpoint #DB causes.

### fx-16 amendment: boundary seeds on demand (2026-10-23)

Accepted by the lead at the fx-16 merge (2026-10-23): the Windows 95
backward-byte pattern rule is admitted as the last resort for CLI sites no
forward decode can reach; the f3-08 refusals and the f3-06/f3-34 forward
proofs remain the first choice.

Seeds (the return tracker's breakpoints and fallback steps, the forward
boundary proof at every physical IRQ in PM, the 32-byte stack snapshot at
every PM trap) serve only CLI proofs. With proven CLI sites kept per VM and
the Windows 95 pattern rule as the last resort (`execution-abi.md`, fx-16
amendment to F3-44), they are acquired only while a CLI needed a proof in
the last `DPMI_SEED_IDLE_MS` (1 s) or before any site was proven since the
client started; an acquisition already in flight completes. A CLI that
misses the cache re-enables seeding at once, so a new site is proven at its
next execution at latest. T23 trial e831e685 (BOOT.LOG, DOOM full screen,
5 s banks n=7/n=8): 6,536-6,719 checkpoint #DB exits at 37 us and 7,236-7,568
CLI/STI exits at 57 us cost 0.68 s of every 5 s. In the host T23-time model
of the same game (`validation/fx-16-doom-stutter-crackle.md`) the monitor's
share falls from 43.5 % to 30.4 %. The `vm_perf_tracker` and checkpoint rows
remain; the `[vm-audio] cadence_clock` record now also reports
`cli_hits`, `cli_learned`, `cli_missed` and `seeds_skipped`.

### fx-27 amendment: lean port exits (2026-10-23)

A #GP(0) on IN/OUT to a hot emulated port (VGA register file, PIT, PIC,
OPL, DMA) may retire without the per-exit housekeeping when the exit owes
the client nothing at that boundary: no STI shadow, IF
recovery, return tracker, client watchpoint or seed demand (the fx-16 rule
above: a lean exit is never taken while seeds are wanted, so CLI proofs are
unchanged), not a host stub, and not the saved return point of an open
hardware-interrupt record (the `dpmi_irq_resync` predicate, checked before
and after the instruction). After the instruction no virtual IRQ may be
pending; otherwise `dpmi_deliver` and the rest of the ordinary tail run at
that same boundary. A lean exit leaves the state `dpmi_pm_trap` and
`dpmi_deliver` would: the decode seed at the returned boundary, no stack
snapshot, the skipped-seed count (not while a flags image is armed) and
`if_recovery_end`. An armed flags image (a PUSHF image patched with TF at
its CLI) does not prevent a lean exit: until its POPF loads TF the ordinary
path does nothing for it at an I/O exit, and it keeps its owner until
`flags_restored`. Details and the full eligibility list:
`f3-acceptance.md`, fx-27.

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

## DOS file API in the VMM

**[F3-48, proposed contract; cross-review required before f3-49 code]**
This section anticipates the former F4 file bridge. It supersedes that phase
label for file services only. It specifies acceptance, not an implemented API.
The owner's binding decision is [2026-10-19-06](../../dev_diary/2026-10-19-06-decisione-ciukidos-resta-int21-file-nel-vmm-freedos-riferimento.md):
“CiukiDOS resta il kernel DOS del progetto e continua a crescere” and
“FreeDOS serve come riferimento di conformità”. FreeDOS remains an explicit
per-game fallback. Neither an NTVDM design nor replacement of CiukiDOS follows.

Research and the source/line inventory are recorded in
[f3-48 validation](../validation/f3-48-dos-api-inventory.md). Historical facts
and Ciuki policy below are distinguished; numeric deadlines are Ciuki targets.

### DF1: interception and the return boundary

One `dos_file_dispatch(vm, frame, origin)` owns file-service selection. Its
internal result is `CHAIN`, `DONE`, or `PENDING(token)`, never a DOS error
overloaded as a chain signal. `origin` distinguishes V86 INT, DPMI 0300h and
extender reflection, host-issued real-mode calls, and the CiukiDOS hook.
No new native syscall is specified. f3-49 supplies the host-testable module.

| INT 21h service | Owner after f3-49/f3-50 |
| --- | --- |
| 0Dh, 0Eh/19h, 1Bh/1Ch, 36h | VMM flush, drive view and allocation answers; no fabricated geometry for host volumes |
| 39h–43h, 45h–47h, 4Eh/4Fh, 56h, 5700h/01h, 5Ah/5Bh, 5C00h/01h, 6C00h, 68h | VMM, through VFS; 40h/CX=0 means truncate/extend at current position |
| 1Ah/2Fh, 59h/BX=0, 60h, 67h | VMM-owned DTA/error/path/JFT bookkeeping, synchronized with the kernel |
| 4400h/06h/07h/0Ah on regular-file handles, 4408h/09h/0Eh on mapped drives; 440Bh retry policy | VMM; distinguish file/device by SFT type, not handle number |
| Device control channels and generic device IOCTL | CiukiDOS/device driver; unsupported subfunctions fail, never silently report success |
| 0Fh–17h, 21h–24h, 27h/28h, 29h | f3-50 FCB adapter over the same VMM handle/search service; 29h uses the same CP437 mapping as NLS and aliases |
| 38h, 6501h–6507h, 6520h–6523h, 6601h/02h | VMM country 1/CP437; stable guest tables; unsupported country/page requests fail |
| 00h–0Ch console; 25h/35h; 26h/31h; 48h–4Dh; 30h–34h; 50h–52h/55h/62h; 58h | CiukiDOS process, vectors, memory and console services; lifecycle/bookkeeping notifications go to VMM |

26h is Create PSP, **not FCB I/O**. Unassigned gaps (18h/1Dh–1Fh etc.) are not
new file calls. Private F1h memory services and existing DPMI protocols remain.
The version-1 private handoff's packed records, ROM reservations and exact
far-CALL/interrupt stack layout are specified in the
[f3-50 binding note](../validation/f3-50-ciukidos-vmm-hook.md#dfvm-version-1-wire-layout).
Its CLFN sector-extension exclusion and bound AH55 allocation-owner correction
are implementation refinements for lead cross-review. The unchanged legacy
fallback is retained; no public native syscall ABI changes.

3Fh/40h on character descriptions chain through the registered device route.
An open of `CON`, `NUL`, `AUX`, `PRN`, `COMn`, `LPTn` resolves devices before
disk lookup, including extensions and directory prefixes according to DOS
device-name rules. Disk-backed standard handles use VFS after redirection.

DF1 acceptance: intercept before DOS pushes a second frame. For `CHAIN`, leave
all registers, guest stack bytes, CS:IP, virtual IF and saved FLAGS untouched,
and invoke the existing monitor chain once in that frame. Use the saved next
vector, never execute another INT 21h. Account for AH=25h updates without
self-chaining. For `DONE`, advance/return exactly as the monitor requires and
publish function-specific registers and saved CF; preserve other registers and
non-result flag bits. A DPMI host's 48h/49h/4Ah/62h calls chain unchanged.
Every translated DOS/4GW and DJGPP call reaches the same service once.
Checked guest reads/writes include segment limit, A20, wrap, DTA and JFT extent;
preflight every output range before changing handles or file data.

### DF2: SFT, JFT, SDA and lifetime

The VMM owns one SFT namespace per VM. Each SFT entry owns one VFS description,
mode/inheritance, effective PSP owner, identity `(volume, mount generation,
node)`, file position and reference count. Separate opens have independent
positions; dup/force-dup and EXEC inheritance share position. No native fd
number or host pointer is exposed to a guest. Admission fails with DOS 4 before
mutating a file if JFT/SFT/VFS capacity is exhausted.

PSP:18h is the initial 20-byte JFT, FFh means free; PSP:32h holds its count and
PSP:34h its far pointer. Read the current pointer/count, never assume 18h after
67h. Support up to the existing 232-handle personality limit, bounded by actual
admitted descriptions; EXEC copies at most the DOS-defined first 20 entries.
Bit 7 of open mode suppresses inheritance. A guest JFT edit cannot invent
authority: validate its SFT index against live VM state. Dup increments shared
refs; force-dup closes the target atomically; final close releases its claim.

Publish a DOS 4+ SFT mirror at the existing List-of-Lists SFT pointer
(ES:BX+4 from AH=52h). Chain header: next far pointer, word entry count, entries
at +6, each 3Bh bytes. Relevant entry offsets: refs +00h, open mode +02h,
attribute +04h, device flags +05h, DOS time/date +0Dh/+0Fh, size +11h,
position +15h, 11-byte short name +20h, owner PSP +31h. Populate device/DPB
fields with guest addresses only. The complete DOS4+ layout, including reserved
bytes, is the [RBIL SFT table](https://fd.lod.bz/rbil/interrup/dos_kernel/2152.html#table-01642).
Sync after transfer, metadata change, duplicate, inheritance and close, before
returning to guest code; the native registry stays authoritative. Guest writes
to its mirror cannot forge VFS identity or a live description.

CiukiDOS retains PSP creation/selection and its memory allocator. The negotiated
f3-50 hook reports `PSP_CREATE`, `PSP_SELECT`, `EXEC_BEGIN`, `EXEC_COMMIT`,
`EXEC_ROLLBACK`, `PROCESS_EXIT`, `TSR` and `VM_DESTROY`. Select must not change
immutable EXEC ancestry. An exit releases exactly that PSP's inherited/JFT
references, search cursors and locks; VM teardown invalidates pending tokens
and drains completions before freeing descriptions. Failed EXEC restores the
parent and returns every acquired reference. TSR retains only resident owners.

SDA synchronization includes InDOS/critical-error state, last error, DTA far
pointer, selected/owning PSP and default drive; expose consistent 34h and 5D06h
pointers. [RBIL SDA](https://fd.lod.bz/rbil/interrup/dos_kernel/215d06.html)
defines versioned offsets; do not transplant a DOS3 layout into the DOS4 view.
Per-PSP context records own DTA/JFT/extended-error associations. They refer to
the VM's shared per-drive CDS/default-drive object: ordinary DOS cwd is not
rolled back on child exit. This corrects f3-48's phrase “per PSP as in DOS”:
private cwd snapshots would be a different observable policy. AH=50h selects
the context but does not copy independent volume state. Lead review must
explicitly acknowledge this wording correction.

### DF3: namespace and the private image

Guest C: is the VM's private FAT16 base plus write overlay. Mount that logical
volume **once**, in a private VFS instance, through a virtual block provider
that uses `dosvm_disk_read/write`'s f3-47 cache/overlay primitives without
executing INT 13h. Both BIOS sector requests and FAT file access use that same
provider/generation. Native exposed volumes use the existing global VFS and
its existing node/share registry, not copies in the private instance. A guest
drive-view entry selects private or global VFS table plus backing letter and
mount generation. Guest letters never become global mount identities.

Keep E: for the exposed removable stick when present; expose the native system
volume as D: read-only by default. Additional approved native letters preserve
their names except collisions, resolved in ascending free-letter order and
logged when the VM is created. A:/B: are absent unless assigned virtual media;
no physical disk, raw native volume or host path escapes the view. Existing
mount write protection and launcher policy always win. Absence, media generation
change and denied access are distinct results. An alias cannot bypass a claim.

This is a new **mount adapter**, not another cache. The FAT adapter must accept
the f3-47 block-provider interface (including overlay updates, generation and
short compact-image zero tail); do not allocate a second private FAT data
cache. The f3-47 cache remains charged to its VM and reclaim policy; outer VFS
uses the common physical cache. Map cluster chains with native FAT code, not
new FAT parsing in `dos_file_dispatch`. Mapped-chain metadata is invalidated
on every overlapping raw sector write, allocation or mount generation change.
Serialize sector/file writes under one private-volume gate. Host native
volumes reject raw writes while mounted. No incoherent dual mount is allowed.
This choice avoids a separate VMM path walker, while preserving f3-47's work.
The existing indefinitely retried disk gate is not a valid deadline interface.

Paths are DOS case-insensitive CP437 8.3 views; preserve trailing-dot, drive
relative (`C:FILE`), absolute, `.`/`..`, wildcard and device behavior. Use actual
FAT short aliases where present. For a non-FAT/read-only source without aliases,
create a stable directory-generation alias map: uppercase CP437, remove spaces
and extra dots in the stem, replace invalid/unmappable characters by `_`, use
the final dot for a three-character extension; preserve a legal unique 8.3
name, otherwise truncate stem to `8-1-digits(N)` and append `~N`, N=1..999999.
Reserve every real short name first, then allocate remaining aliases in byte
order of original UTF-8 names with a stable node-id tiebreaker. No reuse within
a live directory generation. Fail explicitly if the alias space is exhausted.
This is deterministic MS-DOS-style naming, not a claim to reproduce every
undocumented VFAT collision heuristic. Writable FAT aliases use its persistent
short entries and existing generator. A 300-entry scan must not rebuild an
alias map on every FindNext.

Find DTA is exactly 43 bytes: 00h–14h reserved, attribute 15h, time 16h, date
18h, size dword 1Ah, NUL-terminated short name 1Eh (13 bytes).
[FindFirst format](https://fd.lod.bz/rbil/interrup/dos_kernel/214e.html).
Reserved bytes hold a versioned opaque cursor token, VM/PSP search id and mount
generation, never a host pointer. Copying the DTA preserves the cursor;
independent DTAs interleave. Validate tokens against bounded VM state, reclaim
on termination/exhaustion, never scribble past byte 42. Preserve DTA on failure
unless the documented call explicitly defines changes. Honor directory,
hidden/system and volume-label search rules, `*.*`, `?`, empty directories,
missing leaf and missing parent separately. Report real packed FAT dates.

### DF4: sharing and record locks

Normal deny modes are bidirectional: reject if `(new access & old deny)` or
`(old access & new deny)`. Encode READ=1, WRITE=2; DOS deny 1/2/3/4 maps to
ALL/WRITE/READ/NONE, not enum ordinal. Compatibility mode is a separate mode,
not deny-none: same owner may reopen compatibility; different PSPs may share
read-only files for reading, otherwise conflicting compatibility opens invoke
the documented critical-error path. Mixed compatibility/explicit opens use the
DOS 2–6.22 matrix [RBIL #01403](https://fd.lod.bz/rbil/interrup/dos_kernel/213d.html#table-01403),
including the read-only exception and asymmetric INT24/error-5 outcomes.
Do not silently use DOS7's differing access-time mode matrix. Native opens
participate in the same node claims with distinct owner identities; compatible
reopens cannot waive native explicit denial. SHARE behavior is always enabled
for the Ciuki bridge; the reference boots FreeDOS 1.3 with SHARE loaded.

5Ch uses BX handle, CX:DX offset, SI:DI length, AL=0 lock/1 unlock; arithmetic
uses a widened end to detect 32-bit overflow. Lock identity is node plus owning
PSP and open description; inherited/duplicated descriptions preserve identity,
but a child has a different effective PSP. A conflicting read/write/lock fails
with extended 33; exhausted lock storage is 36. Unlock requires the owner's
exact range; do not silently split/unlock another owner. Close/exit rules and
overlaps, zero length and EOF-spanning ranges are differential cases, not a
POSIX advisory-lock substitution. Release all locks on process termination and
the DOS-defined close of an owning handle even when duplicate refs remain.
See [5Ch reference](https://fd.lod.bz/rbil/interrup/dos_kernel/215c.html).

### DF5: errors, critical continuations and bounded waits

Return CF/AX per function; FCBs use AL and transfer-count/status conventions,
not CF errors. EOF read succeeds with AX=0; partial transfers return their
actual count. Failed seeks/open/dup do not publish partial table state. 6C00h
is atomic under a node claim; DL's exists actions 0/1/2 and absent actions 0/1
return CX=1 opened/2 created/3 replaced; honor BH's no-critical-error and
auto-commit bits. Invalid reserved combinations fail with 87. See
[6C00h](https://fd.lod.bz/rbil/interrup/dos_kernel/216c00.html).

59h/BX=0 returns the saved extended AX, BH class, BL recommended action, CH
locus. Publish before INT24, preserve through it and through 59h itself; never
replace it with a logger's/VFS-cleanup error. The following is the bridge's
explicit initial mapping; legacy 00h–12h calls additionally use their documented
DOS2-visible error mapping. Reference discrepancies require a recorded review,
not changing tests to accept any nonzero code.

| Failure | Extended AX (decimal) | BH / BL / CH |
| --- | --- | --- |
| Unsupported function / invalid access / invalid parameter | 1 / 12 / 87 | 7 / 4 / 1 |
| Missing leaf / parent | 2 / 3 | 8 / 3 / 2 |
| JFT/SFT full | 4 | 1 / 4 / 1 |
| Permissions or forbidden mutation | 5 | 3 / 3 / 2 |
| Invalid handle | 6 | 7 / 4 / 1 |
| Allocation failure | 8 | 1 / 4 / 5 |
| Invalid drive / current-dir removal / cross-volume rename | 15 / 16 / 17 | 8/3/2 ; 3/3/2 ; 9/3/2 |
| Find exhausted | 18 | 8 / 3 / 2 |
| Write protected / not ready / read fault / write fault | 19 / 21 / 30 / 29 | 11/7/2 ; 11/7/2 ; 5/1/2 ; 5/1/2 |
| Sharing / lock conflict | 32 / 33 | 10 / 2 / 2 |
| Lock table full / disk full | 36 / 39 | 1 / 4 / 2 |
| Create-new collision | 80 | 8 / 3 / 2 |
| INT24 Fail | 83 visible when specified; preserve underlying extended cause | class/action/locus of cause |

The [extended-error register](https://fd.lod.bz/rbil/interrup/dos_kernel/2159_bx0000.html)
defines these code domains; values above are Ciuki's reviewable mapping policy.
EROFS from media is write-protect, policy denial is access-denied; EBUSY from
sharing is not an undifferentiated hardware error. ENOENT resolution identifies
parent/leaf before mapping. Timed-out storage retains its media/read/write
cause; cancellation on VM destruction does not invent a guest completion.

INT24 removable-media failures use the existing media generation/error records.
Suspend the original call at a safe continuation, release all VFS/VM gate
locks, publish InDOS/critical state, construct the guest handler's documented
register/stack frame and invoke its current vector. AL=1 Retry resubmits only
after revalidation and within the original deadline; AL=2 Abort uses the normal
DOS process termination path (parent survives); AL=3 Fail returns the mapped
error. AL=0 Ignore is honored only when allowed by the INT24 input flags;
otherwise Fail. A handler may query 59h or use allowed console calls without
recursive filesystem entry. A silent guest handler is bounded by the call
deadline; expiry fails the call or stops that VM cleanly, not the desktop.
[Critical-error frame](https://fd.lod.bz/rbil/interrup/dos_kernel/24.html).

Every request carries VM generation, PSP, sequence, mount generation, absolute
monotonic deadline and cancellation token. Default total wall-time budget is
2000 ms for local file calls, including VFS admission, device wait, retries and
critical callbacks; 440Bh cannot extend it. Cache-only monitor work yields at
bounded preemption points; no file operation holds interrupts disabled across
I/O. Use a genuinely cancellable/timed VFS reservation/transport interface:
checking elapsed time after synchronous `vfs_read()` is insufficient. Preserve
FIFO ordering when cancelling a ticket; do not leave holes or leaked locks.
Reuse the storage asynchronous/deadline work if integrated, otherwise f3-49
implements the needed internal subset. `vfs_wait` records include original
reason, drive, owner, elapsed time and timeout/cancellation attribution.

### DF6: measurements and compatibility closure

On a T23-class CPU, warmed 4 KiB reads take at most 60 microseconds of monitor
CPU time; a 300-entry FindFirst takes at most 2 ms. Measure min/p50/p95/max over
10,000 reads and 100 scans, cold versus warm separately, copying to guest
memory included. Report wall time, host-ready wait, monitor CPU and guest IF
time separately with executable/image hashes and clock calibration. A host
Python or x86emu duration is never T23 evidence. Native/VM copies and audio
load must not bypass the bounded wait policy.

File reads after mount/cluster mapping issue zero guest INT13 calls. Cold
mapping and deliberate raw-sector tests have separate counters. Cite physical
device cache misses separately from f3-47 hits. A per-call trace records entry
function and AL/BX/CX/DX/DS/ES/SI/DI, PSP/origin; return CF/AX and result
registers, byte count, elapsed wall/monitor time, VM generation/sequence and
dropped count. Pair returns at the actual guest return boundary, not at initial
vector dispatch. Trace in a bounded RAM ring and drain after the workload;
no per-read synchronous UART logging. f3-49 owns this production addition.

Acceptance requires every [inventory](../validation/f3-48-dos-api-inventory.md)
row's tests, the identical COM battery against pinned FreeDOS 1.3, SHELL.COM
built-ins, COM/MZ EXEC and rollback, 16/32-bit DPMI clients, DOS/4GW translation,
DJGPP Quake and the three original library games. No source rewrite or patched
game/extender. f3-49 covers handles/state/errors, f3-50 FCB/NLS/handoff and dead
paths, f3-51 reference/game/performance closure and opt-in fallback. The owner
permits QEMU for this battery only; ordinary QEMU gates remain suspended.

**[F3-51c]** Game launch digest cache. The f3-51 item 8 identity (SHA-256 of
the whole base, `FREEDOS.SHA` check, overlay `G<g>K<k><sha>V<gen>.COW`,
`game_launch` record) is unchanged; `dosvm_disk.c` only avoids re-hashing
unchanged bytes. With the base open deny-write, `vfs_handle_version` gives its
content version (`vfs-storage-contract.md`, write generations); a matching
entry of a 16-entry session LRU supplies the digest, otherwise the base is
hashed once and the digest stored only if the version is unchanged after the
hash. Any write, truncate, rename over, delete/recreate, raw change, media
change or remount yields another version; the first launch after boot always
hashes. `game_launch` adds `cached=1|0`.

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

## Personality changes recorded

2026-10-11, directive f3-00 (implementation; guest qualification pending):

- The private FAT16 payload assembles the existing `src/runtime/ciukidos.asm`
  include of `src/boot/floppy_stage1.asm`; neither source is changed for this
  directive. Geometry remains 262144 sectors, 8 sectors/cluster, 73 reserved
  sectors, two 128-sector FATs, 512 root entries, 63 sectors/track and 16 heads.
- The recipe retains external XMS, external shell boot, disabled autorun,
  self-test/runtime probes/debug traces and hardware validation screen.
  `ENABLE_PS2_MOUSE_INIT=0` suppresses the legacy physical mouse initialization
  in this text-only VM payload. It is a fixed personality build recipe, not a
  test variant; XMS and full virtual devices remain later directives.
- `COMMAND.COM` is the existing `COMMAND_COMPAT=1` shell. A new small
  `SYSTEM\SHELL.COM` bootstrap runs AUTOEXEC through `COMMAND /C`, then the
  interactive interpreter. AUTOEXEC runs DOSREADY.COM; shell return writes
  zero to the VM control port. No FreeDOS source is included.
- DOSREADY, MEMORY and CMDSTAT print through DOS and report bounded ASCII
  lines through virtual port E9. EXIT37, VMPRIV and VMPORT are local test
  payloads; the latter two request a privileged-instruction/forbidden-port
  fault. The base is read-only with an ephemeral per-generation file overlay.
- Virtual equipment, conventional memory, A20, BIOS time/date and text/input
  services come from private memory and host scheduler time. Extended memory,
  XMS, EMS and DPMI are not advertised. These are firmware service answers,
  not changes to CIUKIDOS's INT21 implementation. See
  [f3-acceptance.md](f3-acceptance.md) for sources, records and remaining gates.

2026-10-11, directive f3-04 (implementation; QEMU qualification pending):

- `src/boot/floppy_stage1.asm` gains a fourth EXEC return-context slot
  (kernel, SHELL.COM, COMMAND.COM, program and extender or child), kept
  outside the EXEC allocator snapshot.
- INT 21h/48h with BX=0 grants one paragraph instead of failing; DOS/4GW
  allocates the DPMI host private area this way even when 1687h reports SI=0.
- DPMI is answered (INT 2Fh/1687h) only in VMs created with
  `dosvm_config.dpmi` while `DOSVM_DPMI_PUBLIC` is 0. The DOS image adds the
  f3-04 clients and `SYSTEM\DOS4GW.EXE` (Open Watcom 2.0 runtime, pinned
  hash, redistribution text not located). Details and sources:
  [f3-acceptance.md](f3-acceptance.md), section f3-04.

### F3-47 private image read cache and disk measurements (2026-10-19)

Each VM owns an LRU of 64 KiB blocks at the private disk boundary. Amendment 1
sizes it to the base image rounded up to 64 KiB, capped at 16 MiB. If this
does not fit the admitted commitment's remaining headroom, it falls back
to 2 MiB (or the smaller image size). Creation reserves the future worker's
stack/task charge before checking headroom; reopening includes its existing
charge. Insufficient headroom even for the fallback returns `ENOMEM`.
Buffers are physical 16-page runs,
claimed as `MM_VM_PRIVATE`, included in `dosvm_snapshot.pages` and
`info.resident`, and released on close or failed creation. They consume the
admitted private allowance. Amendment 2 raises the default commitment to
48 MiB while retaining the 24 MiB eager DPMI/XMS/EMS pool and independent
32 MiB host headroom. The final creation check excludes separately charged
cache pages from the 8 MiB low/device/table bound, while retaining the total
commitment bound. Normal default creation can therefore hold a 12 MiB image
or the full 16 MiB ceiling without a synthetic commitment override. Smaller
explicit commitments retain the 2 MiB fallback. Admission remains
`request + headroom <= free - (reserved - resident)`; low-memory hosts can
refuse admission rather than partially start a VM. Host budget models and
their limits are recorded in the f3-47 validation note.
No free 64 KiB run returns ordinary `ENOMEM` with complete unwind.
The base remains deny-write, and reopening releases every old cache entry.

A fill reads the base through VFS, zeroes a cut image's tail, then merges
all indexed overlay sectors in the region before publishing validity.
Successful writes synchronously update the overlay and any resident block.
Failed writes invalidate that block because a partial transfer may already
have changed an indexed sector. Failed read-ahead publishes no cache entry;
the requested sector is retried against its authoritative source. The disk
kwait gate still covers seek/transfer/publication. No new yield is introduced
inside it or the worker's INT 13h transaction. Cache allocation checkpoints
run before the VM starts, after accounting is published.

Every existing five-second perf interval now emits two `vm_perf_disk` rows:
`reads`, `bytes` (actual VFS bytes returned), `writes`, `hm` (sector cache
hits/misses), and `regions` (exact distinct 64 KiB regions requested); and
`part=lat bins=a,b,c,d,e` for all INT 13h service calls, in microseconds:
`<100`, `[100,1000)`, `[1000,10000)`, `[10000,50000]`, `>50000`.
Latency is elapsed service time including scheduler suspension, unlike the
existing vector CPU-time counters. Without TSC calibration it falls back to
millisecond ticks. Detached interval banks keep reporting/resetting separate
from the next producer interval. VFS transfers include filesystem-cache hits;
these records are not physical ATA command counts.

`vm_perf_mem` records retain the first/latest successful 0500h largest/total
free-byte results and the latest successful 0501h request and remaining
largest/total. They use the perf cadence, with no synchronous logging in the
DPMI path. Allocation decisions and guest return values are unchanged.

Reference: [Microsoft MS-DOS SMARTDRV help](https://www.infania.net/misc/dos622help/smartdrv.html)
(read-ahead with write-behind disabled), and
[id Software's zone allocator](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/z_zone.c).
Host evidence and the DOS `-maxzone`/guest disk-icon discrepancies are recorded
in [F3-47 validation](../validation/2026-10-19-f3-47-disk-cache.md).

Lead review, Amendment 1: the bottom-right floppy icon belongs to DOS DOOM,
whose `I_BeginRead`/`I_EndRead` draw and restore it around WAD reads; the
desktop has no disk-activity tray icon. The original tray-indicator and
100 ms hold-time requirement is withdrawn. Shorter service latency reduces
the time the game's icon is visible, as with a DOS read cache; cache hits
do not suppress the game's drawing and no frame-count guarantee follows.
The [DOS platform reconstruction](https://github.com/gamesrc-ver-recreation/doom/blob/master/i_ibm.c#L1427-L1603)
caps `I_ZoneBase` at `0x800000` (8 MiB); the bundled DOS DOOM 1.9 parses no
`-maxzone` option. The launcher stays unchanged. A modeled 16 MiB zone is
only a capacity experiment, not a supported configuration of this binary.
The 0500h/0501h verification and `vm_perf_mem` records remain applicable.

### F3-46 pageable pool, protected I/O and the combined gate (2026-10-20)

Contract D1 of [Windows 98/ME-class resource management](win98-class-resource-management.md),
on the f0-47 objects, f0-48 replacement and f0-49 pagefile. Research and
host evidence: [F3-46 validation](../validation/2026-10-20-f3-46-pageable-pool.md).

**Quota, commitment, residency, pins.** The guest-visible XMS/EMS/DPMI pool
(address quota, 24 MiB by default) is one memory object per VM
(`vm/dosvm_pool.c`): a committed slot per page, a frame only while resident,
a pager slot while paged out. XMS EMBs, EMS handles and DPMI blocks are page
ranges of it; the pool linear address and each EMS frame window are aliases
of one page and fold their access/dirty bits into its PFN descriptor before
any translation is revoked or replaced (window remap, page-out, discard). The
pool's quota moves from the VM's admission promise (`MM_VM_PRIVATE`) to a
`MM_VM_POOL` commitment admitted by `ledger_commit` (RAM or published
pagefile; a refusal first asks the pager to grow, then refuses before any
state is published). Allocation-time zeroing returns pages to demand-zero
instead of clearing frames; freed blocks keep their bytes as before. Pins
(per page, mirrored in the PFN descriptor) keep resident: the DPMI host stack,
DPMI 0600h-locked pages (all or nothing; 0601h releases, 0703h discards only
whole unlocked pages, 0702h drops access history), the pinned VBE linear
buffer (the presenter and full-screen flush read it and its PTE dirty bits
from other tasks), and each unmasked virtual DMA channel's 64/128 KiB window
where it overlaps the pool. Low memory (conventional, UMB, HMA, ROM and the
VGA window frames) stays resident: a deliberate difference from Windows 98,
whose V86 memory is pageable, because the monitor, the VGA model and DPMI
real-mode stacks use it directly.

**101-d direct VBE amendment.** DPMI 0800h continues returning the VM-local
pool address for `DOS_VBE_PHYS`, never a physical BAR. During exclusive native
full screen, a matching provider may temporarily replace only its visible
page-rounded PTEs with borrowed LFB pages. Original RAM stays pinned and
charged, excluded from replacement and the global sampler. Guest dirty bits
remain frame evidence, but no per-frame copy is needed. Kernel transfers and
screenshots read the exported LFB alias while mapped. The display-driver-model
101-d protocol holds the VM, withdraws guest access, copies back, restores
PTEs and only then restores desktop ownership. Hotkey/focus loss, stop, fault,
mode change and entry rollback all revoke; any admission refusal retains the
pool-backed copy path. A parked VM cannot be destroyed during the transfer.
Revocation reports whether copy-back and PTE restoration have completed;
another continuation copying is not completion. A leave that yielded before
copy-back began must defer provider restoration and retain the VM hold when
revocation is incomplete, even after park timeout. Mode changes own a separate
pin, never a saved copy of the display worker's busy flag. Existing poll/leave
continuations recheck that pin after yielding; return resumes on a later poll
once the copy and mode-change continuation finish.

**Locked compatibility mode.** `dosvm_config.memory_locked`, set per game by
the launcher (`/system/settings/dos-game-N.cfg` line `DOS memory=locked`,
F12 key 4, `--dos-memory=`) and passed in CREATE `launch.memory = 1`, keeps
the eager, zeroed and wholly pinned pool of f3-47 (the role of the Windows 98
PIF "XMS memory locked"/"EMS memory locked" flags). Default is paged.

**Blocking points.** A pool page is materialized (zero) or paged in only in
task context with physical IF set: (1) a guest not-present #PF on a pool or
EMS-window page, resolved at the trap entry before the transaction hold and
before any emulation (the instruction restarts); (2) a kernel transfer,
which resolves each page immediately before its bytes move (a page with a
present translation is copied directly; a kernel store into a pool page
always advances the object's dirty version, since it sets no PTE D bit); (3) the
destination of string `IN` (V86 through the monitor pointer, DPMI by an
explicit prefault) and the RAM source of a VGA `REP` run, before the device
or plane effect; (4) DMA pins at the trap's settle point, after the `OUT`
that programmed the channel completed; (5) the guest stack page before an IRQ
is committed with physical IF clear. While the worker is inside a monitor
transaction (`hold`), the provider refuses, defers or keeps this VM's pages,
so a page resolved in that transaction stays resident to its end; idle and
BIOS waits release the hold. The clock work's virtual DMA reads resident
frames only: a page whose pin is pending is an unanswered DREQ, never a
fault. A failed page-in is contained (transfer `EFAULT`, guest fault stops
the VM); zeros are never fabricated.

**Disk path.** Private image reads and writes keep the f3-47 VFS path (the
VFS uses the f1-83 request framework below it) and the per-VM gate that keeps
each seek+transfer pair atomic; no explicit-offset VFS read exists, so the
gate is retained rather than replaced. The read cache's 64 KiB blocks are
clean pages of the f0-48 replacement (discarded without I/O, never
guest-visible); a reclaimed block regrows on a miss only while the system is
out of pressure. The C5 page identities (256 entries, no data) cannot yet
replace the windows with their batching benefit, so the cache participates in
global reclaim instead. The 1024-row overlay index is accounted separately
(`dosvm_ledger.overlay_rows`) and is never reclaimed. The private image is
opened deny-write and its native mount reads through the same gate and cache,
so no second, incoherent mount of the image exists.

**Scheduling boundary.** PIT/SB advancement stays in the clock work, which
never waits for a page. Per elapsed tick, a pending guest IRQ is attributed to
the worker's page-in wait, the guest's own virtual IF clear, or host
descheduling (`vm_perf_page part=wait irq_ms=page/guest/host`).

**Reports.** Every perf interval of a VM with a pool emits three
`vm_perf_page` rows: mode, quota, resident, swapped and pinned pages and
fill/page-in/page-out/discard counts; waits and the IRQ split; cache pages,
reclaimed/regrown blocks and overlay rows.

**Accounting change outside the VM.** The f0-48 sampler no longer clears or
folds A/D bits of frames that are pinned or not of a replaceable owner
(`MM_USER_PRIVATE`, `MM_USER_SHARED`, `MM_VM_POOL`): the VGA window (VM
private) and the pinned VBE buffer harvest by PTE dirty bits.

### fx-4 creation refusal records, unified partition, cancelled cold hash (2026-10-23)

T23 trial of image `9a60215f` (source `9e6e8721`): DOOM and Quake windows
ended `[launch] … error=-5 stage=vm-create` with no other record, and the
CiukiDOS window `error=-4`. Root cause: the game inventory in
`dosvm_programs.c` (run by every game CREATE before admission) accepted only
MBR type `06h`; the canonical `UNIFIED.IMG` carries the reference MBR of
`scripts/dos_conformance.py` (one active type `0Eh` FAT16 partition at
LBA 63), so it mounted the MBR itself as a BPB, the FAT driver refused it
(`EINVAL`, "BPB signature/sector size/jump") and `closed_error` reported
`EIO`. Low memory was not involved: zone 1 stayed at `usable=3840
free=574` in all 47 ledger snapshots, before and after each refusal, and
every creation allocation is `ZONE_NORMAL` first (low pages, pool, read
cache, worker).

- **One C: rule.** `partition_dos_primary()` (`fs/partition.h`) is the
  FreeDOS kernel's `ScanForPrimaryPartitions` order
  ([initdisk.c](https://github.com/FDOS/kernel/blob/master/kernel/initdisk.c):
  first active primary FAT partition, else the first primary FAT one) over
  the FAT system IDs `01h 04h 06h 0Bh 0Ch 0Eh`
  ([Microsoft Disk Partition Types](https://learn.microsoft.com/en-us/windows/win32/fileio/disk-partition-types),
  [PARTITION_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntdddisk/ns-ntdddisk-_partition_information):
  `PARTITION_XINT13` is the Windows 95 FAT16 partition using INT 13h
  extensions). The VM's C: view (`dos_file_vm.c`), game inventory/removal
  (`dosvm_programs.c`) and save export (`dos_save_store.c`, which had the
  same type-`06h`-only test) now share it; a disk without a valid table
  keeps the whole-disk (superfloppy) view.
- **Refusal record.** Every failed `dosvm_create` emits one
  `[dosvm] create refused stage=<stage> error=<internal errno> game=<n>
  reason=<memory|vm-slots|owner-gone|busy|not-installed|invalid|io|…>`
  record before the client's `[launch]` line. Non-memory refusals add
  `step=` (inventory or disk-open step) and `detail=` (FAT mount
  diagnostic); admission refusals add the arithmetic (`free_kib`,
  `outstanding_kib`, `request_kib`, `headroom_kib`); both add
  `dma_free_pages` and `normal_free_pages`. The window ABI error mapping is
  unchanged.
- **Cancelled cold hash.** `dosvm_config.cancelled` lets the creating owner's
  exit end the cold base identity hash at the next 64 KiB block with
  `EINTR` (nothing cached). On the trial the CiukiDOS CREATE ran from about
  289 s (DOS Box input boost until 289.526 s) to the `maximum-vm-created`
  snapshot at 308.784 s (cold hash of the 27,676,672-byte base,
  `cached=0`); the owner had been ended meanwhile, so the completed VM was
  closed `owner-gone` 17 ms later and CREATE returned `EINTR`. That
  cold-launch cost is the open f3-63 start-up item, not changed here.

### fx-4b background verification of the DOS system image (2026-10-23)

Lead decision 2026-10-23 13:46: the user never waits for the cold SHA-256 of
`UNIFIED.IMG` (about 20 s on the T23, fx-4). Windows 98 does not hash its
system files at each start: System File Checker (`Sfc.exe`) "verifies the
integrity of system files" on demand against a verification data file
(`Default.sfc`), and only Registry Checker runs at every start (Windows 98
Resource Kit, `wrkc27_2.htm` and `wrkc10_7.htm`, read in place; public
summary: [System File Checker](https://en.wikipedia.org/wiki/System_File_Checker),
[Microsoft: System File Checker](https://learn.microsoft.com/en-us/windows/win32/wfp/system-file-checker)).
CiukiOS keeps the f3-51 guarantee (the digest names the bytes the VM boots
and must equal `UNIFIED.SHA`) and moves the work out of CREATE
(`src/kernel/vm/dosvm_image.c`):

- **Background check.** The first full desktop frame (or the console
  stand-in) arms one verification; deterministic test and hardware-check
  boots keep the synchronous path. After 5 s (or at once when a launch asks)
  an idle-class task hashes the image in 64 KiB blocks with 4 KiB hash steps
  and preemption points, pausing at least three times each block's read time
  (at most a quarter of the disk; no starvation boost applies to the idle
  class). A launch that watches (`CIUKI_DOSVM_IMAGE_QUERY`) or waits (CREATE)
  makes it urgent: no pauses, normal class, and a CREATE waiter lends its
  priority through the `kwait` owner. Results go to the session digest cache.
- **Verified stamp** `C:\SYSTEM\DOS\UNIFIED.VFY`, 128 bytes: magic, the
  file's on-disk identity (size, first cluster, directory cluster and slot,
  write date and time), the digest, and a SHA-256 of the record. It is
  written only when a complete hash matched `UNIFIED.SHA` and the version did
  not change during the hash. A later boot reuses it, without reading the
  image, only when the identity and the current `UNIFIED.SHA` match. FAT has
  no persistent write generation, so the stamp also lapses when the volume
  was recovered dirty at this mount (FAT[1] `ClnShutBitMask`: "If bit is 0,
  volume is dirty … the file system driver did not Dismount the volume
  properly", Microsoft, *FAT: General Overview of On-Disk Format* 1.02,
  [copy](https://www.cs.miami.edu/home/burt/learning/Csc521.071/docs/FatFormat.pdf))
  or a `REMOVEn.PND` game-removal marker exists; game removal, the kernel's
  only writer of the image, stops a running check (it held the image
  deny-write), deletes the stamp after committing its marker and re-arms the
  check. Residual risk, as in Windows 98: a foreign in-place edit that keeps
  size, clusters and time stamp is detected only by an explicit re-check
  (delete the stamp).
- **Launches.** DOS Box polls the check and shows "Checking the DOS system
  files: N%" with a determinate bar, answers desktop pings and honours a
  close request, then calls CREATE. CREATE uses, in order: the session cache,
  the in-flight or armed check of the same version (waiting, EINTR when the
  owner stops, never a second hash), a valid stamp, else the synchronous hash
  (stamped when it matches). The `game_launch` record names the source
  (`session`, `background`, `stamp`, `hash`).
- **Mismatch** is published with the actual digest, recorded
  (`[dosvm] image verify result=mismatch expected=… actual=…`), never
  stamped; CREATE refuses at the sidecar check
  (`[dosvm] create refused stage=disk error=-5 … step=base-sidecar`) and DOS
  Box names it ("The DOS system files are damaged").
- **Records:** `[dosvm] image verify stage=start bytes=…`,
  `result=verified|cancelled|stopped source=… bytes ms blocks pauses`,
  `result=failed step=… error=…`, `[dosvm] image stamp write error=…`.

Host evidence: `tests/host/dos/test_image_verify.py` (production FAT/VFS:
background hash and progress, stamp reuse without image reads, re-hash on
write time, pending removal, dirty volume and sidecar changes, launch joining
the in-flight check and EINTR on owner exit, mismatch never stamped, removal
hold/release), `tests/host/dosvm_fx4_test.inc` (`test_fx4b_launch_paths`:
CREATE with each result, record sources, mismatch refusal) and the DOS Box
replay scenarios 20 and 21 (`tests/host/desktop/dosbox_loading_test.c`).

### fx-23 amendment: no launch hashes, CREATE off the window thread (2026-10-23)

T23 trial f441c5bd: right after the background check reported
`result=verified`, DOOM's CREATE ran about 53 s inside the kernel (DOS Box at
35–47 % CPU, all kernel time; its first 2 s of compute are consistent with a SHA-256 of the
27.7 MB base on the T23), the window thread was inside that call, answered no
desktop PING and was marked "Not responding"; closing it turned the finished
CREATE into `[launch] … error=-4 stage=vm-create`.

- **Finished results count.** `dosvm_image_lookup` uses a check that already
  finished in this session for the same file version, before the stamp. The
  stamp's "recovered dirty at this mount" lapse does not apply: that hash was
  made after the mount.
- **No synchronous launch hash while a verifier exists.** When neither the
  session cache, the finished check nor the stamp matches, the lookup re-arms
  one fresh background check of the current version and waits for it
  (cancellable by the owner, progress in `CIUKI_DOSVM_IMAGE_QUERY`). Only boots
  without a verifier (test-request, hardware check) or a held image still hash
  in `base_identity`.
- **CREATE on a worker thread.** DOS Box issues CREATE from a `du_job`
  worker. The window thread keeps answering PING, CLOSE and FOCUS, shows the
  check's percentage when CREATE waits for it and otherwise "Starting the DOS
  session..." with the 250 ms pulse, and sleeps in `poll()` (20 ms, then
  100 ms) between turns. A close during CREATE ends the process; the kernel
  sees the owner stopping and cancels or releases the VM (`owner-gone`).
- **Unresolved in the records.** The trial's records do not name the code
  that kept CREATE in the kernel for the remaining ~50 s (it held the CPU in
  short turns alternating with the storage writer, with no disk I/O, while
  the boot log and Files waited). The writer decoupling and the
  `vfs_hold_long`/`writer_busy` records of the storage amendment
  (`vfs-storage-contract.md`, fx-23) remove the writer's per-event wakeups and
  name any such hold in the next trial.

Host evidence: `tests/host/dos/test_image_verify.py` section 7 (finished
result reused on a recovered volume with no disk reads; an identity change
after the check re-arms it, the hash runs on the verifier) and DOS Box replay
scenarios 22 (a 6 s CREATE: PING answered, three check-progress paints,
pulses, 61 polls of 100 ms) and 23 (close during CREATE).

### DOS Box memory: XMS in front of the guest INT 2Fh chain (2026-10-24)

**Defect.** On the image of `9fe0a02e` Wolfenstein 3-D 1.4 shareware
(`C:\WOLF3D\WOLF3D.EXE`) quit at once with "You do not have enough memory to
run Wolfenstein 3-D". DOOM (DOS/4GW) and Quake (DPMI) were unaffected.

**Research.**
- XMS 3.0 ([text](https://www.phatcode.net/res/219/files/xms30.txt)):
  "The recommended way of determining if an XMS driver is installed is to set
  AH=43h and AL=00h and then execute INT 2Fh. If an XMS driver is available,
  80h will be returned in AL"; AX=4310h returns the control function in ES:BX;
  "the base XMS driver continues to be the only one installed on INT 2Fh
  AH=43h" ([RBIL 2F/4300h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4765.htm)).
  HIMEM.SYS is a CONFIG.SYS driver, so its INT 2Fh hook sits in front of the
  DOS kernel's handler.
- Windows 95/98 MS-DOS sessions are V86 VMs of the VMM (not NT's NTVDM):
  the Windows XMS driver serves every VM with "its own set of handles" and
  passes unknown handles to the real-mode HIMEM
  ([The Old New Thing, 2016-05-03](https://devblogs.microsoft.com/oldnewthing/20160503-00/?p=93397));
  EMS, XMS and DPMI are per-program PIF memory settings, "Auto" by default
  ([Windows 95 Resource Kit, MS-DOS-based applications](https://techshelps.github.io/MSDN/DNWIN95/HTML/S65BF.HTM)).
- The FreeDOS 2043 kernel's INT 2Fh (`kernel/int2f.asm`, unified sources
  `third_party/freedos/unified/kernel-2043-sources.zip`) answers 11h, 12h,
  16h, 4A01h/4A02h, 10h, 08h and 14h and returns every other function with
  IRET (`Int2f?iret`): it does not chain AH=43h. The VMM answered 4300h/4310h
  only in its ROM default handler (F000:02F0), behind DOS, so after the
  kernel installed INT 2Fh no program saw XMS. The retired CiukiDOS kernel
  chained to the ROM, which is why Wolf3D ran on 2026-10-12 (f3-06).
- FreeDOS 1.3 `COMMAND.COM` is FreeCOM 0.85a XMS_Swap (`BIN/COMMAND.COM` is
  byte-identical to `BIN/FREECOM/XSWAP/CMD-EN.COM`; `shell/init.c`
  `FEATURE_XMS_SWAP`): with XMS it swaps its transient part out while a
  program runs; without XMS it stays resident.
- Wolfenstein 3-D source ([id Software release](https://github.com/id-Software/wolf3d)):
  [`WL_MAIN.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/WL_MAIN.C)
  `InitGame` shows `ERRORSCREEN` and exits when `mminfo.mainmem < 235000`;
  [`ID_MM.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_MM.C)
  `MM_Startup` sets `mainmem = nearheap + farheap` from `coreleft()` less
  `SAVENEARHEAP` (400h) and `farcoreleft()` less `SAVEFARHEAP` (0)
  ([`ID_MM.H`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_MM.H)).
  `MML_SetupXMS` (UMBs) exists but `MM_Startup` never calls it: only
  conventional memory counts. The earlier statement in this contract that its
  memory manager uses XMS/UMBs is corrected here: UMBs are unused, and
  [`ID_PM.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_PM.C)
  pages graphics/sound into EMS (found only through the LIM "open handle"
  test, opening the `EMMXXXX0` device) or XMS when present, else main memory.
  The shipped `WOLF3D.EXE` is LZEXE 0.91 over a Borland C++ medium-model
  program: load image 287040 bytes, DGROUP at 3783h, `_stklen` 1000h,
  `_heaplen` 0, so c0 keeps a 64 KiB DGROUP and the far heap is the rest of
  the program's DOS block. The check passes when that block is at least
  **80BDh paragraphs (527312 bytes, 515 KiB)**.
- LIM EMS 4.0 Appendix B ([text](https://www.phatcode.net/res/218/files/limems40.txt))
  gives two detection methods, the "open handle" technique (DOS 3Dh on
  `EMMXXXX0`, IOCTL 4400h bit 7, 4407h) and the "get interrupt vector"
  technique (name at offset 0Ah of the INT 67h handler segment).

**Measurement** (host replay of the canonical `kernel=unified` guest on the
production VM code, `tests/host/dos/run_dos_memory.sh`; paragraphs in hex,
program `DOSMEM.COM` running from the prompt):

| | before | after |
| --- | --- | --- |
| INT 12h / DOS | 640 KiB / 7.10, DOS=LOW, no UMB link | same |
| Kernel low (`SC` text, `SD` data/BUFFERS=10/FILES=40/CDS/CIUKIDF.SYS) | 0A55h (42320 B) + 0357h (13680 B) | same |
| CIUKISH.COM + environment | 0028h + 0006h | same |
| FreeCOM resident while a program runs | **110Ch (69824 B)** | **0091h (2320 B)** (swapped to XMS) |
| SHARE | 0235h (9040 B) | same |
| COMMAND environment (top of memory) | 0040h | same |
| DOSREADY largest block at boot | 7BA2h (495 KiB; QEMU 7BD2h) | 8C1Dh (561 KiB) |
| Block a program gets at the prompt | ~7BDFh (507376 B): Wolf3D short by 19936 B | ~8C5Ah (574880 B): Wolf3D margin 47568 B |
| XMS (2Fh/4300h) | **absent** (AL=00h) | 3.00, HMA free (DOS=LOW), 24480 KiB free |
| UMB (XMS 10h query) | unreachable | largest 1700h paragraphs (92 KiB, C900h–DFFFh); DOS does not link them (DOS=LOW, no DOS=UMB) |
| EMS (INT 67h) | LIM 4.0, frame E000h, 512 pages | same; "open handle" detection fails (no `EMMXXXX0` DOS device) |

**Decision.** The VMM answers INT 2Fh AX=4300h (AL=80h) and AX=4310h
(ES:BX = C800:0000, the patchable XMS entry) at the V86 interrupt trap,
before the guest's INT 2Fh chain, as HIMEM.SYS does in front of DOS and as
the VMM already answers DPMI 1687h and MSCDEX 15xxh
(`src/kernel/vm/dosvm.c` `interrupt_dispatch`, `dosvm_xms.c`
`dosvm_xms_multiplex`; the ROM default handler keeps the same answer for
reflected calls). Other multiplex functions are untouched and reflect as
before. This is the smallest change: no CONFIG.SYS, FILES/BUFFERS, DOS=LOW
(f3-56 Amendment 1) or memory cap changes, no guest XMS/EMS driver, no new
reservation. The f3-48 conformance profile is unchanged; its `multiplex`
case already expects AL=80h from 2Fh/4300h, which both the pristine FreeDOS
1.3 reference and the unified guest now return.

**Not done (measured headroom, later decisions).** DOS=UMB with LOADHIGH
SHARE would move about 9 KiB into the 92 KiB UMB arena (done on 2026-10-25,
next section, with the `EMMXXXX0` device); DOS=HIGH would free
the 42 KiB `SC` block but needs the DFV2 final bind to accept kernel text in
the HMA (f3-56 Amendment 1). EMS visibility to "open handle" programs needs
an `EMMXXXX0` character device in the DOS device chain (IOCTL 4400h bit 7,
4407h ready); Wolf3D does not need it (it pages into XMS). Windows 98 sessions
typically show about 600 KiB free with DOS=HIGH,UMB; this profile gives
561 KiB.

**Risks.** DOS/4GW and Quake select the DPMI host through 2Fh/1687h before
XMS, so their path is unchanged; FreeCOM's swap now takes about 96 KiB of
the shared 24 MiB XMS/EMS/DPMI pool while a program runs.

Host evidence: `tests/host/dos/run_dos_memory.sh` (fails on the old VMM:
XMS absent, FreeCOM 110Ch resident, largest 7B74h, Wolf3D back at the prompt
after 1 s in text mode; passes after: XMS visible, FreeCOM 0091h, largest
8BEFh, Wolf3D still in mode 13h unchained after 8 s, pristine FreeDOS 1.3
sees XMS) and `tests/host/dosvm_test.c` (trap answer with a non-chaining
INT 2Fh vector). The Wolf3D replay sets `GUEST_NO_X87` so Borland's startup
uses its INT 34h–3Dh emulator, because the libx86emu runner has no x87.

### DOS Box memory: UMBs linked, SHARE high, the EMMXXXX0 device (2026-10-25)

**Gaps.** After the XMS fix above, two differences from a Windows 98 MS-DOS
session remained. (1) EMS was visible only to the "get interrupt vector"
test: a program that looks for the expanded memory manager by opening the
`EMMXXXX0` device got DOS error 2 and ran without EMS. Wolfenstein 3-D is
one: its page manager finds EMS only that way. (2) The 92 KiB UMB arena
(C900h–DFFFh) was reachable only through XMS 10h; DOS did not link it, so
SHARE (9 KiB) and FreeCOM's environment sat in conventional memory.

**Research.**
- LIM EMS 4.0, Appendix B ([text](https://www.phatcode.net/res/218/files/limems40.txt),
  "Testing for the presence of the expanded memory manager") gives the two
  tests a program may use. "Open handle": INT 21h AX=3D00h on `EMMXXXX0`; then
  IOCTL 4400h, where bit 7 of DX set means a device and not a file; then IOCTL
  4407h, where "the memory manager passes a status value of 0FFh" when it is
  ready; then close. "Get interrupt vector": `EMMXXXX0` at offset 0Ah of the
  INT 67h handler segment, the test device drivers must use. An EMS provider
  has to pass both.
- Wolfenstein 3-D [`ID_PM.C`](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_PM.C)
  `PML_StartupEMS` uses only the "open handle" test (3D00h, 4400h with
  `and dx,0x80`, 4407h with AL nonzero, close), then INT 67h 40h/46h/41h/42h/43h.
- Windows 98 Resource Kit, chapter 25 ("Configuring Applications"):
  "Windows 98 automatically provides expanded memory for MS-DOS-based
  applications that require it to run". Chapter 26 ("Optimizing Conventional
  Memory"): the methods "are the same as for MS-DOS 6.x": load real-mode
  drivers and programs "using devicehigh or loadhigh statements". The
  Windows 95 Resource Kit says the same about EMS
  ([MS-DOS-based applications](https://techshelps.github.io/MSDN/DNWIN95/HTML/S65BF.HTM)).
  No Microsoft document found says how V86MMGR exposes `EMMXXXX0` without
  EMM386 in CONFIG.SYS; the device follows from the LIM tests above.
- EMM386-compatible device: Jemm (`third_party/jemm/source-e96bb6bb.tar.gz`,
  [upstream](https://github.com/Baron-von-Riedesel/Jemm)) `JEMM16.ASM` declares
  `EMMXXXX0` with attribute `0C000h` ("character device, supports IOCTL - like
  EMM386"); `DEV.ASM` answers output status (10) and open/close (13/14) with
  done, writes (8/9/11) with write fault, unsupported IOCTL read functions
  with read fault, everything else with unknown command. DOSBox Staging
  ([`src/ints/ems.cpp`](https://github.com/dosbox-staging/dosbox-staging/blob/main/src/ints/ems.cpp))
  adds an `EMMXXXX0` DOS device whose device information is `0xC0C0` and
  writes the name at offset 0Ah of its INT 67h segment.
- IOCTL 4400h ([RBIL](https://www.ctyme.com/intr/rb-2820.htm)): for a
  character device bit 7 is set, bit 6 is EOF on input, bit 14 means the
  driver processes IOCTL requests. FreeDOS 2043 (`kernel/ioctl.c`) returns the
  SFT flags' low byte with the driver attribute's high byte, so attribute
  C000h gives DX=C0C0h; 4407h sends command 10 and returns AL=FFh unless the
  busy bit is set. `kernel/dosfns.c` `DosOpenSft` checks for a device before
  the redirector, so the open does not reach the C: IFS.
- FreeDOS 2043 `kernel/config.c`: `Dosmem()` reads DOS= tokens only while
  they are `UMB` or `HIGH`, so `DOS=LOW,UMB` stops at `LOW` and leaves the
  UMBs unlinked; `DOS=UMB,LOW` links them and keeps the kernel low (no HMA
  move). `umb_init()` runs after a DEVICE= line (`LoadDevice`, main.c:337),
  before the tables are allocated (main.c:345), and `KernelAllocPara` forces
  low allocation until UMBs exist; the tables (SFT, CDS, stacks) and the
  device headers therefore stay conventional only because the config has no
  `*HIGH` line and no `DOSDATA=UMB` (config.c:460-477, :1873). The real
  safeguard is `test_config_links_umbs_and_keeps_every_kernel_structure_low`
  in `tests/host/dos/test_dos_box_memory.py`, which fails if the generated
  fork FDCONFIG.SYS contains DEVICEHIGH, FILESHIGH, LASTDRIVEHIGH, STACKSHIGH
  or DOSDATA=UMB. `init_device()`
  does not link a driver whose end address equals its header.
- FreeCOM 0.85a: `shell/loadhigh.c` (LH/LOADHIGH links the UMBs and sets
  the allocation strategy for one EXEC); `shell/init.c` resizes the
  environment with `ENV_USEUMB`, so `/E:1024` moves into a UMB when DOS has
  them.
- The DFV2 bind (f3-56 Amendment 1, `dos_unified.c` `final_bind`) reads
  every device header from NUL in conventional memory, so a DEVICEHIGH driver
  would refuse the bind; the lifecycle checks (`arena`, `read_psp`) required
  a conventional PSP, so `LH SHARE` failed with "Undefined error 87" and
  SHARE did not load (measured before the change below).

**Decision.**
- `CIUKIEMM.SYS` (`src/com/dosemm.asm`, 164 bytes, 6 paragraphs resident)
  is the `EMMXXXX0` device: attribute C000h as EMM386; command 10 ready, 13/14
  done, 8/9/11 write fault, 3 (IOCTL read) read fault (no EMM386 private API
  is offered), others unknown command. Its INIT keeps the device only when
  the INT 67h segment carries `EMMXXXX0` at 0Ah (read from the IVT) and
  INT 67h AH=40h returns 00h. It loads with DEVICE= (low), before
  CIUKIDF.SYS. EMS itself is unchanged (`dosvm_ems.c`, vector F100:0000).
- `FDCONFIG.SYS` of `kernel=unified`: `DOS=UMB,LOW` (kernel text and data
  low, f3-56 Amendment 1 unchanged; no HIGH, DOSDATA or *HIGH lines).
  `FDAUTO.BAT` of `kernel=unified`: `LH C:\SHARE.COM`. The f3-48 reference
  profile (`scripts/dos_conformance.py` CONFIG/AUTO, pristine FreeDOS 1.3)
  is unchanged; `build_reference` now takes the guest's FDAUTO.BAT as it
  already took its FDCONFIG.SYS.
- DFV2 lifecycle (`dos_unified.c`): a PSP, its arena header and its JFT may
  lie in conventional memory or in the VM's UMB arena, which the monitor
  publishes with `dos_unified_upper` (`dosvm_umb_range`: C9000h to the arena
  end, none when UMBs are disabled; D0000h with the D000h frame). Kernel
  text/data, SDA, SFT, CDS, device headers, NLS tables and the root PSP stay
  conventional; video RAM, the EMS frame, ROM and an arena that runs past the
  range are refused.

**Measurement** (host replay as above, `tests/host/dos/run_dos_memory.sh`;
paragraphs in hex; DOSMEM.COM now links the UMBs for its MCB walk as MEM
does and runs the "open handle" test):

| | before | after |
| --- | --- | --- |
| DOS= / UMB link | DOS=LOW, no link | DOS=UMB,LOW: link MCB 9FFFh, arena C900h–DFFFh owned by DOS (XMS 10h: B1h, none left) |
| `SD` (kernel data, buffers, SFT, CDS, drivers) | 0357h | 035Eh (+7: EMMXXXX0 device) |
| `SC` (kernel text) | 0A55h, low | same |
| SHARE | 0235h low | 0235h at C909h (UMB) |
| FreeCOM environment | 0040h at 9FBFh | 0040h at DFBFh (UMB) |
| UMB free after boot | — | 1486h (82 KiB) |
| DOSREADY largest block at boot | 8C1Dh (561 KiB) | 8E95h (570 KiB) |
| Block a program gets at the prompt | ~8C5Ah (574880 B) | ~8ED2h (584992 B, +10112 B); Wolf3D margin 57680 B |
| EMS "get interrupt vector" | LIM 4.0, frame E000h, 512 pages | same |
| EMS "open handle" | 3D00h: error 2 | handle 5, 4400h DX=C0C0h, 4407h AL=FFh, 4402h refused (error 5), close |
| Wolfenstein 3-D 1.4 after 8 s | mode 13h; no EMS (`PML_StartupEMS` fails at the open), XMS 24573 KiB held | mode 13h, EMS 2656 KiB and XMS 21917 KiB held |
| Pristine FreeDOS 1.3 reference | DOS=LOW, SHARE low, no device | same |

DOOM and Quake on the same runner (scratch replay, old and new profile, same
image otherwise): DOOM 1.9 shareware (`DOOM.EXE -config CIUKI.CFG -nosound`,
DOS/4GW) is a DPMI client at 1 s and in mode 13h unchained from 5 s, with 18
page flips and 1183 IFS reads at 10 s under both profiles. Quake 1.06
shareware (`QUAKE.EXE -nosound`) enters the VMM's DPMI host (one client) at
its first sample under both, then stops in the runner, which has no x87, as
before the change; its game loop needs QEMU or hardware.

**Not done (headroom).** `SHELLHIGH=C:\CIUKISH.COM` measured another 30h
paragraphs (768 bytes); left out: CIUKISH.COM is the session wrapper, not a
TSR, and its SHELL= line belongs to the EXIT decision of 2026-10-24. DOS=HIGH (the
42 KiB `SC` block into the HMA) remains refused by the f3-56 bind, and
FreeDOS 2043 puts BUFFERS in the HMA only with the kernel there
(`BUFFERSHIGH` is an alias of BUFFERS); FILESHIGH/LASTDRIVEHIGH or
`DOSDATA=UMB` would move the SFT and CDS into UMBs, which the bind refuses.
Windows 98 sessions typically show about 600 KiB.

**Limits.** A user-edited FDCONFIG.SYS with DEVICEHIGH, FILESHIGH,
LASTDRIVEHIGH or DOSDATA=UMB now places structures the bind reads (device
headers, SFT, CDS) in UMBs; the bind refuses
(`unified_activation_refused`) and the session runs without the unified IFS.
Accepting them would need an f3-56 amendment. The direct-role frame check
(`dos_unified.c`, "direct iregs frame") reads the caller's iregs through
conventional addresses only, so if a direct-role claim is ever added it would
refuse INT 21h calls from high-loaded programs; it is not switched to the
published range now because no direct claim exists to test it. The published
window is C9000h to E0000h (D0000h with the D000h frame); `dos_unified_upper`
refuses any other range.

**Risks.** XMS 10h no longer returns UMBs (DOS owns them, as with
DOS=UMB under HIMEM/EMM386 on Windows 98); a program that wants upper memory
gets it from DOS (5803h, 48h, LH). Wolfenstein 3-D now pages into EMS through
the VMM's frame instead of XMS. A program that opens `EMMXXXX0` and uses the
EMM386 private IOCTL (4402h) gets an error, as with any non-EMM386 manager.
The adapter's process range covers the whole arena; a DOS PSP anywhere in it
is checked as in conventional memory (MCB, PSP signature, JFT).

Host evidence: `tests/host/dos/run_dos_memory.sh` (on the unchanged
profile and code it fails 5 checks: no UMB link, SHARE low, no `EMMXXXX0`
device, prompt block 8C5Ah below the new 8E80h floor, Wolf3D without EMS;
passes after with the figures above), `tests/host/dos/unified_binding_test.c`
H56-07 (UMB processes accepted only once published; video RAM, EMS frame,
overrun, CDS and root PSP in UMBs refused), `tests/host/dos/test_dos_box_memory.py`
(profile, reference unchanged, driver header and answers),
`tests/host/dos/run_unified_file.sh` (H61 boots the new profile, DOS=UMB,LOW
and `LH SHARE`, on the production VM; H57 boots its own `DOS=LOW` CONFIG and
does not exercise it). No host boot yet runs the H56 inheritance probe (a
high-loaded parent EXECing a child, file I/O from a high-loaded program):
H57 has no UMB arena or published range, so that case needs the QEMU/T23
DOS Box or a UMB-capable H56 variant.

### Boot-time check: the image's version is pinned (2026-10-24)

QEMU runs of image 56e58374: every boot ended the background check with
`[dosvm] image verify result=failed step=version error=-11`, so the first
DOS Box after boot re-hashed `UNIFIED.IMG` inside CREATE (2471 ms in QEMU,
27.7 MB) although the image had not changed. Cause: a file's write
generation (f3-51c) is its own change record or, without one, the volume's
change floor; the ledger keeps `VFS_CHANGES` (64) records and raises the
floor when it evicts one. The image is never written in a normal session, so
it has no record, and more than 64 other files changing on C: during the
check (boot log, settings, spooler, Files) moved its version.

- The check pins the version it starts from with `vfs_version_pin`, the
  bounded version watch the session digest cache already uses
  (`dosvm_disk.c`): unrelated changes no longer move it, while a write,
  truncate, rename or unlink of the image itself, or a volume expiry (raw
  write, removal), still advances the pinned sequence and voids the result.
- The pin stays with a finished result (verified or mismatch), so fx-23's
  rule holds under the same churn after the check: a launch reuses the
  finished check for the same file version even when the session digest has
  been evicted. A new check, a failed or cancelled one, game removal's hold
  and release, and a launch's own synchronous publication release it. With
  no free watch the check runs unpinned, as before, and says so in the log;
  `VFS_VERSION_PINS` (24) leaves a watch beside the 16 session digests
  (`dosvm_disk.c` asserts it).

Host evidence: `tests/host/dos/test_image_verify.py` section 8: 80 files
created and rewritten during the check (the change floor rose from 0 to 178
in the run) keep the image verified and stamped; 80 more changes after the
check, then a launch with an empty session digest: `source=background`, no
hash, no disk reads; an mtime change of the image itself during a check
still ends `failed step=version`; no version watch remains after a hold,
a release, a void check or at the end. Fails on 56e58374, passes with the
pin.
