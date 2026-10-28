# Win32 IPX and SPX (f6-25)

Research and implementation decision, 2026-10-28. This extends f6-11's
Ethernet-II-only, network-zero admission; IPX still bypasses lwIP's IP stack.

## Primary sources and decisions

- Novell's [IPX packet structure](https://www.novell.com/zh-cn/documentation/nw6p/ipx_enu/data/hc1w6pvi.html)
  defines the 30-byte header and network/node/socket addresses. Socket and
  protocol fields are in network order. LAN datagrams use checksum FFFF;
  no routing is offered. Native nodes use the physical MAC, DOS nodes retain
  their assigned distinct MACs; socket allocation is per node.
- Novell's [Ethernet framing account](https://support.novell.com/techcenter/articles/ana19930905.html)
  distinguishes Ethernet II (8137), raw 802.3 (length followed by FFFF), LLC
  (E0 E0 03), and SNAP (AA AA 03 00 00 00 81 37). Validate each discriminator
  and both lengths, including Ethernet padding. The bridge preserves the
  encapsulation of direct NE2000 frames; the emulated DOS API uses the detected
  host framing and normalizes receive frames.
- Windows 98 Resource Kit, local read-only `build/rskt/wrkc15_4.htm`,
  “Configuring the IPX/SPX-Compatible Protocol”, frame-type table and footnote 1:
  Windows 98 sends general RIP requests in each format at startup and selects
  the most prevalent router response. It does **not** specify a fixed probe
  ordering. Probe LLC, raw, Ethernet II, SNAP; count distinct router replies
  during a bounded discovery window. Ties prefer LLC, raw, II, SNAP; with no
  router, retain Ethernet II for the already qualified DOS LAN. This fallback
  is Ciuki policy, not a claim about an undocumented Windows 98 fallback.
- Microsoft's [IPX protocol identifiers](https://learn.microsoft.com/en-us/windows/win32/winsock/ipx-family-of-protocol-identifiers-2)
  define 1000–1255 for datagrams (packet type = protocol minus 1000) and 1256
  for SPX streams and sequenced packets. Protocol zero selects the type's
  default. Microsoft's [IPX socket options](https://learn.microsoft.com/en-us/windows/win32/winsock/nsproto-ipx-socket-options)
  define packet-type selection/filtering and adapter/network queries.
  The [adapter record](https://learn.microsoft.com/en-us/windows/win32/api/wsnwlink/ns-wsnwlink-ipx_address_data)
  measures maxpkt excluding the IPX header and linkspeed in 100 bytes/second;
  LAN wan/status are zero. The [network record](https://learn.microsoft.com/en-us/windows/win32/api/wsnwlink/ns-wsnwlink-ipx_netnum_data)
  uses host-order hop/delay/card numbers and a zero router on a direct network.
  Some queries extend the documented Windows 9x getter subset as explicitly
  required by f6-25; this is not a claim that all getters existed on Win98.
  SO_BROADCAST gates broadcast sends. Unknown remote networks return
  ENETUNREACH; this LAN implementation does not fabricate a RIP route.
- Wine's [wsipx.h](https://github.com/wine-mirror/wine/blob/master/include/wsipx.h)
  supplies an independent check of the **14-byte** SOCKADDR_IPX layout;
  [wsnwlink.h](https://github.com/wine-mirror/wine/blob/master/include/wsnwlink.h)
  checks option values and the 24-byte adapter record. The native socket ABI
  carries IPX in a padded 16-byte address; Winsock translates both ways.
  [wsock32/socket.c](https://github.com/wine-mirror/wine/blob/master/dlls/wsock32/socket.c)
  and [ws2_32/socket.c](https://github.com/wine-mirror/wine/blob/master/dlls/ws2_32/socket.c)
  distinguish Winsock errors and datagram truncation. Existing DLL forwarders
  retain one socket table. The historical [Wine 1.0 IPX option handlers](https://github.com/wine-mirror/wine/blob/wine-1.0/dlls/ws2_32/socket.c)
  are also checked: their filter placeholder is not a behavioral oracle, and
  their hard-coded link speed is not used. Microsoft defines the adapter units.
  No Wine implementation is copied into the kernel.
- Novell's [SPX trace and protocol explanation](https://support.novell.com/techcenter/articles/ana19951203.html)
  defines packet type 5, the 12-byte SPX header, SYS/ACK/EOM, initial destination
  connection FFFF, next-expected acknowledgements, inclusive allocation, FE/FF
  termination and system-packet watchdogs. Implement classic SPX with one
  outstanding data packet, immediate ACKs, a bounded receive window, capped
  exponential retransmission and finite idle watchdog retries. SPX II extensions
  are not advertised. Streams may return partial writes; sequenced packets
  preserve one record per send, with a 534-byte classic packet payload limit.

## Ownership and containment

The physical bridge and native IPX endpoint hub share one nonblocking port
gate. Neither callback sleeps or retains caller memory. Packet copies and
retransmissions use the existing global PBUF_POOL, never an independent payload
heap. The socket adapter retains the existing fd reservation, generation,
capture, cancellation and poll ownership. The worker services protocol timers
without requiring a running DOS VM. Network loss fails connections and drains
references; process teardown closes endpoints; all queues and retries are bounded.

## Lead validation plan

Run the host IPX/SPX tests first, then the kernel/Win32/desktop suites and ABI
oracle. After integration and the canonical build, the lead should connect two
guests with a runner-owned socket segment or host bridge (never user NAT),
verify broadcast discovery and directed datagrams in both framings, run an
SPX stream and sequenced connection with packet loss, and run Win32-to-DOS
IPXSETUP discovery on the same host and across the segment. Repeat discovery
with five nodes and qualify the T23 physical NIC. The directive's two-guest
plan conflicts with the older one-QEMU contract limit; execution/concurrency
belongs to the lead. No QEMU evidence is produced in this worktree.
