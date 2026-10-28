# f6-26: DirectPlay for Win32 games (TCP/IP and IPX service providers, lobby)

- **Written:** 2026-10-28 12:04 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`. Base: main. f6-25 (Win32 IPX/SPX) runs in parallel; the IPX provider uses only the Winsock `AF_IPX` API, so test it against a host Winsock fake until f6-25 is merged.
- **Goal (1.0 Beta, LAN party):** DirectX 5–7-era Windows games that host and join LAN games through DirectPlay (`IDirectPlay2/3/4`, `IDirectPlayLobby2/3`) work on CiukiOS between up to five machines, and with Windows 98 machines on the same LAN where the protocol allows.
- **Binding specification:**
  - `docs/design/network-contract.md`;
  - the Wine upper-DLL pipeline (`docs/directives/f5-14b-wine-upper-dlls-pe-builds.md`, `config/wine-upper.json`, `make upper`);
  - inventory rows NW-13 and the DirectX rows in `docs/design/os-feature-inventory.md`.

## Research first (cite in a new `docs/design/directplay.md`)

- The DirectX 6.1/7 SDK DirectPlay documentation (sessions, players, groups, guaranteed messages, the service provider interface, `DPSP_*` callbacks).
- The DirectPlay TCP/IP wire protocol: the 47624 enumeration port and the 2300–2400 dynamic ports. Use Wine's `dlls/dplayx` and `dlls/dpwsockx`, and the MS-DPDX / MS-DPSP open specifications.
- The IPX service provider behaviour of Windows 98's `dpwsockx`.
- Which period games use DirectPlay versus raw Winsock. Use this to pick the test titles; never commit game bytes.

## Scope

1. Build `dplayx.dll`, `dplay.dll` and `dpwsockx.dll` from Wine through the upper-DLL pipeline, completing what Wine leaves unimplemented and the games need:
   - session enumeration and host/join;
   - player and group messages;
   - guaranteed and non-guaranteed send;
   - host migration where Wine has it;
   - `DPSESSION` flags.
2. **TCP/IP service provider:** LAN broadcast enumeration on 47624, and join by address dialog.
3. **IPX service provider** over Winsock `AF_IPX`, as Windows 98 offers. Registry entries under `HKLM\Software\Microsoft\DirectPlay\Service Providers`.
4. **DirectPlayLobby:** registration of lobbyable applications, `RunApplication` with a connection, enough for games that launch through the lobby.
5. **Host tests:**
   - two to five simulated peers on the host Winsock model: enumerate, create, join, chat-style messages, guaranteed delivery under loss, host leaving;
   - an independent conformance replay of the MS-DPDX message formats.

   Record the QEMU two-guest plan for the lead.

## Acceptance

- The new host tests pass.
- The Win32 suite (`-t .`), the census (`test_census`) and the upper-DLL checks gain no failure against the base.
- `config/wine-upper.json` is regenerated.
- `python3 scripts/gen_api_reference.py --check` passes.
- The inventory rows are updated.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-f6-26` (branch `wt/f6-26`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/f6-26-report.md`. Leave the changes uncommitted.
