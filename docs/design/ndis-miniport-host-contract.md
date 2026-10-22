# NT 5.x NDIS miniport host (F6)

Status: f6-03 research and implementation contract; host census only. No binary
driver has been loaded or qualified by this step. The owner-pack binaries stay
outside Git. The lead must review this contract before f6-04–f6-07 code.

## Research and repository comparison

Research date: 2026-10-13. Microsoft's [PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
defines file offsets versus RVAs, PE32/PE32+, import descriptors, ordinal
thunks, original-thunk fallback and relocations. The census reads file-backed
RVAs with bounds checks; it never maps or executes an image. The F5
[PE loader contract](win32-personality-contract.md#pe-loader) supplies reusable
parsing and relocation rules, but its GUI/console subsystem policy cannot be
used for a kernel native-subsystem image.

Microsoft's [Manufacturer section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-manufacturer-section),
[TargetOSVersion rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/combining-platform-extensions-with-operating-system-versions)
and [DDInstall.Services section](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-services-section)
require model-to-install-to-service association. An undecorated Manufacturer
entry is not proof of Win9x support, and a decoration is a minimum OS target,
not an exact runtime-version test. `scripts/build_hwids.py` extracts labels
from all sections without following that chain; its 1155 PCI labels therefore
cannot establish binary-driver compatibility. The census retains the chain,
architecture, weak file-list references and unresolved dependencies separately.

The current `src/kernel/include/ciuki/net.h` is a provisional native driver
seam with `net_e100`, `net_rtl8139`, `net_ne2000`, bounded copied queues and
quarantine. [network-contract.md](network-contract.md#kernel-ethernet-boundary-and-driver-compatibility)
requires a richer completion/ownership seam and a future lwIP adapter. The NDIS
host must bind to that common seam, not create another stack. `registry.h`
already owns generation-based claims, shared INTx, synchronized removal and
permanent quarantine. `mm.h` has physical contiguous allocation and a 768 MiB
direct map, but no NT MDL/map-register API. `sync.h` has waits and mutexes,
short IRQ sections and bounded delays; none is an existing NT IRQL/DPC runtime.
The [hardware baseline](hardware-baseline.md) defines platform eligibility,
not qualification of every NIC family listed in the f6-03 directive.
The representative NatSemi mapping uses the upstream [DP8381x PCI table](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/natsemi/natsemi.c)
(`100b:0020`); the different DP83820 device is not counted as DP8381x. Family
tables retain full INF hardware IDs and do not authorize all boards with a
vendor/device pair.

## Scope and binding policy

**[ND1]** Host native-subsystem, i386 PE32, NT 5.0/5.1/5.2 connectionless
NDIS 5.0/5.1 **wired PCI Ethernet miniports**. A Net class, XP filename or
import of `NdisMRegisterMiniport` alone does not establish the NDIS version:
the host must check the registered characteristics at runtime. No Microsoft
`ndis.sys`, `ntoskrnl.exe` or `hal.dll` binary is installed; these module names
resolve exclusively to CiukiOS's native shim exports. Each exact driver hash,
PCI revision/subsystem, INF selection and parameter set needs qualification.
Unknown versions, imports or driver kinds fail before device activation.
Qualification must also check the binary's CPU instruction requirements on
the i686/CMOV baseline; a PE Machine field does not prove absence of SSE or
other optional instructions.

| Excluded surface | Technical reason |
| --- | --- |
| Win9x VxD/NDIS3 (including LE/LX and native-Win9x PE subsystem 8) | VMM service calls, VxD device descriptors, Win9x loading/resource model and NDIS3 binding ABI differ from NT DriverEntry and NDIS5. A Win9x session is not NT NTVDM. |
| NT NDIS3/4, NDIS6, intermediate/filter/protocol/WAN/connection-oriented drivers | Different registration structures and binding/lifecycle or net-buffer APIs; this host supplies one Ethernet miniport upper edge. |
| WDM audio/`portcls.sys` and display/`videoprt.sys` drivers | Require kernel streaming/PortCls, codecs/DMA/audio topology or video miniport plus graphics/display state. NIC compatibility supplies none of those frameworks. |
| USB, PCMCIA, Wi-Fi, Bluetooth, virtual/bus-enumerator drivers | Require additional bus/PnP/URB, radio/authentication or proprietary lower-edge frameworks. A `Net` INF classification does not make them PCI Ethernet. |
| x64, IA64 and other architectures | CiukiOS's kernel and ABI are i686/32-bit. PE32+ census support is inventory only. |
| Windows services, coinstallers, arbitrary kernel extensions and ring-0 game protection | No complete NT executive, service manager or arbitrary device-control personality. |

**[ND2]** Keep the native `e100`, `rtl8139`, `ne2000` paths as the default for
their allowlisted devices. An explicitly installed and qualified miniport may
replace a native owner only after successful stop, synchronized IRQ removal
and DMA idle proof; never run two owners on one PCI function. Failed binary
load leaves an untouched native device available; a device already quarantined
must not fall back or reprobe during the same boot. Safe mode loads no physical
miniport. Startup loads only installed qualified manifests, not every pack file.

The native compatibility section of `network-contract.md` excludes Windows
binary loading **from the BSD source shim**. ND1 is a separate, owner-requested
F6 extension to that boundary, not a widening of the BSD API or the F5 Win32
application personality (which continues to reject kernel drivers).

## Loader shared with F5

**[ND3]** f6-04 and f5-05 must use one host-testable PE parsing/relocation library
with policy and allocator callbacks, compiled into both consumers. Share byte
readers, overflow/extent checks, section layout, data-directory walking,
name/ordinal thunk parsing, original-first-thunk fallback and relocation
validation/application. The census Python parser is an independent inventory
implementation and fixture producer, not code to embed in the kernel.

| Shared mechanism | F5 policy | Kernel host policy |
| --- | --- | --- |
| PE32/i386 layout and bounds | GUI/console, process address space, F5 error reporting | Native subsystem 1, executable driver entry in executable section, supervisor mappings; malformed load rolls back before DriverEntry |
| Sections and private relocations | Per-process image views | Pinned kernel image, per-module references; no relocation into unowned memory or live code |
| ABSOLUTE/HIGHLOW; reviewed HIGH/LOW/HIGHADJ | F5 relocation contract | Same checked arithmetic; absent relocations require preferred base to be available; reject unsupported relocation types |
| Imports/ordinals/forwarders | Builtins plus application search paths | Only the declared native NDIS/NT/HAL export allowlist, exact x86 calling conventions and data exports; no user DLLs or filesystem search fallback |
| TLS, delay imports, bound imports | TLS/DllMain/user helper behavior | Reject TLS initially; ignore bound timestamps and re-resolve; resolve declared delay imports eagerly or reject with reason before entry; no user helper execution |
| Entry/unload | DllMain/TLS/process module lock | `DriverEntry(DRIVER_OBJECT*, UNICODE_STRING*)`, followed by miniport start; image remains pinned until all callbacks/DMA/references drain |

The kernel must not relax bounds because an image is vendor-signed. Do not
silently zero-fill damaged import/relocation tables. The later shared parser
must preserve F5's specifically accepted unusual layouts without granting the
kernel host those application admission policies. Keep parser/mechanism tests
common and subsystem/search/entry-policy tests separate. Before entry, resolve
the **entire** required import set and dynamic API requirements of that
qualification profile; never provide success-returning catch-all stubs.

`NdisGetRoutineAddress`/`MmGetSystemRoutineAddress` must use the same allowlist
and return NULL for unsupported optional names. Ordinals must resolve against
a pinned NT5-compatible export mapping; an unknown ordinal is not an inferred
name. Shim ABI fixtures must cover stdcall stack cleanup, HAL `Kf*` and kernel
`Kef*` fastcall conventions, cdecl CRT helpers, counted UTF-16 strings, packed
NDIS structures/MDLs and `KeTickCount` as a **data** export. No host/compiler
native-width assumptions may enter the 32-bit ABI.

## Miniport initialization and lifecycle

Microsoft's [NdisMRegisterMiniport](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553602(v=vs.85))
and [MiniportInitialize](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff550472(v=vs.85))
define registration separately from starting each adapter. CiukiOS must preserve
that distinction. Its lifecycle is DISCOVERED → VERIFIED → CLAIMED →
INITIALIZING → RUNNING → STOPPING → STOPPED, or permanent QUARANTINED.
Only RUNNING publishes a usable netif; every object carries a fresh generation.

**[ND4]** After PCI inventory validation and image/import verification, prepare
the bounded per-adapter context, claims, parameters and resident callback
storage with bus mastering off. Call DriverEntry at PASSIVE_LEVEL; provide
only the qualified DRIVER_OBJECT/lifecycle ABI. `NdisInitializeWrapper`
(the exported implementation of the DDK `NdisMInitializeWrapper` macro)
creates an owned wrapper; `NdisMRegisterMiniport` copies the version-sized
characteristics and checks every callback address against executable driver
sections. Require initialize/halt/query/set/reset, a send path and a supported
receive path. Require the 5.1 PnP/shutdown fields when the version demands them;
reject connection-oriented/intermediate registration explicitly.

Start only the selected PCI function after DriverEntry returns. Invoke
MiniportInitialize at PASSIVE_LEVEL with `NdisMedium802_3` as the offered
medium, a host-owned adapter handle and configuration context. Check the
returned medium index, status and MAC/MTU before publication. Support no async
PENDING initialize fiction. Resource rollback applies even when the driver
fails to clean up its own allocations.

[NdisMSetAttributesEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsetattributesex)
must establish the miniport's resident context, PCI type, bus-master capability,
serialized/deserialized mode and hang interval **before** resource/interrupt/DMA
registration. `NDIS_ATTRIBUTE_DESERIALIZE` is not a license to bypass lifetime
and completion rules. Reject unsupported attribute combinations. Preserve the
driver's declared callback concurrency contract: serialize ordinary callbacks
for a serialized driver; retain correct deserialized packet ownership and
reentrant host indications even on the initial uniprocessor system.

**[ND5]** The host invokes CheckForHang on the requested interval (zero uses
the DDK default) and handles MiniportReset SUCCESS/PENDING, `AddressingReset`
and `NdisMResetComplete` with one tracked request and a deadline. Reapply
filter/lookahead/multicast settings when reset invalidates them. Reset is
allowed only for an active, still-owned qualified device; never retry a
quarantined driver. Halt/shutdown and PnP notifications must follow the
registered NDIS version. Platform suspend/WoL/hotplug remain disabled until
their full resource/power lifecycle is separately qualified.

## IRQL, DPCs, interrupt and shim semantics

IRQL is Windows execution priority, not a Ciuki scheduler priority number.
Microsoft's [hardware-priority rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/managing-hardware-priorities)
forbid blocking waits at DISPATCH_LEVEL or above and distinguish these levels
from hardware IRQ masking. Map the initial uniprocessor host as follows;
f6-05 must supply an explicit IRQL state and restore it across every callback.

| Windows context | Ciuki execution and restrictions |
| --- | --- |
| PASSIVE_LEVEL (0) | Kernel control/work task with IF=1; initialization/configuration/halt and allowed waits; no driver callback under registry or stack-core locks |
| APC_LEVEL (1) | Logical level, no user APC execution; preserve raise/restore semantics and reject unsupported APC-dependent services |
| DISPATCH_LEVEL (2) | `P_DEVICE` NIC worker with IF=1, no sleep/page fault/blocking allocation; serialize DPC dispatch on UP; hardware ISRs may interrupt it |
| DIRQL (translated interrupt resource) | Registry shared-IRQ callback or a short interrupt-synchronization section; no sleeping, frame copying, registry mutation or lwIP calls |

**[ND6]** `KeRaiseIrql`/`KfRaiseIrql`, lower/restore and GetCurrentIrql must
check transitions and preserve old state. Raising to DISPATCH_LEVEL must not
globally CLI for an entire DPC. Above DISPATCH_LEVEL, use bounded IRQ-save
sections for qualified interrupt synchronization; do not claim arbitrary NT
PIC priority emulation or SMP support. Preserve nested IRQ state. A wrong-level
wait or illegal transition faults the adapter instead of silently proceeding.

[DPC objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-dpc-objects)
map to a preallocated, bounded queue serviced after hard IRQ return, not to
inline ISR completion. Implement KeInitializeDpc/insert/remove and NDIS timer
DPCs with one queued instance per object, generation cancellation, the Windows
queue-result semantics and a drain operation. KeSetTimer/periodic timers use
monotonic deadlines; NT relative due-times use negative 100 ns units, absolute
NT times require explicit conversion rather than treating them as PIT ticks.
Callbacks and queue nodes stay pinned through removal and execution. Exhaustion
fails closed; no allocation in hard IRQ. Drain bounded batches and yield between
callbacks, while preserving the network contract's eight-task ceiling and
32-frame/2-ms worker dispatch bound. A foreign callback cannot be split by that
batch rule; its independent execution-bound requirement is discussed below.

[Spin locks](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-spin-locks)
must provide actual exclusion and raise/restore semantics. Executive locks
raise to DISPATCH_LEVEL with device interrupts still possible; interrupt locks
and NdisMSynchronizeWithInterrupt use the relevant short IRQ-save section.
On UP, reentry/contention in a nonpreemptible callback is a bug, not a reason
to spin forever. Never implement these APIs as sleeping kmutexes at raised
IRQL or as no-ops. Dpc-level acquire/release variants verify the caller level.
An ISR must not acquire an executive lock held by the interrupted worker.

[NdisMRegisterInterrupt](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553596(v=vs.85))
maps only the owned raw/translated resource pair to `irq_chain_add`; reject
foreign vectors, invalid level routing and exclusive requests on a shared line.
Shared PCI mode requires a MiniportISR. Call it at DIRQL with recognized and
queue-DPC outputs; return not-mine immediately for another device. Mask/ack only
the selected source. Queue [MiniportHandleInterrupt](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff549458(v=vs.85))
on the NIC worker at DISPATCH_LEVEL, then re-enable with a pending-cause recheck.
Only the PIC service sends EOI. Keep the existing 50 µs per-source target and
250 µs aggregate chain budget, and synchronize deregistration before freeing
any object. An unshared no-ISR path needs its own qualified source-masking
profile; do not apply it to a shared PCI line.

[IoConnectInterrupt](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioconnectinterrupt)
and IoDisconnectInterrupt, if a chosen miniport imports them, must reach that
same registry-owned interrupt service, not install another IDT/PIC owner.
Validate resident context, vector/IRQL, sharing, affinity CPU 0 and
FloatingSave=false; synchronize via the associated interrupt object.

**[ND7]** Native NT/HAL exports must supply these families with real semantics:

| Family | Required meaning and boundary |
| --- | --- |
| Pools/lookasides/MDLs | Tagged, per-adapter accounted resident allocations; pool failure returns the documented error/NULL. PagedPool requests may be backed resident at permitted levels, but that must not legalize paged-pool use at high IRQL. MDL PFNs/offsets/counts describe owned pinned memory; lookaside depth is bounded; double-free/foreign-free is a fault. |
| Events/semaphores/waits/work items | Native `kwait` predicates and monotonic timeouts at permitted levels; event reset/synchronization vs notification behavior, cancellation and NTSTATUS results; work items at PASSIVE_LEVEL. Never block the worker waiting for its own DPC. |
| Time/delay | Real tick count/time increment and coherent NT system-time/data exports, performance counter/frequency, relative deadlines; no invented CPU count or clock values. |
| Stall | [KeStallExecutionProcessor](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-kestallexecutionprocessor) busy-waits with calibrated timing; use bounded `udelay` only when its semantics hold. No sleeping replacement. Each qualified call and aggregate IRQ path must fit its deadline; unsupported duration/call pattern rejects the profile, not truncates the delay. |
| Interlocked/list/string/CRT | Correct 32-bit atomic/order semantics, structure offsets, counted UTF-16 conversion, checked output buffers and NT statuses; implement imported compiler integer helpers with their x86 calling convention. |
| I/O/device objects | Minimal owned driver/device objects and completion semantics only for explicitly qualified paths. IoCreateDevice/IoCallDriver/IRP imports in the broader Net cohort are evidence of additional work, not harmless stubs. USB/proprietary bus stacks remain excluded. |
| Port/MMIO/PCI | Read/write only the claimed port/BAR extent; serialize PCI config access; reject BAR sizing, foreign BDF or PCI command writes bypassing the host's DMA-enable lifecycle. |
| Registry/file queries | Only the installed adapter parameter tree and declared driver-owned read-only support files; no unrestricted system registry/file namespace or vendor service execution. |
| Bugcheck/fault | `KeBugCheckEx` records and stops this adapter through the emergency fence when safe; never return to a driver after a fatal error, or label memory corruption as recovered. |

[ExAllocatePoolWithTag](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exallocatepoolwithtag)
requires nonpaged allocations at DISPATCH_LEVEL and distinguishes pool types;
the shim must enforce this even though CiukiOS initially keeps driver memory
resident. Imported symbols include data and CRT helpers as well as APIs.
Exports absent from the initial qualified profiles are explicit load errors,
not a claim that the entire NT kernel has been implemented.

## DMA and memory budget

**[ND8]** Use the kernel physical allocator and the F6 DMA ledger, not ordinary
heap memory whose virtual address is treated as a bus address. Validate
address mask, alignment, boundary, maximum segment length/count and ownership.
Prefer NORMAL RAM for 32-bit PCI DMA; the below-16-MiB ZONE_DMA is not the
default PCI zone. Keep reservations and the direct-map ceiling unchanged.
No user, DOS guest or lwIP pbuf pointer may be passed to a miniport for DMA.

[Map registers](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff552300(v=vs.85))
are bounded host mapping objects reserved during initialization (DDK limit
64 per miniport), not ISA DMA channels. Implement start/complete/free mapping
and validated physical segment lists with a live ownership transition.
[NdisMAllocateSharedMemory](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismallocatesharedmemory)
returns a paired pinned CPU pointer and device-visible physical address;
preserve cache attributes. Async shared-memory allocation completes once at
the registered AllocateComplete callback, including failure, after resources
are reserved; do not grow allocations automatically under traffic.
NDIS5 scatter/gather initialization and per-packet SG metadata must be
implemented when the selected binary requires them, with the same constraints.
Coherent x86 still requires publication/completion barriers and DMA sync
ownership; a successful no-op mapping is forbidden.

Only the host enables PCI bus mastering after complete descriptor ownership
and the emergency-stop profile are valid. `MmGetPhysicalAddress`, contiguous
allocation/cache and locked-page mapping APIs cannot expose arbitrary physical
pages. Pair every DMA/MDL/map/packet allocation with a ledger entry and pin it
until the driver and hardware have both released it.

Count image resident pages, shim state, pools, MDLs, DPC/timer nodes, stacks,
buffers and quarantine in the existing **4 MiB total F6 network budget**;
retain the 128 KiB/card DMA reservation and existing packet/task ceilings until
the lead measures and reviews a change. Admission must reserve a worst-case
qualified allocation profile before entry. If a vendor's default ring counts
exceed it, a documented INF parameter may reduce them only after qualification;
never silently ignore the request or raise the cap. The census measures image
sizes only, not runtime pools/DMA demand; no lower-memory hardware success is
claimed here. Physical available/reserved memory must be measured before any
later limit change, as required by AGENTS.md.

## Ethernet, send/receive and OIDs

**[ND9]** Expose an `ndis` implementation of `netif_driver` beside the three
native drivers. The shim owns NDIS packets, buffer descriptors and DMA mappings;
F6 owns bounded copied Ethernet frame slots. `transmit` copies the caller's
frame before success, returns EAGAIN on exhaustion and retires every accepted
token exactly once. Dispatch [MiniportSendPackets](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff550524(v=vs.85))
or MiniportSend according to characteristics; implement serialized and
deserialized completion differences, SUCCESS/PENDING/RESOURCES, resource-available
notification, cancellation and `NdisMSendComplete`. A pending TX packet cannot
be freed or reused before final completion. Reentrant completion must be safe.
For serialized packet-array sends, RESOURCES requeues that packet and the
remaining suffix in order until resources-available or completion. For a
deserialized packet-array send, ignore OOB status for completion and require
one SendComplete per submitted packet; such a miniport queues internally and
must not use SendResourcesAvailable. These modes need distinct fixtures.

[NdisMIndicateReceivePacket](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553533(v=vs.85))
must validate all buffer-chain lengths and copy the complete frame before
returning or queuing a return callback. Serialized SUCCESS means immediate
driver reuse; serialized PENDING entails a later MiniportReturnPacket.
Deserialized SUCCESS entails MiniportReturnPacket even if the host promptly
copies; queue that return safely after indication, with no stale-generation
reuse. RESOURCES always retains ownership at the miniport: never issue a
host ReturnPacket for it. Respect the caller's saved status rather than
rewriting deserialized ownership as serialized ownership.

Support the older Ethernet lookahead/TransferData path used by otherwise
eligible NDIS5 miniports: retain the receive context only for its permitted
lifetime; a pending transfer owns a host packet until NdisMTransferDataComplete.
Bounds and teardown tests cover partial lookahead, short completion and
late indications. Supply the DDK-visible NDIS packet/buffer layout and
callback fields, including macro-generated calls that never appear as PE
imports. The import census is a lower bound on the host's ABI surface.

F6 validates 14–1514 bytes excluding FCS, pads TX to 60, rejects malformed
chains and unsupported VLAN/jumbo/offload traffic, and copies to its common
RX/TX queue. Only the network worker demultiplexes host ARP/IPv4 versus guest
IPX and hands copied pbufs to `tcpip_input`; the shim never calls lwIP from
an ISR or directly accesses a DOS VM. Keep distinct administrative/carrier
state, counters and quarantine reasons in the public network snapshot.

**[ND10]** The host **issues** OID queries/sets to MiniportQueryInformation and
MiniportSetInformation; the miniport answers for its hardware. This is the
NDIS5 direction, unlike the NDIS6 initialization attributes. Track one control
request per adapter with owned buffer, byte counts, status, generation and
deadline; handle PENDING through Query/SetInformationComplete and discard
late completions safely without freeing still-owned storage.

| OID group | Host duty |
| --- | --- |
| `OID_GEN_SUPPORTED_LIST`, HARDWARE_STATUS, MEDIA_SUPPORTED, MEDIA_IN_USE | Query and validate Ethernet and operational capabilities; [SUPPORTED_LIST](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-supported-list) is mandatory for NDIS5.1, not an NDIS6 attribute shortcut. |
| GEN_MAXIMUM_FRAME_SIZE, MAXIMUM_TOTAL_SIZE, MAXIMUM_LOOKAHEAD, CURRENT_LOOKAHEAD | Validate 1500-byte MTU/1514-byte frame and set needed lookahead; reject undersized buffers or unsafe sizes. |
| GEN_LINK_SPEED, MEDIA_CONNECT_STATUS, MAC_OPTIONS, VENDOR_ID, VENDOR_DESCRIPTION, DRIVER_VERSION, MAXIMUM_SEND_PACKETS | Build coherent state; never infer carrier from one successful send. Process media connect/disconnect indications and status-complete. |
| `OID_802_3_PERMANENT_ADDRESS`, CURRENT_ADDRESS, MAXIMUM_LIST_SIZE | Query and validate unicast six-byte MAC and multicast capacity. |
| `OID_GEN_CURRENT_PACKET_FILTER`, `OID_802_3_MULTICAST_LIST` | Set directed/broadcast and required multicast only; promiscuity for the DOS bridge requires separate qualified capability and ingress filtering. |
| GEN_XMIT_OK/RCV_OK/XMIT_ERROR/RCV_ERROR/RCV_NO_BUFFER and relevant 802.3 counters | Query supported counters; preserve widths and distinguish unsupported from zero; host queue drops remain separate. |
| PNP_CAPABILITIES, QUERY_POWER, SET_POWER, ENABLE_WAKE_UP | Query only where advertised; qualify D0 start/stop for first profiles; disable WoL, sleep transitions and unsupported power promises. |
| TASK_OFFLOAD and vendor OIDs | Do not enable checksum/LSO/VLAN/offloads; enumerate/query only when profile requires it, reject unsupported settings and never fabricate success. |

The host exports OID-backed snapshots through Ciuki networking APIs, not raw
NDIS user handles. Malformed sizes, buffer overrun, impossible completion,
timeouts and unsupported mandatory capabilities fail initialization or fault
the active adapter. Optional unsupported OIDs use their documented status.

## Parameters and Device Manager install flow

**[ND11]** Device Manager exposes the English action **Install driver from pack**.
The user selects a local directory or CD; the pack stays on the owner's media.
Read candidate INFs without executing installers. Match complete PCI hardware
IDs (revision/subsystem before compatible IDs), model OS/architecture and
service role; filter for ND1 before offering choices. Show vendor, version,
matching ID, qualification status and reason for rejected candidates. Preserve
user choice among eligible packages; do not pick a random basename duplicate.

Stage only the selected INF, `.sys` and declared permitted dependencies into
`/System/drivers/<inf-dir>/` with a unique sanitized package directory; prohibit
absolute paths, traversal, symlink escape and basename collisions. Include/Needs
must resolve explicitly in f6-07; the census warnings are not install permission.
Compressed source payloads require a separate bounded extraction/validation
path before they can be offered; f6-03 does not extract `.sy_`/CAB files.
No vendor EXE/coinstaller/service, arbitrary AddService or AddReg command is run.

Commit a versioned manifest atomically after validation: selected full hardware
ID, PCI function/revision/subsystem, source INF and hashes, selected models/
install/services, associated miniport service, binary/dependency hashes,
driver version and a bounded typed parameter tree. Rollback leaves the previous
manifest and driver owner intact. Replacement after successful stop is explicit;
quarantine blocks replacement until reboot. Uninstall drains or reports the
quarantine instead of freeing live resources. Reboot/start revalidates hashes
and the hardware match, leaving a disabled device with a reason on mismatch.

Materialize only the selected install/service AddReg tree and `Ndi\\Params`
defaults/enumerations, plus allowlisted host facts/resources. [NdisReadConfiguration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndisreadconfiguration)
returns driver-owned typed configuration values through a host handle; keep
returned storage alive until CloseConfiguration. Serialize reads/writes and
support nested keys, string/integer/binary types, proper conversion and
missing-key status. Validate lengths/ranges without inventing values. NetworkAddress
override is applied only if valid and requested. Scope `ZwOpenKey/QueryValueKey`
and [RtlQueryRegistryValues](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlqueryregistryvalues)
to the installed adapter/service view with Windows buffer/status/callback
semantics. This view is separate from the user-mode F5 registry.

CiukiOS distribution contains its shim, tests and descriptive hardware IDs,
not the owner pack. Vendor redistribution rights are **not assumed** from
possession of a pack or from CiukiOS/ndiswrapper being GPL. Packaging/release
tools must not ingest these driver payloads automatically. Installation is a
user-selected local import; any later redistribution requires its own vendor
license evidence, outside this directive.

## Failure containment and bus ownership

**[ND12]** Use the native registry quarantine and DMA lifetime rules, with an
independent per-device emergency fence established **before** foreign entry.
Each qualified PCI revision needs a documented stop/reset sequence callable
without driver cooperation: block new submissions, invalidate generations,
mask only that source, disable bus mastering through the PCI owner and verify
readback, synchronize/drain the interrupt/DPC path and prove the engine idle.
Revoke port/MMIO mappings only when safe. Clear all pending host work and
complete accepted network tokens with final error exactly once.

If idle cannot be proved, retain every DMA page, image and callback context;
quarantine the claims permanently. A timeout, PCI-command readback or returned
MiniportHalt alone is not proof that in-flight DMA is finished. Do not reset
an unrelated function/bridge or hold peers' IRQ line indefinitely as recovery.
Record driver hash, PCI identity, generation, fault/timeout and fence result.
Safe mode must allow recovery without loading the offending binary.

**Containment prerequisite, not current capability:** arbitrary ring-0 code
can write kernel memory, execute CLI, spin indefinitely or program DMA outside
the shim. This UP nonpreemptible kernel has no IOMMU or foreign-callback fault
recovery/watchdog boundary. A worker deadline, checked import or guarded stack
cannot guarantee recovery when control never returns. Therefore no claim of
security isolation or universal bus-fault containment is justified by NDIS
shims alone. The requirement that a faulting driver must not retain the bus
is a release/admission gate, not an assumed property of vendor binaries.

f6-04 must specify and the lead must cross-review the executable callback
boundary and independently reachable fence/watchdog before driver execution
is enabled; host and QEMU fault fixtures must prove exception and nonreturn
paths. If a profile cannot meet that requirement, leave it disabled. Existing
native panic paths must not be advertised as per-adapter recovery. A fault
inside a host critical section or detected host-memory corruption may require
a controlled system stop; report it honestly and do not resume a corrupted
kernel. No routine or watchdog may reclaim pinned DMA from an uncertain stop.

## ndiswrapper scope reference and licensing

The [ndiswrapper upstream project](https://sourceforge.net/projects/ndiswrapper/)
is GPLv2; CiukiOS's [LICENSE](../../LICENSE) is GPLv2. The upstream
[1.63 source file manifest](https://fossies.org/linux/ndiswrapper/driver/Makefile)
(archived source, not a binary dependency) separates `pe_linker`, loader,
`ntoskernel`, `ntoskernel_io`, `hal`, `ndis`, `wrapndis`, PnP, USB, memory and
Windows/Linux ABI bridges. That breadth explains why a short list of Ndis
exports is not a complete host. Linux workqueues, PCI/DMA, synchronization
and net_device services supply machinery CiukiOS must implement itself.

The upstream [wrapndis.c 1.63 source](https://fossies.org/linux/ndiswrapper/driver/wrapndis.c)
shows separate initialization, serialized OID/reset dispatch, pending
completion waits and explicit halt cleanup of leftover IRQ/timer objects.
Its source notice is GPLv2-or-later, compatible with this project's GPLv2-only
use of an eligible fragment. The [project source README](https://github.com/pgiri/ndiswrapper)
describes NDIS5/XP support and an incomplete separate NDIS6 path. Use these as
scope/pitfall references only: do not import Linux glue or repeat its
unsupported-success behavior. Browser access to several individual loader/
NT source pages failed; no detailed review of those implementations is claimed.

Implement from Microsoft specifications and the reviewed Ciuki contracts.
**No external code was copied in f6-03.** Any later copied fragment must record
its original path, release/commit, copyright and license and retain its notice;
license compatibility of source code gives no rights to vendor Windows drivers.

## Acceptance and following directives

**[ND13]** Acceptance is tied to the exact locally measured driver/INF hashes
and the image SHA-256, not to a filename or family. QEMU/hardware runs belong
to the lead and were not run in f6-03. First profiles:

| Device | Local XP candidate | Qualification needed |
| --- | --- | --- |
| QEMU `e1000`, 8086:100e | `UIU/e1000325.sys`, `e1000325.inf` | Validate QEMU revision/subsystem, NDIS5 registration, complete imports/parameters/DMA and traffic |
| QEMU `rtl8139`, 10ec:8139 | `UIU/Rtlnicxp.sys`, `NetrtOEM.inf` (XP binary candidate) | The associated OEM INF has no generic 8139 row or QEMU SUBSYS_11001AF4 match. Resolve a matching INF/source package before this gate; do not discard subsystem checks. `added/.../Netrtlx.inf` has generic revision rows for `Rtnic.sys`, but its XP `Rtnicxp.sys` reference is unresolved locally. Compare against native rtl8139 after qualification. |
| QEMU `pcnet`, 1022:2000 | `UIU/atnicm51.sys`, `NETATNIC.INF` | Allied Telesis package; INF offers SUBSYS_00000000 plus vendor-specific IDs. QEMU's [PCI PCnet implementation](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/pcnet-pci.c) explicitly sets both subsystem IDs to zero, so that INF row is a static match. Record the tested QEMU revision and verify the real guest IDs; this does not generalize to arbitrary PCnet boards. |
| Owner T23, Intel PRO/100/e100 | `UIU/e100b325.sys`, `e100b325.inf` (also referenced by e100a325) | Measure exact PCI/revision/subsystem and firmware ownership; A/B boots against native e100 on the same machine, no concurrent ownership |

For each, require load/registration/initialization, resource claim rollback on
failure, MAC/carrier/status OIDs, ARP and IPv4/UDP bidirectional traffic,
link down/up, bounded sustained RX/TX, packet-chain/cancel/pending completion,
stop/unload and repeated install/remove without ledger growth. Keep all earlier
QEMU gates passing on the integrated image. Optional DOS IPX uses the same F6
bridge and qualifies after the stack/VM paths exist, not in this census step.

Inject missing import/bad relocation, IRQ budget overrun and not-mine on a
shared line, wrong IRQL/reentrant locking, pool/map exhaustion, malformed RX,
lost/duplicate/late TX and OID completion, stalled DMA stop, exception and
nonreturning callback. Assert independent bus fencing, intact peers, persistent
quarantine, no stale callbacks/reuse and no ordinary panic falsely recorded
as recovery. Pure synthetic host fixtures cover ABI/loader/state machines;
the owner pack must never become a repository test input.

| Directive | Scope after contract review | Required host evidence before lead's runtime gates |
| --- | --- | --- |
| f6-04 | Kernel PE loader sharing mechanism with f5-05; exact export ABI/data imports; callback containment/fence prerequisite | Shared hostile-layout/relocation/import fixtures, policy separation, entry rollback and fault/nonreturn boundary model |
| f6-05 | Native ntoskrnl/HAL shim for pinned acceptance profiles | i686 ABI, IRQL/DPC/locks/waits/time, pool/MDL/registry/PCI/DMA ownership, bounds and cancellation |
| f6-06 | Native `ndis.sys` host and `netif_driver` adapter | Synthetic miniports exercising initialize/attributes, IRQ/DMA, serialized/deserialized sends/receives, OIDs, teardown and quarantine |
| f6-07 | Device Manager local pack install flow | Exact ID/OS/service selection, typed parameters, Includes/Needs, safe paths/dependencies, transactional manifest install/rollback/uninstall; no payload redistribution |

The later directives must select complete per-driver symbol and behavior sets
from the census, not declare implementation success at an import percentile.
Shim omissions require explicit refusal and a reviewed follow-up; no implicit
expansion to audio/display/USB drivers.
The [QEMU RTL8139 source](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/rtl8139.c)
sets revision 20h and inherits the [PCI default subsystem](https://raw.githubusercontent.com/qemu/qemu/master/hw/pci/pci.c)
whose [ID constants (upstream maintainer patch)](https://lists.gnu.org/archive/html/qemu-devel/2022-09/msg05208.html)
are 1af4:1100. This source-derived identity is not guest runtime evidence;
the lead must record the actual tested version and guest IDs. Resolving the
packaged XP INF mismatch is part of preparing the acceptance inputs.

## Measured expectation

The reproducible census and detailed tables are in the handoff report
`build/codex/f6-03-report.md`, with JSON `build/pe-import-census.json`.
The initial run finds six requested representative wired families with
uncompressed NT5-candidate miniport binaries: RTL8139, RTL8169, PRO/100,
PRO/1000, Broadcom 440x and PCnet. Four potentially add coverage beyond native
e100/rtl8139/ne2000: **RTL8169, PRO/1000, Broadcom 440x and PCnet**. This is a
family-level import/INF opportunity, not four newly working cards; qualified
additional devices today: **zero**. 3Com has compressed-only evidence that
is outside this census's import denominator. The other requested families
have no associated matching uncompressed binary evidence in this pack scan.

The six-family import union is initially **126 symbols** (81 NDIS, 35 NT kernel,
10 HAL); their semantics, macro callback ABI and lifecycle work are additional.
For the broad Net/NT5 candidate cohort (including wireless/USB/bus helpers),
the final scan has 77 content-distinct candidate binaries and 237 unique imports,
including proprietary and USB dependencies. Restricting to binaries that import
legacy miniport registration leaves 76 distinct candidates and 227 imports;
both cohorts still include excluded wireless/bus-dependent drivers.
The report supplies content-deduplicated, complete-import-set 50/80/95 percent
thresholds. These are export-count workload estimates, not executable shim
bytes, source LOC estimates, runtime coverage or a guarantee that 95% of this
pack is in scope. Runtime memory and execution containment remain qualification
gates; neither can be inferred from static imports.

## Amendment ND14 (lead, 2026-10-23 00:10): user-mode driver host

f6-05 part 1 showed that ring-0 containment of foreign code cannot be complete on i686 without hardware isolation:
- a CLI-free spin inside an interrupt handler that runs with IF clear cannot be preempted;
- a 1 kHz PIT cannot enforce a 50 µs ISR budget;
- a linear instruction scan can be defeated by jumps into immediates or by self-modification;
- the PCI fence does not prove that DMA has stopped.

**[ND14]** Each miniport therefore runs in a dedicated **user-mode driver-host process** at ring 3, one per adapter. The f6-04 loader and the shim run inside it.
1. **Port I/O.** The process gets its BAR port ranges in the TSS I/O permission bitmap with IOPL 0.
   - `in`/`out` to its own ports run natively; any other port faults.
   - `cli`, `sti`, `pushf`/`popf` with IF and `hlt` raise #GP and are emulated: a virtual interrupt flag per host, the way the Windows 9x VMM virtualises IF for V86 machines, and `hlt` as a yield.
   - No instruction scan is required.
2. **Memory.** MMIO BARs are mapped uncached into the process. DMA memory comes from the kernel physical allocator and the F6 DMA ledger, pinned and mapped into the process with its physical addresses reported through the shim. The process cannot map anything else.
3. **Interrupts.**
   - **Top half:** the kernel acknowledges the interrupt controller and masks the source, using the PCI 2.3 Interrupt Disable bit when the device supports it, otherwise the line.
   - **Delivery:** the kernel wakes the host thread. The host runs `MiniportISR`/`MiniportHandleInterrupt`, then unmasks through a syscall.
   - **Shared lines:** a not-mine result unmasks at once; masking on a shared line is bounded by the watchdog.
   - **IRQL:** a state of the host's shim, enforced in software as before.
4. **Budgets, enforced by a kernel watchdog on the host process.**

   | Callback | Budget |
   | --- | --- |
   | Initialize, reset, halt | 10 s |
   | Other passive-level callbacks | 500 ms |
   | DPC-class work, handle-interrupt | 50 ms |
   | ISR | 5 ms |

   An overrun, a fault or an exit kills the host process. The kernel then applies the fence: Bus Master Enable off, the I/O and memory decode off, Interrupt Disable on, a D3hot→D0 reset where the PM capability allows it. It quarantines the adapter for the boot and keeps the DMA pages pinned until reboot when idle cannot be proved.
5. **Network path.** The `netif_driver` adapter of ND9 talks to the host process over a shared ring of packet descriptors in the pinned memory, with a doorbell syscall in each direction. A crash or quarantine takes the interface down with a record; lwIP and other adapters continue.

ND5–ND8 keep their semantics inside the host process. ND12's scan becomes unnecessary and is dropped. f6-04's in-kernel resident loader is reused with a user-process allocator policy.

### Amendment ND14.1 (lead, 2026-10-23 00:13): Codex plan review

A Codex `gpt-6.1-sol` plan review returned ten findings with code evidence (three blockers); the lead accepts all of them, and they amend ND14:

1. **Flags virtualisation.** CLI/STI/HLT trap; PUSHF(D), POPF(D) and IRET(D) do not provide a #GP virtualization boundary. Keep physical IF enabled. A separately reviewed flag-save/restore mechanism and its supported patterns must pass executable tests before admitting dependent profiles.
2. **Teardown order.** Before ordinary process teardown, atomically stop admission, invalidate the adapter generation, prevent further user execution, fence the device and detach callbacks. Device mappings are borrowed mappings; unmapping never frees MMIO or DMA backing. Transfer uncertain backing to kernel quarantine ownership before reaping.
3. **Shared interrupt lines.** Shared-line admission requires verified per-function masking or a qualified native source masker; otherwise require exclusive ownership or refuse. Mask before dispatcher-owned EOI. Add generation-tagged pending delivery, separate from not-mine accounting, and validated rearm with pending-cause recheck.
4. **DMA containment.** This boundary contains CPU execution and CPU memory accesses, not arbitrary DMA or bus faults. Retain ND12’s per-revision emergency-stop gate and qualify peer survival only for stated fault classes. PM capability presence alone does not qualify D3hot→D0 as a reset or idle proof.
5. **I/O permission bitmap.** Install the incoming task’s complete effective port allowlist atomically on every switch; default-deny ordinary tasks and retain the FF sentinel. Bind permissions to active registry generations, revoke before resumption, and prevent inheritance through process creation or replacement.
6. **User device mappings.** Provide typed, revocable user mappings for owned BARs and ledger allocations, preserving cache attributes and backing ownership. Validate overflow, page-rounded exposure and cache aliases; refuse BAR layouts that expose unrelated resources. Device mappings cannot be inherited, remapped or converted into ordinary anonymous backing.
7. **Loader user policy.** Reuse the PE mechanism with an explicit user-host policy: separate staging addresses from execution addresses, relocate against user VAs, and resolve function/data imports to process-local shim mappings. Kernel services cross validated syscalls; no imported kernel address or direct ring-0 callback is callable.
8. **Watchdog and termination.** Kernel-owned deadlines cover queued, running, yielded and blocked hosts, independently of shim progress. Specify timer granularity and maximum detection delay. Host faults bypass recoverable application signals. Timeout fences immediately; bounded syscall cancellation then unwinds kernel ownership before reap. Nested callbacks cannot extend an outer deadline.
9. **IRQL and scheduling.** Define callback scheduling, permitted ISR reentry and interrupt-lock exclusion explicitly. Virtual IF and synchronization defer host callback delivery, never physical timer interrupts. Distinguish the kernel’s 50 µs hard-IRQ target from user callback kill thresholds; explicitly reconcile the 50 ms threshold with the existing 2 ms worker bound.
10. **Shared ring trust boundary.** The kernel independently validates descriptor snapshots, indices, lengths, slot ownership, adapter generations and completion tokens. Use bounded offsets, never trusted user pointers. Specify publication barriers, backpressure, notification delivery and teardown; copy accepted frames into kernel-owned storage before stack consumption.

Foreign execution stays closed until the host suites pass and the lead has ring-3 runtime evidence for faults, nonreturn, permission switching, shared interrupts and quarantine teardown. DMA is not contained by ND14; that limit is stated in the user-visible driver properties.
