# F6-08 second NIC set: implementation and qualification boundary

Research before implementation, 2026-10-13. The existing `net.c` seam uses
registry claims, contiguous pinned DMA, a shared INTx callback and a thread
poller. The second set uses the same lifecycle, counters, bounded queues and
`[net]` records; it does not introduce another PCI scanner or network stack.

Manufacturer programming references checked:

- [Intel 8254x software developer manual, 317453](https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf):
  legacy 16-byte descriptors, 128-byte ring alignment/length, IOADDR/IODATA,
  EEPROM EERD, RAL/RAH, CTRL/STATUS and reset. CTRL.RST clears RAH.AV;
  read the permanent MAC from EEPROM and explicitly restore the address filter.
  Use an uncached 128 KiB BAR0 MMIO mapping on 82540/82545 copper controllers;
  accept 32-bit BARs or 64-bit BARs with a zero high half, outside the RAM
  direct map. The [QEMU 9.2 e1000 model](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/net/e1000.c)
  exposes an inert I/O BAR, so indexed I/O cannot satisfy the emulated gate.
  Claim MMIO before mapping; revoke the mapping on proven healthy stop,
  retain it and DMA pins on quarantine. Check access width/alignment/extent.
  Checksum offload stays disabled. Other Intel families
  require their own initialization and are not admitted by a broad vendor match.
- [AMD Am79C973/975 datasheet, 21510D](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/21510.pdf):
  CSR/BCR access, 32-bit software style 2 initialization/rings, OWN/STP/ENP,
  negative buffer sizes, CSR0 STOP/INIT/IDON and MII. Use word I/O and bounded
  initialization, eight receive descriptors and four transmit descriptors.
  The 970/970A's link LED status and the 973's MII status are separate paths.
  Software pads TX and strips RX FCS, with hardware pad/strip disabled.
- [DEC 21143 hardware reference, EC-QWC4D-TE](https://manualzz.com/doc/37144430/digital-equipment-21143-reference-manual):
  CSR0/5/6/9, serial ROM, MII/SIA, chained descriptors, setup frames and FCS
  length. Use a perfect filter setup frame for the adapter and broadcast MACs,
  never unconditional promiscuous mode. 21140 uses an external MII PHY;
  21143 also has a built-in SIA media path. Board-specific non-MII 21140 media
  wiring needs separate hardware evidence.
  MDI is sampled in the low clock phase, before the next rising edge: the
  [QEMU Tulip register model](https://raw.githubusercontent.com/qemu/qemu/master/hw/net/tulip.c)
  advances its serial read value on that edge. Host fixtures cover this timing
  as well as the normal PHY data phase. QEMU's static PHY/SIA carrier values
  do not establish real cable-disconnect behavior.
- [3Com 3C90x/3C90xB technical reference, 89-0766-000](https://bitsavers.org/communications/3Com/89-0766-000_3Com_3C90x_3C90xB_NICs_Technical_Ref_199808.pdf):
  command/window registers, EEPROM node address, MII, UPD/DPD lists,
  upload completion and download versus transmission completion. Admit
  9050/9055/9200 copper variants. Use eight UPDs and one outstanding DPD;
  require a transmit-complete indication as well as download retirement.
- [VIA VT6102 datasheet, August 1999](https://people.freebsd.org/~wpaul/VIA/VT6102_021.PDF)
  and [VT86C100A datasheet](https://people.freebsd.org/~wpaul/VIA/VT86C100A03.pdf):
  byte/word command registers, chained DMA descriptors, FCS length,
  ownership and bounded MIICR operations. Use eight RX/four TX descriptors,
  aligned private buffers and no VLAN/checksum extensions for VT6105.
  Rhine I lacks Rhine II's MIDLE bit: use its direct MII programming mode,
  rather than waiting on a nonexistent bit. Rhine III's integrated PHY is at 1.

Linux upstream drivers were checked for variant/erratum differences and are
GPLv2 references, not imported implementations:
[e1000](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/intel/e1000/e1000_hw.c),
[pcnet32](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/amd/pcnet32.c),
[tulip](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/dec/tulip/tulip_core.c),
[3c59x](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/3com/3c59x.c),
[via-rhine](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/via/via-rhine.c).
These moving URLs are research references only, never build dependencies.
Code is written from register/descriptor specifications; no Linux code is copied.

Lifecycle decisions: claim before selector writes, refuse firmware-active
engines, prepare descriptors and DMA addresses with bus mastering disabled,
then enable mastering and start. Interrupts only mask/ack/publish cause bits.
Thread service validates descriptor addresses/lengths before copying and retires
TX independently of interrupts. Reset/stop waits are bounded. An unproved stop
or controller fault retains DMA and registry claims until reboot.
The first MII paths for Tulip/3Com/Rhine advertise 10/100 half duplex and keep
their MAC in half duplex. Intel uses automatic MAC speed/duplex detection;
PCnet retains its documented automatic media selection. Full-duplex media
qualification and board-specific PHY/GPIO wiring remain physical test work.

Each new adapter owns seven contiguous pages (28 KiB) for descriptors and
private payloads. Existing static contexts and receive queues are reused;
no unbounded allocation or extra worker is introduced. These object sizes
are not a measurement of available RAM on physical targets.

Device Manager continues to resolve labels through `assets/hwids/pci-ids.json`.
Six missing manufacturer labels are supplied through `native-pci-ids.json`;
regeneration fills gaps while retaining the owner's INF-derived labels.
The fixed sixteen-row driver catalogue includes detected new NICs, replacing
unbound older NIC rows when necessary, without changing the public ABI.
An ID label is not proof of hardware support. Host results and the exact
allowlist/limitations are recorded in `build/codex/f6-08-report.md`.
QEMU e1000/pcnet/tulip link and ARP evidence, and all physical qualification
(especially 3Com/Rhine), remain the lead's responsibility.
The existing `f6-net` runner matrix retains its twelve first-set cases and
adds e1000/pcnet/tulip on the four established profiles (24 cases total).
New READY predicates require the selected driver, fixed MAC, INTx and carrier
up; ARP, budget and proven cleanup predicates remain mandatory. The runner
admits these three explicit NICs only with `f6:net-arp`; hardware-only drivers
and arbitrary network argument strings remain refused.
