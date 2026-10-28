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

## Review round 1: APM soft-off and panel mode ownership

Lead amendment 1 supersedes native-first selection on an unqualified flat
panel. The private serial and disk records were read in place. Register
readback alone did not establish a usable panel scanout.

- X.Org [Savage's manual](https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man4/savage.4.html)
  defaults `UseBIOS` on because mobile LCD timings need firmware knowledge.
  The upstream [2.3.9 source release](https://www.x.org/releases/individual/driver/xf86-video-savage-2.3.9.tar.bz2),
  inspected locally, sets that default in `savage_driver.c:1352`, identifies
  CR6B bit 1 as ActiveLCD at lines 982–988, and implements the BIOS mode
  transaction in `savage_vbe.c:59–93`. CiukiOS must refuse native panel
  entry before blanking, VRAM transfer or clock programming, restore the
  temporary register locks, and release its claims. This clean refusal is
  not a hardware strike. Only the single CRT head retains native admission;
  panel, dual-head, TV and unknown routing use firmware or remain windowed
  on firmware failure. There is no native retry after a panel BIOS failure.
- The generic display service owns the fallback for both present leases and
  DOS. [VESA VBE 2.0, sections 4.4–4.7](https://www.phatcode.net/res/221/files/vbe20.pdf)
  defines 4F01 mode information, 4F02 bit 14 for LFB, 4F03 verification and
  4F04 state save/restore. The DOS firmware transaction admits only the four
  existing synthetic DOS VBE modes, validates the actual geometry, pitch,
  pixel format and physical aperture before borrowing the stopped desktop's
  framebuffer, and revokes its alias before restoring desktop ownership.
  The existing guest buffer, banked copy path and direct LFB lease remain
  the transport. No raw guest INT10 forwarding is added.
- The local Windows 98 Resource Kit `wrkc30_4.htm`, “Understanding Advanced
  Power Management”, assigns APM power control to BIOS. Linux's
  [APM implementation](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/apm_32.c)
  and [APM constants](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/apm_bios.h)
  corroborate real-mode connect/version negotiation and all-device Off.
  Replay the production APM backend through the production BIOS broker and
  V86 monitor: 5301 at F000:462D writes ED, 530E negotiates 1.2, 5307 uses
  BX=0001/CX=0003 after shutdown proof. A returning Off call remains an
  error: host transport success cannot prove that physical power disappeared.
  ACPI S5 belongs to the separate directive and is unchanged here.

## Review round 2: DAC precision and quarantined video ownership

- [VESA VBE 2.0, section 4.2 and section 4.11](https://www.phatcode.net/res/221/files/vbe20.pdf)
  specifies six-bit DAC precision after a mode set. Eight-bit operation
  requires an explicit 4F08 change. [SeaBIOS 1.16.3, vbe_104f08](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/vgasrc/vbe.c)
  implements that separate format service. Keep BIOS full-screen modes at
  their mandated six-bit default, without extra firmware calls or budget.
  The existing native packed setup selects and checks SR1B bit 4 for an
  eight-bit CLUT. Export the provider's indexed precision through the
  kernel-only display contract: six-bit guest components pass unchanged to
  a six-bit DAC and expand only for an eight-bit DAC. Direct-colour scanout
  requires no palette upload. No guest or public SDK ABI changes are needed.
- Windows 98 Resource Kit, chapter 30, `wrkc30_4.htm`, “Understanding
  Advanced Power Management”, assigns APM power control to the BIOS;
  [Linux's APM power-off implementation](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/apm_32.c)
  also issues a dedicated firmware power request. Neither source defines
  CiukiOS's quarantine mechanism. The local admission decision is to
  distinguish an active video transaction from permanently retained video
  resources: a completed fatal video execution must still pin its leases,
  disable subsequent video entry, and revoke temporary I/O/mappings, while
  permitting a healthy APM family after the worker returns. Cleanup clears
  full-screen transaction metadata without reclaiming quarantined hardware.
