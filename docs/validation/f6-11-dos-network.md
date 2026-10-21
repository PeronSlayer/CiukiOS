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

## Interfaces and limits

Bridge mutation is serialized in thread context by its selected-port owner.
Queues hold references to caller-supplied packet-pool slots, at most eight RX
and eight TX per VM and two VMs. The owner must supply the global pool; there
is no allocation fallback. TX admission copies the complete frame before success.
Physical TX rejection is counted; local unicast stays off the wire. Broadcast
excludes its sender and uses the physical port once. Only checked Ethernet II
IPX enters either guest surface; host ARP/IPv4 is excluded.

The host identity adapter loads an existing 16-byte random record, or obtains
and durably writes one before allowing admission. Missing/corrupt identity,
failed persistence, or unsupported receive mode fails closed. The lead supplies
the installation identity store and selected driver's receive-mode operation.

IPX has 32 socket slots and 64 outstanding ECBs per VM (therefore <=64 per
socket), with up to 16 nonoverlapping fragments and 1500 datagram bytes.
Completion values are 00 success, FC cancelled, FD malformed, FE undeliverable,
FF hardware error. No incoming packet allocates memory or asserts a host panic.

## Verification and integration

Exact commands, outcomes, limitations and outstanding integration are recorded
in `build/codex/f6-11-report.md`. No image SHA or guest qualification is claimed
from host checks. The existing-files conflict rule must be resolved before
wiring the models into VM lifecycle, the command validator and DOS Box UI.

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
