# DirectPlay 2–4 and Lobby 2–3

Directive: [f6-26](../directives/f6-26-directplay.md). Research checked on
2026-10-28. The implementation uses the pinned Wine 10.0 sources and the
existing CiukiOS upper-DLL pipeline; it does not import Microsoft's DLLs.

## Primary sources and implementation decisions

| Source | Finding and decision |
| --- | --- |
| [Microsoft DirectX 7 SDK, basic connection sequence](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpover_6ddl.content.htm) (archived SDK documentation) | Preserve the COM interfaces, EnumConnections/InitializeConnection, session enumeration, Open, CreatePlayer and Send/Receive sequence. The service provider is below the session/player/group implementation. |
| [Wine 10.0 dplayx](https://gitlab.winehq.org/wine/wine/-/tree/wine-10.0/dlls/dplayx), [dpwsockx](https://gitlab.winehq.org/wine/wine/-/tree/wine-10.0/dlls/dpwsockx), [provider ABI](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/include/wine/dplaysp.h) | Compile the actual pinned sources, with reviewable patches. The base provider rejects host creation and IPX; Reply and legacy Send are stubs. Lobby RegisterApplication falsely succeeds. Those base behaviours cannot support the requested LAN gate. |
| [MC-DPL4CS transport](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-dpl4cs/e61a374b-be48-42a1-9fd4-c7a6efbd8b4b), [host initialization](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-dpl4cs/7cf81854-85ac-4d4e-9959-6c6ba7a1466c) | A host listens for UDP enumeration on 47624 and has TCP/UDP endpoints within 2300–2400. Broadcast destination is 255.255.255.255. Retain Wine's split TCP/UDP allocation within that range. TCP carries guaranteed messages; it is responsible for retransmission after underlying packet loss. |
| [MC-DPL4CS overview](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-dpl4cs/bf75488a-d4a6-4aa1-9292-e1b8a994b597), [MC-DPL4R](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-dpl4r/23064f42-7a50-45ae-a4e2-fdde34019f27) | Distinguish the DirectPlay 4 core protocol from the optional reliable protocol. Do not advertise unsupported reliability/session options as implemented. Wine 10.0 has no host-migration implementation in dplayx; transport teardown alone is not migration. |
| [MS-DPDX](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpdx/fa7b2fcb-3b46-42db-9e47-2d8069741263), [transport](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpdx/2246db93-82c5-4fc0-bc85-981449b4f173), [userdata header](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpdx/d56986a3-60ab-4f06-a6f6-8f10a8a21542) | DXDiag uses the later DirectPlay protocol. A replay of its packets is independent format evidence, not proof that DirectPlay 4 peers interoperate. |
| [MS-DPSP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpsp/3e44be62-2067-472a-9ef0-e937298b68fb) | This is Digest Protocol Extensions, not the DirectPlay service-provider protocol. The directive's reference is a naming error; use MC-DPL4CS/MC-DPL4R for DirectPlay 4. |
| [Wine 10.0 provider registration](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/loader/wine.inf.in) | Seed TCP/IP and IPX provider names, GUIDs, descriptions, DLL paths and reserved DWORDs under HKLM\\Software\\Microsoft\\DirectPlay\\Service Providers. Use the existing new-hive-only policy and preserve persisted user changes. The presence of an IPX registry entry does not qualify its wire protocol. |

The readable Windows 98 Resource Kit `wrkc12_1.htm` describes DirectPlay as
game connectivity over modem or network; it does not specify the IPX provider
wire format. It is not redistributed. Windows 98 AF_IPX is a Winsock provider,
not a V86 or NT NTVDM interface. The f6-26 provider work did not require a DOS
bridge or kernel ABI change; f6-27 adds the machine framing setting described
in [win32-ipx.md](win32-ipx.md).

## Validation boundaries

Host tests must exercise the patched provider callbacks and their Winsock
calls, rather than a separate implementation of sessions. Enumeration replies
and guaranteed sends need complete TCP writes, including short writes. Invalid
datagrams must not stop the receive loop. Each failed setup must release its
sockets, events and Winsock startup reference. Tests distinguish provider-level
peer simulation from end-to-end IDirectPlay COM execution.

No Windows 98 IPX provider packet capture or primary specification of its
DirectPlay 2–4 framing is currently available in this checkout. Guessing a
private framing or equating SPX with DirectPlay's reliable protocol would not
establish Windows 98 compatibility. This remains an explicit qualification
item; neither a registry entry nor a host fake can supply that evidence.

## Baseline f6-26 guest and title plan (superseded)

The f6-27 live two-guest plan supersedes this earlier concurrency restriction;
see [the current LAN plan](../validation/f6-27-lan-round2.md).
The original f6-26 proposal used the canonical image and two separate runner-owned overlays on the
qemu-t23 profile. Run the guests sequentially under the project's QEMU caps,
with a host Winsock/frame peer preserving the other guest's recorded protocol
exchange, or obtain explicit authorization for a simultaneous two-guest
exception. Never present a sequential replay as a live two-guest session.
Use an isolated Ethernet backend for IPX, not user-mode IPv4 NAT.

Start with redistributable SDK-style fixtures: one host, one joining peer,
then three through five peers; application-GUID filtering; session full and
join-disabled flags; player/group creation and deletion; reliable and lossy
chat; host close; lobby register/enumerate/launch/connection retrieval.
Record the exact image and PE hashes, serial evidence, packets and teardown.
Repeat TCP/IP with an independently licensed Windows 98/DirectX 7 peer.
IPX interoperability requires a captured Windows 98 reference exchange first.

Select period title fixtures from verified imports/documentation: a game
importing DirectPlayCreate/DirectPlayLobbyCreate or creating the DirectPlay
COM class exercises this work. Raw Winsock games (for example the upstream
[Quake network driver](https://github.com/id-Software/Quake/blob/master/WinQuake/net_wins.c))
are controls for the underlying stack and do not qualify DirectPlay. Keep
title binaries and private owner title lists out of tracked files. The report
records which title and end-to-end gates remain unrun.

For a period DirectPlay title, select **Age of Empires II: The Age of Kings**:
Microsoft's original [KB Q242556](https://jeffpar.github.io/kbarchive/kb/242/Q242556/)
(archived text) explicitly identifies its DPLAYX/DirectPlay 6.1a dependency.
[KB Q242904](https://jeffpar.github.io/kbarchive/kb/242/Q242904/) describes its
multiplayer connection choices. This is a research-selected title, not a claim
that a licensed fixture is present or has passed. Quake/WinQuake remains the
raw Winsock control, based on id Software's source linked above.

## Baseline f6-26 host evidence and outstanding work

The final patched PEs build twice identically, with zero unresolved eager
imports or forwarders. H0 production-loader binding passes for all nineteen
reused candidates. The sandbox kills the i386 H1 preflight with SIGSYS before
PE execution; the three new DLLs are explicitly pending qualification and are
not staged. The previously qualified sixteen DLLs retain their exact PE hashes
and their staged H1 evidence. This does not qualify DirectPlay sessions.

The native tests run the patched provider callbacks with two through five
simulated endpoints: host listener, enumeration, join endpoints, player/group
send and deletion, complete stream writes, datagram loss, host close and
setup rollback. The fake models TCP's recovery below the socket API; it does
not test CiukiOS TCP retransmission. The lobby test executes the exact patched
registration/enumeration helpers against a registry fake. Neither test is an
end-to-end COM session or a real child-process lobby handoff. ASan found and
verified the fix for Wine's group-removal use-after-free (unlink from the group
tree, not the player's tree). Both harnesses then reach their functional PASS
markers; LeakSanitizer's exit check fails under this sandbox's ptrace policy.
Leak detection remains enabled, and these runs are not reported as clean
sanitizer passes.

The independent replay covers the MS-DPDX EnumQuery layout and malformed
variants and distinguishes it from DirectPlay 4 framing. It does not add a
DirectPlay 8 stack. Windows 98 IPX transport interoperability, full IDirectPlay2/3/4 multi-peer conformance,
DirectPlay reliable-protocol/session flags, complete lobby lifecycle and
Windows 98 interoperability remain open. In particular, the inherited Wine
lobby message worker/lifetime and shared-memory handoff need end-to-end
validation before the launch path is qualified.

## f6-27 provider choices before implementation

The Microsoft [MS-DPDX transport](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpdx/2246db93-82c5-4fc0-bc85-981449b4f173)
uses datagrams and shares its IPX/UDP format, but describes DXDiag/DirectPlay 8.
[MS-DPSP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-dpsp/3e44be62-2067-472a-9ef0-e937298b68fb)
is Digest authentication. Neither specifies Windows 98 DirectPlay 2–4's IPX
provider. [MC-DPL4CS transport](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-dpl4cs/e61a374b-be48-42a1-9fd4-c7a6efbd8b4b)
specifies TCP/IP addresses and ports, not an IPX socket assignment. The pinned
[Wine provider](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/dpwsockx/dpwsockx_main.c)
rejects IPX; its session core has no migration implementation. The archived
[DirectX 7 SDK sequence](https://library.thedatadungeon.com/directx7/devdoc/live/directx/dpover_6ddl.content.htm)
defines the service-provider boundary; it supplies no missing IPX wire format.

Implement an explicitly CiukiOS IPX profile through AF_IPX: retain Wine's
20-byte provider header with a zero-padded 14-byte SOCKADDR_IPX in its address
slot, 32-byte paired stream/datagram player addresses, broadcast enumeration
to IPX socket 47624 (0xBA08), and endpoint sockets 2300–2399. These socket
assignments and provider framing are assumptions for host tests and require
Windows 98 capture qualification. Guaranteed messages use classic SPX streams;
this is a CiukiOS transport choice, not MC-DPL4R or a claim about Microsoft's
provider. Do not advertise optional DirectPlay reliable-protocol or migration
session flags as implemented. Preserve TCP/IP's wire format and reliability.

For lobby launches, the SDK's suspended-child/connection-settings sequence and
[Wine lobby source](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/dplayx/dplobby.c)
are the references. Propagate reservation, settings, notification-worker and
resume failures; terminate a failed suspended child and release local and
remote duplicated handles. Exercise the actual launch helper with OS seams.

The f6-27 callback model runs both providers with two through five endpoints,
including peer node/socket addressing, discovery/reply, bounded datagrams,
short/interrupted stream writes, failed-stream replacement and cleanup. Actual
kernel fixtures independently inject TCP and SPX packet loss/reordering.
The provider fake supplies stream semantics; it does not itself prove the
kernel transport. The lobby model executes the patched launch and notification
helpers, all nine failure stages, A/W conversion and copied connection fields.
Its shared-memory and child-process APIs are seams, not live Windows processes.
The authored Win32 fixtures and current live plan in
[f6-27-lan-round2.md](../validation/f6-27-lan-round2.md) are the next COM gate.

The IPX path uses NSPROTO_IPX datagrams (packet type zero) and does not request
SO_REUSEADDR, which the exclusive per-node endpoint hub does not implement.
The host fake rejects that option on AF_IPX. One host enumeration listener per
machine is supported by this profile; additional concurrent hosts on the same
machine are not advertised. The inherited lobby SendLobbyMessage and
ReceiveLobbyMessage channel remains unqualified; event lifetime checks alone
are not evidence for that channel.
