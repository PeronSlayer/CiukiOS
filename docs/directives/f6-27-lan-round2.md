# f6-27: LAN round 2: Windows 98 framing default, DirectPlay IPX provider, DirectPlay completion

- **Written:** 2026-10-28 13:50 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`. Base: branch `wt/lan2` = main (with the security programme) + f6-25 (c6be84829) + f6-26 (a63af1fb4), conflicts resolved by the lead.
- **Inputs:**
  - the f6-25 and f6-26 reports (`../CiukiOS-wt-f6-25/build/codex/f6-25-report.md`, `../CiukiOS-wt-f6-26/build/codex/f6-26-report.md`);
  - `docs/design/win32-ipx.md`;
  - `docs/design/directplay.md`;
  - the directives f6-25 and f6-26.

## Lead decisions

1. **Frame type follows Windows 98.**
   - Network properties → IPX/SPX-compatible Protocol → Advanced → Frame type offers Auto, Ethernet 802.2, Ethernet 802.3, Ethernet II and Ethernet SNAP (Resource Kit `wrkc15_4.htm` and `wrkappd_1.htm`, `Frame_Type` with default 4 = Auto). Store the setting in the machine network configuration through the SYSTEM service; a Standard user cannot change it.
   - Auto sends RIP requests in each frame format and adopts the most prevalent reply (f6-25).
   - When no router answers, Auto uses **Ethernet 802.2**. This is the Windows 95/98/NT NWLink default; find and cite the Microsoft documentation, and if a primary source says otherwise for Windows 98, follow it and record the source.
   - CiukiOS receives all four framings. A unicast reply goes out in the framing the peer used last; broadcasts use the selected frame type. CiukiOS machines, real Windows 98 machines and DOS games in the VMs therefore meet on one router-less LAN.
2. **DirectPlay IPX service provider** over Winsock `AF_IPX` (f6-25), as Windows 98's `dpwsockx` offers it.
   - Research the provider's wire behaviour: enumeration socket and broadcast, player addressing, guaranteed delivery. Use the MS-DPDX/MS-DPSP specifications, Wine and period SDK documentation.
   - Record every assumption the sources do not settle as a host-tested CiukiOS choice.
3. **DirectPlay completion.** Fix the f6-26 failures that are DirectPlay's own: host migration where Wine has it, guaranteed send under loss, lobby `RunApplication`. Separate them from the LeakSanitizer-environment failures listed in the report.

## Scope and tests

- Host tests:
  - two to five peers;
  - TCP/IP and IPX providers;
  - mixed framings (a Windows 98-style 802.2 peer beside CiukiOS peers);
  - framing setting changes;
  - administrator and Standard-user writes of the setting.
- Write the lead's QEMU plan: two guests on one socket or multicast netdev (the owner allows up to three QEMU only for LAN tests). Cover:
  - a DOS IPX game between two guests;
  - a Win32 IPX datagram test program between two guests;
  - a DirectPlay session between two guests.

  Write the test programs as Win32 console/GUI test binaries built from source in `tests/`, never commercial games.

## Acceptance

- The new host tests pass.
- The Win32 (`python3 -B -m unittest discover -s tests/host/win32 -t .`), kernel (`bash scripts/test/host_kernel_tests.sh`) and desktop suites gain no failure against the base, apart from failures you show are LeakSanitizer-environment only. The lead reruns those outside the sandbox.
- `python3 scripts/gen_api_reference.py --check` passes.
- `config/wine-upper.json` is current.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-lan2` (branch `wt/lan2`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/f6-27-report.md`. Leave the changes uncommitted.
