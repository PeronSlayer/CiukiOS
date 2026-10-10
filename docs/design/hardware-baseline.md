# Generic PC hardware baseline (F1–F2)

Directive [f1-20](../directives/f1-20-generic-pc-baseline.md), owner hardware
scope decision [2026-10-11-05](../../dev_diary/2026-10-11-05-pc-generici.md).
Status: implementation handoff; lead review and QEMU qualification pending.
Research and repository comparison: 2026-10-10. Dates on the scope decision
and directive are retained as recorded. This document does not claim that
the new kernel has passed physical qualification.

One canonical image serves the laptops and assembled desktop PCs of roughly
1998–2004. Hardware detection selects existing generic paths at boot.
Profiles are runtime test configurations, never compiler flags, alternative
kernels or per-machine disk builds. Qualification refers to a measured
machine/firmware configuration and image SHA-256.

## Baseline and rationale

| Component | Baseline | Reason and present limit |
| --- | --- | --- |
| CPU | i686 class, CPUID basic leaf 1 and CMOV (`EDX[15]`); Pentium Pro/II/III/4, Celeron, Athlon/Duron/XP and VIA C3 **Nehemiah** | Kernel and SDK use `-march=pentiumpro`. Check features, not a brand string or marketing date. FXSR is optional; SSE is not required. CPU identities alone do not qualify a board. |
| BIOS | Conventional PC BIOS with a valid, complete E820 map; common 1997+ BIOS class | Only normalized type-1 pages are allocatable. E801/AH=88 are diagnostics, never an allocation fallback. UEFI-only systems without a compatible BIOS path are outside this baseline. |
| Boot firmware | INT 13h extensions for the system disk; working A20 through BIOS, i8042 or port `92h`; PCI configuration mechanism #1 | The canonical MBR/FAT32 layout needs reliable disk addressing. Native inventory uses CF8/CFC; optional PCI BIOS discovery is not proof of a qualified driver. PIC/PIT and BIOS ownership follow `device-firmware-ownership.md`. |
| RAM | 64 MiB F0–F2 **scope target**, 128 MiB reference gate; eventual ceiling 3 GiB usable | Current acceptance requires ≥128 MiB, and current direct-map allocator uses only RAM below 768 MiB. Neither the 64 MiB gate nor use of 3 GiB is established by this directive; see contract issues below. |
| Storage | Parallel ATA PIO with legacy primary `1F0h/3F6h` or secondary `170h/376h` task files, MBR and FAT32 system volume | No bus-master DMA required. SATA is a candidate only when firmware presents legacy IDE ports; AHCI/RAID-only controllers are unsupported here. ATAPI install CD is later F4 work. |
| Input | PS/2 keyboard and mouse through i8042 | Generic native backend; retain the exact Armada E500 firmware-first rule. BIOS USB-legacy emulation counts only while it reliably presents the PS/2 interface and remains active. There is no native USB input stack in F1. |
| Display | VBE 2.0+ controller with a validated linear framebuffer at 640×480 or better | F1 uses framebuffer access only. S3, ATI Rage, NVIDIA TNT/GeForce, Matrox, Cirrus, Intel 8xx and Voodoo3 are candidates when their installed BIOS actually offers an eligible mode. Acceleration needs later per-chip qualification. Text/serial recovery remains available. |
| Platform | Compatible 8259 PIC, 8254 PIT and firmware-configured legacy services | Generic paths first; no chipset, power, SMM or thermal-policy takeover. BIOS versions and routing still need physical evidence. |
| Audio / network | No required device at this gate | F4 planning: SB16, SB Live!, ES1370/1371, ICH AC'97. F6 planning: RTL8139, 3C905, NE2000, Intel PRO/100. These are candidates, not active drivers or qualification claims. |

The current F1 boot-volume binding is ATA disk 0, primary partition 1 with
`qualified=0 reason=loader_fingerprints_absent` (f1-12 amendment in
`f1-acceptance.md`). The table describes hardware interfaces, not automatic
boot-volume discovery across every controller/channel. `disk.inc` also retains
its existing BIOS CHS fallback; f1-20 adds CPU/E820/VBE guards, not a new EDD
refusal policy.

