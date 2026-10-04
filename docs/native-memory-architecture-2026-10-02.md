# Native memory architecture (2 October 2026)

## Decision and sources

CiukiOS needs a separate native memory domain above the DOS compatibility
arena. Increasing XMS capacity alone does not give a native app a 32-bit
address space. The current full profile keeps CiukiDOS and JemmEx for DOS
compatibility while a native protected-mode host is introduced and the desktop
apps are migrated. Existing DOS and boot interfaces remain compatibility
interfaces, not the allocator for new native app code and data.

- [Microsoft Q125691, Windows 95 virtual address-space layout](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/125/691.HTM): the 32-bit VMM uses paging; the low compatibility arena, process-private Win32 arena, shared arena, and ring-0 arena have different owners. A DOS VM occupies the low part of the compatibility arena. CiukiOS need not copy Windows' exact addresses, but must preserve this separation of ownership.
- [Microsoft's Windows 98 VMM description](https://learn.microsoft.com/en-us/archive/msdn-magazine/2000/july/under-the-hood-happy-10th-anniversary-windows): native Windows code uses protected mode and page-based management; DOS prompts are V86 machines. A V86 session is therefore compatible with the target architecture, but its conventional-memory number does not describe native app capacity.
- [DPMI 1.0 specification](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf): a protected-mode DOS client uses a host and INT 31h services within a DOS VM. CiukiOS's existing patched HDPMI is useful compatibility infrastructure, but it is not itself a native desktop process manager.
- [ACPI system address-map interface](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html): firmware's E820 map distinguishes usable RAM from reserved, ACPI and NVS regions. A future physical-page allocator must use this map and reserve its own image, stacks, page tables, framebuffer and device ranges before allocating pages.
- [Pinned JemmEx manual](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Readme.txt) and [JLOAD module interface](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/JLOAD.TXT): JemmEx owns the XMS/EMS/VCPI memory pool; a JLOAD module executes in Jemm's protected address space and can use its page services. `src/vm/session_vmm.inc` already reserves, commits and frees pages through those services.
- [Intel 80386 Programmer's Reference Manual](https://pdos.csail.mit.edu/6.828/2018/readings/i386.pdf), chapters 6, 9 and 11: user execution requires privilege-checked descriptors, a valid TSS ring-0 stack for transitions, page protection and exception handling. Entering protected mode or mapping memory alone does not create a user process.

For the initial non-PAE 32-bit target, `scripts/read_memory_map.py` now derives
*firmware page candidates* only from complete, nonoverlapping type-1 ranges
with the valid extended-attribute bit set, above 1 MiB and below 4 GiB,
rounded inward to 4 KiB boundaries. Unreported, reserved, ACPI reclaim and
NVS ranges remain unavailable. This follows ACPI's type definitions and its
warning that the map omits some standard PC address ranges. A candidate is
**not** a free page: no native allocation can occur until one owner excludes
all currently occupied pages, especially JemmEx/XMS-owned frames. The two
physical captures yield 65,264 candidate pages on the E500 and 130,656 on
the T23; their spans match the raw firmware-usable counts exactly. The parser
rejects incomplete and overlapping maps before deriving candidates.

## Free API integration assessment, 3 October 2026

The owner's proposed [Free API](https://github.com/openeggbert/free-api) is
reusable under its [MIT license](https://github.com/openeggbert/free-api/blob/develop/LICENSE),
with the copyright and license notice retained. Its
[scope policy](https://github.com/openeggbert/free-api/blob/develop/docs/scope.md)
defines source-level Win32 compatibility for Free Eggbert and Planet Blupi,
not a general Windows binary runtime. The
[WinMain bridge](https://github.com/openeggbert/free-api/blob/develop/src/winmain_bridge.cpp)
calls a linked function pointer; it does not load an existing PE executable.
The [build definition](https://github.com/openeggbert/free-api/blob/develop/CMakeLists.txt)
requires C++20, SDL3, SDL3_image and SDL3_mixer, and builds a static library.
DirectDraw, DirectSound and DirectPlay belong to the separate Free Direct
project rather than this library.

CiukiOS currently detects PE/NE in `src/apps/dosvm.c` and shows its Windows
support message without executing the file. The native CN32 path is a
different executable format. The
[Microsoft PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
defines image sections, imports and base relocations that a PE32 loader must
handle. Binary Win32 support additionally needs Windows calling conventions,
DLL import binding and process services, and implementations of each API the
chosen application uses. Free API alone supplies neither that loader nor a
CiukiOS implementation of its SDL/host dependencies.

Decision: treat Free API as a reference and possible source of selected API
implementations. For recompiling supported game source, first provide a
CiukiOS graphics/input/audio and C++ runtime backend. For existing Win32 EXEs,
first implement a bounded PE32 loader and the imports of one selected simple
application; adapt compatible API code behind that ABI. Do not import the
library wholesale or advertise arbitrary Win32 compatibility. This assessment
used upstream documentation and code only; no integration, build or runtime
test was performed.

## Page ownership during the migration

JemmEx currently owns extended/XMS RAM in the normal full profile.
CVSESSION calls Jemm's `_PageReserve`, `_PageCommit` and `_PageFree` for its VM
memory. HDPMI runs protected DOS clients within a DOS VM; the 16-bit CAPP
loader uses DOS blocks below 1 MiB. None of these is yet a native desktop
process manager. The first native manager must obtain pages through one
trusted Jemm service and account for process mappings and cleanup under that
same owner. It must not turn the E820 candidate ranges into a competing
allocator while JemmEx is active. The `HARD` profile skips JemmEx, so its
native host must have an explicit page-ownership transition before it can
run. This decision keeps existing DOS sessions working during migration; it
does not make the present CAPP modules 32-bit processes.

## Current implementation and measurements

`src/apps/app.h` and `src/com/shell_apps.inc` define the desktop app ABI: each
APP is a flat 16-bit image, loaded by SHELL.COM into a block below 1 MiB, with
CS=DS=SS and shell callbacks. `scripts/build_apps.sh` rejects a module above
64 KiB. `hi_alloc` first requests an XMS UMB, which is still within the first
MiB, then falls back to DOS conventional memory. Paint uses XMS separately for
its undo pool and copies through a conventional-memory row buffer. These paths
do not provide a native 32-bit process heap.

Task Manager previously used the 16-bit XMS `AH=08h` query and could display
at most about 64 MiB even while JemmEx offered much more. Its register bridge
now preserves EAX's high word, and Task Manager uses XMS 3.0 `AH=88h` for
32-bit largest/free counts, as specified by the
[Microsoft/Lotus/Intel/AST XMS 3.0 document](https://ps-2.kev009.com/basil.holloway/ALL%20PDF/Microsoft_XMS_3%5B1%5D.0_Specification.pdf).
Task Manager keeps separate conventional and XMS readings for diagnosis.
The desktop top bar now reads the complete pre-Jemm `MEMMAP.BIN` E820 capture
once at open and shows the BIOS-usable capacity below 4 GiB as `RAM` in MiB.
It rejects an incomplete map, an unknown format and truncated entries.
There is no RAM usage gauge: XMS `AH=88h` reports a pool managed by Jemm,
not all free physical pages, and a DOS block measures still less. This makes
the user-facing number a system capacity reading without inventing a
whole-system free-memory measurement. The E820 and XMS specifications above
are the basis for that distinction; `src/apps/desktop.c` implements it.

An earlier full-profile build measured 487 KiB largest DOS block in the system
prompt and 544 KiB in a forked DOS VM. The current allocator/TSR layout is
being revalidated below. At 128/256 MiB QEMU guest RAM, JemmEx
reports 125/253 MiB free XMS and passes allocate/free probes. These figures
measure DOS allocation and XMS, not native virtual memory. The earlier
approximate report of 512 MiB for the Compaq E500 in
`docs/validation/2026-10-01-e500-boot/README.md` is superseded for allocation
purposes by the physical E820 capture below. The two supported laptops must
retain separate measured limits.

The new pre-Jemm E820 capture in QEMU reports 7 descriptors at 128 MiB,
including 126.9 MiB usable above 1 MiB; at 256 MiB it reports 254.9 MiB
usable above 1 MiB. `INT 12h` returns 639 KiB and the EBDA starts at
`9FC0h:0000`. The XMS allocator's lower 125/253 MiB free figures are
consistent with these firmware totals after its own reservations. The
capacity gate now extracts `MEMMAP.BIN` from both booted images, validates
its format and checks that the XMS free count does not exceed firmware-usable
RAM.

The 2026-10-02 fork diagnostic measured 485 KiB with the original `VMFORK`
and 507 KiB after resident compaction, with an ISA NIC present and no packet
driver loaded. Its MCB chain still had a 37.8 KiB free interval below the
relocated residents and a 507 KiB interval above them. CiukiDOS's earlier
`INT 21h/AH=48h` implementation split every first-fit block from its high
edge (`src/boot/floppy_stage1.asm`, `int21_mem_find_free_gap` and
`int21_mem_table_alloc_from_free`). The [upstream FreeDOS kernel memory
manager](https://github.com/FDOS/kernel/blob/master/kernel/memmgr.c) instead
splits first/best-fit blocks from the beginning and reserves high-end carving
for last fit. The [MS-DOS Programmer's Reference](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/)
defines the three strategy choices. CiukiDOS now carves first/best-fit blocks
from the low edge and last-fit blocks from the high edge. The best/last-fit
scanner also retains a previously found hole when the final allocated block
reaches the arena limit. The runtime `MEMSTRAT.COM` probe checks all three
placements and their rebuilt MCB owners; the capacity gate keeps its original
540 KiB minimum.

The VM clock probe also exposed a nested-launcher case: `VMCTEST.COM` launches
`VMFORK.COM`, so the immediate parent PSP lies above AUXSTACK and LFN. The
initial compactor shrank that parent's PSP and could not allocate a lower
relocation destination. `VMFORK` now shrinks the root ancestor PSP in its
private VM copy; this lies below the resident hooks. A scratch diagnostic
identified failure at the first AUXSTACK relocation before the change, and
the same two-VM clock gate passed afterward with 181/182 guest ticks in
10 seconds and the console still live. Evidence is in
`build/tests/vmfork-compact-debug/clock-stage` and `clock-fixed`.

JemmEx already moves its own resident body into an upper-memory block in the
full profile. Its command-line `LOAD` path does not link those UMBs into DOS,
as the [pinned Jemm manual](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Readme.txt)
states. AUXSTACK and LFN load before Jemm, and CiukiDOS does not yet provide
the UMB link/`DEVICEHIGH` services required by the
[Microsoft UMB loading rules](https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/71/865.HTM).
Moving those residents upward requires an explicit UMB allocator and driver
relocation design with parent/fork vector tests; changing startup order alone
would change which vectors the VM manager snapshots. The present 543 KiB
forked block is the verified conventional-memory result, not the whole RAM
available for a future native process.

## Trusted page service and 32-bit execution status

`src/vm/session_native_pages.inc` now provides Jemm-owned, zero-filled page
blocks with owner IDs and generation-tagged handles. Allocation reserves
linear space with Jemm `_PageReserve` and commits it with `_PageCommit`; release
uses `_PageFree`. A failed release retains its ownership record and prevents
the JLOAD module from unloading. A failed commit attempts rollback; if Jemm
also refuses that rollback, the reservation stays recorded under its owner
for a later cleanup attempt. `native_page_release_owner` provides the cleanup
operation needed at a future process exit. Its current 64 records cap the
number of simultaneous blocks, not available extended RAM.

The `NATPAGE.COM` guest probe invokes this trusted service through CVSESSION,
without receiving a protected-mode pointer in DOS. Each of eight repetitions
allocates two separate 257-page blocks (1,052,672 bytes each), confirms their
linear addresses exceed 1 MiB, checks zeroing and independent contents over
each full block, rejects a foreign owner and a stale handle, and checks zeroed
reuse and owner-wide reclamation. This is a page-ownership test, not a native
process test: the blocks are currently supervisor mappings in Jemm's shared
address space.

`src/native/README.md` defines a provisional `CN32` image and `INT 80h`
contract, and `scripts/build_native_image.py` builds a deterministic sample
with code, data and stack requirements. The original analysis below preceded
the bounded loader and records why JLOAD alone was insufficient. The pinned
JLOAD implementation only switches a JLM into Jemm's ring-0
context via VCPI. Its `Set_PM_Int_Vector` modifies selector and offset in the
shared IDT, but does not install a dedicated, validated syscall gate; its
protected-mode fault hooks are stubs. CVSESSION originally had no per-process
CR3, ring-3 entry/return, TSS stack ownership, syscall dispatcher or protected
fault teardown. The bounded entry implementation now supplies these for one
synchronous invocation; a scheduled native process manager remains pending.
HDPMI's existing 32-bit DOS clients remain DPMI clients inside a DOS VM.

### Original JLOAD boundary and remaining process-manager work

This is a runtime architecture blocker, not just a missing image parser. The
pinned [JLOAD source at `e96bb6b`](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/VMM.ASM)
implements `Set_PM_Int_Vector` by changing only an IDT entry's selector and
offset; it does not create a dedicated, validated syscall gate. Its
`Hook_PM_Fault` and `Unhook_PM_Fault` functions are empty, with an explicit
note that Jemm has no
protected-mode application support. The pinned
[Jemm monitor](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/JEMM32.ASM)
routes the shared IDT into `V86_Monitor`: its frame classification tests the
fixed V86 monitor stack location and treats other entries as monitor reentry.
The GDT currently has flat ring-0 code and data descriptors, not user code and
data descriptors. Jemm's [initialization source](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/src/INIT.ASM)
actually creates generic present DPL3 interrupt gates (`0xEE`) for all 256
vectors and gives the TSS a ring-0 stack for V86 transitions. Those existing
pieces do not recognize, dispatch or safely recover a protected CPL3 process;
in particular, an arbitrary generic DPL3 gate must not be exposed as a CN32
service boundary. The [JLOAD manual](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/JLOAD.TXT)
also states that JLMs always run at ring 0. Although JLOAD's `_PageCommit`
accepts `PC_USER`, the current `native_page_alloc` commits supervisor mappings
into the shared Jemm address space, not process-private user mappings.

The existing HDPMI path is a working 32-bit **DOS compatibility** path, but
it cannot serve as proof of native CN32 execution. `DPMIRUN.COM` is a DOS COM
launcher which starts children through DOS `INT 21h/4Bh` within a forked DOS
VM; it does not load CN32, implement its `INT 80h` ABI, or own desktop native
processes. The [DPMI 1.0 specification](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf),
chapter 2, defines the DOS environment and its protected clients collectively
as a DPMI VM. It states that clients in one 32-bit VM share a page directory
and that DPMI defines no multitasking among those clients. Separate DOS VMs
can isolate DOS programs, but do not provide the requested native desktop
process model.

For actual CN32 support, first extend or replace the monitor with a protected
CPL3 interrupt and exception path, user GDT descriptors, a TSS ring-0 stack
owned and safe for the process transition, a dedicated syscall gate, and a
process-specific CR3 that retains the
supervisor mappings. The
[Intel 80386 protection rules](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s06_04.htm)
require both PDE and PTE user permissions for CPL3 access; its
[interrupt rules](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s09_06.htm)
require a valid privilege transition and exception frame. Only after these
paths work can the loader validate and map CN32 code, data and stack, enter
ring 3, dispatch `INT 80h`, and reclaim every mapping on normal exit or fault.
The required end-to-end gate is two isolated live native processes, access
above 1 MiB, and complete cleanup on both paths while DOS VMs still run.
The bounded CN32 sample now runs at CPL3 with a private CR3 and returns to
DOS. The two-live-process end-to-end gate remains unmet. A strict
256-instruction budget and synchronous DOS transport prevent this prototype
from serving as the native desktop process manager.

In a disposable 128 MiB QEMU image,
`build/tests/native-guards-20261002/report.json` passed the sample plus eight
actual CPL3 probes. User writes to supervisor and read-only code pages caused
page faults, as did an unmapped stack guard. Port output and a non-`80h`
software interrupt caused general-protection faults. A loop reached the exact
256-instruction budget. A user writable-data/unknown-syscall probe and a
nonzero exit status completed; each case was followed by another sample and
a DOS prompt. Eight NATPAGE owner/zero/release cycles still passed after the
faults. The test verified its source image SHA-256 was unchanged. A separate
256 MiB run in `build/tests/native-isolation-distinct-20261002` passed a write
in one invocation followed by zeroed data at the same virtual address in a
fresh invocation. Distinct successful exit statuses 3 and 4 prove the intended
writer and reader images loaded. These are sequential isolation and cleanup tests, not concurrent
native process scheduling or dynamic memory allocation inside CPL3.

### Reviewable implementation path for the pinned monitor

The pinned Jemm source can be extended; replacing its version pin is not a
prerequisite. This is one integrated runtime change, spanning at least the
monitor, JLOAD/CVSESSION boundary, native loader and guest launcher. A change
to only `Hook_PM_Fault` or the image parser cannot safely launch a process.

1. Add a separate, selected CiukiOS patch to `scripts/build_jemm_monitor.sh`
   after the existing scheduler patch. In Jemm `src/INIT.ASM`, reserve explicit
   user code/data selectors and a dedicated `INT 80h` gate. Audit the existing
   256 generic `0xEE` gates before allowing CPL3: a native client must not be
   able to invoke unrelated monitor vectors. Preserve the exact legacy V86
   and VCPI behavior when no native process is active. Keep the current Jemm
   revision and provenance manifest.
2. In Jemm `src/JEMM32.ASM`, discriminate protected CPL3 frames by saved
   privilege and VM flag before the current fixed-stack V86 classification.
   Save the entire user frame, route `INT 80h` to one validated native syscall
   dispatcher, route CPL3 exceptions to process teardown, and return to the
   interrupted V86/system context. Keep IRQ and V86 frames on their existing
   path. JLOAD `Tools/JLOAD/VMM.ASM` needs an owned registration/unregistration
   ABI for those callbacks; its current protected fault hooks are stubs.
3. In CVSESSION, obtain code/data/stack frames only from Jemm's owned page
   service and build a separate CR3 for each process. Map the monitor, IDT,
   GDT, TSS and callback code/data at their existing linear addresses with
   supervisor permissions. Make the DOS low-memory arena inaccessible to
   CPL3. Map only the process's code, data and stack as user pages, with a
   guard page. Keep page tables themselves supervisor-only. Switch to the
   Jemm owner CR3 before calling its page APIs or resuming its V86 scheduler;
   track every frame, table and stack for rollback and exit/fault cleanup.
4. Add a DOS-side file transport and a trusted native loader. The transport
   may read a CN32 file through DOS while the system VM is active, but the
   running image must use its own CPL3 context and `INT 80h`, not DPMI or DOS
   execution. Validate header, lengths, CRC, entry and mapped extent before
   creating mappings; enter with the documented register state. The launcher
   returns to its DOS caller only after the native process has exited and all
   mappings have been released. Add concurrent process scheduling before
   claiming two independent native apps.
5. First run a disposable QEMU profile with Jemm/VMSTART and a minimal CN32
   sample: assert CPL3, private CR3, `INT 80h` report/exit and a clean DOS
   prompt afterward. Then use `scripts/qemu_test_native_process.py` with a
   real guest fixture for two live processes, wrong-owner access faults,
   zeroed page reuse and exact reclamation on normal exit and fault. Re-run
   the DOS/V86, protected DOS/4GW, audio/network, full HDD and full CD gates
   before calling this production-ready. A static assembly or format check
   cannot establish privilege transition or fault recovery.

## Native memory contract

1. The boot path records the firmware memory map before Jemm or a protected
   host changes BIOS services. The shell runs `MEMMAP.COM` after selecting the
   boot session and before loading long names or JemmEx, including in `HARD`
   and `SAFE` sessions;
   it writes `SYSTEM/MEMMAP.BIN` with a 16-byte `CMAP` v1 header and up to 64
   raw 24-byte E820 descriptors. The capture records `INT 12h` conventional
   memory and the BIOS Data Area EBDA segment, preserves unknown/reserved
   types, and marks unsupported or truncated results. The capture makes no
   claim on physical memory. `scripts/read_memory_map.py` validates and
   decodes the file. A future allocator must reject incomplete maps and
   account for overlapping ranges before using them.
2. One system owner claims usable physical pages above 1 MiB after subtracting
   boot images, page tables, stacks, I/O and firmware reservations. It exposes
   allocation, release, accounting and failure semantics with no implicit
   overlap with Jemm's XMS-owned pages.
3. Native apps get 32-bit code/data/stack, a process-private address space and
   a size-checked service boundary to the desktop. The loader validates image
   sizes and cleans up all mappings on exit or fault. The current 16-bit APP ABI
   remains only for unmigrated modules while native apps are introduced.
4. DOS V86 sessions retain their 640 KiB conventional map plus XMS/DPMI
   interfaces. Their reported conventional-memory size is a compatibility
   metric, not a total-RAM metric. The system owns and accounts for any pages
   backing a VM separately from native process pages.
5. Page replacement or a swap file follows only after page-fault handling,
   ownership and backing-store integrity are tested. It is not required to
   remove the 1 MiB ceiling from native apps.

## Implementation sequence

1. Extend the existing Jemm/JLOAD integration with a trusted page-allocation
   service, using Jemm's page APIs and explicit ownership records. Prove
   reserve, zeroed commit, release, failed allocation and cleanup without
   changing the 16-bit desktop loader or DOS VM behavior. The firmware map is
   used to validate the hardware profile, never as a second free list.
2. Add a protected 32-bit CiukiOS process host with a separate user address
   space, stack and service call boundary. A DOS/4GW program under HDPMI can
   test the toolchain, but does not satisfy this milestone: HDPMI is a DOS
   client host inside a VM, not the desktop's native process owner.
3. Define a versioned 32-bit app format and migrate one small desktop module
   end to end. Demonstrate memory above 1 MiB, isolation from a second app,
   cleanup on normal exit and fault, and unchanged DOS VM operation.
4. Migrate the remaining desktop modules and expose separate native and DOS
   memory accounting in Task Manager. Only then can the current CAPP/UMB
   dependency be removed from the desktop. Adapt `HARD` boot to start a
   protected host before claiming native app support on the two laptops.

## Validation gates

- Capture and compare E820, low-memory boundary and XMS totals in 128 and
  256 MiB QEMU profiles, then on the IBM T23 and Compaq E500. Reject overlap
  or arithmetic overflow before handing pages to a native allocator.
- For native allocations, verify page-aligned accounting, zeroing, contents
  across multiple pages, failure without corruption, per-process isolation,
  reclamation on normal exit and fault, and repeated app launches.
- Run the existing DOS memory, XMS capacity, LFN/AUXSTACK, Doom/multiple-VM,
  full HDD and full CD gates after each integration step. Verify the sanitized
  Windows portable archive after each canonical full build.

The future `scripts/qemu_test_native_process.py` uses a guest conformance
fixture named `NPROCCHK` and waits for these serial records, each on its own
line: `[NATIVE-PROC] ABOVE1M PASS addr=XXXXXXXX bytes=N checksum=XXXXXXXX`,
`[NATIVE-PROC] ISOLATION PASS processes=2 private=1`,
`[NATIVE-PROC] RECLAIM_EXIT PASS allocated=N freed=N`,
`[NATIVE-PROC] ZERO_REUSE PASS pages=N nonzero=0`,
`[NATIVE-PROC] RECLAIM_FAULT PASS allocated=N freed=N`, and
`[NATIVE-PROC] DONE PASS`. The harness independently checks the reported
address exceeds 1 MiB, the byte span covers at least one page, checksums are
nonzero, and allocated page counts equal freed counts. These markers describe
the guest-side assertions needed from the future allocator/process fixture;
until that fixture and native loader exist, the harness is intentionally not
a passing runtime gate. The ownership boundaries and rationale follow the
ACPI system address-map and pinned Jemm/JLOAD sources listed above.

## Current validation

- The latest capped full HDD build and capped full CD build passed on 2 October.
  The refreshed Windows portable ZIP has SHA-256
  `34e5f3ca461f5f1ef89722ea7713f7b047d2476a032b8b0373d584800aad6dc4`;
  archive integrity, image manifest hash, `VM/JEMMEX.EXE` and
  `VM/NATPAGE.COM`, and exclusion of all eight configured private game paths
  were checked. `SYSTEM/MEMMAP.BIN` is generated during boot and is absent
  from the unbooted release image by design. The Linux full HDD smoke
  passed with SB16, AdLib and PCI NE2000 attached; the 128/256 MiB full CD
  boot smokes passed with SB16 and AdLib attached. The full network FTP gate in
  `build/full/qemu-network-ftp.report.txt` passed with SB16, AdLib and
  NE2000 attached: packet driver, resident bridge, outbound ICMP, FTP list,
  download, upload and committed image readback. Its launcher was updated to
  enter the system shell from the current desktop with F4. The Windows
  launcher was not run on Windows.
- The latest 128/256 MiB capacity and memory-strategy gate is in
  `build/tests/final-memory-capacity-20261002`: 487 KiB system DOS block,
  543 KiB forked DOS block, 125/253 MiB free XMS and 126.9/254.9 MiB usable
  extended firmware RAM. First, best and last fit passed MCB placement and
  ownership checks. `build/tests/final-dos-memory-20261002` passed the MCB,
  PSP, fork isolation and `COMMAND /C` and `/K` regressions. The corrected
  nested-launcher compaction passed the two-VM clock gate in
  `build/tests/final-vmm-clock-20261002`, with 181/182 VM ticks in ten seconds.
- `build/tests/final-dos-audio-slice2-20261002` passed all four runtime audio layers:
  real-mode SB16 DMA/IRQ, protected DOS/4GW SB16 DMA/IRQ, external DOS/4GW
  AdLib/OPL2 music and mixed Sound Blaster/AdLib DoomVan gameplay through
  AC'97. A one-tick VMM quantum left a second COMMAND.COM window blocked in
  BIOS input after its prompt; restoring two ticks fixed a short A/B probe.
  The final canonical full M4 gate in
  `build/tests/final-m4-slice2-canonical-20261002` passed DoomVan player
  input, PCM, mouse, two DOS windows, VM2 command input and independent close.
  The final VMM `clock,multi` gate passed too. The isolated diagnosis and the
  remaining uncertainty about the exact SeaBIOS interruption point are in
  `docs/validation/2026-10-02-dos-audio-scheduling.md`.
- A 256 MiB full-profile QEMU desktop capture at
  `build/tests/topbar-apps/pci-vm/topbar.png` shows `RAM 255M` from the E820
  usable-capacity sum. The pre-change DOS-block reading is absent from the
  top bar; Task Manager retains its separate conventional and XMS diagnostics.
- The 2026-10-02 capped full build and QEMU memory-capacity gate passed at
  128 and 256 MiB after the allocator correction: 487 KiB largest block in
  the system DOS prompt, **543 KiB** in a forked DOS VM, 125/253 MiB free
  XMS, and 126.9/254.9 MiB firmware-usable RAM above 1 MiB. The integrated
  strategy probe passed first/best/last fit and MCB ownership in both real
  guests, and the nested EXEC memory probe passed. The separate DOS memory
  regression passed MCB/PSP boundaries and allocation isolation in the system
  and forked VMs, plus `COMMAND /C` exit status and `/K` return. A best-fit
  diagnostic had failed before the saved-candidate fix, so this gate covers
  an observed regression. Evidence is in
  `build/tests/memory-capacity-integrated-20261002` and
  `build/tests/dos-memory-frontfit-20261002`.

- With the Jemm-owned page service, the canonical full HDD build passed; the
  `NATPAGE.COM` gate passed at 128 and 256 MiB. The guest checks two independent
  257-page blocks per repetition, wrong-owner and stale-handle rejection,
  zeroed reuse, and owner-wide release, eight repetitions in each profile.
  The XMS/E820 capacity gate still passed at both sizes, and the M4 two-VM
  desktop integration gate passed. The full CD was rebuilt and reached the
  desktop in 128 and 256 MiB smoke tests. The refreshed Windows portable ZIP
  passed `ZipFile.testzip()`, manifest/image SHA-256 matching, presence of
  JemmEx/NATPAGE/MEMMAP and absence of every configured private guest path.
  Archive SHA-256 is
  `ab44edd216b84f7706a22b24a26f5b44d3f3c30adb54d4289e878dcce871914c`.
  The Windows launcher has not been run on Windows. These gates cover the
  page service and existing DOS compatibility; no `CN32` process ran.
- The standalone `CN32` sample builds deterministically to 79 bytes (39 code,
  4 data, 4,096 requested stack); repeated builds match byte for byte, and
  its header length and CRC-32 (`22559c50`) validate. This is a format/tooling
  check only, with no runtime execution.
- The canonical HDD capacity gate passed at 128 and 256 MiB after the E820
  capture and Task Manager changes: 126.9/254.9 MiB usable pre-Jemm,
  125/253 MiB free XMS, 487 KiB system DOS block and 544 KiB forked DOS block.
- A historical 256 MiB Task Manager screenshot in `build/tests/native-memory-final-ui3`
  shows `Extended (XMS) free: 259,104 KB`, with the then-current top bar labelled `DOS`
  for its 379 KiB conventional free block. The M4 Task Manager focus and VM
  termination gate passed in `build/tests/native-memory-m4-tasks-final`.
- The rebuilt full CD passed its 128 and 256 MiB boot smokes. The refreshed
  Windows portable ZIP passed integrity, manifest image SHA-256, JemmEx and
  MEMMAP presence, and absence of all eight configured private guest paths;
  archive SHA-256 is `ceaf8b65beb8415df278ae312d236523dd329082a7a32121968523cfb80bb1ad`.
  Its Windows launcher has not been run on Windows.
- After moving the E820 capture to the shell's common startup path, the
  canonical HDD capacity gate passed again at 128 and 256 MiB, and the full
  CD passed both boot smokes. A disposable QEMU boot of the physical `HARD`
  profile at 512 MiB reached the desktop and wrote a complete, nonoverlapping
  seven-entry E820 map: 510.9 MiB usable above 1 MiB, 639 KiB conventional.
  The tested physical prefix is
  `build/hardware/native-memory-20261002/ciukios-memory-map-prefix-v2.img`,
  SHA-256 `bc50cc7203571c547138648b64774a9e56f07fa822095a99c0d372e2ad185744`.
  The serial-pinned, backup-first guarded installer wrote this prefix to the
  Transcend SSD on 2 October at 06:42 UTC and verified the complete
  134,249,984-byte physical readback. The receipt is
  `build/hardware/native-memory-20261002/physical-memory-map-install-result.json`;
  its backup of the previous prefix has SHA-256
  `b125e0f78e63f0a5326d126c981c75c77cc5da1b3ebefc08624dddd9d9a8cbb8`.
  The SSD was powered off cleanly after verification.
- The owner booted this image on the Compaq E500 first and reconnected the SSD.
  Its captured `MEMMAP.BIN` has SHA-256
  `5135aaa2ff998cef568c77b842a84d121014fee2ffb2d0425d00e1f8cb67a2b6`:
  six complete, nonoverlapping E820 entries; 254.9375 MiB usable above 1 MiB;
  639 KiB from INT 12h; EBDA at `9FC0h`. This firmware map reaches 256 MiB,
  so the earlier approximate 512 MiB report must not be used as the allocation
  ceiling. The raw capture and decoded JSON are under
  `docs/validation/2026-10-02-physical-memory/`. The owner reports no screen
  flashing, but keyboard and mouse do not work in this boot. The `HARD` boot
  log reaches `S18`; the two input-poll markers `S13` and `S14` are present.
  These observations do not identify the input failure's cause. The SSD was
  unmounted and powered off after capture.
- The owner then booted the same image on the IBM T23. Its captured
  `MEMMAP.BIN` has SHA-256
  `43d6a18a663b8322509ffd9d9e816c71b9a513caa802d3ac2bb20e921ba07e58`:
  ten complete, nonoverlapping E820 entries; 510.375 MiB usable above 1 MiB;
  636 KiB from INT 12h; EBDA at `9F00h`. The map reaches 512 MiB and also
  marks reserved ranges near 4 GiB. The raw map, decoded JSON and boot log
  are saved under `docs/validation/2026-10-02-physical-memory/`. The SSD was
  unmounted and powered off after capture. The owner reports that the desktop,
  keyboard and mouse all worked on the T23. These are firmware-usable totals,
  not free pages: the native allocator must still subtract all currently
  owned pages and must never compete with JemmEx for the same frames.
