# F6 network contract: Ethernet, lwIP, native sockets and DOS LAN play

Author: Codex. Directive: f6-00. Date: 2026-10-12.
Status: draft for cross-review and lead approval, before stack/socket code.
No build, QEMU, hardware or networking qualification is claimed here.

## Scope and authority

**[F6]** Implementations MUST follow this contract together with
[network-foundations.md](network-foundations.md),
[device-firmware-ownership.md](device-firmware-ownership.md),
[execution-abi.md](execution-abi.md), [posix-subset.md](posix-subset.md),
[dos-dpmi-contract.md](dos-dpmi-contract.md#network-for-dos-vms-f6) and
[test-architecture.md](test-architecture.md). MUST identifies a requirement,
SHOULD a recommendation and MAY an option. Normative tables specify requirements.
Numbers, queue sizes, budgets and syscall layouts below are proposed CiukiOS
policy, not values required by POSIX, lwIP or a controller datasheet.

**[F6]** The canonical i686 image MUST provide wired Ethernet, native IPv4
TCP/UDP sockets, DHCP/static addressing and DNS without replacing the F2
newlib port or adopting a Linux binary ABI. The same image MUST detect the
supported machines at boot. F6 MUST retain the uniprocessor, non-preemptible
kernel with interruptible user waits, existing scheduler/FPU policy and
firmware ownership. Earlier QEMU gates MUST continue to pass on each integrated
image. IPv6, Wi-Fi/net80211, PPP, VLANs, routing/NAT, NIC hotplug, Wake-on-LAN,
offload, MSI, CardBus/PCMCIA activation and Wine winsock implementation are
deferred. F8 MAY build winsock on this native API through its own contract.

**[F6]** DOS networking MUST use a private virtual NE2000 and a kernel Ethernet
bridge; physical NIC access from a guest is forbidden. IPX MUST NOT be treated
as an lwIP IP protocol or translated through user-mode NAT. The DOS model and
required LAN gate are specified below; implementing them needs a separate lead
directive after the physical frame path is qualified. Their absence MUST remain
a visible open F6 gate, even if f6-01 through f6-04 have passed.

### Repository comparison before implementation

The checkout inspected was `977ad4ef`. Semble discovery preceded bounded source
reads. These are existing mechanisms, not evidence of working networking:

| Existing interface | Consequence for F6 |
| --- | --- |
| `src/kernel/include/ciuki/registry.h`: `pci_count/pci_get`, generation-based claims, activate/quiesce/release/quarantine and `irq_chain_add/remove`; eight shared IRQ participants, 250 microseconds aggregate budget | NICs MUST consume this registry and IRQ service. An independent PCI scanner, PIC owner or unconditional reset of every Ethernet-class device is forbidden. |
| `src/kernel/drivers/ac97.c`: BAR/routing validation, pinned buffers, claim rollback and stop proof | Reuse the lifecycle pattern, not its audio-specific routing or allocation constraints. Its use of `RES_MMIO` to track DMA RAM is an existing convention. |
| `src/kernel/include/ciuki/mm.h`: NORMAL/DMA page allocation, contiguous allocation, heap ledger, 768 MiB direct-map ceiling | There is no general BSD bus-DMA API here. F6 MUST add constrained mapping/ownership to the compatibility layer; ZONE_DMA means below 16 MiB, not all PCI DMA. |
| `src/kernel/include/ciuki/task.h` and `sync.h`: `task_create_kernel`, priorities, 32 KiB guarded kernel stacks, `kmutex`, predicate-based `kwait`, monotonic deadlines | These underpin `sys_arch`; mailboxes and semaphores still need a port. lwIP tasks MUST NOT be native user pthreads or run on the idle/IRQ stack. |
| `src/kernel/include/ciuki/abi.h`: syscalls 0–5 and 16–74; 128 process fd slots, 1024 global descriptions | Reserve new numbers starting at 75. Networking MUST use the existing process fd namespace and global description ledger. |
| `src/kernel/proc/fdtable.c`, `surface.c`, `process.c`: distinct object dispatch, reference counting and explicit spawn inheritance | Socket lifetime, mixed-object readiness and cancellation require integration; an lwIP descriptor number MUST NOT escape to userspace. |
| `sdk/sysroot-overlay/include/ciuki/runtime.h` and `sdk/build_sdk.py`: MIT public ABI and generated wrappers | SDK socket headers/wrappers MUST use CiukiOS layouts and errors, not export lwIP private headers. |
| `src/apps/control.c` and `devices.c`: retained legacy networking inventory/UI | These are not the F2 native configuration service. The F6 applet MUST consume the kernel status/control boundary below. |
| [f6-01-nic-drivers.md](../directives/f6-01-nic-drivers.md) | Its three register-level drivers and ARP probe are independent of sockets. Its interface MUST be reconciled with this contract before integration. It does not assign 3c905 or the DOS bridge implementation. |

### Research and source decisions

Research performed on 2026-10-12 used the following primary sources. Archived
vendor PDFs below are manufacturer documents hosted by mirrors, not independent
driver recipes. Source links to moving branches are research references; they
MUST NOT become unpinned build dependencies.

| Source | Resulting decision |
| --- | --- |
| [lwIP release archive](https://download-mirror.savannah.gnu.org/releases/lwip/) and [2.2.1 option definitions](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/include/lwip/opt.h) | Pin 2.2.1, `NO_SYS=0`, explicit options and fixed pools. This is a selected baseline, not a claim of newest or vulnerability-free software. |
| [lwIP threading documentation](https://www.nongnu.org/lwip/2_1_x/multithreading.html), [2.2.1 OS abstraction](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/include/lwip/sys.h) and [netconn API](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/include/lwip/api.h) | Use one TCP/IP core thread and serialized netconn operations. Documentation is for 2.1; the pinned 2.2.1 source controls option/API details. |
| [2.2.1 sequential implementation](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/api/api_lib.c), [socket adapter reference](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/api/sockets.c) and [DNS implementation](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/core/dns.c) | Implement POSIX wrappers over netconn; preserve partial I/O, readiness and copied-buffer lifetime. Use the bounded asynchronous resolver in the core thread. |
| [Intel 8255x software developer manual](https://www.intel.com/content/dam/doc/manual/8255x-10-100-mbps-ethernet-controller-software-dev-manual.pdf), host interface/CU/RU/EEPROM/MDI chapters | e100-family support MUST use CU/RU command blocks and variant-specific initialization, including the ICH/82562 path; an e1000 driver is unrelated. |
| [Realtek RTL8139C(L), revision 1.6](https://dreamcast.wiki/wiki/images/8/8d/Spec-8139c%28160%29.pdf), sections 6 and 8 | Support the classic receive buffer and four transmit slots; do not assume a C+ descriptor ring. |
| [National Semiconductor DP8390D/NS32490D, July 1995](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf), sections 7, 9–11 | Implement page registers, remote-DMA PIO and documented overflow recovery; remote DMA is not guest/host PCI bus mastering. |
| [Haiku FreeBSD network compatibility implementation](https://github.com/haiku/haiku/tree/master/src/libs/compat/freebsd_network), [FreeBSD wired-driver boundary](https://people.freebsd.org/~jhb/papers/drivers/slides.pdf), [FreeBSD RTL8139 source](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/dev/rl/if_rl.c) | Provide a small, audited wired-driver source compatibility layer; importing FreeBSD's IP stack or assuming complete driver binary compatibility is unnecessary. |
| [QEMU network devices/backends](https://www.qemu.org/docs/master/system/devices/net.html), [network invocation options](https://www.qemu.org/docs/master/system/qemu-manpage.html#network-options), [8255x model](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/eepro100.c) and [NE2000 model](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/ne2000.c) | Specify each NIC model/backend explicitly, separate IPv4 NAT cases from Ethernet/IPX cases, and qualify real hardware separately. |
| [DHCP, RFC 2131](https://www.rfc-editor.org/rfc/rfc2131.html), [address conflict detection, RFC 5227](https://www.rfc-editor.org/rfc/rfc5227.html), [POSIX socket interfaces](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/sys_socket.h.html) | Distinguish configuration, carrier and reachability; use conflict detection, explicit source-porting semantics and bounded user records. |

**[F6]** The lwIP import MUST retain BSD-3-Clause notices, archive/signature
provenance, SHA-256 and a patch list in the dependency manifest. It MUST compile
only the selected core, Ethernet and API files, not upstream HTTP/TFTP/SNMP/SMTP
servers or example applications. Before integration the lead MUST review
upstream fixes affecting enabled parsers and record any minimal backports against
the pinned release. Optional vulnerable applications MUST stay absent.
Initial f6-01 controller code MUST be datasheet-driven, as its directive requires;
no GPL driver code MUST be copied. Later BSD imports MUST pin an exact source
revision, audit every compiled file's license/dependencies and list supported
symbols before implementation. Public ABI/SDK additions MUST remain MIT.

## lwIP execution and resource contract

### Threads and `sys_arch`

**[F6-02]** lwIP MUST run in OS mode. `NO_SYS=1` forbids the chosen sequential
API and is not an alternative build profile. `tcpip_thread` MUST own raw/core
calls, timeout dispatch and netif/configuration changes. Drivers MUST deliver
through `tcpip_input`; other core work MUST use posted callbacks/netifapi.
Core locking and core-locking input MUST be disabled. Assertions MUST check
core-thread identity and reject ISR use. The current kernel has no general
priority-inheritance mutex on which to base a core-locking port.

**[F6-02]** A kernel socket worker MUST serialize normal netconn operations, using
nonblocking connect/accept/receive/write attempts and readiness notifications.
Blocking POSIX waits MUST occur in the calling task outside lwIP and outside
object mutexes. One caller waiting for input MUST NOT hold up sends, another
socket, DNS, link processing or close. The worker MUST process at most 16
commands per dispatch before yielding; NIC workers MUST yield after at most
16 frames or one millisecond of service, whichever comes first. The port MUST
use `LWIP_TCPIP_THREAD_ALIVE` or equivalent audited core-loop glue to yield
after at most 16 core messages when the mailbox stays busy. Timer callbacks
MUST remain bounded and MUST NOT perform device polls or disk I/O.
Core and NIC workers MUST use `P_DEVICE`; the socket worker SHOULD use
`P_NORMAL`. No more than eight network kernel tasks, including
two NIC workers, configuration and cleanup, MUST be created. A separate teardown
worker MUST own deletion after an exclusive ownership handoff; waiting for TCP
close MUST not stall the socket worker or physical ingress.
Transport-blocking shutdown work MUST use the same exclusive control-worker
handoff; public shutdown MUST mark the requested direction closed and enqueue
that work promptly. A half-closed live netconn MAY return to its normal owner
only after the control call completes. The normal worker MUST NOT wait for
transport progress inside a supposedly nonblocking control attempt.

**[F6-02]** `sys_arch` MUST implement mutexes, binary semaphores, bounded FIFO
pointer mailboxes (including null messages), task creation and monotonic
millisecond time with the upstream signatures. `sys_arch_sem_wait` and mailbox
fetch MUST return elapsed milliseconds or `SYS_ARCH_TIMEOUT`; timeout zero
means forever, not poll. Try-fetch MUST return `SYS_MBOX_EMPTY` without sleeping;
try-post MUST return `ERR_MEM` on a full queue. Ordinary post MUST wait for room
in thread context and MUST NOT drop a message. Allocation failures MUST be
reported before network activation. A mailbox MUST NOT be freed while tasks or
callbacks still reference it. Wakeups MUST use the existing compare/enqueue
predicate protocol; a cancelled user request MUST not cancel the core's internal
completion semaphore. ([Pinned `sys.h`](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/include/lwip/sys.h))

**[F6-02]** `sys_now` MUST expose low 32 bits of kernel monotonic milliseconds;
wall-clock correction MUST NOT change protocol deadlines. Wraparound MUST be
tested. `SYS_ARCH_PROTECT` MUST use short, nestable save/restore IRQ sections,
never a sleeping mutex. No allocation, pbuf free, netconn/core call, mailbox
wait, EEPROM/MII polling or frame copy MUST happen in a hard IRQ. A hard IRQ
MAY publish cause bits and wake an already allocated worker. Lock order MUST
follow `sync.h`; no mutex MAY cross a user return, task exit or firmware entry.

### Baseline configuration

**[F6-02]** The following options MUST be identical in kernel and host stack
tests. Unlisted options MUST use the pinned release defaults, except port hooks
which MUST be reviewed explicitly. Runtime test selection MUST NOT change them.

| Group | Required configuration |
| --- | --- |
| Execution/API | `NO_SYS=0`, `SYS_LIGHTWEIGHT_PROT=1`, `LWIP_TCPIP_CORE_LOCKING=0`, `LWIP_TCPIP_CORE_LOCKING_INPUT=0`, `LWIP_TIMERS=1`, `LWIP_TIMERS_CUSTOM=0`, `LWIP_NETCONN=1`, `LWIP_SOCKET=0`, `LWIP_NETCONN_FULLDUPLEX=0`, `LWIP_NETCONN_SEM_PER_THREAD=0` |
| Protocols | `LWIP_IPV4=1`, `LWIP_IPV6=0`, `LWIP_ETHERNET=1`, `LWIP_ARP=1`, `LWIP_ICMP=1`, `LWIP_TCP=1`, `LWIP_UDP=1`, `LWIP_DHCP=1`, `LWIP_DNS=1`, `LWIP_RAW=0`, `LWIP_IGMP=0`, `LWIP_AUTOIP=0`, `IP_FORWARD=0`, `IP_REASSEMBLY=0`, `IP_FRAG=0`, `ETHARP_SUPPORT_VLAN=0` |
| Heap/pbuf | `MEM_ALIGNMENT=4`, `MEM_SIZE=1048576`, `MEM_LIBC_MALLOC=0`, `MEMP_MEM_MALLOC=0`, `PBUF_POOL_SIZE=128`, `PBUF_POOL_BUFSIZE=1536`, `MEMP_NUM_PBUF=128` |
| Protocol pools | `MEMP_NUM_TCP_PCB=40`, `MEMP_NUM_TCP_PCB_LISTEN=8`, `MEMP_NUM_TCP_SEG=256`, `MEMP_NUM_UDP_PCB=20`, `MEMP_NUM_NETCONN=48`, `MEMP_NUM_NETBUF=64`, `MEMP_NUM_SYS_TIMEOUT=32` |
| Core messages | `MEMP_NUM_TCPIP_MSG_API=64`, `MEMP_NUM_TCPIP_MSG_INPKT=64`, `TCPIP_MBOX_SIZE=64`, `DEFAULT_TCP_RECVMBOX_SIZE=8`, `DEFAULT_UDP_RECVMBOX_SIZE=8`, `DEFAULT_ACCEPTMBOX_SIZE=4` |
| TCP | `TCP_MSS=1460`, `TCP_WND=11680`, `TCP_SND_BUF=11680`, `TCP_SND_QUEUELEN=32`, `TCP_LISTEN_BACKLOG=1`, `LWIP_WND_SCALE=0`, `LWIP_TCP_KEEPALIVE=0` |
| Adapter/close policy | `SO_REUSE=1`, `SO_REUSE_RXTOALL=0`, `IP_SOF_BROADCAST=1`, `IP_SOF_BROADCAST_RECV=0`, `LWIP_TCP_CLOSE_TIMEOUT_MS_DEFAULT=1000`, `LWIP_SO_SNDTIMEO=0`, `LWIP_SO_RCVTIMEO=0`, `LWIP_SO_LINGER=0` (public timeouts are adapter waits, not blocking netconn waits) |
| ARP/DHCP/DNS | `ARP_TABLE_SIZE=16`, `ARP_QUEUEING=1`, `ARP_QUEUE_LEN=2`, `MEMP_NUM_ARP_QUEUE=32`, `LWIP_DHCP_DOES_ACD_CHECK=1`, `DNS_TABLE_SIZE=8`, `DNS_MAX_NAME_LENGTH=256`, `DNS_MAX_SERVERS=2`, `LWIP_DNS_SECURE=7`, `LWIP_DHCP_MAX_DNS_SERVERS=2`, `LWIP_DHCP_GET_NTP_SRV=0` |
| Loopback/status | `LWIP_NETIF_LOOPBACK=1`, `LWIP_HAVE_LOOPIF=1`, `LWIP_LOOPBACK_MAX_PBUFS=16`, `LWIP_NETIF_STATUS_CALLBACK=1`, `LWIP_NETIF_LINK_CALLBACK=1`, `LWIP_STATS=1`, `LWIP_STATS_DISPLAY=0` |
| Checksums | All IPv4/UDP/TCP/ICMP `CHECKSUM_GEN_*` and `CHECKSUM_CHECK_*` options MUST be 1. Driver checksum offload MUST remain disabled. |

These choices enable client TCP/UDP and LAN broadcast with bounded queues.
**[F6]** Fragmented incoming IPv4 datagrams MUST be dropped/counted; UDP payloads
over 1472 bytes MUST fail EMSGSIZE without partial transmission. IPv4 multicast
membership, raw sockets and IPv6 MUST fail explicitly. These limitations MUST
be visible in SDK documentation; passing Quake is not general IP compatibility.
The port MUST provide `LWIP_RAND` with a reviewed, per-boot-seeded generator for
DNS, DHCP and initial transport randomization. It MUST NOT hard-code a seed or
claim cryptographic security from PIT/TSC/MAC alone. Entropy provision remains
an integration question for the lead and a prerequisite for an Internet-facing
security claim; reproducible seeds MAY exist only in isolated runtime tests.

### Admission at 128 MiB

**[F6]** Network-owned RAM MUST have a hard **4 MiB aggregate ceiling**, including
static BSS, retained pool/allocator pages, page rounding, kernel stacks,
descriptors, copied requests, packet data and quarantine. This is a proposed
admission ceiling, not a measurement of free memory. Installed 128 MiB is not
proof that this commitment fits.

| Ledger bucket | Maximum KiB |
| --- | ---: |
| lwIP heap including allocator/alignment overhead | 1028 |
| lwIP fixed pools, pbufs, PCBs, segments and timer/message metadata | 508 |
| Physical NIC DMA, at most 128 KiB per card, two cards | 256 |
| Shared copied Ethernet packet pool and queues | 256 |
| `sys_arch`, socket objects, mailboxes and request/control metadata | 256 |
| Network task stacks/task/FPU metadata, at most eight tasks | 288 |
| Copied socket I/O scratch, at most sixteen 32 KiB operations | 512 |
| Virtual NE2000 RAM and DOS bridge state/queues, at most two VMs | 256 |
| Remaining rounding, implementation overhead and reserve | 736 |
| **Total** | **4096** |

**[F6]** The compiler/linker map and `sizeof`-based host accounting MUST prove
that the configured pools fit their bucket; the table MUST NOT be assumed exact
for an unknown port. Non-network emergency reservations MUST stay intact.
Before activation, the lead MUST record installed, E820 usable/reserved, current
PMM/heap/stack commitments and free pages on the 128 MiB profile and supported
hardware. Admission MUST fail cleanly if commitments cannot fit. Increasing
any memory limit requires measured evidence and lead/owner review under the
existing memory contract; this draft authorizes no such increase.

**[F6]** Limits MUST be two attached physical NICs, one selected default route,
48 public socket objects globally, 16 distinct socket objects per process,
at most 32 TCP objects (including listeners), eight listeners, 16 UDP objects,
and four outstanding DNS requests. Duplicated fds MUST reference one socket;
all descriptors still count against 128 per process and 1024 descriptions
globally. Internal DHCP/DNS UDP PCBs MUST have reserved capacity outside the
16 public UDP objects. Closing/TIME_WAIT PCBs MUST count against the fixed TCP
pool; exhaustion MUST NOT evict a live public connection to satisfy a new one.

**[F6]** Queue/pool exhaustion MUST drop new RX frames with counters, return
EAGAIN/ENOBUFS/ENOMEM for uncommitted user operations as specified below, or
backpressure TCP. It MUST NOT grow queues, allocate in IRQ context, reuse
DMA-owned buffers or panic. Ten thousand create/close cycles, cancellation and
link resets MUST return the ledger to a stable baseline, allowing only bounded
documented TIME_WAIT and retained allocator pools. Quarantine MUST retain its
charged pages and prevent repeated activation from exhausting RAM.

## Kernel Ethernet boundary and driver compatibility

### Frame and `netif_driver` interface

**[F6-01]** `struct netif_driver` MUST identify driver name, copied PCI identity,
registry claim generations, MAC, MTU, carrier capability/state, operations and
statistics. The shared interface MUST be hardware-neutral and MUST NOT include
lwIP types. Driver-private register I/O MUST be injectable in host tests.

| Operation | Required ownership/context |
| --- | --- |
| `init(pci, resources)` | Thread context; validate variant/BAR/routing, acquire bounded resources, read MAC and prepare disabled engines. Publish no usable interface on failure. |
| `start()` | Thread context; publish rings and handler before unmasking/start. Reject stale/quarantined ownership. |
| `stop(deadline)` | Thread context; block submissions, disable engines/sources, drain callbacks/completions and prove idle; otherwise quarantine. |
| `transmit(frame, length, token)` | Nonblocking worker call; return 0 only after copying into driver-owned TX storage. Caller retains its frame on both success and error. A full ring returns EAGAIN; no completion is promised for an unaccepted token. |
| `receive_poll(max_frames, deadline)` | Worker call; validate hardware status/length and move copied frames into the bounded shared RX queue; return work remaining/errors. Never invoke userspace or lwIP. |
| `tx_reap(max_completions)` | Worker call; report each accepted token exactly once as sent/error, then reclaim its storage. Wire completion is distinct from socket send success. |
| `link_state()` / `get_mac()` / `statistics()` | Coherent snapshots, including UNKNOWN carrier when hardware cannot report it; no reset or implicit enabling. |

**[F6]** Frame buffers MUST be kernel-owned contiguous copies with a maximum
1536-byte storage slot and length 14–1514 excluding FCS. Incoming runts MUST be
rejected according to device status; emulated short ARP frames MAY be normalized
only in the documented driver path. TX MUST zero-pad to 60 bytes before hardware
adds FCS. No VLAN, jumbo frame, unvalidated length or descriptor-supplied host
pointer MUST cross the boundary. A packet MUST carry interface and generation,
length and ownership state, with no user/guest pointer. The shared packet pool
MUST have 128 slots total; each physical RX queue MUST hold at most 32 and each
TX queue at most 16. VM queue limits below share this pool, not add another 128.

**[F6-02]** A network worker MUST demultiplex frames before IPv4 ingress: host
ARP/IPv4 to lwIP, approved guest IPX to the DOS bridge, everything else dropped
with reason counters. It MUST allocate `PBUF_POOL`, copy a frame, free its shared
slot and call `tcpip_input`. Success transfers the pbuf to lwIP; failure leaves
the worker responsible for `pbuf_free`. TX `netif.linkoutput` MUST copy a complete
pbuf chain into an available TX slot before returning `ERR_OK`; it MUST NOT
retain a borrowed chain or wait for NIC completion. Exhaustion returns `ERR_MEM`
without partial enqueue. Drivers MUST NEVER DMA directly into a userspace or
guest buffer. Optional zero-copy is deferred until separately reviewed.

Ingress destination checks MUST accept host unicast/broadcast ARP/IPv4 only;
enabling promiscuous receive for VM MACs MUST NOT feed unrelated unicast traffic
to lwIP. Guest IPX classification MUST happen independently of host IP readiness.

**[F6-02]** Each lwIP Ethernet netif MUST have MTU 1500, six-byte MAC,
`etharp_output`, the copied `linkoutput`, Ethernet/ARP/broadcast flags and
distinct administrative/link states. `netif_set_link_up/down`, address/default
route changes and DHCP MUST execute on the core thread. Loopback 127.0.0.1 MUST
work with no physical adapter. Two adapters MUST NOT enable forwarding or
automatically bridge their physical ports.

### Registry, interrupt and DMA ownership

**[F6-01]** Probe MUST use `pci_count/pci_get`, Ethernet class and a vendor/device/
revision allowlist. A device's presence is not an ownership grant. Validated
BAR extent, overflow, alignment, decode, IRQ routing and idle firmware handoff
MUST precede device-register writes. CF8/CFC accesses MUST stay serialized by
the PCI service. BAR-sizing writes to an active/firmware-owned function are
forbidden; documented extents MUST be checked against registry overlap and
variant knowledge. Unknown routing/variant MUST leave the device disabled with
a reason. Polling MAY be used only for a documented, separately qualified path.

**[F6-01]** Register windows and shared INTx MUST be claimed and activated
through generation-checked registry handles. Device contexts, rings and IRQ
handlers MUST be published before the source is unmasked. Shared callbacks
MUST inspect/ack only their source, return handled/not-mine and retain all peers;
the PIC service alone sends EOI. IRQ work SHOULD finish within 50 microseconds
and MUST fit the aggregate 250-microsecond chain budget. Expensive receive,
recovery and PHY work MUST be deferred. Source masking/re-enable MUST recheck
pending state so a completion between clear and unmask cannot be lost.

**[F6-01]** The compatibility DMA allocator MUST accept device address mask,
alignment, segment count/length, boundary and direction. Initial bus-master
drivers MUST declare 32-bit addresses, little-endian descriptors and their
datasheet alignment. Contiguous buffers MUST be physically contiguous, not
just virtually contiguous. It MUST prefer NORMAL RAM when the declared mask
allows it, preserve LOW/ISA reservations, check every mapped segment and track
ownership/sync barriers. On coherent x86, compiler/CPU ordering at descriptor
publication/completion remains required. PCI command bus mastering MUST stay
off until all descriptors and claims are valid. NE2000 PIO MUST keep bus
mastering disabled. `RES_DMA` MUST NOT be misused as an ISA-channel claim for
PCI memory; until a dedicated RAM resource kind is added, use the AC97
`RES_MMIO` RAM-range tracking convention plus a separate DMA ledger.

**[F6-01]** Stop MUST drain accepted TX tokens with final status, synchronize
`irq_chain_remove`, fence the engine, prove no DMA, then quiesce/release claims
and free pages. A timeout is not proof. Failed stop MUST retain pinned pages,
quarantine claims and invalidate the interface generation; late RX/TX/timer
callbacks MUST be discarded safely. Neither reset/reprobe nor configuration
changes MAY reuse quarantined resources. Safe mode MUST leave physical NICs
inactive. No firmware network/PXE calls MAY run after native takeover.

### Minimal wired BSD source compatibility

**[F6]** The layer MUST expose the following semantics for the first wired
drivers; a BSD port MAY request additional symbols only through a reviewed
dependency list. It MUST NOT advertise a complete FreeBSD kernel API, binary
driver loading, Linux NDIS wrappers, FreeBSD sockets or a second IP stack.

| Surface | Required subset/mapping |
| --- | --- |
| Device/PCI | Probe/attach/detach, softc, resource allocation/release, PCI identity and serialized config read/write; match only registered allowlisted functions. |
| Bus space | Checked map/unmap, read/write 1/2/4, multi-region access and barriers over claimed I/O/MMIO; handle generation checked; no arbitrary physical map. |
| Bus DMA | Tags/maps, bounded alloc/free/load/unload/sync with the constraints above; no user mbuf DMA, bounce buffer implicit growth or ISA restrictions applied to every PCI device. |
| Interrupts/locks | Registry INTx registration/teardown, device-only acknowledgement, short critical sections, sleeping mutexes and worker taskqueues; callbacks cannot own the physical PIC. |
| Timers | Monotonic callout init/reset/stop/drain, generation cancellation and bounded worker dispatch; teardown waits for running callbacks before freeing softc. |
| mbufs/queues | Bounded packet-header/cluster allocation, chain copy/free, length checks, enqueue/dequeue and Ethernet input/output adaptation to the shared packet pool. `if_input` consumes its chain exactly once. |
| Ethernet/MII | MAC/MTU/flags/statistics, multicast filter programming needed for qualified traffic, link/media query, MII/EEPROM helpers with deadlines; no net80211 or hardware-specific inference from a PCI class alone. |

**[F6]** No-op lock, DMA-sync, drain or resource stubs MUST NOT stand in for
required semantics. Unused unsupported entry points MUST fail explicitly.
f6-01 MUST implement the common services its clean drivers consume. A later
BSD driver import MUST adapt its Ethernet edge to this boundary and meet the
same host/register-model tests; enabling unsupported offload flags is forbidden.

### Initial controller targets

**[F6]** The four families below are F6 targets, not already-qualified devices.
An implementation MUST list its exact supported PCI IDs/revisions; marketing
names and a QEMU twin MUST NOT imply all derivatives work.

| Family / driver identity | Required initial implementation and evidence |
| --- | --- |
| Intel 8255x, `e100` (FreeBSD source equivalent `fxp`) | f6-01 MUST implement SCB, CU/RU start/resume/abort, EEPROM MAC and MDI/MII link. Start with 32 RX and 16 TX command slots, at most 1536 bytes per payload and 128 KiB total DMA. Validate completion/link pointers and RU no-resource recovery. Initial candidates include `8086:1229`, QEMU `8086:1209` (`i82559er`) and the T23 PRO/100 VE/ICH variant. The T23's actual PCI/subsystem/revision/PHY MUST be inventoried before its allowlist/initialization is frozen; QEMU's 82559ER is not its hardware twin in every detail. |
| Realtek classic RTL8139, `rtl8139` (BSD `rl`) | f6-01 MUST support `10ec:8139` with a 32 KiB RX ring plus the documented padding/wrap safety allocation and four 1536-byte TX buffers. RX header/FCS length and CAPR advancement MUST be checked; reset/overflow and lost-link behavior MUST be modeled. Variant detection MUST keep C+ mode off. QEMU twin: `rtl8139`. |
| NE2000 PCI/RTL8029, `ne2k` (BSD `ed`) | f6-01 MUST initially support `10ec:8029`, PROM MAC, page switching, 16-bit remote-DMA data port, TX completion and bounded RDC waits. Device-local 16 KiB packet RAM is not PMM/DMA RAM. RX next-page/count/wrap and BNRY/CURR separation MUST be checked; OVW MUST use the documented stop/recover sequence. Odd-length transfers MUST not read beyond a caller's buffer. Carrier MAY be UNKNOWN where no qualified PHY link bit exists. QEMU twin: `ne2k_pci`. |
| 3Com 3c905, `3c905` (BSD `xl`) | A later assigned driver directive MUST cover 3c905/905B/905C separately, windowed registers, EEPROM/PHY and upload/download descriptors with <=32 RX/16 TX slots and <=128 KiB DMA. `10b7:9050/9055/9200` are candidates, not a blanket allowlist. A pinned BSD import or manufacturer programming reference and register models MUST precede code. No 3c905 QEMU twin is required or claimed; host plus physical qualification is necessary. |

**[F6-01]** Every activated interface MUST emit a bounded English diagnostic
record `[net] driver=<name> mac=<six-octets> link=<up|down|unknown> irq=<n>`
with a separate record for PCI identity, claims/generation, ring sizes and
failure reason. `[net] arp=ok mac=<peer-mac>` MUST mean a matching ARP reply
was validated, not merely that TX completed. Counters MUST distinguish RX/TX
bytes/frames, malformed/status errors, full queues, timeouts, IRQ budget faults
and quarantine. Payloads MUST NOT be logged by default.

## Native sockets, readiness and resolver ABI

### SDK and descriptor semantics

**[F6-02]** SDK headers MUST provide `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`,
`netdb.h`, `poll.h` and `sys/select.h` with the supported subset below.
`socklen_t`, `nfds_t` MUST be u32; socket fds are i32. Network byte order applies
only to ports/addresses; CiukiOS record integers remain little-endian ILP32.
Public constants MUST be owned by the SDK and translated, even when lwIP uses
the same value. The public API MUST NOT expose PCBs, mbufs, netconns or pointers
to kernel memory. This is source compatibility, not POSIX option-group
conformance. The new SDK MUST NOT undo F2's conformance-macro exclusions.

| Interface | Required F6 behavior / lwIP mapping |
| --- | --- |
| `socket` | AF_INET SOCK_STREAM/TCP and SOCK_DGRAM/UDP only; protocol 0 selects the matching protocol. Type MAY include SOCK_NONBLOCK/SOCK_CLOEXEC, applied atomically to the new O_RDWR description/fd. Otherwise both flags are clear. Map to `netconn_new_with_callback`. Other family/type/protocol combinations fail distinctly. |
| `bind`, `connect` | Translate copied IPv4 address; TCP and UDP connect, TCP nonblocking completion; bind only local/ANY address, enforce port conflict. Map to netconn bind/connect. UDP disconnect via AF_UNSPEC is supported. |
| `listen`, `accept` | TCP only; backlog 1–4 (larger positive requests clamp to 4); accept returns a new fd and optional peer. New fd is blocking and CLOEXEC clear, regardless of listener NONBLOCK. No boot-started listener. |
| `send/recv`, `sendto/recvfrom` | TCP partial bytes and UDP datagram semantics; copied kernel buffers; netconn write-partly, send/sendto, receive and retained TCP remainder. Receive MUST not acknowledge discarded TCP bytes as read. |
| `shutdown`, `getsockname/getpeername` | SHUT_RD/WR/RDWR, TCP FIN and EOF semantics, coherent copied local/peer address; netconn shutdown/getaddr. UDP shutdown MAY fail EOPNOTSUPP. |
| `getsockopt/setsockopt` | SOL_SOCKET SO_ERROR/SO_TYPE read-only; SO_REUSEADDR, SO_BROADCAST, SO_RCVTIMEO/SO_SNDTIMEO; IPPROTO_TCP TCP_NODELAY. Exact subset and values below; no arbitrary ioctl. |
| `close`, `dup/dup2`, `fcntl`, `read/write` | Existing fds/calls MUST operate on sockets. Status NONBLOCK is shared by duplicate descriptions; CLOEXEC is per fd. `read/write` map to recv/send with flags zero. Explicit spawn inheritance works as in F2. |
| `poll`, `select` | Kernel poll over common fd readiness; SDK select translates fd sets to one poll request, not one call per socket. This MUST cover supported mixed descriptors. |
| `getaddrinfo/freeaddrinfo/gai_strerror`, `inet_pton/ntop`, byte-order functions | SDK-owned records and allocations; bounded kernel DNS A lookup, numeric and passive address handling in libc. No host libc resolver pointers cross into the kernel. |

**[F6-02]** All live public netconns MUST stay nonblocking internally. Each
netconn MUST have exactly one sequential-API owner at a time: the socket worker
for normal operations, then the teardown worker after commands and callers
drain. No concurrent sequential call on one netconn is allowed. The command/
notification path MUST retain stable object references and generations.
Netconn callback notifications MUST update readiness and wake predicates
without sleeping or calling user code.
The socket object MUST represent connected/connecting/listening, retained RX
data, EOF, pending error and close explicitly. The worker MUST preserve accepted
netconn notifications that arrive before public fd installation. This avoids
depending on concurrent calls to one netconn or the experimental full-duplex
setting described by [lwIP](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts__netconn.html).

**[F6-02]** In-flight syscalls MUST copy input and retain the socket object;
close in another thread MUST invalidate the fd immediately, wake affected waits
and prevent their operation on a replacement fd with the same number. Existing
operation references MAY finish already committed I/O; uncommitted operations
MUST return EBADF after that fd generation is closed. Last close MUST queue
bounded worker cleanup, discard unread data safely and release the object only
after callbacks/commands drain. It MUST return 0 without waiting for a remote
FIN and MUST defer caught signals as existing close does. Transport cleanup
MUST use the one-second per-connection close limit; an isolated close SHOULD
reclaim its netconn within two seconds, excluding charged TIME_WAIT. Burst
cleanup MUST stay within the fixed object/queue limits and keep live traffic
runnable. Failure MUST be visible and must not free referenced memory.
Process/thread termination MUST cancel requests, unpin outputs and retain
storage until late completion drains.

Close/shutdown control and cleanup queue entries MUST be reserved with each
socket object at creation, so these valid operations cannot fail allocation or
queue admission after the fd/direction state changes. Repeated shutdown requests
MUST coalesce by direction. No new object MAY consume that reserved capacity.

**[F6-02]** `O_NONBLOCK`, `MSG_DONTWAIT` and `F_SETFL` MUST select EAGAIN for
empty receive/accept or no send space. Nonblocking TCP connect MUST return
EINPROGRESS, then signal poll writability/error and expose its result through
SO_ERROR; repeated pending connect returns EALREADY. Blocking connect interrupted
before completion returns EINTR, leaves its state queryable and MUST NOT be
silently restarted. SO_ERROR MUST return and clear the pending socket error,
translated to positive SDK errno. A later error MUST not erase bytes already
accepted. A send after write shutdown MUST return EPIPE and raise SIGPIPE under
the existing F2 policy unless MSG_NOSIGNAL is supplied.

**[F6-02]** TCP I/O MUST accept <=32768 requested bytes per operation and MAY
return fewer; larger requests fail EINVAL. UDP sends MUST be all-or-error and
<=1472 bytes; UDP receive truncation MUST discard the remainder of that datagram
and return the copied count. A zero-length UDP datagram MUST be transmitted
and consumed as a datagram, not mistaken for TCP EOF. `MSG_PEEK` MUST retain
queued data; flags outside DONTWAIT/PEEK/NOSIGNAL and direction-appropriate
combinations MUST return EINVAL. MSG_WAITALL, ancillary data/sendmsg/recvmsg,
out-of-band data, socketpair/AF_UNIX, multicast and ioctl are deferred.

TCP receive MUST use `NETCONN_DONTBLOCK | NETCONN_NOAUTORCVD`, retain an unread
pbuf tail and call `netconn_tcp_recvd` only for bytes committed to the caller.
PEEK MUST neither consume bytes nor advance the advertised receive window.
Zero-length TCP read/write MUST return 0 without consuming stream data; an
ordinary TCP read MUST distinguish that case from peer EOF. Send MUST use
`NETCONN_COPY` and report the actual write-partly count before any later error.
UDP netbuf data MUST be copied/owned through send completion, then freed once.
No user's scratch allocation MAY be borrowed by a later retransmission.

### Frozen extension proposal: syscalls 75–87

**[F6]** This table reserves the next contiguous numbers for lead approval;
it MUST NOT renumber 0–74 or the 6–15 reservations. f6-02 MUST add the approved
records/constants to `ciuki/abi.h` and update SDK generation/ABI layout tests.
Until implemented the calls MUST return ENOSYS. `B,C,D,S,I,P` mean
EBX, ECX, EDX, ESI, EDI, EBP as in the existing ABI; unused registers MUST be
zero. `N` means nonblocking kernel operation, `I` interruptible user wait.

| No. | Call / registers | Result |
| --- | --- | --- |
| 75 | `socket(B=domain,C=type,D=protocol)` | New lowest available fd; N. |
| 76 | `bind(B=fd,C=addr*,D=addrlen)` | 0; N. |
| 77 | `connect(B=fd,C=addr*,D=addrlen)` | 0 or pending/error; I. |
| 78 | `listen(B=fd,C=backlog)` | 0; N. |
| 79 | `accept(B=fd,C=addr*,D=addrlen*)` | New fd; I. Both output pointers MAY be null together. |
| 80 | `sendto(B=io*)` | Accepted byte count; I. Also implements send with no destination. |
| 81 | `recvfrom(B=io*)` | Copied byte count or TCP EOF 0; I. Also implements recv with no address output. |
| 82 | `shutdown(B=fd,C=how)` | 0; N. |
| 83 | `sockname(B=fd,C=which,D=addr*,S=addrlen*)` | 0; N. which=0 local, 1 peer. |
| 84 | `sockopt(B=option*)` | 0; N. operation=0 get, 1 set. |
| 85 | `poll(B=records*,C=count,D=timeout_ms)` | Number of records with nonzero revents; I. Signed timeout: -1 forever, 0 query, >0 monotonic milliseconds. |
| 86 | `resolve4(B=request*)` | 0 with DNS result/status; I. libc implements getaddrinfo around it. |
| 87 | `netctl(B=control*)` | 0 with copied snapshot/accepted configuration operation; N. f6-03 owns control implementation. |

**[F6]** `sa_family_t` MUST be u16. `sockaddr` MUST be 16 bytes (family@0,
data[14]@2); `sockaddr_in` MUST be 16 bytes (family@0, port@2, address@4,
zero[8]@8). Family is host order; port/address are network order. The input
length MUST be 16 and padding MUST be zero; UDP AF_UNSPEC disconnect MUST use
the same zero-filled layout. Output capacity in `socklen_t*` MAY be 0–65535;
the kernel MUST copy min(capacity,16) bytes and return length 16, after validating
the entire written range. Accepted fd reservation/output validation MUST precede
dequeue; pointer faults MUST NOT consume a pending connection or datagram.

**[F6]** Every extension record below MUST use four-byte alignment, version=1,
exact size and zero reserved fields. Addresses in records are u32 user virtual
addresses, never kernel/native pointers. The record itself and input buffers
MUST be copied once; writable output ranges MUST be validated/pinned before
committing receive/dequeue, then unpinned on every exit. No user pointer MAY be
posted to lwIP or retained after its request is cancelled. Call limits MUST be
checked before arithmetic/allocation. Address-length pointers are u32.

| Record | Layout (byte offsets, total bytes) |
| --- | --- |
| `ciuki_socket_io` | size@0, version@4, fd:i32@8, buffer@12, bytes@16, flags@20, address@24, address_length@28, reserved[2]@32; **40**. sendto address_length is an input value; recvfrom it is an optional user `socklen_t*`. Null address requires zero address_length. |
| `ciuki_socket_option` | size@0, version@4, fd:i32@8, operation@12, level:i32@16, name:i32@20, value@24, length@28, reserved[2]@32; **40**. Set length is bytes; get length is a user `socklen_t*`. |
| `pollfd` | fd:i32@0, events:i16@4, revents:i16@6; **8**. count 0–128; no recursive pointers. |
| `ciuki_resolve4` | size@0, version@4, name@8, name_bytes@12, address@16, status:i32@20, reserved[2]@24; **32**. name_bytes includes NUL and is 2–254. Address/status are inline outputs; exactly one IPv4 A result is promised. |
| `ciuki_netctl` | size@0, version@4, operation@8, interface_id@12, authority_fd:i32@16, payload@20, payload_bytes@24, reserved@28; **32**. Operations and payloads below. |

**[F6]** Public values MUST be:

```text
AF_UNSPEC=0 AF_INET=2 SOCK_STREAM=1 SOCK_DGRAM=2
SOCK_NONBLOCK=0x800 SOCK_CLOEXEC=0x80000 IPPROTO_IP=0 IPPROTO_TCP=6 IPPROTO_UDP=17
SHUT_RD=0 SHUT_WR=1 SHUT_RDWR=2 MSG_PEEK=2 MSG_DONTWAIT=0x40 MSG_NOSIGNAL=0x4000
SOL_SOCKET=1 SO_REUSEADDR=2 SO_TYPE=3 SO_ERROR=4 SO_BROADCAST=6
SO_RCVTIMEO=20 SO_SNDTIMEO=21 TCP_NODELAY=1
POLLIN=1 POLLOUT=4 POLLERR=8 POLLHUP=16 POLLNVAL=32 FD_SETSIZE=128
```

SO_REUSEADDR/SO_BROADCAST/TCP_NODELAY MUST take a four-byte i32 0/1;
SO_ERROR/SO_TYPE MUST return i32. REUSEADDR MUST NOT allow two live TCP
listeners on the identical address/port or SO_REUSEPORT load distribution.
Timeout options MUST use a 16-byte `timeval`: i64 seconds@0, i32 microseconds@8,
zero@12; nonnegative, microseconds <1000000. Zero disables the timeout; other
values MUST round up to milliseconds and reject overflow beyond INT32_MAX ms.
TCP_NODELAY on UDP and unknown options MUST return ENOPROTOOPT. Poll fd sets
MUST be four u32 words; SDK timeval/timespec MUST retain F2's 64-bit time types,
not lwIP's platform-dependent timeval. Timeout expiry on send/recv/accept
returns EAGAIN if no bytes committed; TCP connect uses ETIMEDOUT.

**[F6]** Error numbers MUST extend the existing Linux i386-numbered errno set:
ENOTSOCK=88, EDESTADDRREQ=89, EPROTOTYPE=91, ENOPROTOOPT=92,
EPROTONOSUPPORT=93, ESOCKTNOSUPPORT=94, EAFNOSUPPORT=97, EADDRINUSE=98,
EADDRNOTAVAIL=99, ENETDOWN=100, ENETUNREACH=101, ECONNABORTED=103,
ECONNRESET=104, ENOBUFS=105, EISCONN=106, ENOTCONN=107,
ECONNREFUSED=111, EHOSTUNREACH=113, EALREADY=114, EINPROGRESS=115.
Existing EAGAIN/EWOULDBLOCK, EPIPE, EMSGSIZE, EOPNOTSUPP and ETIMEDOUT
values MUST stay intact. Ordinary wrappers return -1 and set thread errno;
successful calls MUST NOT clear errno. No lwIP `err_t` MAY be returned as an ABI
errno without operation-specific translation.

| Calls | Complete allowed errors (existing fd/pointer checks included) |
| --- | --- |
| socket | EINVAL, EAFNOSUPPORT, ESOCKTNOSUPPORT, EPROTONOSUPPORT, EMFILE, ENFILE, ENOBUFS, ENOMEM |
| bind | EBADF, ENOTSOCK, EFAULT, EINVAL, EAFNOSUPPORT, EADDRINUSE, EADDRNOTAVAIL, ENETDOWN, ENOBUFS, ENOMEM |
| connect | EBADF, ENOTSOCK, EFAULT, EINVAL, EAFNOSUPPORT, EADDRINUSE, EADDRNOTAVAIL, EISCONN, EALREADY, EINPROGRESS, ENETDOWN, ENETUNREACH, EHOSTUNREACH, ECONNREFUSED, ECONNRESET, ECONNABORTED, ETIMEDOUT, EINTR, ENOBUFS, ENOMEM |
| listen | EBADF, ENOTSOCK, EINVAL, EOPNOTSUPP, EADDRINUSE, ENETDOWN, ENOBUFS, ENOMEM |
| accept | EBADF, ENOTSOCK, EFAULT, EINVAL, EOPNOTSUPP, EAGAIN, EINTR, ECONNABORTED, ENETDOWN, EMFILE, ENFILE, ENOBUFS, ENOMEM |
| sendto / socket write | EBADF, ENOTSOCK, EFAULT, EINVAL, EAFNOSUPPORT, EDESTADDRREQ, EISCONN, ENOTCONN, EMSGSIZE, EACCES, EAGAIN, EINTR, EPIPE, ENETDOWN, ENETUNREACH, EHOSTUNREACH, ECONNREFUSED, ECONNRESET, ETIMEDOUT, ENOBUFS, ENOMEM |
| recvfrom / socket read | EBADF, ENOTSOCK, EFAULT, EINVAL, ENOTCONN, EAGAIN, EINTR, ENETDOWN, ECONNREFUSED, ECONNRESET, ETIMEDOUT, ENOBUFS, ENOMEM |
| shutdown | EBADF, ENOTSOCK, EINVAL, ENOTCONN, EOPNOTSUPP |
| sockname | EBADF, ENOTSOCK, EFAULT, EINVAL, ENOTCONN |
| sockopt | EBADF, ENOTSOCK, EFAULT, EINVAL, ENOPROTOOPT, ENOMEM |
| poll | EFAULT, EINVAL, EINTR, ENOMEM |
| resolve4 | EFAULT, EINVAL, EINTR, ENOMEM; DNS/network outcomes use the result status below |
| netctl | EFAULT, EINVAL, EBADF, EPERM, ENODEV, EOPNOTSUPP, EBUSY, EIO, ENOMEM |

Invalid fd returns EBADF; an open non-socket passed to socket-specific calls
returns ENOTSOCK. Validation order MUST be fixed record/scalars, fd type,
copied address/ranges, reservation, then commit. `close/dup/dup2/fcntl` keep
their existing error sets. F_SETFL on sockets MUST accept NONBLOCK, reject
APPEND with EINVAL and preserve O_RDWR. Fstat on a socket MUST return
EOPNOTSUPP; lseek/pread/pwrite ESPIPE; fsync/ftruncate EINVAL.

### Readiness and name resolution

**[F6-02]** Poll MUST atomically register waiters and recheck readiness,
using fd generations so close/reuse cannot wake for a different object.
Negative fd entries MUST be ignored; invalid nonnegative fds produce POLLNVAL,
not EBADF. ERR/HUP/NVAL MUST be reported even when unrequested. Duplicate
entries count separately. Readable TCP includes EOF; buffered input MUST remain
readable after FIN. Writability includes completed/failed nonblocking connect;
SO_ERROR disambiguates. UDP zero-length datagrams MUST signal POLLIN.
Timeout with no readiness returns 0 and cleared revents; no-fd requests still
wait for the specified timeout or signal. Signals MUST return EINTR without
silently restarting or extending deadlines. Unknown event bits return EINVAL.

**[F6-02]** Poll MUST support sockets, channels (message/peer close/capacity),
input queues (events), and regular files/directories (immediate read/write
readiness appropriate to access). Audio playback SHOULD expose writable queue
capacity when its device integration is assigned. Non-pollable valid objects
MUST report POLLNVAL and SDK documentation MUST list them; polling MUST NOT
pretend they are sockets. SDK select MUST validate nfds 0–128, translate
requested read/write sets in one call, use an empty exception set (OOB deferred),
return EBADF for invalid fds and clear/rebuild sets on success. Its result MUST
count ready set bits, including a descriptor ready in both sets. A nonempty
exception set MUST fail EOPNOTSUPP. Select MUST preserve the caller's timeout
and round positive submillisecond values up.

**[F6-02]** `getaddrinfo` MUST support AF_INET/AF_UNSPEC, TCP/UDP, numeric dotted
IPv4, numeric service ports 0–65535, `localhost`, null node (ANY with AI_PASSIVE,
loopback otherwise), AI_PASSIVE/AI_NUMERICHOST/AI_NUMERICSERV, and DNS A lookup.
It MUST return at most one IPv4 address, with at most two entries if both socket
types were requested. Service names, AI_CANONNAME/AI_ADDRCONFIG/AI_V4MAPPED,
AAAA, reverse lookup and gethostbyname's static storage are deferred. Unknown
flags MUST return EAI_BADFLAGS; unsupported combinations/family/type/service
MUST return their corresponding EAI code. IDN conversion belongs to a later
browser/library decision; the kernel accepts only bounded ASCII DNS labels.

**[F6-02]** resolve4 MUST return status OK=0, NONAME=1, AGAIN=2, FAIL=3;
only OK makes the inline IPv4 address valid. The SDK MUST map these to success,
EAI_NONAME, EAI_AGAIN, EAI_FAIL; syscall errors map to EAI_SYSTEM with errno,
except ENOMEM to EAI_MEMORY. DNS addresses/ports MUST not be host-order values.
SDK EAI values MUST be BADFLAGS=-1, NONAME=-2, AGAIN=-3, FAIL=-4,
FAMILY=-6, SOCKTYPE=-7, SERVICE=-8, MEMORY=-10, SYSTEM=-11.
freeaddrinfo MUST free only SDK-allocated user records. Cache TTL, compression,
response ID/source/question checks MUST come from the pinned lwIP parser.
No fabricated distinction between NXDOMAIN and transport failure MAY be made
if the upstream callback cannot expose it; a small reviewed status adaptation
or a documented FAIL outcome is required.

**[F6-02]** DNS MUST have two servers, eight cache entries and at most four
outstanding lookups with a ten-second caller deadline. Posted core callbacks
MUST hold copied names and generation-tagged request storage. Cancellation or
timeout MUST not free a request before an uncancellable lwIP callback drains;
those requests continue to count against the limit. No DNS request MAY run
blocking on the socket worker or freeze the desktop. Resolver/server changes
MUST invalidate cached answers and stale completions through a new generation.

## Configuration service and desktop applet

**[F6-03]** The kernel service MUST own link/address/DHCP/DNS/default-route state;
the Control Panel MUST use copied control records, not issue device I/O or call
lwIP. Normal boot MUST select DHCP on the first qualified enabled NIC in stable
PCI BDF order. The second NIC MAY have a static connected subnet but MUST NOT
obtain another default route automatically. Link UNKNOWN MUST be displayed
honestly and MAY attempt DHCP when hardware cannot measure carrier. Loopback
remains available with absent/disabled NICs. Safe mode MUST disable physical
activation and guest bridging regardless of saved settings.

**[F6-03]** DHCP MUST perform DISCOVER/OFFER/REQUEST/ACK, lease renewal at T1,
rebinding at T2 and expiry/NAK removal using the pinned stack. Options MUST be
length-checked; only address, contiguous subnet mask, one selected router,
up to two DNS servers and lease timing enter configuration. The client MUST
use ARP address conflict detection/DECLINE, never install an unvalidated offer.
If no lease arrives within 30 seconds the UI MUST show "No address" while
bounded background retries continue. No automatic static fallback, random
169.254 address or public DNS server MUST be substituted.

**[F6-03]** Static mode MUST require a valid unicast IPv4 address and contiguous
prefix /1–/30, reject network/broadcast addresses, validate an optional gateway
on the selected subnet and at most two unicast DNS servers, and perform ARP
conflict detection before installing it. Off-subnet gateway, duplicate address,
invalid mask and conflicting connected routes MUST fail visibly. No gateway
and no DNS is valid for numeric-address LAN play. Ethernet bridging of IPX
MUST work independently of a host IPv4 address. DHCP and static state MUST
never be active together on one interface. Link loss MUST revoke usable route/
address status, wake affected waits with network errors and restart acquisition
on link recovery; no live TCP session preservation is promised.

### Control ABI and persistence

**[F6-03]** netctl operations MUST be STATUS=0, APPLY=1, RENEW=2, DISABLE=3.
STATUS is public read-only with authority_fd=-1. Mutations MUST require a
generation-checked network authority descriptor explicitly delegated by the
supervisor to the desktop/applet; possession of a guessed fd or membership in
a process group is insufficient. The authority object MUST be non-inheritable
by default and obey explicit delegation as the existing mixer/task authorities
do. No new root/user permission model is introduced. The desktop MUST use its
existing channel to relay/coalesce status generation changes; poll on that
channel supplies notifications without an additional networking syscall.

| Payload | Proposed frozen v1 layout/meaning |
| --- | --- |
| STATUS, 96 bytes | size@0, version@4, generation@8, interface_id@12, flags@16, driver[16]@20, mac[6]@36, zero[2]@42, pci_vendor:u16@44, pci_device:u16@46, address@48, mask@52, gateway@56, dns[2]@60, lease_remaining_seconds@68, reason@72, zero@76, rx_frames:u64@80, tx_frames:u64@88. Strings MUST be NUL terminated and zero filled; IP fields network order, counters four-byte aligned. |
| APPLY, 48 bytes | size@0, version@4, expected_generation@8, mode@12, address@16, mask@20, gateway@24, dns[2]@28, default_route@36, reserved[2]@40. mode=0 disabled, 1 DHCP, 2 static; default_route=0/1; DHCP/disabled require zero static fields. |
| RENEW/DISABLE | No payload (payload=payload_bytes=0); RENEW is valid only for DHCP. |

STATUS interface_id=0 MUST query the selected default adapter or return ENODEV;
IDs 1/2 MUST identify attached cards in BDF order for that boot. IDs MUST NOT be
persisted across boots; settings MUST use the recorded MAC plus PCI identity.
Flags MUST be ENABLED=1, CARRIER_UP=2, CARRIER_KNOWN=4, ADDRESS_READY=8,
DHCP=16, DEFAULT_ROUTE=32, QUARANTINED=64, CONFIGURING=128. Reasons MUST be
NONE=0, NO_LINK=1, NO_LEASE=2, ADDRESS_CONFLICT=3, INVALID_CONFIG=4,
DRIVER_FAULT=5, NO_MEMORY=6. APPLY with stale expected_generation MUST return
EBUSY before changing state. Accepted APPLY/RENEW MUST enqueue configuration
work and return promptly, not wait for a lease; the result appears in STATUS.

**[F6-03]** The native applet MUST persist versioned desired settings in
`C:/SYSTEM/NETWORK.CFG`, <=4 KiB, using existing VFS atomic rename/durability
rules. It MUST distinguish saved desired settings, pending acquisition and
effective settings; a failed save MUST not be shown as a saved change.
The parser MUST reject duplicate/unknown fields, bad identities/addresses and
overlong records. Corruption MUST fall back to DHCP defaults with a visible
reason. Leases, cached DNS, authority descriptors and QEMU test configuration
MUST NOT be persisted as user settings. Exact file grammar and desktop message
IDs MUST be frozen in f6-03 before implementation, alongside parser tests.

### User interface

**[F6-03]** Control Panel "Network" MUST show adapter/driver/MAC, carrier,
DHCP/static mode, effective IPv4/mask/gateway/DNS, lease time and failure reason;
offer Apply, Renew and Disable; and show validation errors beside the relevant
field. Device Manager MUST distinguish unsupported/discovered, active, disabled
and quarantined adapters. Fields MUST be editable without blocking the GUI
event loop. Slow acquisition MUST use pending status, never a modal wait.

**[F6-03]** The tray MUST distinguish "No adapter", "Disabled", "Cable
unplugged", "Link unknown", "Acquiring address", "Connected (LAN)" and
"Network error". A DHCP lease, ARP response or carrier bit MUST NOT be labelled
"Internet connected". Tooltip/details MUST show effective address and reason;
activity indicators SHOULD coalesce updates to <=4 per second. All copy MUST
remain English. Applet/tray/device icons MUST come from the approved Tango
family; any Ciuki identity icon MUST deterministically derive from the approved
portrait. Existing boot photo, logo and tagline MUST remain unchanged.

## DOS virtual NE2000 and Ethernet/IPX bridge

**[F6-DOS]** Each admitted network-enabled VM MUST receive its own ISA-style
NE2000: virtual ports 0x300–0x31f, virtual IRQ3, 16-bit PIO data port, PROM
MAC and 16 KiB private packet RAM (0x4000–0x7fff). The VM device allocator MUST
reject a virtual port/IRQ collision; these numbers MUST NOT claim host ports,
host IRQ3 or any physical DMA channel. The model MUST implement DP8390 command/
page 0/1/2 registers, reset/PROM, PSTART/PSTOP/BNRY/CURR, remote byte-count/
address/data transfer, TX page/count and ISR/IMR semantics required by the
selected DOS driver. Unsupported registers MUST behave as documented by the
selected model and MUST NOT reach host I/O.

**[F6-DOS]** Guest "remote DMA" MUST copy only between virtual device RAM and
guest PIO accesses, never program the physical PCI/8237 engine. Invalid page,
length, next-pointer or remote-transfer wrap MUST affect only the owning VM.
RX MUST write the ring/header/status before asserting the pending virtual IRQ.
TX MUST copy the complete validated frame into a bounded kernel queue before
completion/error is reported. ISR write-one-clear and IMR, guest IF/CLI and
virtual PIC masking/EOI MUST control delivery; a pending cause MUST remain
pending while the guest suppresses interrupts. Reset/exit MUST invalidate
generation-tagged callbacks without losing host ticks or another VM's events.
DP8390 register behavior MUST be tested against its
[datasheet](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf)
and the [QEMU model](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/ne2000.c);
emulation does not establish physical-card timing.

**[F6-DOS]** There MUST be at most two network-enabled VMs per host, each with
a unique locally administered unicast MAC, stable during that VM's lifetime,
distinct from both physical NICs and other active VM MACs. MACs MUST derive
from a persisted per-host random identifier plus VM identity and collision
checks, not the same fixed bytes on every installation. A guest MUST NOT spoof
another assigned MAC; TX source checks and receive destination filtering MUST
enforce this policy. Broadcast IPX MUST fan out to the other local eligible
VMs and the selected physical port exactly once; receive from that port MUST
fan out only to eligible local VMs. A unicast for a local VM MUST not be sent
back onto the wire. No second physical port, bridge learning loop or IP router
MUST be created.

**[F6-DOS]** The bridge MUST preserve accepted IPX Ethernet frames, including
their guest MACs and selected frame format. Required first qualification is
Ethernet II IPX (0x8137); raw 802.3 and LLC IPX SHOULD be supported only with
explicit discriminator/length tests and a driver/game fixture. The implementation
directive MUST freeze the exact selected format and peer configuration before
the DOOM gate. Unknown protocols MUST be dropped; IPX MUST bypass lwIP and
remain IPX on the switch. Physical hardware MUST filter additional guest MACs
or use a bounded, explicitly enabled promiscuous receive mode with kernel
software filtering; physical host ARP/IPv4 MUST not be duplicated to guests.
Promiscuous mode MUST stop with the last bridge VM.

**[F6-DOS]** Each VM MUST have RX/TX queues of at most eight frames sharing the
global packet pool, with round-robin service and rate limiting to 256 frames
per second (burst 32). Full RX MUST produce virtual overflow/drop state;
failed TX MUST report an error, never grow storage or stop host scheduling.
Bridge disable/link loss/VM destruction MUST drain references safely. Guests
MUST NOT gain native network configuration authority, arbitrary capture or
physical NIC reset capability.

**[F6-DOS]** A qualified NE2000 DOS packet driver MUST support the guest's
INT 60h packet-driver ABI, not a host syscall masquerading as that ABI. Guest
TCP/IP through such a driver MAY later use its own guest stack over a reviewed
Ethernet bridge extension; transparent translation of guest TCP into native
socket calls is deferred. Guest IPX MUST use selected NE2000 ODI/IPX or a
qualified packet-driver/IPX shim. A packet driver alone does not provide IPX.
The lead MUST select exact versions, licensing and configuration of the DOS
driver/IPX stack and IPXSETUP; no proprietary driver or game payload MUST be
published without distribution authority. The required first workload is DOOM
with IPXSETUP, not a compatibility claim for every DOS game or NT NTVDM.

## Security and failure containment for 0.9

**[F6]** A normal 0.9 boot MUST start no inbound application service: no web,
FTP/TFTP, telnet, file share, remote shell, debugger or game server. DHCP client,
ARP/ICMP and replies to user-created sockets are protocol handling, not an
automatic application service. `listen/accept` and UDP bind MUST remain available
for explicitly launched LAN games and tests; process exit MUST close them.
This resolves the LAN/server requirement without a boot-time daemon. UI MUST
not imply a firewall or isolation boundary that is not implemented.

**[F6]** No process or guest MAY obtain raw sockets, NIC register mappings,
physical descriptor pages, arbitrary Ethernet injection/capture or network
configuration authority through ordinary sockets. Broadcast TX MUST require
SO_BROADCAST. Parsers MUST enforce Ethernet/IP/UDP/TCP/DHCP/DNS length and
checksum checks before dispatch. Floods, malformed descriptors, missing TX
completion, stale callbacks and resource exhaustion MUST remain local with
bounded queues, rate-limited diagnostics and worker yields. No received packet
MAY cause an allocation above the ledger ceiling or a kernel assertion through
an untrusted field. Routing, port forwarding, UPnP, IPX-over-UDP tunnelling and
automatic Internet reachability probes MUST stay off.

**[F6]** TLS, certificate stores, trustworthy wall time, cryptographic entropy
and HTTPS validation are not supplied by lwIP sockets. f6-04 MUST specify them
if its selected browser needs them; certificate failures MUST NOT be bypassed
to make a demo pass. Until that port is qualified, a controlled HTTP fixture
MAY demonstrate transport without claiming modern web/TLS support.

## Test architecture and acceptance gates

**[F6]** Tiers MUST run cheapest first and follow the common runner, one canonical
build per source state and runtime fw_cfg selection. Host suites MUST run one
at a time at `nice -n 19`; no QEMU MAY overlap a build, host runner suite or
another QEMU. The lead MUST check available memory and use the established
systemd caps for full builds/QEMU. Codex host work MUST NOT run QEMU or systemd
scopes. F6 MUST NOT add a private build flag, copied base image, second VM or
per-machine image.

### T0: host tests with the real stack

**[F6-02]** The host harness MUST compile the exact pinned lwIP files/options
with a Linux process `sys_arch` port and synthetic Ethernet netif/peer, plus the
CiukiOS adapter under host-test hooks. It MUST NOT substitute the host kernel's
sockets for the stack under test. A host socket MAY implement a peer/fixture.
Virtual time and deterministic injected seeds MAY be used in this harness;
port-specific synchronization MUST also be exercised with real host threads.

| Host group | Required evidence |
| --- | --- |
| Driver/framework | Three register models: EEPROM/PROM/PHY, ring wrap and corrupt lengths/pointers, odd PIO, completion/error/overflow, lost interrupt, absent NIC, shared IRQ with another owner, budget deferral, stop timeout/quarantine, no writes on failed claim. Later 3c905 needs equivalent models. |
| `sys_arch`/frame path | Mailbox FIFO/full/null/timeout, semaphore wake race, timer wrap, cancellation/drain, pbuf copy/ref ownership and chained TX, queue flood, generation stale callback and no ISR allocations. |
| Stack/socket adapter | Real ARP, IPv4 checksums, TCP handshake/partial send/retransmit/FIN/RST, UDP loss/reorder/zero-length/truncation/broadcast, loopback, select/poll race/timeout/mixed fds, nonblocking connect+SO_ERROR, interrupted waits, concurrent close/reuse, spawn/dup lifetime. |
| Malformed input/resources | Bad lengths/checksums/fragments, DHCP/DNS malformed/truncated options and compression, repeated callbacks after exit, 10,000 socket cycles, all pools/queues exhausted, ledger/canaries stable. Host sanitizer checks SHOULD run where feasible, serially. |
| Configuration/DOS | Fake DHCP lease/T1/T2/expiry/NAK/conflict and static/DNS changes; applet parser/copy/authority failures; later NE2000 register/IRQ/CLI/reset and two-MAC bridge fanout/filter/flood/teardown fixtures. |

### T1–T3: one QEMU and a host peer

**[F6]** Static checks MUST verify dependency/license manifest, payload absence,
kernel/SDK layouts and canonical image hash. Existing boot smoke MUST pass
before focused networking. Runner cases MUST specify one of `rtl8139`,
`ne2k_pci`, `i82559er`, exact QEMU/machine/CPU/RAM and fixed unique test MACs;
omitting the device and relying on QEMU defaults is forbidden. At 128 MiB all
three NIC cases MUST use the same canonical image. Existing T23/E500/desktop
CPU/firmware profiles MUST remain part of regression qualification.

**[F6-01]** The `f6-net` suite MUST send/validate ARP against 10.0.2.2 through
`-netdev user,id=net0,ipv6=off` and each explicit `-device <model>,netdev=net0`.
ARP-only success MUST be recorded as driver/frame-path evidence. Ping SHOULD
target the virtual gateway; external ICMP MUST NOT be a mandatory NAT gate.
Absent NIC, backend link down and unsupported ID cases MUST boot normally with
an explicit reason and no socket readiness fabrication.

**[F6-02/03]** User-mode IPv4 cases MUST cover DHCP at the QEMU-provided subnet,
DNS and TCP/UDP to deterministic runner-owned host fixtures, not public websites.
QEMU's guest gateway/host alias is normally 10.0.2.2 and DNS proxy 10.0.2.3;
the runner MUST record the effective backend configuration. A `hostfwd` bound
only to host loopback MAY test guest accept/listen/UDP receive. The runner MUST
allocate/clean up ports, servers, sockets and overlays on success, failure and
interruption; Internet access MUST not be required. Test-specific static settings
MUST be ephemeral runtime configuration, not build flags or saved defaults.

**[F6-DOS]** Ethernet/IPX suites MUST use one QEMU with a runner-owned host frame
peer over a socket backend or an already provisioned TAP. User-mode NAT MUST
NOT be used for IPX. A host peer MUST validate preserved source/destination MAC,
IPX frame format and broadcast/unicast fanout and send matching reply frames.
Socket-backend framing, privilege-free setup/cleanup and timeouts MUST be covered
by host runner tests. TAP configuration MUST never silently modify the host LAN.
Two VMs inside CiukiOS MAY test local bridging; two simultaneous QEMU processes
are forbidden. Host frame tests do not replace the physical two-machine game.

**[F6]** Each result MUST record image SHA-256/revision, QEMU/version/model/backend,
test options, markers, pool/queue high-water, PMM/heap deltas, IRQ maxima and
fixture identity. Passing runs retain only the common result/log evidence;
overlays/captures/peers MUST be cleaned up under existing retention rules.
Payload dumps MUST NOT be routine evidence. No TCG result qualifies laptop PHY,
EEPROM, PCI routing, descriptor ordering or wire-speed timing by itself.

### T4: physical qualification

**[F6]** The lead MUST record exact NIC PCI/subsystem/revision, BAR extents,
IRQ/route/ELCR, PHY, MAC, RAM ledger, switch/link speed and image hash for each
target. T23 PRO/100 VE and the actual E500 network adapter MUST be qualified on
the same image; E500 NIC identity and any needed CardBus card remain unmeasured
here. Unknown devices MUST remain disabled. RTL8139, NE2000 PCI and 3c905 require
representative physical cards before family qualification, or explicit pending
coverage in the matrix. CardBus ownership/bridge support needs its own directive.

**[F6]** Hardware tests MUST include cold boot/restart, cable removal/reinsertion,
DHCP/static/DNS, bidirectional checked TCP/UDP transfers, native-game networking,
shared IRQ where available, concurrent audio/input/disk activity, pool pressure
and shutdown. The lead MUST measure IRQ-disabled maxima and timer gaps against
the existing ownership budgets. A 30-minute native UDP Quake session between
T23 and E500 over a switch MUST run without network-caused crash, corruption,
unbounded growth or loss of host responsiveness. The same machines MUST later
complete a 30-minute DOS DOOM/IPXSETUP session with selected licensed fixtures,
guest CLI/IRQ containment and MAC/bridge evidence. Game data stays local.

### Acceptance by implementation directive

| Gate | MUST be complete before that gate closes |
| --- | --- |
| **f6-01: drivers** | Common registry/packet/IRQ/DMA interface plus Intel 8255x, RTL8139 and NE2000 PCI; register-model/queue/quarantine host tests; i686 `-Werror`; lead ARP evidence on all three QEMU devices and physical T23. Socket/DHCP/UI/DOS success MUST NOT be claimed from ARP. |
| **f6-02: stack and sockets** | Pinned lwIP/import notices, actual kernel `sys_arch`, copied netif path, approved calls 75–86 and SDK subset, common fd readiness/lifecycle, loopback/numeric IPv4 and DNS/DHCP hooks; real-lwIP host harness and ABI/layout tests; lead 128 MiB TCP/UDP/broadcast/nonblocking/poll/limits evidence with deterministic peers for each driver. Native Quake LAN is an F6 requirement; engine-specific port changes need a written assignment. |
| **f6-03: configuration and applet** | Approved call 87/control authority, frozen persistent grammar/desktop messages, DHCP/static/DNS/conflict/lease transitions, Network applet/Device Manager/tray; host state/parser/authority/UI tests and lead QEMU/hardware evidence for link loss, bad configuration, save/reboot and responsive acquisition. |
| **f6-04: browser port** | Lead-selected browser/version/license/backend and exact API dependency list before code; SDK/network port, desktop launch and bounded responsiveness; local HTTP/DNS fixture and failure/offline/cancellation tests on canonical image; measured text/data/heap/stack at 128 MiB. If HTTPS is claimed, qualified TLS library/store/time/entropy, certificate verification and negative tests MUST also pass. This gate MUST NOT claim a full modern browser solely from rendering a local page. |
| **Remaining F6 gates** | Assigned 3c905 implementation and physical coverage; separately assigned virtual NE2000/IPX bridge/guest-driver tests; measured native Quake and DOS DOOM LAN gates between T23/E500. f6-01–04 passing MUST NOT silently retire these owner requirements. |

## Decisions requiring lead cross-review

This draft fixes interfaces sufficiently for review without implementing them.
The lead MUST resolve the following before the dependent coding directive;
hardware evidence MAY remain pending until its qualification tier.

1. Approve/reserve calls 75–87, the netconn worker model, common poll behavior,
   copied records and error sets; update execution-abi/POSIX documents with the
   approved extension when integration is assigned. No ABI header changed here.
2. Validate the proposed 4 MiB ceiling/pool split with real object sizes and
   installed/usable/reserved/committed/free measurements; choose and review the
   per-boot entropy source. No memory measurements are claimed in this draft.
3. Confirm T23 PRO/100 VE PCI/PHY variant, E500 adapter and available 3c905/
   RTL8139/NE2000 cards. Assign 3c905 programming reference/BSD source pin and
   any required CardBus ownership work; f6-01 presently assigns only three drivers.
4. Assign the DOS bridge directive and freeze redistributable packet/ODI/IPX
   drivers, MAC host-identity provisioning, IPX Ethernet format and game fixtures.
   These are required later F6 gates, not features removed from the roadmap.
5. Select browser/version/GUI backend and decide the exact f6-04 HTTPS scope;
   assign any missing select/audio readiness or native Quake API adaptation.
6. Freeze f6-03 persistent grammar, authority delegation and desktop message IDs
   against the native Control Panel/Device Manager work before implementing UI.

The lead owns plan approval, cross-review, integration, QEMU/hardware evidence
and subsequent commits. f6-00 changes documentation only.
