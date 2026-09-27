# DOS monitor, video mapping and DPMI ownership contract

Status: monitor/session foundation and focused CPU qualification, **not completed
desktop DOS virtualization**. A separate experimental QEMU image has executed an
ordinary DOS child under Jemm, including shadow video access, exact PTE restore
and bounded protected framebuffer copying. A fresh packaged HDPMI host in the
same session passes a bounded protected-mode video-memory and port-trap probe,
then unloads before the module and monitor. The 27 September extension also
executes the unchanged packaged doom-vanille `DOOMVAN/PCDMCORE.EXE` DOS/4GW
client with a physical-IRQ host
scheduler, separate HDPMI page-table ownership and exact callback/PTE unwind.
The current production desktop still has restricted
BIOS-text execution and cooperative game previews. This record identifies the
interfaces that a monitored original-binary session must integrate and their
actual limits.

The [27 September artifact audit](vm-integration-audit-2026-09-27.md) records an
initial combined run before subsequent scheduler reentry and guest-IF fixes.
Its PASS is limited to the recorded cases and binaries. Final runtime evidence
for the corrections is pending; observed host ticks alone do not prove correct
virtual IF handling for every client.

## Use the shipped HDPMI source

`scripts/build_hdpmi_host.sh` builds official HX/HDPMI 3.24 at
`f2276db9accfc57facf2588bc016a27130597bb1`, with the existing XMS/status patch,
for both 16-bit and 32-bit IOPL=0 clients. The earlier research checkout at
`build/external/HX`, commit `43182b4a0efd008ae5aab3ccfcc6d644437cd111`, has a
different port-trapping ABI and is **not** the packaged host. Its register-based
AX=6 callback, 16 range slots and IRQ function numbers must not be copied into
the production adapter. No new HDPMI range patch is needed: the shipped allocator
already rejects an empty range, overflow, overlapping ownership and a full table
before changing its I/O permission bitmap.

The official interface is discovered in protected mode through **INT 2Fh,
AX=168Ah, DS:E/SI pointing to `HDPMI` plus NUL**. Success returns AL=0 and
ES:E/DI as the far API address. The first paragraph of `HDPMIAPI.TXT` says INT31;
the implementation and the existing VSBHDA adapter use INT2F. See
[official dispatcher](https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/INT2FAPI.ASM)
and `src/HAPI.ASM` in the pinned VSBHDA checkout.

| API AX | Parameters and behavior |
| --- | --- |
| 6 | DS:E/SI points to `TRAPPROCS`; DX is first port; CX is count. CF clear returns an EAX handle. Eight ranges exist. |
| 7 | EDX is the exact returned handle; releases that range. CF reports failure. |
| 8 | Simulates physical I/O using flags in BX and the documented register/buffer parameters. Do not use it to bypass a virtual VGA device. |
| 9 | Registers a CLI or STI handler at CX:E/DX, selected by BL. This is not virtual IRQ delivery. |
| 10 | Reserved; simulated hardware interrupt delivery is not implemented. |

`TRAPPROCS` contains IN and OUT far pointers: offset32/selector16 twice for
HDPMI32I, offset16/selector16 twice for HDPMI16I. The handlers receive an
exception frame, including instruction length and I/O width/string flags. They
must update saved EIP after emulating the instruction. The older fork's byte
callback/RETF convention is incompatible. The complete flags and frame behavior
are documented in [the official API](https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/HDPMIAPI.TXT).

VSBHDA 2.0 already consumes ranges for SB, OPL, DMA and PIC services. Its
`src/PTRAP.C` deliberately groups ports around the eight-range limit and clears
separate-context mode before installing them. The video adapter must preserve
those handles, reserve capacity and unwind only its own successful registrations.
Increasing the host limit is a separate implementation/qualification decision;
silently stealing a port or treating a failed registration as successful breaks
audio ownership. The source adapter is pinned at
`75fa4bbfea70cbcc0c40d1212f04952ff8abbf16`.

## Jemm/JLOAD bridge

The inspected monitor is Jemm at `e96bb6bb80cbdc3e6585544db79a6b133a5566dd`.
Its `Include/JLM.INC` declares the ABI; `Tools/JLOAD/VMM.ASM` implements it.
Use the symbolic `@VMMCall` names, which encode INT20 followed by service and
device words. These are JLOAD service ordinals, not DOS interrupt functions.

