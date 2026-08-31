# Network File Sharing - 2026-08-29

> Current validation update (2026-09-01): the bounded Phase 8 result and open boundaries below pass on the release checkout, including the resident ICMP and FTP lanes after the generic nested-EXEC correction described below.

## Status

CiukiOS now has a repeatable, bidirectional host file-sharing lane on the FAT16
`full` profile. The implemented path is:

`QEMU NE2000 -> Crynwr INT 60h -> resident ARP/ICMP bridge INT 61h -> mTCP/FTP -> C:\SHARE`

The same-checkout gates prove Packet Driver initialization, FTP login/listing,
byte-identical download and upload, FAT16 persistence, and a real inbound ICMP
Echo Reply with validated checksums, addresses, identifier, and sequence. Both
gates use disposable images and require the canonical image to remain unchanged.

This completes the first bounded Phase 8 milestone; it does not close all
networking scope. FTP was selected before SMB, WebDAV, or SFTP because it gives
current Linux and Windows systems an interoperable client while fitting the
available real-mode DOS Packet Driver and TCP/IP ecosystem. FTP is plaintext,
so this default profile is intentionally bound to QEMU localhost forwarding.

## Open-source components

- [mTCP 2025-01-10](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/html/en/net/mtcp/20250604.0/index.html), GPLv3: IPv4 tools, FTP client, and FTP server.
- [Crynwr Packet Driver collection 2006-09-02c](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/html/en/net/crynwr/20250409.8/index.html), GPL package: NE2000 Packet
  Driver and `PKTCHK` diagnostic.

`scripts/fetch_network_stack.sh` downloads the versioned FreeDOS packages and
requires these SHA-256 values before packaging:

- mTCP: `087ae50048004a8fd23f26f3e3a1bac7985107d1912e9244cf5ec0488432aedc`
- Crynwr: `b5761639a1bf4ad4fa93bfa4643ca62ae46d08412c78d45ae1b5e10d91d0b9c8`

The full image keeps license texts and corresponding source archives under
`C:\NET` and `C:\NET\SOURCE`.

## Start the server

Build and open the normal graphical QEMU profile:

```bash
bash scripts/build_run_full.sh
```

The visual runner enables QEMU user networking by default, exposes an NE2000
at IRQ 3 / I/O `0x300`, forwards FTP control to `127.0.0.1:8021`, and forwards
the required passive range `127.0.0.1:2048-2303`.

The shell searches `C:\NET` directly. At the CiukiOS prompt, run:

```text
netstart
ipconfig
ping 1.1.1.1
ftpsrv
```

`IPCONFIG.COM` reads the path exported by `MTCPCFG` and reports the current
host name, IPv4 address, mask, gateway, DNS, Packet Driver interrupt, and lease
time. It therefore shows values persisted by `DHCP`, rather than hard-coded
QEMU defaults, and reports whether resident ICMP is active. `NETSTART` loads
the physical NE2000 driver on `INT 60h`, installs the resident ARP/ICMP service,
and exposes an mTCP-compatible Packet Driver on `INT 61h`. Incoming Echo
Requests work before, during, and after `FTPSRV`; `Ctrl+C` stops FTP only.

Save IP, subnet mask, gateway and DNS atomically (the previous profile remains
available as `C:\NET\MTCP.BAK`) with:

```text
netcfg static 10.0.2.15 255.255.255.0 10.0.2.2 1.1.1.1
```

`NETCFG DHCP` obtains a lease and reloads the resident responder. `NETCFG
APPLY` reapplies `MTCP.CFG` or starts the network service if it is absent.

The default guest configuration is static:

- address: `10.0.2.15/24`
- gateway: `10.0.2.2`
- DNS: `10.0.2.3`
- physical Packet Driver: `INT 60h`
- mTCP/resident bridge: `INT 61h`
- shared directory: `C:\SHARE`
- user/password: `ciukios` / `ciukios`

Stop the server with `Ctrl+C`.

## Incoming ping from a Linux host

QEMU user-mode NAT is intentionally not a host-routable Ethernet segment: it
supports guest-to-gateway ping and forwarded FTP, but a host `ping 10.0.2.15`
cannot reach the guest. For direct host-to-CiukiOS ICMP plus guest Internet
access, create a TAP endpoint with forwarding (the script asks for root
privileges for TAP setup, IPv4 forwarding, and scoped firewall rules):

```bash
scripts/ciukios_tap.sh up-nat
bash scripts/build_run_full.sh --tap
```

