# f6-25: IPX/SPX for Win32 programs, one IPX network with the DOS VMs

- **Written:** 2026-10-28 12:04 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`. Base: main.
- **Goal (1.0 Beta, LAN party):** Windows 95/98 games of the period that play over IPX (through Winsock `AF_IPX`/`NSPROTO_IPX`/`NSPROTO_SPX`, or through DirectPlay's IPX service provider, f6-26) see each other and the DOS games of the DOS VMs on the same LAN, on up to five machines.
- **Binding specification:**
  - `docs/design/network-contract.md`, `docs/design/network-foundations.md`, `docs/design/ndis-host-netif.md`;
  - `docs/directives/f6-11-dos-ne2000-ipx-bridge.md` and the bridge in `src/kernel/net/bridge*.c`;
  - inventory row NW-13 in `docs/design/os-feature-inventory.md`.

## Research first (cite in a new `docs/design/win32-ipx.md`)

- Novell IPX/SPX packet format (Ethernet II type 0x8137, 802.3 raw, 802.2 LLC, SNAP framings) and the frame type auto-detection Windows 98's NWLink does.
- Windows 98 NWLink IPX/SPX compatible protocol: Resource Kit networking chapters in `build/rskt`.
- Winsock 1.1/2 IPX: `wsipx.h`, `wsnwlink.h`, `SOCKADDR_IPX`, `IPX_PTYPE`, `IPX_FILTERPTYPE`, `IPX_GETNETINFO`, `IPX_MAX_ADAPTER_NUM`, `IPX_ADDRESS`, `SO_BROADCAST` on IPX, and SPX `SOCK_SEQPACKET`/`SOCK_STREAM` semantics. Use Microsoft documentation and Wine's `dlls/wsock32` and `dlls/ws2_32` as references.

## Scope

1. **One IPX endpoint layer in the kernel network stack**, shared by Win32 sockets and the existing DOS VM IPX bridge, so a DOS game and a Win32 game on the same machine and across machines are on one IPX network.
   - Socket number allocation (dynamic 0x4000–0x7FFF, well-known by request).
   - Node address from the NIC MAC, network number 0 until learned.
   - Broadcast, packet type filtering.
   - At least the Ethernet II and 802.3 raw framings, with the detection order Windows 98 uses.
2. **Winsock:** `socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX[+ptype])`, `bind`, `sendto`/`recvfrom`, `select` and `WSAAsyncSelect` readiness, `getsockname`, and the options above, in `ws2_32` and `wsock32` (the forwarders share state).
3. **SPX:** `SOCK_SEQPACKET` and `SOCK_STREAM` over IPX packet type 5.
   - Connection establishment, sequencing, acknowledgement, retransmission with bounded timers, watchdog, orderly and abortive close.
   - Enough for games that use SPX connections.
4. **Host tests:**
   - two to five simulated nodes on one fake segment: Win32↔Win32, Win32↔DOS bridge;
   - broadcast discovery, packet types, socket exhaustion;
   - SPX loss, reordering and duplicate injection;
   - framing auto-detection.

   Record the QEMU evidence plan for the lead: two QEMU guests on a socket or bridge netdev, with QEMU allowed for F6.

## Acceptance

- The new host tests pass.
- The Win32 (`python3 -B -m unittest discover -s tests/host/win32 -t .`), kernel (`bash scripts/test/host_kernel_tests.sh`) and desktop suites gain no failure against the base.
- New `abi.h` records get `abi_layout` rows.
- `python3 scripts/gen_api_reference.py --check` passes.
- NW-13 is updated with what is done and what remains.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-f6-25` (branch `wt/f6-25`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/f6-25-report.md`. Leave the changes uncommitted.