| Service | Ordinal | Contract |
| --- | --- | --- |
| `Get_Cur_VM_Handle` | 1 | Returns the single current VM control block in EBX. |
| `Hook_V86_Int_Chain` | 4 | EAX interrupt, ESI handler; handler has a preceding pointer to previous-handler storage. |
| `Install_IO_Handler` | 31 | DX port, ESI protected handler; CF means failure. Register every port individually. |
| `Remove_IO_Handler` | 34 | DX owned port; preserves Jemm's underlying reserved-port trap policy. |
| `_PageReserve` | 39 | Stack arguments: page selector, count, flags; returns a linear base or -1. |
| `_PageCommit` | 40 | Stack: linear page, count, pager, pager data, flags; returns nonzero on success. |
| `_PageCommitPhys` | 42 | Stack: linear page, count, physical page, flags. Maps a physical region into reserved space. |
| `_LinMapIntoV86` | 20 | Stack: source linear page, VM, destination page, count, flags. Copies current PTEs. |

I/O handlers receive EDX=port, EAX=value, ECX=operation and EBP=client frame.
Operation codes are byte IN/OUT 0/4, word 8/12 and dword 16/20; higher bits
describe string/repeat/address direction. `Client_Reg_Struc` is 76 bytes;
EAX is at 28, EIP at 40, EFLAGS at 48, ESP at 52, SS at 56, and ES/DS/FS/GS
at 60/64/68/72. See [JLM declarations](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Include/JLM.INC).

