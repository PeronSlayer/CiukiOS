# f6-11 DOS Ethernet/IPX implementation record

Research checked 2026-10-21. This is implementation and host-model evidence;
QEMU, DOS driver qualification and physical LAN qualification belong to the lead.

## Primary sources and decisions

- [National Semiconductor DP8390D/NS32490D, July 1995](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf), sections 7, 9 and 10: private receive ring, boundary/current separation, remote address/count, register pages, W1C ISR and masked IRQ. Remote DMA is a memory copy; no host ports, DMA or IRQ are claimed. Invalid guest geometry fails locally.
- [QEMU NE2000 implementation](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/ne2000.c): duplicated word-wide PROM, 0x57 signature, word DMA alignment, zero-count RDC, reserved-register behavior. The model is a functional register reference; physical timing is not inferred. The emulator additionally rejects transfers outside its private RAM/PROM instead of aliasing host memory. Page 3 is reserved, with no RTL8029 PCI extensions.
- [Novell NetWare 16-bit DOS Protocol Stacks and MLIDs, v1.03, 1996](https://www.novell.com/developer/documentation/lancomp/dos_psta/dos_pstack.pdf): separate protocol/physical boundaries, bounded promiscuous mode and serialized event ownership. This is publicly accessible copyrighted documentation, not a public-domain driver or a redistribution licence for IPX.COM. Its LSL/ODI ECB is not the application's legacy IPX ECB.
- [Novell communications description](https://support.novell.com/techcenter/articles/ana19900903.html): same-segment IPX uses the destination node as the Ethernet destination. The directive freezes network zero, Ethernet II 0x8137, no routing, no raw 802.3/LLC, no UDP tunnel.
- [id Software IPXNET.C](https://github.com/id-Software/DOOM/blob/master/ipx/IPXNET.C), `InitNetwork`, `OpenSocket`, `CloseSocket`, `ListenForPacket`, `GetLocalAddress`, `SendPacket`, `GetPacket`; [IPXNET.H](https://github.com/id-Software/DOOM/blob/master/ipx/IPXNET.H): INT2F/7A00 discovery, far-call entry, BX 0/1/3/4/9/10, big-endian socket word, 30-byte header, 36-byte ECB followed by far fragment descriptors. Snapshot descriptors, pin destinations before writes, gather into a bounded private wire copy, fill the caller's outgoing header, publish completion, then dispatch the guest ESR at the VM callback boundary.
- [id Software IPXSETUP.C](https://github.com/id-Software/DOOM/blob/master/ipx/IPXSETUP.C), `LookForNodes`: socket 0x869c, setup broadcasts with time -1, nodesfound agreement, then addressed gameplay traffic. The harness reconstructs the sequence and field layout; it contains no game executable or game data.

The lead explicitly selects VMM IPX API emulation in place of the contract's
older ODI/IPX-shim wording. The NE2000 remains an independent packet-driver
surface; no translation through lwIP or native sockets exists.

## Amendment 3 research and implementation

Checked the following primary sources online on 2026-10-21 against the
accepted amendment-2 implementation before the corresponding fixes:

- [DP8390D datasheet, section 10.3, ISR](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D%2CNS32490D.pdf): overflow sets both OVW and RST. RST is not writable or interrupting; start clears stop/reset status, and removing ring packets clears overflow status. Track overflow separately until a valid changed BNRY releases ring storage. Keep OVW acknowledgement independent.
- [QEMU `ne2000_ioport_write`](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/ne2000.c): with STOP clear and count zero, either remote read or remote write sets RDC. Extend the existing model to both commands and retain IMR-controlled IRQ delivery.
- [DPMI Committee specification 1.0, pages 34–35, 85–91](https://docs.pcjs.org/specs/dpmi/1991_03_12-DPMI_Spec_v10.pdf): 0300h enters the IVT target with an IRET frame; 0301h uses a far-return frame; a real-mode callback can subsequently enter a protected-mode procedure. Dispatch the synthetic 2Fh/7Ah entries without consuming their original IRET frames. An asynchronous ESR uses the existing bounded DPMI reflection record and real-mode stack with a far-return trampoline, preserving the full PM continuation, including nested callbacks. No new guest DPMI service is introduced.
- [Released BUILD `MULTI.C`, `simulateint`, `neton`, receive handler](https://github.com/videogamepreservation/dukenukem3d/blob/master/SRC/MULTI.C): discovery/open/listen use DPMI 0300h; AL=FF requests a long-lived socket, and conventional receive code reposts its ECB. Transient AL=00 sockets retain their opening PSP and are closed without ESR delivery at the validated DOS process-exit/EXEC-rollback hook. Existing AL=FF sockets persist.

Physical link-loss/revocation notifications latch an epoch on the selected
netif. The existing bounded network worker observes it independently of VM
execution, takes the nonblocking bridge owner gate and drains RX/TX references.
A down/up transition is retained until consumed; gate contention retries on
the next pass. Generation mismatch also drains without programming a replacement
NIC. Exact host commands and limitations are in the Amendment 3 report.

## Interfaces and limits

Bridge mutation is serialized in thread context by its selected-port owner.
Queues hold references to caller-supplied packet-pool slots, at most eight RX
and eight TX per VM and two VMs. The slots retain ownership metadata only; payloads come from the native
f6-10 PBUF_POOL, with no allocation fallback. TX admission copies the complete frame before success.
Physical TX rejection is counted; local unicast stays off the wire. Broadcast
excludes its sender and uses the physical port once. Only checked Ethernet II
IPX enters either guest surface; host ARP/IPv4 is excluded.

The host identity adapter loads an existing 16-byte random record, or obtains
and durably writes one before allowing admission. Missing/corrupt identity,
failed persistence, or unsupported receive mode fails closed. The production
VFS store now reads `C:\System\NETID.BIN`, commits a new 16-byte candidate to
`NETID.TMP`, closes it, renames without replacement and flushes C: before
returning success. A valid pending candidate is recovered before generating
another identifier; malformed final/pending records are refused. The first LAN
launch obtains the approved f6-10 SplitMix64 stream, persists
the record, then publishes the primary-port binding. Existing identities do not
consume fresh entropy. LAN remains off by default.

IPX has 32 socket slots and 64 outstanding ECBs per VM (therefore <=64 per
socket), with up to 16 nonoverlapping fragments and 1500 datagram bytes.
Completion values are 00 success, FC cancelled, FD malformed, FE undeliverable,
FF hardware error. No incoming packet allocates heap memory or asserts a host panic.

## Verification and integration

Exact commands, outcomes, limitations and outstanding integration are recorded
in `build/codex/f6-11-report.md`. No image SHA or guest qualification is claimed
from host checks. Amendment 2 authorizes the VM lifecycle,
command validator, DOS Box wiring and primary-port/pool integration. The lead's
in-progress main merge now supplies f6-10. All merge markers were reconciled;
index resolution and the merge commit remain the lead's responsibility.

## Application API source matrix

| API | Primary reference and observed use |
| --- | --- |
| INT2F AX=7A00; far entry; INT7A | DOOM `IPXNET.C::InitNetwork`; [ROTT IPXNET.C](https://github.com/videogamepreservation/rott/blob/master/rottcom/ROTTIPX/IPXNET.C)::InitNetwork; [Descent BIOS/IPX.C](https://github.com/videogamepreservation/descent/blob/master/BIOS/IPX.C)::ipx_init; [Duke/BUILD MULTI.C](https://github.com/videogamepreservation/dukenukem3d/blob/master/SRC/MULTI.C)::neton. |
| BX=0/1 open/close | DOOM/ROTT OpenSocket/CloseSocket; Descent ipx_init/ipx_close; BUILD neton/netoff. BUILD uses AL=FF lifetime; DOOM uses AL=00. |
| BX=2 local target | Descent ipx_get_local_target: ES:SI input address, ES:DI immediate node. Network-zero same-LAN target only; other networks return FA. |
| BX=3/4 send/listen | DOOM/ROTT SendPacket/ListenForPacket; Descent ipx_send_packet/ipx_listen_for_packet; BUILD neton/netsend. Both one-fragment receives and separate 30-byte-header/payload receives occur. |
| BX=5 schedule IPX event | Explicit directive requirement, not an observed DOOM call; timer units follow the 18.2 Hz interval marker. No claim that the listed game files call this API. |
| BX=6 cancel | Descent ipx_cancel_listen_for_packet; cancels a pending ECB without ESR, with FC completion. |
| BX=7 AES event | Optional matching scheduler surface; same bounded timer table, no background service. |
| BX=8 interval marker | Explicit directive requirement; host monotonic PIT-derived intervals, unaffected by guest wall-clock/PIT writes. |
| BX=9 address | DOOM/ROTT GetLocalAddress; Descent ipx_init; BUILD neton. Ten-byte output: zero network and assigned MAC. |
| BX=10 relinquish | DOOM/ROTT SendPacket polls the in-use flag and relinquishes; bounded host service and a scheduler safe point. |
| ESR | BUILD MULTI.C::neton installs a conventional-memory receive ESR and two receive fragments. The supplied real-mode routine reposts the ECB and returns far. Host code enters it through the V86 frame boundary, preserves the interrupted frame and validates the private return trampoline. |

Duke's `SRC/MMULTI.C::callcommit` delegates to an external communication driver;
`SRC/MULTI.C` is the separately included BUILD implementation with direct IPX
calls. Neither source inspection establishes that the shipping Duke fixture
uses exactly that driver. The selected game/driver remains a lead gate.

ESR stack/code and all outstanding ECB ranges in the production adapter are
restricted to stable conventional guest memory. Pageable EMS/DPMI/VGA mappings
are refused before publication; no physical IF-clear callback can fault them
in. This matches the reviewed games' real-mode buffers. Tables, guest generations
and copy bounds are tested independently from integration into the VM lifecycle.

## Amendment 2 research and integration decisions

The amendment grants production wiring after the lead merges f6-10. The
current merge supplies its real netif, packet pool, entropy and MM_NET ledger.
The bridge asks `net_stack_primary()` for the successfully attached default
physical interface, rather than independently choosing the first running NIC.
It retains that device's generation and uses `netif_promiscuous` under the
existing device mutex. No failover or second physical bridge port is created.

`bridge_runtime.c` provisions lazily at the first IPX launch, through the exact
production NETID VFS callbacks. It refuses missing storage, malformed identity,
failed persistence and unsupported receive mode. Concurrent provisioning returns
EBUSY. It checks the selected index and generation again after sleeping VFS
operations. Active VMs retain the old generation; teardown performs no register
write against a replacement/stopped device. A later launch can reopen the port
after the last old VM retires, reusing the persisted identifier.

Receive-mode operations use the physical device mutex and retain normal error,
runt and broadcast policy. Sources checked before implementation:

- [Intel 8255x developer manual](https://www.intel.com/content/dam/doc/manual/8255x-10-100-mbps-ethernet-controller-software-dev-manual.pdf), Configure command, byte 15 bit 0: change destination acceptance with a bounded CU command, without resetting the RU or overwriting an outstanding TX command. A busy CU returns EBUSY.
- [Linux RTL8139 driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/realtek/8139too.c), `rx_mode_bits`/`__set_rx_mode`: RxConfig bit 0 accepts other physical destinations. Preserve FIFO/DMA configuration and reject bad/runt frames.
- DP8390 datasheet section 10, RCR bit 4: accept physical destinations without enabling multicast, runts or errored packets. The selected IPX format uses unicast/broadcast only.
- [Intel 8254x developer manual](https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf), RCTL.UPE: accept other unicast destinations without changing CRC stripping, ring geometry or multicast policy.
- [Novell's NWSIPX API summary](https://support.novell.com/techcenter/articles/dnd19960702.html) describes the legacy ESR and timer roles, and the need to lock ECBs, data buffers and ESRs. This later 32-bit API does not specify the legacy INT7A register ABI. The per-call matrix above explicitly identifies the released DOS game source used for discovery, open/close, send/listen, address and relinquish semantics; timer/interval details are directive requirements rather than DOOM observations. No complete legacy Programmer's Reference was retrieved and none is claimed.

Unsupported physical receive-mode operations fail closed. Native ingress must
retain destination filtering when promiscuous reception is active: only host
unicast, broadcast and existing multicast traffic can reach the native stack;
the IPX EtherType tap consumes guest traffic independently.

The receive-mode operation is implemented for e100, RTL8139, NE2000 PCI and
e1000. PCnet, Tulip, 3c90x and Rhine explicitly return ENOSYS for this operation.
The bridge retains the last VM's network owner when revocation returns busy;
retrying destruction retries revocation before releasing guest memory.

The launch/lifecycle fixture uses production CREATE validation, VM
creation/boot, automatic primary-port selection, INT2F dispatch and CLOSE,
including failed admission rollback and deferred receive-mode cleanup. A new
primary-port fixture selects NIC one while NIC zero is also running, checks
persistence before publication, refuses faults/reentrancy, and proves generation
revocation and stable NETID reuse. Exact verification results and sandbox
limitations are recorded in the report, without treating supplementary UBSan
as a replacement for the normal ASan/UBSan/LSan gate.

The identity fault fixture exercises short/oversized/zero records, every
publication failure, retry/recovery, handle retirement and destination
collision. A second fixture calls the actual store through production FAT/VFS
on in-memory FAT16 sectors, then unmounts/remounts and verifies identical bytes,
read-only access, pending-candidate recovery and corrupt-record refusal. It
builds no boot image and changes nothing in `tests/host/fs` or `tests/host/proc`.
A failed partial pending write is retained and refused, rather than silently
generating a new identity on the next launch.

The [pinned lwIP 2.2.1 pbuf implementation](https://raw.githubusercontent.com/lwip-tcpip/lwip/STABLE-2_2_1_RELEASE/src/core/pbuf.c)
allocates PBUF_POOL from MEMP_PBUF_POOL and releases it through pbuf_free.
`packet_pool.c` uses exactly that allocator, like native `net_stack_input`:
128 entries, 1536 payload bytes each. Each bridge metadata slot owns one pbuf;
local fan-out references keep that owner until the final RX/TX queue releases
it. No additional frame array, heap fallback or IPX `tcpip_input` call exists.
The linker includes pool/runtime metadata and DOS network state in the same
`__net_start..__net_end` range that `net_stack_init` moves to MM_NET and reserves
under its existing 4 MiB ceiling; the packet pages are not charged twice.

The full real-stack regression parks the tcpip core at a controlled boundary,
queues one actual native ingress pbuf, retains 113 native pool allocations and
queues seven frames from each of two VMs. At all 128 entries occupied, both
VM sends and native ingress fail within their bounds. Link loss frees only the
14 VM frames; broadcast fan-out shares one payload until its final reader or
VM teardown. Disable/destroy drains do not release native ownership. Releasing
the native buffers and draining tcpip_input returns the actual allocator to its
original baseline. The isolated VM fixtures also use the upstream allocator,
with only the absent TCP core's optional out-of-order reclaim callback refused.
