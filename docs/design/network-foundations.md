# Network foundations (F6): what must be specified before code

Decision record, 2026-10-09 (dev diary 2026-10-09-10). The owner requires
complete networking for LAN play. This file lists what the F6 contract has
to settle; it is written when F5 closes and reviewed like every contract.

## Decisions already taken

- TCP/IP stack: [lwIP](https://savannah.nongnu.org/projects/lwip/)
  (BSD-3-Clause), running in the kernel, exposing POSIX sockets to native
  programs (`execution-abi.md`, F2 POSIX subset) and the winsock layer of
  Wine in F8.
- NIC drivers: through a driver-compatibility layer in the style of Haiku's
  FreeBSD compatibility layer, so the era's cards come from existing
  BSD-licensed drivers; first targets RTL8139, NE2000 PCI, 3Com 3c905,
  Intel 82557/8 (e100); CardBus/PCMCIA cards for the laptops to be chosen
  from what the owner has.
- DOS VMs: a virtual NE2000 with selected, qualified DOS drivers; guest
  IPX frames over an Ethernet bridge; no guest bus-master DMA; guests never
  access the physical NIC (`dos-dpmi-contract.md`, "Network for DOS VMs").
- DHCP client, static configuration, and a small control panel in the
  desktop.

## To specify in the F6 contract

- Resource registry claims for NICs (PCI IRQ sharing, bus-master DMA
  buffers per `device-firmware-ownership.md`).
- The compatibility layer's API surface (bus space, DMA mapping, interrupt
  handlers, timers, mbufs) and how much of FreeBSD's `net80211`-free wired
  path is needed.
- lwIP configuration: sockets API, memory pools at 128 MiB, threading model
  with the kernel scheduler.
- Virtual NIC model for VMs: frame injection, virtual IRQ, PIO-only with
  no guest bus-master DMA, Ethernet bridging of guest IPX frames, and the
  MAC policy for several VMs on one physical card.
- Gate workloads: a DOS IPX game (DOOM with IPXSETUP) and a native UDP game
  (Quake) between the T23 and the E500 over a switch. Automated Ethernet
  tests use one QEMU and a host frame peer (a tap or socket backend driven
  by the runner), never two simultaneous QEMUs and never user-mode NAT for
  IPX, which cannot form an Ethernet LAN.
