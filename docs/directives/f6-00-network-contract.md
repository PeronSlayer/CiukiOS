# f6-00 — The F6 network contract (lwIP, NIC drivers, sockets, DOS NE2000/IPX)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` allowed. **Worktree:** `../CiukiOS-wt-f6-00` from `main`. Document-only deliverable, reviewed by the lead before any code.

## Owner request (2026-10-12)

"Manca il supporto alla rete." The owner's 2026-10-09 scope decision places networking at step F6: lwIP TCP/IP, a NIC driver compatibility layer and a virtual NE2000/IPX for DOS VMs; `docs/design/network-foundations.md` lists what the contract must specify.

## Deliverable

`docs/design/network-contract.md` answering every item of `network-foundations.md` with MUST/SHOULD language in the style of `docs/design/posix-subset.md`: lwIP version and configuration (no-OS or OS mode on the kernel's threads, memory pools and limits within the 128 MiB budget), the kernel/lwIP boundary (netif driver interface, packet buffers, interrupt to thread hand-off), the NIC driver layer (PCI probing on the registry, descriptor rings, the first drivers: Intel 8255x (e100, T23 PRO/100 VE), RTL8139, NE2000 PCI, 3Com 3c905; QEMU twins `rtl8139`, `ne2k_pci`, `i82559er`), DHCP/static configuration and DNS, the POSIX socket subset in the SDK (socket/bind/connect/listen/accept/send/recv/select or poll, non-blocking, getaddrinfo) mapped on lwIP's sockets or netconn API, the syscall numbers, the desktop/Control Panel network applet and tray status, the DOS side (virtual NE2000 on a VM with IPX/packet driver bridging to lwIP: scope and deferral), security limits (no inbound services in 0.9), test architecture (host tests with lwIP in a host process, QEMU user-mode networking cases, hardware qualification), and the acceptance criteria per directive (f6-01 drivers, f6-02 stack and sockets, f6-03 configuration and applet, f6-04 browser port). Cite lwIP documentation, the datasheets (Intel 8255x, RTL8139, DP8390) and QEMU device docs.

## Constraints

No code, no commits, no diary, no QEMU. Report to `build/codex/f6-00-report.md` with the open questions for the lead.