Pentium/MMX and the AMD K6 family lack the required integer CMOV baseline
and are unsupported. Earlier VIA C3 variants are not covered by the Nehemiah
claim. This is an owner-revisitable compiler-baseline decision: supporting
i586 would require reviewed `-march=i586` code/SDK policy and renewed evidence.
Adding a separate i586 build variant conflicts with the one-image rule;
f1-20 leaves the canonical flags unchanged.
[Intel optimization manual, §2](https://download.intel.com/design/PentiumII/manuals/24512701.pdf),
[AMD K6 BIOS guide, CPUID tables](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/application-notes/23913a.pdf),
[VIA Nehemiah datasheet, §2.3.2](https://datasheets.chipdb.org/VIA/Nehemiah/VIA%20C3%20Nehemiah%20Datasheet%20R113.pdf).

## Compatibility matrix

“Qualified on hardware” below describes the recorded **legacy 0.8** unit
evidence only. New F0/F1/F2 physical qualification is pending and takes place
once after F2 QEMU closes. “Emulated on QEMU” identifies a defined twin;
new desktop results remain `not_run` until the lead supplies evidence.

| Machine class | CPU | Chipset / display / input / storage | Status |
| --- | --- | --- | --- |
| ThinkPad T23, owner's unit | Pentium III | Intel 830MP/ICH3-M, S3 Savage, PS/2, ATA | Qualified on hardware for recorded 0.8 paths; F0–F2 pending |
| Armada E500, owner's unit | Pentium III | Intel 440ZX-M/PIIX class, ATI Rage Mobility, OEM PS/2 firmware-first, ATA | Qualified on hardware for recorded 0.8 paths; F0–F2 pending |
| 1998 desktop | Pentium II | i440BX/PIIX4, Cirrus or S3, PS/2, ATA | Expected; actual board and video BIOS require qualification |
| 2002 desktop | Athlon XP or Pentium 4 | VIA KT266/VT8233 or Intel i845/ICH, GeForce, PS/2, ATA | Expected; no vendor engine activation implied |
| 2000 desktop | Pentium III / Celeron | Intel i815/ICH, Intel graphics or AGP, PS/2, ATA | Expected |
| 2003 desktop | Athlon XP | NVIDIA nForce2/MCP, GeForce, PS/2, legacy IDE | Expected; generic compatibility only |
| qemu-t23 | pentium3 | pc-i440fx-9.2/PIIX, std VGA, i8042, IDE, 512 MiB | Emulated on QEMU; existing evidence remains image-specific |
| qemu-e500 | pentium3 | pc-i440fx-9.2/PIIX, std VGA, i8042 with forced firmware policy, IDE, 256 MiB | Emulated on QEMU; policy twin, not OEM firmware |
| qemu-desktop-1998 | pentium2 | pc-i440fx-9.2/PIIX, Cirrus GD5446, i8042, IDE, 128 MiB | Emulated on QEMU; defined, lead validation pending |
| qemu-desktop-2002 | athlon | pc-i440fx-9.2/PIIX, Bochs std VGA, i8042, IDE, 512 MiB | Emulated on QEMU; defined, lead validation pending |
| Pentium/MMX or AMD K6 desktop | i586 without CMOV | Any | Unsupported: CPU baseline |
| AHCI-only / USB-only input / BIOS without E820 | Any | Missing required generic interfaces | Unsupported for the affected F1 path |

Add the owner's assembled PCs when measured: board/revision, CPU signature,
BIOS/video-BIOS identity, installed/usable/reserved RAM, PCI IDs, active input
backend, boot-disk path and image hash. A successful QEMU twin cannot replace
that row's physical evidence.

## Primary-source research and repository decisions

Vendor specifications are used even when an original document is hosted by
a university/archive. No source establishes that every card or board in a
marketing family has usable firmware. Hardware targets above are CiukiOS scope
decisions, subject to the generic-interface tests.

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  CPUID instruction, and [AMD CPUID specification 25481](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/design-guides/25481.pdf):
  EFLAGS.ID detects CPUID; basic leaf 1 reports signature and CMOV. Decode
  extended family only for base family 15, extended model for 6/15. Existing
  `src/kernel/core/cpu.c` assumes CPUID and records the signature; the loader
  now guards that assumption before any kernel code executes. FXSR/SSE remain
  optional in the existing kernel feature policy.
- [VESA VBE 2.0](https://www.phatcode.net/downloads.php?action=get&file=vbe20.pdf&id=221)
  and [VBE 3.0](https://pdos.csail.mit.edu/6.828/2012/readings/hardware/vbe3.pdf),
  functions 00h–03h: controller version, LFB availability, framebuffer address
  and mode readback determine eligibility. Existing `video.inc` validates
  pitch, capacity, extent and masks; version 3 uses the separate linear fields.
  The pre-disk check queries the controller without changing mode; configured
  and safe-mode choices still happen after BOOT.CFG/menu.
- [ACPI E820 interface](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html):
  preserve complete 20/24-byte descriptors and restrictive reservations.
  `memory.inc` already required E820; the check now runs before even disk
  parameter discovery and its failure prints an understandable refusal.
- [Intel 440BX datasheet 290633-001](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/82443BX.pdf),
  [Intel i815 GMCH datasheet 298351-002](https://download.intel.com/design/chipsets/datashts/29835102.pdf),
  [ASUS A7V266 vendor manual listing (VIA KT266)](https://www.asus.com/it/supportonly/a7v266/helpdesk_manual/),
  and [NVIDIA nForce2 product overview](https://download.nvidia.com/ndemand/Collateral/Product_Overviews/NVIDIA_nForce/PO_NVIDIA_nForce2.pdf):
  representative PCI/AGP and IDE-era platforms. nForce2 documents PCI IDE,
  AC'97 and Ethernet functions; those optional engines stay disabled. The VIA
  manual's download was unavailable during research; its listing establishes
  provenance, not register behavior. No VIA-specific programming is introduced.
- [PCI Local Bus 2.2](https://www.ics.uci.edu/~harris/ics216/pci/PCI_22.pdf),
  configuration-space and interrupt sections; mechanism #1 is the PC baseline
  in `device-firmware-ownership.md`. Inventory/resource ownership precedes
  activation; enumeration is never license to enable a vendor engine.
- [T13 ATA/ATAPI-6 draft 1410D r3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf),
  task-file, PIO and IDENTIFY rules: legacy ATA paths remain the storage
  baseline. F1 does not enable DMA, ATAPI or AHCI.
- Intel's [keyboard-controller interface](https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html)
  and [SeaBIOS PS/2 source](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/hw/ps2port.c)
  provide the controller/device reference. Existing `input_platform.inc`
  retains only the exact ATI/ESS/subsystem E500 match; desktop profiles have
  no platform override. [SeaBIOS USB HID source](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/hw/usb-hid.c)
  also shows why BIOS USB keyboard service alone does not prove native i8042
  mouse/key-release compatibility. USB-legacy operation requires physical
  qualification across the native controller handoff; otherwise use PS/2.

## Desktop QEMU profiles and deviations

| Profile | Machine | CPU | RAM | VGA | Common runtime configuration |
| --- | --- | --- | --- | --- | --- |
| qemu-desktop-1998 | pc-i440fx-9.2 | pentium2 | 128 MiB | cirrus | TCG, icount shift=1,sleep=on, one CPU, PIIX IDE, i8042, COM1 file |
| qemu-desktop-2002 | pc-i440fx-9.2 | athlon | 512 MiB | std | TCG, icount shift=1,sleep=on, one CPU, PIIX IDE, i8042, COM1 file |

[QEMU 9.2 invocation](https://qemu.readthedocs.io/en/v9.2.0/system/invocation.html)
defines Cirrus GD5446 and Bochs VBE standard VGA and disclaims icount cycle
accuracy. [QEMU v9.2 CPU definitions](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/target/i386/cpu.c)
contain both requested models: pentium2 is family/model/stepping 6/5/2,
athlon is 6/2/3 with the Pentium Pro feature set. Thus no pentium3 fallback
is selected. This is source evidence of model availability, not a local
runtime launch. The repository pins QEMU **major 11** while retaining the
versioned `pc-i440fx-9.2` machine; the lead must record actual version and
firmware hashes and verify both models in that binary.

SeaVGABIOS's [shared VBE implementation](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/vgasrc/vbe.c)
reports version 3.0 and populates linear geometry/masks from its mode data.
Its [Cirrus backend](https://raw.githubusercontent.com/coreboot/seabios/master/vgasrc/clext.c)
offers 24-bit direct-color modes `0112h` (640×480), `0115h` (800×600) and
`0118h` (1024×768), alongside 8/15/16-bit modes; no 32-bit modes appear in
that table. Its LFB address comes from PCI BAR0. The
[Bochs backend](https://raw.githubusercontent.com/coreboot/seabios/master/vgasrc/bochsvga.c)
uses Bochs DISPI/VBE hardware extensions. These backend source links are
upstream master because the corresponding release-tag files were unavailable
through research access; the runtime VGA ROM hash remains authoritative.

Expected from that source table: Cirrus normal mode chooses 1024×768×24,
safe chooses 640×480×24, if queries, memory capacity and readback validate.
`0118h` on Cirrus is **24 bpp**, not an assumed 32-bit standard-VGA mode.
Do not force a mode or modify the loader to compensate for bad ROM data.
Actual chosen mode/pitch/masks and F1 presenter evidence are pending the lead.

Both desktop twins use i440FX/PIIX rather than real i440BX, KT266/i845 or
nForce2; std VGA is not a GeForce. They do not model board SMM, thermals,
USB-legacy handoff, native chip acceleration or real interrupt routing.
Athlon is an older K7 model, not an Athlon XP replica. TCG/icount timing is
synthetic instruction time, separately recorded from host elapsed time,
as in `f0-acceptance.md`; it cannot qualify physical latency or CPU/FPU quirks.

The existing runner already emits CPU, machine, RAM, VGA, IDE and icount
arguments. No new runner field is needed. Each new profile adds ten F0 boots
(five cold, five restart), one run each of the other existing core probes,
and one each of registry, input-fault, native input, framebuffer and safe
(fw_cfg source) in F1. Existing laptop entry-source/fallback cases remain.
All use qcow2 overlays of the same canonical image, unchanged deadlines and
predicates, and no audio/network/USB tablet addition.

## Refusal, diagnostics and unknown hardware

CIUKLDR performs UART discovery, CPU checks, E820 collection/normalization
and VBE controller query before platform disk-parameter discovery or FAT reads.
CPUID absence, a missing basic leaf 1 or missing CMOV emits:

`CIUKI: this PC needs an i686 CPU with CMOV (Pentium Pro or later)`

Invalid/missing E820 emits:

`CIUKI: this PC needs a valid BIOS E820 memory map (L:E820)`

A responding malformed/pre-2.0 VBE controller emits:

`CIUKI: this PC needs VBE 2.0 or later for a linear framebuffer`

Each refusal goes to BIOS text output and available validated COM1, then
CLI/HLT without a keyboard wait, disk access or kernel entry. The MBR has
already read CIUKLDR to reach these checks; “without disk access” means no
further loader disk operations after detecting an unsupported platform.

The loader reports `L:CPU signature=... family=... model=... stepping=...`
as eight hexadecimal digits per field. Frozen boot-info v1 bytes, size and
reserved fields stay unchanged; the kernel's existing CPUID signature and
boot evidence remain authoritative after handoff.

Unknown hardware uses qualified generic VBE LFB, i8042, ATA PIO and PIC/PIT
paths first. Optional vendor drivers require explicit qualification. Safe
mode stays available on supported minimum platforms, records disabled-device
reasons and preserves the E500 required firmware lease. A missing/unusable
LFB retains text/serial recovery; it does not qualify the graphical desktop.

## Contract issues for lead review

1. **64 MiB:** `f0-acceptance.md` requires ≥128 MiB hardware and the boot
   suite asserts that floor. The 64 MiB scope target needs measured
   usable/reserved/free totals, F1 cache and F2 back-buffer/process budgets,
   graceful allocation failure and the same canonical-image gates at 64 MiB.
   No cap, acceptance predicate or allocation budget changes here.
2. **3 GiB usable:** `boot-memory.md` ignores RAM above 768 MiB in F0–F4;
   `loader_model.page_candidates` mirrors that limit. A reviewed memory
   mapping/high-memory contract and allocator changes are necessary before
   3 GiB usable can be claimed. Installing 3 GiB is not using 3 GiB.
3. **VBE absence:** f1-20's unconditional VBE-minimum wording conflicts
   with explicit no-LFB/text fallback in boot-memory/F1 and existing
   `-vga none` regression cases. This implementation preserves absent-VBE
   recovery and refuses responding malformed/pre-2.0 controllers early.
   Requiring VBE even for recovery needs a lead contract decision and suite
   revision; no hidden fw_cfg-only bypass is introduced.
4. **CPU handoff diagnostics:** frozen `ciuki_boot_info` v1 has no CPU
   fields and requires reserved bytes to be zero. The requested identities
   are screen/serial diagnostics plus the existing kernel CPUID evidence,
   not new fields. A handoff-field requirement needs a separate ABI review.
5. **Profile wording:** F0/F1 currently prescribe pentium3/std for every
   evidence profile. f1-20 extends that set with pentium2/Cirrus and
   athlon/std; the lead should reconcile those paragraphs on integration.
   Machine pinning, TCG, one-image and icount rules remain enforced.

## Host validation

The host model covers CPU/leaf/CMOV rejection, optional FXSR/SSE, display
family/model decoding, E820 failure and responding old/malformed VBE versus
absent-VBE recovery. Static call-order checks protect the pre-disk boundary;
profile tests verify effective arguments, ten boots and bounded probe counts.
`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -v`
completed with **98 tests in 33.970 s, OK (skipped=6)**. Skips: three desktop
build-dependent tests, one Lua archive/build test, and two kernel-map tests;
their required full-build outputs are absent in this worktree. No lock
contention occurred. Full output: `build/f1-20-host-tests.log`.
`nasm -f bin src/boot/ciukldr.asm -o build/ciukldr.bin` exited 0; the loader
is 24,064 bytes, within its existing 64 KiB segment limit.
These are host evidence, not execution of BIOS interrupts or CPU refusal in
an emulator. Assembly and full host-test results accompany the handoff;
QEMU `f0-smoke`, desktop `f0-core`, native `f1-input` and `f1-safe` remain the
lead's required validation on one newly built image.