Run `NETSTART` in CiukiOS; `FTPSRV` is needed only for file transfer. From
another host terminal:

```bash
ping 10.0.2.15
curl --noproxy '*' --ftp-skip-pasv-ip -u ciukios:ciukios ftp://10.0.2.15/
```

The host endpoint is `10.0.2.2/24`. `up-nat` discovers the default Linux uplink
and restricts forwarding/NAT rules to the TAP subnet. Use `ciukios_tap.sh up`
for an isolated host/guest LAN. For a custom subnet, set
`CIUKIOS_TAP_HOST_CIDR` and use `NETCFG STATIC` with the TAP host address as
gateway. Remove the interface and its tagged firewall rules when no longer
needed:

```bash
scripts/ciukios_tap.sh down
```

## Linux client

List the share:

```bash
curl --noproxy '*' --ftp-skip-pasv-ip -u ciukios:ciukios \
  ftp://127.0.0.1:8021/
```

Download a file:

```bash
curl --noproxy '*' --ftp-skip-pasv-ip -u ciukios:ciukios \
  -o README.TXT ftp://127.0.0.1:8021/README.TXT
```

Upload a file using an 8.3-compatible destination name:

```bash
curl --noproxy '*' --ftp-skip-pasv-ip -u ciukios:ciukios \
  -T local.txt ftp://127.0.0.1:8021/HOSTPUT.TXT
```

## Windows client

Current Windows installations include `curl.exe`. In PowerShell or Command
Prompt, list and transfer files with:

```powershell
curl.exe --noproxy "*" --ftp-skip-pasv-ip -u ciukios:ciukios ftp://127.0.0.1:8021/
curl.exe --noproxy "*" --ftp-skip-pasv-ip -u ciukios:ciukios -o README.TXT ftp://127.0.0.1:8021/README.TXT
curl.exe --noproxy "*" --ftp-skip-pasv-ip -u ciukios:ciukios -T .\local.txt ftp://127.0.0.1:8021/HOSTPUT.TXT
```

FileZilla can use host `127.0.0.1`, port `8021`, protocol FTP, passive mode,
and the same credentials. Plain FTP must be selected, not FTPS or SFTP.

## Validation

Run the isolated end-to-end gates:

```bash
make qemu-test-full-network-ftp
make qemu-test-full-network-icmp
```

Reports are written to `build/full/qemu-network-ftp.report.txt` and
`build/full/qemu-network-icmp.report.txt`. The FTP gate first proves outbound
Internet ICMP against `1.1.1.1`, then validates transfer. The ICMP gate persists
IP/mask/gateway/DNS, does not start FTP, injects Ethernet ICMP without root
privileges, and rejects malformed or checksum-invalid replies.

## Security and current limits

1. FTP transmits credentials and file contents without encryption. Keep the
   default forwarding on localhost or use only a trusted isolated LAN.
2. Change `config/network/FTPPASS.TXT` before any bridged or physical-network
   experiment. The shipped account is a development default, not a secret.
3. The validated virtual adapter is NE2000. Other Packet Driver/NIC combinations
   need their own configuration and evidence.
4. Static configuration and resident reload are gated. `NETCFG DHCP` is
   integrated but still requires a DHCP server on the selected Ethernet LAN.
5. FAT16 and the current DOS APIs use 8.3 names; clients should choose compatible
   remote names.
6. SMB/CIFS, WebDAV, SFTP, IPv6, TLS, DNS application workflows, and physical
   hardware interoperability remain future milestones.

## Kernel compatibility fixes exposed by mTCP

mTCP also provided a useful external DOS ABI workload. Its OpenWatcom runtime
found and now guards three general kernel issues:

1. `INT 21h/AH=43h` recognizes `C:\` as a directory and returns attribute `10h`.
2. `INT 21h/AH=3Bh` preserves non-result registers across `chdir`.
3. `INT 21h/AH=3Ch` preserves the caller's handle-output pointer in `BX`, so a
   successful create is visible to OpenWatcom and uploaded files can be written.

These are title-independent DOS fixes and are not mTCP-specific bypasses.

`NETSTART.COM` also exposed the standard COM parent-memory rule: DOS initially
assigns the process the whole available arena, so a program that performs a
nested EXEC must first release unused paragraphs. `NETSTART` now moves to its
own transient stack, shrinks its PSP block with `INT 21h/AH=4Ah`, and only then
executes `NE2000.COM` through `AH=4Bh`. Failure output includes the returned DOS
`AX` code. This correction is inside the application and does not add a
networking-program name check to CIUKIDOS.