The multi-port installation functions and protected-mode fault hook are stubs.
The implementation has one VM and one current mapping, not independent desktop
DOS machines. `_LinMapIntoV86` ignores its VM/flags arguments and only copies
from source page numbers greater than 0x100. `_PhysIntoV86` also excludes physical
pages at or below 0x100, so it cannot restore VGA's low physical pages directly.
These limits are visible in [JLOAD's implementation](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/VMM.ASM).

## Required mapping and cleanup transaction

This is the integration contract for the adapter. The isolated experiment below
qualifies the bounded mapping/restore and framebuffer-transfer portions; these
steps do not yet run as a desktop DOS window:

1. Before mutation, reserve all backing pages and a protected mapping of the
   host's physical LFB. Validate the monitor version, page layout and allocation
   results. Preserve the original PTE words for every destination page: an
   minimum text/linear-video tier needs A0000–AFFFF (16 pages) and B8000–BFFFF
   (8 pages). The implemented experiment preserves and restores all 32 PTEs
   spanning A0000–BFFFF. Preserve owned interrupt/trap handles and all saved
   video state.
2. Install logical BIOS/video and per-port handlers, then publish the aperture
   as writable/supervisor PTEs. Guest user-mode accesses must fault into the
   shared VGA instruction model; a user-accessible RAM alias cannot implement
   latches, plane masks or VGA write modes. The physical display remains owned
   by the host. No firmware mode set or current real-mode `vc_fb_begin` CR0
   switch may execute from a V86 guest as the host's renderer.
3. On partial failure, restore the exact PTE snapshot and flush the monitor
   TLB, undo only successfully installed traps/hooks, and free allocations only
   after no low alias points at them. On normal termination use the same path.
4. An exact PTE restore needs an explicit monitor operation. The pinned JLOAD
   internally maps its PTE array at FF800000h; direct use is a version-bound
   implementation dependency, not a portable public service. A high alias made
   with `_PageCommitPhys` can expose original VGA memory, but copying its PTE
   back is insufficient for an **exact** restore because mapped/allocation flags
   may differ. Snapshot and restore all original address/permission/cache bits.
5. Any HDPMI client must have terminated and released its aliases/traps before
   the monitor frees shadow memory. Failure to prove cleanup keeps allocations
   alive and reports failure; it must not return a dangling mapping to a normal
   desktop or reuse another driver's memory.

Linear shadow RAM plus palette/port traps can support a bounded text/mode13h
tier. Mapping RAM alone does not implement planar VGA latches, map masks,
read/write modes or banking. The new freestanding `src/vm/virtual_vga.c` supplies
a separately tested model for VGA text, 16-colour planar graphics, mode 13h and
Mode X, including latches, plane masks, DAC/attribute state and scanout. Unsupported
CGA/VBE/chipset cases fail explicitly. Host sanitizer checks and the OpenWatcom
freestanding target build pass; that is device-model evidence only.

Original planar games still require every relevant guest memory access to reach
the model, through a guarded aperture and instruction handling or an equivalent
accurate path. Directly mapping its planes cannot emulate read/write latches.
Memory trapping, port routing, BIOS behavior and presentation must share the
same owned state. Original-game support must be an explicit capability with
actual original-binary evidence.

## Crossing the protected-mode boundary

HDPMI obtains the low-memory page table through VCPI INT67h/DE01h when initializing
its own page tables. It also owns a separate TSS and I/O bitmap. Therefore a
Jemm mapping/trap does not automatically cover a later protected-mode client.
The initial contract should require a fresh HDPMI instance **after** shadow
mapping, separately register its protected-mode port adapter and reject a stale
resident host rather than assuming its copied PTEs changed. Relevant source:
`PAGEMGR.ASM:initpagetab0`, `HDPMI.ASM:setuphoststack` and
`SWITCH.ASM:vcpi_pmentry` in the packaged official checkout.

The combined QEMU experiment now follows that ordering: Jemm and CVSESS arm the
private mapping, the packaged `HDPMI32I.EXE -r -v` starts fresh, and
`DPMIVGA.EXE -map` exercises actual protected-mode writes and I/O exceptions.
JLM readback confirms the A000/B800 writes reached the private shadow. The probe
records 218 traps (209 IN, 8 OUT, one deliberately rejected string operation),
byte/word/dword behavior, DAC state and register/frame restoration. HDPMI unloads
before session END restores the low-memory PTEs. This validates the bounded
mapping and callback route; it does not provide a common complete VGA device,
virtual IRQ/audio/input stack or an arbitrary protected-mode guest launcher.

Do not assume a port exception can safely invoke JLM through simulated INT2F.
HDPMI's `I31SWT.ASM:_callrmproc` implements DPMI 0300 by reading the real IVT
target and far-calling it; JLOAD's V86 interrupt hook operates in the monitor's
dispatch path. That simulated call may bypass the hook. A DOS-side bootstrap
can discover the JLM device through a real INT2F/1684 before entering HDPMI and
retain its entry address. The implemented official-host adapter does this once
at `-cSSSS:OOOO` binding. While that exact client is owned, DPMI 0300h INT 10h
and 0302h calls whose target exactly equals the current INT 10h vector invoke
`VM_OP_VIDEO_INT10` through that JLM entry. Other simulated interrupts and
arbitrary 0301h/0302h real-mode procedures retain the shipped 3.24 behavior.
This bridge is for infrequent BIOS/mode transitions; protected VGA port and
memory instructions remain entirely in the ring-0 adapter.

Use shared bounded shadow state and a protected-mode video adapter for frequent
port access. Batch presentation at a controlled service point instead of adding
a real-mode transition to every pixel/register operation. A callback must avoid
DOS, nested EXEC, firmware video changes, recursive HDPMI entry and blocking
allocation. Preserve the interrupted frame, private stack and nesting state.
Native-window return, virtual IRQ/audio coexistence and physical-machine
behavior need their own gates before this route can launch arbitrary DOS
programs from the production desktop.

## Host scheduling and client ownership

The Jemm patch adds one versioned `Install_Host_Scheduler` service backed by
physical IRQ0. Its callback owns a private stack, a non-reentrancy gate and one
conventional descriptor. Jemm invokes it before deciding whether IRQ0 is
deliverable to the V86 guest. The required behavior is host progress without
altering the guest's virtual IF or delivering masked guest IRQs. The initial
gate observed progress during its CLI and BIOS waits, but an HDPMI path that
restored guest interrupt delivery at the next physical tick and the V86 IOPL
policy require additional correction and qualification. The adapter snapshots
and restores both pre-client PIC masks exactly. Blocked BIOS keyboard waits
are separately exercised. The scheduler's reentry guard correction has its own
assembled-instruction regression; see the audit record.

Every HDPMI client has distinct client-specific storage: official API-6 handle,
IOPB ownership, callback stack, saved CR4/PIC state and a full 32-entry copy of
its own A0000–BFFFF PTEs. Attach maps one supervisor-only global alias to the
shared video block, installs the session shadow in the client's page table and
then publishes ownership. Detach first removes the exact API-7 handle, unmaps
the alias, restores and verifies all client PTEs, and only then clears client
ownership. Fault and partial-attach paths use the same reverse ordering.

This remains one monitored foreground execution context. Source-port callbacks,
the physical scheduler callback and HDPMI's current protected client are not
separate background VMs and are never counted as such.

## Minimal CiukiOS session ABI proposal

Keep the private protocol separate from Jemm and HDPMI vendor numbers. A
versioned descriptor should carry a size/version, session generation, lifecycle
state, capability bits, mode/width/height/pitch, shared shadow offsets/sizes,
palette generation, key-ring indices, dirty generation and last error. All
addresses must be bound to owned allocations; the DOS caller cannot supply an
unchecked protected pointer.

The initial operations can be QUERY, PREPARE, ARM, SERVICE, REQUEST_CLOSE and
RELEASE. PREPARE is allocation/snapshot only. ARM commits traps/mappings or
rolls back. SERVICE consumes bounded damage/input without DOS or device
reinitialization. REQUEST_CLOSE is cooperative until process isolation supports
safe termination. RELEASE requires child/adapter shutdown and exact restore.
Advertise DPMI, planar video and audio bits only after their separate adapter
handshakes succeed. Missing capabilities must leave the original fullscreen
path usable and produce an English graphical explanation.

## Executed allocator qualification

`scripts/test_hdpmi_io_range.py` extracts the unchanged official `checkrange`,
`is0006`, `is0007` and structures, assembles them with JWasm and executes their
instructions in Unicorn. Both client formats pass 17 cases each, including a
ring-3 data selector with a nonzero base, 32-bit offsets above 64 KiB, callback
pointer copying, preservation of registers/segments/stack, all IOPB bytes,
full-table/overflow/overlap rejection, slot reuse and invalid/double release.
This uses a modeled GDT/TSS and does not invoke the copied callbacks or establish
working DPMI exceptions, Jemm interoperation, guest I/O, audio or hardware support.

```sh
uv run --with unicorn python3 scripts/test_hdpmi_io_range.py
```

Report: `build/full/hdpmi-io-range-2026-09-26/report.json`.
The official allocator source SHA-256 is
`ef283e2576534bed3787a20503cda3e9c4767164bf554fe578cf4ed3a901558e`.
No redundant allocator patch was introduced and the production build chain was
not changed by this qualification.

## CiukiOS device discovery and kernel prerequisites

The device-query adaptation is selected explicitly through
`scripts/build_jemm_monitor.sh --ciukios-device-query`. Omitting adaptation
options builds the pinned upstream sources unchanged. This option applies
`patches/jemm-ciukios-device-query.patch` and includes the separate MIT-licensed
`src/vm/jemm_device_query.inc`; the output preserves the original source archive,
Jemm Artistic notice, JLOAD license, patch, helper and modification notice. Its
manifest records the input, toolchain and output hashes. Each invocation creates
a fresh work directory and publishes `CURRENT` only after both binaries build.

This is a narrowly scoped compatibility adapter, not a DOS handle-table change.
It first performs the normal DOS open and considers a fallback only for error 2
and the exact `EMMXXXX0` or `EMMQXXX0` device names. It walks at most 64 registered
character-device headers from the standard AH=52h list of lists, verifies the
name, attributes and conventional-memory bounds, then invokes the real strategy
and interrupt entries with a DOS read-IOCTL request. It checks completion, error
status and returned length. Its private handle is valid only inside these two
utilities for read IOCTL and close; stale-handle operations and write IOCTL fail
locally. The query buffer contract is the actual tiny-model DS=SS caller. Normal
DOS operations pass through. No synthetic Jemm response is returned.

Two kernel defects blocked this route:

- Jemm requests a 32,928 KiB XMS block through extended function 89h. The kernel
  now implements 88h/89h and rejects full-width EDX requests above 65,535 KiB
  before they can truncate into the legacy allocator. Legacy function 09h and
  its handle representation remain compatible.
- AH=52h previously cleared and rebuilt its 1,792-byte SYSVARS area on every
  call. That removed a genuinely registered Jemm device from the NUL chain.
  SYSVARS is now initialized once, and subsequent calls refresh the first-MCB
  anchor without replacing live device, CDS or SFT state.

`scripts/test_xms_extended.py` executes the complete compiled XMS entry point
for 13 cases, including extended-width rejection, zero/one/32,928 KiB allocation,
legacy equivalence, all 16 handles, free/reuse and A20 repair. Only port 92h is
modeled. `scripts/test_dos_sysvars_lifetime.py` executes the compiled AH=52h
routine and memory initializer, then checks that a registered device and live
CDS/SFT data survive repeated queries. Both pass against the 43,217-byte kernel
with SHA-256
`2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af`.
The older compiled kernel fails both corresponding regression checks: extended
XMS query is missing, and repeated AH=52h destroys live SYSVARS bytes. These are
CPU instruction tests with modeled external state, not substitutes for a real
monitor session.

Local reports are in `build/full/vm-session-2026-09-26/`:
`xms-extended-report.json`, `xms-extended-old-regression.json`,
`sysvars-lifetime-report.json` and `sysvars-lifetime-old-regression.json`.
The adapter's assembled-instruction fixture also passes 15 cases in
`build/full/jemm-device-query-2026-09-26/report.json`, including failure bounds,
normal DOS pass-through, real request entry calls and preservation of registers.
That fixture models DOS and the character device; the following QEMU session is
the separate evidence for actual Jemm integration.

## Executed monitor session and current limits

The [archived combined monitor report](validation/2026-09-26-vm/monitor-final.json)
and [session record](vm-session-foundation-2026-09-26.md) identify the exact
qualified binaries and procedures. The QEMU Pentium III / 128 MiB gate uses the
kernel hash above. It loads Jemm,
observes PE=1 and VCPI availability, runs ordinary COM/MZ and DOS file operations,
loads the CVSESS JLM, executes an ordinary DOS child using BIOS video, ports,
direct video memory and file output, and performs the fresh HDPMI protected-mode
probe described above. It then unloads the protected host, JLM and Jemm. The final
probe reports no V86 monitor and COM execution still works. The gate verifies
the exact before/after contents of all 32 VGA PTEs, private A000/B800 readback,
unchanged physical B800 contents during the child, and a 4,096-byte copy into
the firmware-reported physical VBE framebuffer at `FD000000h` in an 800×600×24
mode. Invalid transfer bounds, unbind and unload are checked before native UI
return. The earlier local `sysvars-persistent/report.json` and
`final-monitor/report.json` retain historical hashes and narrower scopes; they
are not the current combined DPMI qualification. The archived report supplies
the authoritative module and adapted Jemm/JLOAD identities rather than a
duplicated table that could silently describe an older build.

The separate `windows-regression/result.json` records the existing focused
Windows 3.1 gate on the new kernel: two launches, application/resize checks,
zero changed pixels in the repaint comparison and non-silent measured WAV/MIDI
captures. This checks that the generic kernel changes preserve that bounded
workflow; Windows is not being run inside the new session module.

The separate `doom-regression-installed/results.json` and `qualification.json`
use a private baseline image with the new kernel and no Jemm. The unchanged
installed-HDD test enters DOS through F4, matches the original Doom WAD's menu
sprites, selects episode/skill, verifies gameplay and movement, quits, runs COM
and returns to the working desktop and console. Its isolated game PCM interval
has RMS 992.76, peak 16,708 and 18,589 distinct sample values. It also checks
38,507 kernel code bytes without unexpected changes. This is fullscreen Doom
compatibility evidence, not proof that the monitor runs Doom in a window.

The newer `scripts/qemu_test_dpmi_lifetime.py` gate stages a different workload:
the packaged doom-vanille DOS engine `APPS/DOOMVAN/PCDMCORE.EXE`, byte-for-byte
(SHA-256
`efbe64359fb1dfe569cde2428f41ef15a40b8975b8eb36a1cfc5a2731b894980`)
under the real Jemm and official HDPMI 3.24 integration. It is not the proprietary
original Doom executable used by the fullscreen regression above, and it is
not recompiled into the cooperative CiukiOS window port. Before this engine, two normal
DOS/4GW clients execute actual INT 31h allocation/free, scalar IN/OUT, VGA
loads/stores, CLI and a blocked BIOS wait; a third client deliberately executes
UD2 to qualify fatal unwind. The one-tic doom-vanille timedemo then performs more than
the required 10,000 protected instructions, 50 port reads and 500 port writes.
The final descriptor records five matching install/remove and entry/exit pairs,
one fault exit, nonzero memory accounting and no retained handle. The harness
compares both active CR3 shadows and, after END, the complete original Jemm PTE
array and all 128 KiB of physical A0000–BFFFF bytes.
These observations do not close the subsequent reentry and guest-IF findings.

The production source image remained unchanged (SHA-256
`08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`).
This lifetime gate does not itself establish desktop presentation (covered
separately by the [video record](vm-video-session-2026-09-27.md)), Doom or
Wolf3D in a native window, arbitrary chipset/VBE routing, virtual audio or keyboard
devices, concurrent DOS sessions, or operation on the physical T23/E500. Those
capabilities require their own implementation and original-binary evidence.
The 43,217-byte experimental kernel has 47 bytes below the unchanged 43,264-byte
guard; monitor/device work belongs to the external modules. Reproduction and
the remaining work are tracked in [the session foundation record](vm-session-foundation-2026-09-26.md).
