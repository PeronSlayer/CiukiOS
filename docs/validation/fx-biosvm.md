# BIOS service isolation and present leases

The T23 trial named in `docs/directives/fx-biosvm.md` faults during APM
real-mode connect on `OUT 0xED`. The private capture was inspected in place.
The later requests select a different VBE identifier with the desktop's
1024x768x32 layout and 4096-byte pitch. No private capture is reproduced here.

## Research and implementation decisions

- [Ralf Brown's port 0080 entry](https://fd.lod.bz/rbil/ports/other/p0080.html)
  identifies manufacturing/POST output and leaves read semantics unspecified.
  [OSDev's IO_WAIT example](https://wiki.osdev.org/Inline_Assembly/Examples#IO_WAIT)
  describes unused-port writes as imprecise short waits.
- [Linux io_delay.c](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/refs/heads/master/arch/x86/kernel/io_delay.c)
  implements 0x80, 0xED, software-delay and no-delay options. Its DMI overrides
  avoid 0x80 on systems that can lock up on that access. This is the evidence
  for 0xED as a delay convention, not a universal chipset delay register.
- Intel's [PIIX4 datasheet, 290562-001](https://dtsheet.com/doc/491890/intel-fw82371)
  and [ICH2/ICH2-M datasheet, 290687-002, table 6-2](https://www.intel.com/content/dam/doc/datasheet/82801ba-i-o-controller-hub-2-82801bam-i-o-controller-hub-2-mobile-datasheet.pdf)
  describe legacy I/O decoding. PIIX4 table 5 lists 0x80 as a reserved DMA page
  register and notes that its writes are broadcast to ISA. ICH2 decodes 0x80 as DMA scratch and forwards
  writes to LPC/PCI; 0xED has no fixed decoded register in that table. Neither
  document establishes a portable hardware-neutral 0xED register.
  Consequently the BIOS broker absorbs byte writes to exactly 0x80 and 0xED
  and returns 0xFF on byte reads, with no physical access. Trapping and software
  emulation already serialize these instructions; this is not a calibrated
  microsecond timing guarantee. Adjacent ports, wider accesses, and DOS guest
  policy remain unchanged.
- The local Windows 98 Resource Kit, chapter 30, `wrkc30_4.htm`,
  “Understanding Advanced Power Management”, describes BIOS-owned APM and
  firmware failures on some machines. It does not justify disabling unrelated
  video calls. The broker keeps permanent, separate input, APM and video fault
  latches. A fault never retries its family. Input/video hardware leases stay
  quarantined only for the affected family; shared initialization/ownership
  failures remain fail-closed. The generic V86 monitor still disables a failed
  execution. Only the broker admits another, healthy service after that
  execution has returned and temporary access has been revoked.
- Present leases compare width, height, depth and pitch rather than mode IDs.
  Equal layouts require no firmware preparation, mode transaction or generation
  change. A changed pitch still requires a transaction. A returned present
  lease already verified its restored layout; a missing desktop repaint is
  logged without issuing another firmware mode set.
- The [display driver model](../design/display-driver-model.md) owns native
  mode programming. Its optional present handoff publishes a verified native
  layout after releasing temporary register/aperture aliases and claims; the
  driver keeps its desktop snapshot until `end` and `complete`. Win32 uses this
  provider before firmware for supported packed modes. Generic code contains
  no graphics-family selection. Native refusal before mutation may use firmware;
  a failed native entry must restore before any firmware fallback.

Host evidence and outstanding hardware validation are recorded in
`build/codex/fx-biosvm-report.md` by the implementer. No image or guest execution
is part of this change's host verification.
