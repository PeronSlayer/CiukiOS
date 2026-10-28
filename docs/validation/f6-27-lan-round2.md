# f6-27 LAN round-two validation and lead guest plan

Host evidence is recorded in `build/codex/lan2-report.md`. No QEMU, image
build or physical trial was executed by the implementer. Windows 98 IPX
DirectPlay interoperability remains unqualified: the CiukiOS transport choices
are listed in [directplay.md](../design/directplay.md).

## Lead execution prerequisites

Finish clean host sanitizer gates outside the sandbox, review the changes,
qualify the three pending upper DLLs with actual H1 evidence, integrate and
produce the single canonical image. Record its SHA-256, the upper DLL hashes
and `build/host/lan2-pe/build.json`. Build the authored Win32 fixtures with
`nice -n 19 python3 tests/win32/lan/build.py` after the upper source/object
headers exist. These are console PEs compiled from tracked source, without
commercial payloads. Copy only the two resulting PEs into the lead's fixture
staging through the normal image/fixture mechanism, before the image hash is
fixed. Do not rebuild an image while any overlay depends on it.

Use `qemu-t23` and separate qcow2 overlays under `build/test-runs/`. The owner's
LAN exception permits two live guests and at most three; this supersedes the
older single-QEMU plan in the earlier design notes. Check free host memory;
each guest remains in its own 1500M/no-swap capped user scope, with distinct
serial/QMP paths. Run no build or unrelated suite concurrently.

Use the profile's supported NIC with distinct MACs (for example
52:54:00:27:00:01 and :02). Replace its ordinary network backend with:

```text
Guest A: -netdev socket,id=lan,listen=127.0.0.1:3027
Guest B: -netdev socket,id=lan,connect=127.0.0.1:3027
Both:    -device <profile NIC>,netdev=lan,mac=<distinct MAC>
```

For three guests, all may instead use
`-netdev socket,id=lan,mcast=230.32.27.1:3027,localaddr=127.0.0.1`.
[QEMU's socket backend documentation](https://www.qemu.org/docs/master/system/invocation.html#network-options)
describes these Ethernet segments. IPX needs the shared Ethernet backend;
IPv4 user NAT does not carry it. Configure TCP/IP statically as 192.0.2.1/24
and 192.0.2.2/24 on this isolated segment; no DHCP server is assumed.

## Required live cases

| Case | Actions and acceptance |
| --- | --- |
| Framing/persistence | As administrator select each of Auto, 802.2, raw 802.3, II, SNAP on the IPX/SPX Properties tab and Apply. Reboot and confirm GET/UI preserve the choice. Standard/Guest Apply must fail without file/runtime changes; direct administrator writes to ipx.cfg must fail. No-router Auto broadcasts must be LLC; capture all four startup RIP probes and prevalence selection with a reference router where available. |
| Win32 AF_IPX | On A run `lan2-ipx.exe host`; on B run `lan2-ipx.exe join`. Both must emit `PASS LAN2 Win32 AF_IPX broadcast/directed echo` within 60 seconds and exit zero. Swap host/join. Repeat with A using Auto/802.2 and B forcing each other framing; verify directed replies use the request's framing and broadcast uses the selected frame. The DLL API and packets must both be observed. |
| DOS IPX | Launch two DOS VMs through the normal desktop path, one per guest. Use the owner's locally licensed IPX game fixture (for example DOOM/IPXSETUP) without copying its bytes into tracked files. Discover, join, play, close and reopen. Repeat with a native AF_IPX fixture active to check endpoint separation; this is a raw IPX control, not DirectPlay interoperability evidence. Keep only a short factual evidence record after diagnosis. |
| DirectPlay TCP/IP | On A run `lan2-directplay.exe host tcp`; on B run `lan2-directplay.exe join tcp`. Both must emit the COM/session/player/guaranteed-chat PASS marker and exit zero. Observe enumeration UDP 47624, TCP/UDP endpoints 2300–2399, player IDs, received payload and balanced cleanup. Swap roles and repeat with three live guests. |
| DirectPlay IPX | Repeat with `host ipx` / `join ipx`. Observe AF_IPX enumeration socket BA08 and the documented CiukiOS paired IPX/SPX endpoints. Check Auto/802.2 against another selected framing. A passing CiukiOS pair does not establish Microsoft's provider wire compatibility. |
| Loss/host close | Inject loss below Winsock in a runner-owned Ethernet relay (or an explicitly reviewed backend fault injector); retain only bounded packet evidence. Guaranteed chat must arrive exactly once or report connection loss, never silently succeed after truncation. Unreliable datagrams may disappear. Close the host; no supported migration engine exists in pinned Wine, and MIGRATEHOST/DIRECTPLAYPROTOCOL must return unsupported. All sockets, threads and session resources must drain. |
| Windows reference | Use a licensed Windows 98/DirectX 7 peer with the same authored fixtures. TCP/IP and raw AF_IPX are separate controls. Capture its DirectPlay IPX enumeration socket, provider header/player addressing and guaranteed-message protocol before deciding whether to change the CiukiOS profile. Record mismatches explicitly. |

Repeat the host provider model with two through five nodes; guest concurrency
stays within the owner's three-guest limit. A live five-guest result is not
claimed. Preserve image/PE hashes, commands, serial PASS/failure evidence,
settings before/after, and observed frame/socket formats. Delete passed overlays
and temporary captures; leave no QEMU process running.
