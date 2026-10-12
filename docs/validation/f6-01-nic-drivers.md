# F6-01 NIC implementation and qualification boundary

Research before implementation, 2026-10-12. The existing registry provides
generation-bound claims, permanent quarantine and a shared INTx chain with a
250 us aggregate budget. AC97 already uses word-sized PCI Command writes and
pinned, registry-owned physical buffers. NICs use those same services.

Sources checked:

- [Intel 8255x developer manual, January 2003](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/8255x%2C82562%20Ethernet%20Controller%20Developer%20Manual.pdf),
  sections 5, 6.3–6.5 and 7: EEPROM dummy-zero address width, SCB commands,
  simplified transmit/RFD layouts, configuration and MDI status.
  Table 3 specifies a 32-byte I/O aperture; QEMU's
  [eepro100 model](https://github.com/qemu/qemu/blob/v9.2.0/hw/net/eepro100.c)
  advertises 64 bytes. The validated QEMU flag selects that aperture;
  physical devices retain the documented 32-byte aperture.
- [Realtek RTL8139C(L) datasheet, revision 1.4](https://www.alphacron.de/download/hardware/RTL8139_14.pdf),
  sections 6.1–6.8 and 6.12: RX status/FCS, CAPR bias, wrap mode,
  four TX slots, W1C interrupt causes and inverse link status.
- [National Semiconductor DP8390D, July 1995](https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf),
  sections 7–11: page ring, boundary/current separation, remote DMA,
  rounded word transfers and stop/reset state.
- [Realtek RTL8029AS datasheet](https://people.eecs.berkeley.edu/~kubitron/courses/cs194-24-S13/hand-outs/rtl8029as.pdf):
  PCI aperture, station-address PROM and DP8390-compatible registers.
- [RFC 826](https://www.rfc-editor.org/rfc/rfc826): Ethernet/IPv4 ARP
  fields and byte order.
- [QEMU network invocation](https://www.qemu.org/docs/master/system/invocation.html#network-options):
  explicit NIC plus user backend; gateway 10.0.2.2. This ARP test is IPv4
  user-network evidence only, not IPX/LAN qualification.

Implementation decisions within the directive:

- Kernel-owned Ethernet frames, 1514 bytes maximum without FCS, padded to
  60 bytes for transmission. No user or DOS guest buffer is DMA-addressable.
- Bounded interfaces and queues; one kernel thread copies received frames
  and calls the later stack input hook. IRQ callbacks mask/ack their source
  and schedule thread service; they never copy frames, wait or issue DMA.
- PCI root-bus PIIX routing is qualified on the QEMU twins by PIRQ and ELCR.
  Other routing uses documented polling with device interrupts masked.
  Safe mode skips discovery/activation. Firmware-running engines, power
  states other than D0 and conflicting registry claims refuse takeover.
- DMA uses contiguous kernel pages within the direct map (well below the
  devices' 32-bit address mask), with ordering barriers at ownership changes.
  Proven stop detaches callbacks before release; a fault or failed idle
  proof retains all resources and pages until reboot, without retry/reset.
- e100 uses simplified CU blocks and eight RFDs with a moving end marker;
  RTL8139 uses an 8 KiB ring with explicit wrap copying and four TX slots;
  NE2000 uses word-wide PIO remote DMA and a page ring in on-card RAM.
  Its station PROM distinguishes the RTL8029AS packed layout (section 6.2.2)
  from the duplicated word layout in the
  [QEMU NE2000 model](https://github.com/qemu/qemu/blob/v9.2.0/hw/net/ne2000.c)
  using the PROM signature, with host fixtures for both.
- e100 reads MII status twice for latch-low link state. RTL8139 reads MSR.
  DP8390 has no PHY link-status register: NE2000 reports `unknown` while
  running, rather than inventing cable-status evidence.
- The new `f6:net-arp` selector is explicit, outside the existing hardware
  sweep. The probe verifies an addressed ARP reply and TX completion.
  The interface is provisional pending the lead's F6-00 network contract;
  this directive does not introduce sockets or a BSD compatibility layer.

Host and target compilation results are recorded in
`build/codex/f6-01-report.md`. QEMU and physical qualification belong to the
lead and have not been performed by the implementer.
