# Universal peripheral driver coverage and port contract

Research snapshot: 2026-10-22. Directive:
[f1-102](../directives/f1-102-universal-peripheral-drivers-census.md).
Repository inspected: `ffd52fad9d66f9fc2df159b97cfbd1bf246996ee`.
Status: research complete; proposed implementation sequence and interface
extensions require lead cross-review. This directive adds documentation only.
It neither activates hardware nor establishes physical qualification.

The target is one detected-at-boot image for the i686/CMOV, BIOS/E820/VBE,
PIC and parallel-ATA machines in [hardware-baseline.md](hardware-baseline.md),
including assembled PCs. “Universal” means coverage of standard interfaces
and explicit device/revision quirks. It cannot mean every peripheral sold in
1998–2004. Missing interfaces, undocumented extensions and faulty firmware
remain visible unsupported cases. AHCI, HDA, xHCI, Win16, suspend/hibernate,
force feedback and a general printer spooler are outside this proposal.

## Evidence and census method

The three current naming databases contain **1,164 PCI pairs**, **218 USB
pairs**, and **6 native PCI supplements** respectively. The supplements are
already merged into the PCI file; do not add six to 1,164. Their sizes are
63,892, 11,623 and 370 bytes. The historical counts in
[the provenance README](../../assets/hwids/README.md) describe earlier
generation states.

The owner's pack is represented by `assets/hwids/{pci-ids,usb-ids}.json`.
`scripts/build_hwids.py:ID_RE/collect` retains PCI/USB pairs and shortest
resolved descriptions, but discards INF Class/ClassGUID, compatible IDs,
revision/subsystem restrictions, ISA PnP and ACPI IDs. Therefore a per-INF
**Media/HID/Ports/USB/System class count cannot be reconstructed** from these
JSON files. The following rows use those requested class groupings as a
census, with explicit present/missing examples; they are not an invented
classification of the original pack. Names such as “Realtek AC97 Audio”
often describe a codec package attached to different PCI engines. IDs and
names confer neither a match nor permission to probe. No Windows driver
binary or private driver-pack material is needed by the proposed ports.

Primary implementation sources, exact versions, every inspected candidate
file's license and blob identity are in
[peripheral-driver-source-audit.md](peripheral-driver-source-audit.md).
That appendix is part of this contract. Linux **v6.12** is the common
reference, rather than an unpinned moving branch. Alternatives examined are
FreeBSD **release/14.2.0**, Haiku **r1beta5**, NetBSD's `netbsd-10` snapshot
**12c634f788f1d67d0a7dc96fa94fba215f532dc7**, and ACPICA **20260930**.
NetBSD's requested release-style tag was unavailable in its GitHub mirror;
the recorded commit is the immutable comparison coordinate, not a claim to
have fetched that tag.

### Period chipset families

These are representative southbridge/controller families, not a measured
market-share ranking or a guarantee for every motherboard with that name.
The [Linux PCI ID definitions](https://github.com/torvalds/linux/blob/v6.12/include/linux/pci_ids.h),
the corresponding ATA/USB/audio files in the appendix, and the existing
hardware baseline provide the programming-family cross-check.

| Family common in the target period | Peripheral interfaces to expect and qualify | Pack evidence and limits |
| --- | --- | --- |
| Intel 440BX/PIIX4; i810/i815/i830/i845 with ICH–ICH5 | Board Super I/O usually supplies PS/2, COM, LPT and FDC. PIIX/ICH supplies IDE bus master and UHCI; ICH4/5 adds EHCI; ICH adds AC-link audio. PIIX4 PM/ICH PM is a power interface, not proof of valid AML. | Examples present: `8086:2442` UHCI, `8086:24cd` ICH4 EHCI, `8086:24c5` audio. Some early PIIX IDs are absent from the names file. T23 ICH3-M is one member of this family. |
| VIA VT82C586/596/686, VT8231/8233/8235/8237 | Legacy Super I/O peripherals, `1106:0571` IDE; UHCI `3038`; later EHCI `3104`; AC'97 engines `3058/3059`; firmware ACPI/APM varies. | Pack contains `0571`, `3058`, `3059` and bridge names `3074/3147/3177/3227`. A codec label must not select Intel register layout. |
| SiS 5595/630/730/735/645/648/655 with 96x southbridges | Super I/O legacy devices, IDE `1039:5513`, OHCI `7001`, later EHCI `7002`, audio `7012` with its own DMA/status details. | `1039:7012` is present as “Realtek AC97 Audio”; several controller IDs are absent. Treat unknown IRQ links as unknown. |
| ALi Aladdin/M1543, M1535/M1535+, M1563 | Super I/O legacy devices, IDE `10b9:5229`, OHCI `5237`, later EHCI `5239`, audio M5451 and later M5455. | Pack has `10b9:5451/5455`; absence of IDE/USB names does not remove the hardware requirement. M5451 and M5455 are different audio engines. |
| NVIDIA nForce/nForce2 MCP, later nForce3 | PS/2/COM/LPT/FDC where the board provides them; PATA; OHCI companions and EHCI; AC'97 controller distinct from the optional APU. | `10de:006a` audio and `0085` PATA are present. The pack extends beyond the baseline, including nForce4 and newer devices. Early audio `01b1` and USB family coverage must come from controller tables, not name matching. |
| Add-in cards and laptop variants | PCI UART/multiport cards; Promise/HighPoint PATA; SB Live!/Audigy, CMI8738, Yamaha, ESS, Trident; Synaptics/ALPS touchpads. | Examples present: Creative `1102:0002/0004`, game ports `7002/7003/7005`, CMI `13f6:0111`, ESS `125d:1978`, Promise `105a:3373`. That Promise entry is SATA378, not evidence that the PATA models are listed. |

COM/LPT/FDC generally belong to a separate Super I/O or ISA-PnP function;
they are not implied PCI functions of a southbridge. Do not unlock arbitrary
Super I/O configuration ports merely because the chipset family matches.

### Coverage against the actual repository

“Implemented” below means source exists; routing, firmware policy, successful
activation and physical qualification remain separate conditions.

| Class | Typical 1998–2004 devices | Current implementation and actual gap |
| --- | --- | --- |
| HID: PS/2 keyboard/mouse | AT/PS/2 set-2 keyboard, three-button mice, IntelliMouse wheel, Explorer five-button mice, TrackPoint; laptop touchpads | `drivers/i8042.c` has raw set-2 and `packet[3]`; initialization requires AUX ID 0. Wheel/Explorer are refused. Preserve E500 firmware-first matching and command/reply separation. Extended keyboard consumer-key mappings are also separate from USB HID work. |
| HID: touchpad absolute mode | Synaptics absolute packets, ALPS versioned absolute/relative interleaving, pass-through sticks | No absolute touchpad driver. PS/2 compatibility mode remains useful. Optional versions must be selected by device query, not laptop branding. |
| USB: HID | Boot keyboards, report keyboards and multimedia keys; mice; composite keyboards; USB gamepads/joysticks | `usb/usb_hid.c` parses mouse report descriptors, including wheel and button fields; keyboard uses boot reports. No general keyboard/Consumer-page or game-controller parser. Mouse parser is not a ready-made generic HID parser. |
| Ports: serial | Super I/O 16550A; older 8250/16450/broken 16550; 16650/16750 and PCI multiport cards; serial mice/modems | `core/serial.c:serial_init/serial_irq` is a bounded IRQ-driven log transmitter. It distinguishes verified FIFO/one-byte operation and conventional IRQ3/4 bases; it is not simply a polling console. Scratch-register validation excludes an original 8250. No RX/user COM API, PnP serial enumeration, serial mouse or DOS UART service exists. |
| Ports: parallel | LPT SPP/PS/2 bidirectional, IEEE 1284 EPP/ECP, printers and copy-protection dongles | No native LPT engine or guest LPT service. Packet-print output does not establish register- or timing-sensitive dongle compatibility. |
| HID/Ports: game port | ISA 201h; PCI audio game-port functions, analog two-stick/four-axis/four-button wiring; some proprietary digital devices | No analog port, normalized joystick API, virtual 201h or INT15/AH=84 service. Creative game-port IDs are in the names database. Digital SideWinder/Gravis protocols and MIDI are additional work. |
| System/storage: floppy | 82077AA/compatible Super I/O, 3.5-inch 1.44 MB, 720 KB and some 5.25-inch drives | No physical FDC driver, removable-media lease or guest virtual FDC. `049f:0054` and `055d:2020` USB FDD names are USB devices, not ISA FDC evidence. USB UFI/CBI transport must be checked separately from existing BOT storage. |
| USB host | Intel/VIA UHCI, SiS/ALi/AMD/NVIDIA/NEC OHCI, later EHCI plus companions | UHCI/OHCI, hub, HID and BOT exist. **EHCI companion routing already exists** in `usb_core.c:ehci_route`: ownership semaphore, halt, BME disable, CF=0. There is no high-speed EHCI transfer engine (`ops` remains absent). Extend this path; do not add a second handoff owner. |
| System: power | PIIX4/ICH/VIA/SiS/ALi/MCP ACPI; older 1998–2000 laptop/desktop APM 1.2 firmware | `thinkpad_ec.c` is SMBIOS-gated vendor support, not generic ACPI EC/AML. No generic power button/lid/battery/thermal/soft-off backend. Existing ownership contract forbids unreviewed ACPI/APM takeover. |
| Media: audio | ICH/VIA/SiS/NVIDIA/ALi AC-link, SB16, ES137x, ESS Maestro, SB Live!/Audigy, CMI8738, Yamaha DS-XG, Trident 4DWave | AC'97, VIA82xx, ES137x and SB16 exist. **SiS 7012 and nForce `01b1/006a` already match** `ac97.c:supported/controller_layout`; non-Intel routing uses `audio_pci.c:audio_pci_routed` with limited `$PIR`/ELCR rules. Remaining work is qualification/quirks and additional revisions, not a new SiS driver from zero. ESS and the other engines listed here are missing. |
| Storage: ATA | Legacy task files plus firmware-configured BMIDE; different vendor timing/cable rules; PCI PATA add-in cards; early SATA compatibility mode | `ata_dma.c` already implements SFF PRDs and firmware-selected UDMA checks. `ata_dma_pci_base` requires legacy channels, and `ata_dma_mode/restore_mode` constrain IDENTIFY state. Gap: native-mode resources, chipset timing negotiation/errata and add-in-controller interfaces. AHCI/RAID-only paths remain outside the baseline. |

Repository paths in this section are under `src/kernel/` unless qualified.
The [unified DOS contract](unified-dos-contract.md#memory-and-device-boundaries)
retains the native VMM as owner. `vm/dosvm_bios.c:dosvm_io` currently returns
all-one reads and discards writes for unregistered ports; it does not pass
UART/LPT/FDC/game-port operations to physical hardware. Its existing virtual
PIC/PIT/DMA, keyboard, mouse, disk and audio services are the integration
points. This is Windows 95/98-style V86 device mediation, not NT NTVDM.

## Protocol decisions from primary sources

These are proposed implementation constraints, to be made binding by the
lead's follow-up directives. They resolve the census gaps without silently
changing existing ownership or memory policy.

### Input

[Microsoft's PS/2 wheel and five-button specification](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers#activating-buttons-4-5-and-wheel-on-ps2-mice)
and Linux `psmouse-base.c:intellimouse_detect/im_explorer_detect` agree:
send sample rates 200,100,80 and read ID 3; then 200,200,80 and read ID 4.
ID 3 uses a signed eight-bit fourth wheel byte; ID 4 uses a signed four-bit
wheel field plus buttons 4/5 in bits 4/5. Commit packet size only after the
ID is validated. Retain the negotiated ID 0/3/4 and a proven fallback; a
failed transaction is not permission to blindly reset the controller or
parse an unknown stream as three bytes. Separate ACK/RESEND, unsolicited
AUX bytes and keyboard traffic, including disconnect/resync. Normalize wheel
sign once at the existing input queue boundary. Explorer 4.0 horizontal
scroll and vendor extensions need their own capability evidence.

Optional Synaptics and ALPS ports should borrow the query/packet algorithms
from their audited Linux files. Start with period-appropriate identified
versions, one absolute event stream, explicit coordinate ranges, pressure
and palm/tap policy, plus pass-through stick separation. Do not enable
absolute mode on E500's firmware-owned controller. Packet fixtures must
exercise relative/absolute interleaving and return to compatibility mode.

[USB HID 1.11](https://www.usb.org/sites/default/files/documents/hid1_11.pdf),
§§6.2.2, 7.2.5 and Appendix B, separates report and boot protocols. The
[USB-IF HUT 1.12](https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf)
defines Keyboard page 07h, Consumer page 0Ch, Generic Desktop joystick/game
pad usages, axes/hats and Button page 09h. Add a bounded common report
decoder with report IDs, signed ranges, array/variable fields, collections,
push/pop, constant padding and checked bit arithmetic. Keep boot fallback
for a keyboard that advertises it. Handle rollover, releases after detach,
LED output and composite interfaces. Consumer usages become explicit
native actions; do not reinterpret them as set-2 scan codes. Normalize
gamepad axes/hats/buttons with per-device calibration and lifecycle before
projecting them onto DOS controls. Generic HID does not cover XInput-only,
vendor-specific or force-feedback devices automatically.

### Serial, parallel and game port

[TI's UART register documentation](https://www.ti.com/product/TL16C550D)
and Linux `8250_port.c:autoconfig_8250/autoconfig_16550a/autoconfig_has_efr`
support capability-based detection. Restore every modified register; test
scratch/loopback only on an acquired inactive port. Distinguish 8250 from
16450, a broken 16550 FIFO from a 16550A, and EFR/32-byte 16650 from the
DLAB-sensitive 64-byte 16750. Clone ambiguity means the safe verified FIFO
depth, not a guessed chip name. Do not run destructive detection on the
live logging UART. PCI UART register stride/clock/multiport IRQ layout comes
from the exact ID table, not the default ISA clock.

BIOS COM1–COM4 bases are four words at physical 400h; three LPT bases start
at 408h. On PS/2-compatible BDA layouts, 40Eh is **EBDA segment**, not a
fourth usable LPT address. [SeaBIOS's BDA definition](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/std/bda.h)
confirms these fields. Validate zero/FFFF/overlap and use firmware/PnP/PCI
resources to establish routing. COM numbering is a retained logical map,
not four hardcoded bases. Common 3F8h/2F8h/3E8h/2E8h addresses do not prove
that IRQ4/3 can be shared by separately registered legacy handlers.

Microsoft/Hayes **Plug and Play External COM Device Specification 1.00**
(1995), available as `pnpcom.rtf` in
[Microsoft's WHDC archive](https://www.microsoft.com/en-us/download/details.aspx?id=53426),
§2.1 requires port availability first. Its
[archived original text](https://citeseerx.ist.psu.edu/document?doi=f48af868c7395b5a8357780c5663011f5ec572fc&repid=rep1&type=pdf)
specifies 1200 baud 7N1 and timed DTR/RTS/DSR negotiation, nominal 200 ms
stages, per-character timeout and a 2.2-second collection bound. Validate
length, delimiters and checksum before binding. The original PDF's indexed
§2.1 text and Microsoft's `serial/serenum/enum.c` were checked; the official
RTF could not be decoded by the available web fetcher. The latter sample is
behavioral research only, not a proposed NT driver transplant. No enumerate
on the kernel logger, an open modem, a guest lease or a conflicting IRQ.
Retain explicit user selection for non-PnP mice. Linux `sermouse.c` supplies
Microsoft/Logitech M+/MZ and Mouse Systems decoders: test Microsoft-style
1200/7-bit framing, Mouse Systems five-byte/8-bit framing, middle buttons,
signed movement, malformed streams and packet resynchronization separately.

Linux `parport_pc.c` and `ieee1284_ops.c` are the chosen implementation
references for SPP, bidirectional nibble/byte, EPP address/data cycles and
ECP direction/FIFO negotiation. Claim the proven base plus optional high
ECP range, IRQ and DMA as independent extents: PCI base_hi need not equal
base+400h. Negotiate only supported modes, clear EPP timeout correctly and
restore direction/control on close. SPP output and a bounded reverse path
come first; ECP DMA needs its own stop proof. DOS printers can use INT17 and
virtual registers. Dongles require a physical-port lease and actual timing
qualification; neither generic printing nor a virtual register model proves
all protection schemes work.

Linux `ns558.c/analog.c/gameport.c` describe a 201h analog game port: a write
starts the RC measurement, axis bits 0–3 decay and button bits 4–7 are
active-low. Discovery must avoid conflicting MIDI/audio functions and
arbitrary ISA scans. Use a bounded sampling worker with measured timing,
calibration and disconnected-axis timeout. Do not transplant upstream long
interrupt-disabled timing loops into Ciuki's IRQ latency policy. DOS gets
a virtual 201h RC model and INT15/AH=84 on normalized state; guest polling
does not repeatedly sample the physical port or change the host PIT.

### Floppy and USB host

[Intel 82077AA, order 290166-007](https://datasheet4u.com/pdf-down/8/2/0/82077AA_IntelCorporation.pdf),
register/command/result phases, and Linux `floppy.c` establish a stateful
FDC, not a sector-read loop. Support motor spin-up/off, reset/recalibrate,
seek, media-change, rate/geometry, ST0–ST2, write protection, terminal count,
bounded retry and full read/write error propagation. Initial media are
1.44 MB and 720 KB; qualify 1.2 MB/360 KB/2.88 MB separately. ISA DMA2 uses
pinned memory below 16 MiB with no 64 KiB crossing. Claim IRQ6 and the
implemented split register ranges: **do not claim 3F0h–3F7h wholesale**, since
3F6h belongs to ATA alternate status/control. Secondary FDC resources require
their own evidence. DOS virtual FDC/INT13 uses an exclusive removable-media
provider; guest DMA addresses never become physical bus-master addresses.
Native raw media read/write is separate from native FAT12 mounting. A future
FAT12 adapter is needed before promising Explorer file browsing; a DOS-owned
FAT12 disk must not have a simultaneous native filesystem writer. This work
does not restore the prohibited floppy boot/build profile.

[Intel EHCI 1.0](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
§§2.3.8–9, 4.2 and Appendix B, distinguishes firmware ownership from routing
to companions. Preserve the existing `ehci_route` handoff and replace CF=0
only after a high-speed engine and all required companions can serve their
ports. Use per-port PortOwner for low/full-speed root devices; disconnect
returns routing to EHCI. High-speed hubs need transaction-translator split
scheduling for downstream low/full-speed devices; unsupported TT topology
must be explicit, never a false successful enumeration. Implement qTD/QH
control/bulk and periodic interrupt scheduling, asynchronous advance/unlink,
short packets, cancellation and lost-IRQ fallback before claiming full HID
and storage coverage. Reject unaddressable DMA rather than truncating.

Existing UHCI/OHCI quirk work remains authoritative. Extend by specific
vendor/device/revision and observable condition, using Linux `pci-quirks.c`,
`uhci-pci.c` and `ohci-pci.c` as references: Intel/VIA over-current polarity,
early Intel queue advancement, AMD756 register erratum and NEC lost-IRQ
behavior are different problems. SiS/ALi/nForce OHCI is a programming family,
not permission to apply every Linux quirk. Newer AMD/ATI/xHCI cases in these
files are outside this period. Qualify legacy SMM handoff and real port
pairing on Intel ICH4, VIA, SiS, ALi and NVIDIA boards separately.

### Power

Use **ACPICA 20260930**, the upstream interpreter/runtime, through a Ciuki
OS Services Layer (OSL). Its audited runtime consists of 164 C files and 43
headers (3,499,399 source bytes); debug/disassembler/tool/foreign-OS glue
is excluded from the initial candidate set. ACPICA provides AML, table,
namespace, resource and event machinery; **it does not supply Ciuki's battery,
lid, thermal, PCI routing, EC transport or desktop shutdown policy**.
The [release](https://github.com/open-acpica/acpica/releases/tag/20260930)
announces BSD-3-Clause OR GPL-2.0-only. Twelve selected files still contain
the older Intel/custom BSD-like/GPL alternatives. Elect their explicit
**GPL version 2** alternative, and GPL-2.0-only for the newer files. Preserve
the original notices. Do not relabel all older text as standard BSD-3-Clause.
The appendix enumerates every file and exception.

The [ACPI software model](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html)
requires table validation, namespace/resource discovery, EC OperationRegion
handling, qualified SCI routing, fixed/GPE dispatch and ownership sequencing.
Separate read-only table discovery from ACPI_ENABLE/event activation.
Use an OSL work queue, waits, locks, monotonic timer, bounded allocation and
checked port/MMIO/PCI access. AML methods can access hardware: a generic
AcpiOsRead/Write function must not bypass registry ownership. Map tables
read-only, preserve ACPI NVS, reject out-of-range addresses/widths and enforce
execution/cancellation budgets through reviewed interpreter seams. One worker
must not block required EC/GPE progress. Neither a firmware timeout nor AML
allocation failure adds a new panic path.

[ACPI device objects](https://uefi.org/specs/ACPI/6.5/09_ACPI_Defined_Devices_and_Device_Specific_Objects.html)
provide button/lid notifications;
[power-source objects](https://uefi.org/specs/ACPI/6.5/10_Power_Source_and_Power_Meter_Devices.html)
define AC adapter status and control-method batteries.
Read `_LID`, `_PSR`, `_BIF`/`_BIX` and `_BST` where actually present;
validate battery units and unknown values. Bind generic EC before dependent
AML. ThinkPad EC remains a DMI/SMBIOS-specific quirk: at most one owner of
62h/66h and one event consumer, with vendor operations routed through the
generic transaction service or disabled. Never run two EC protocol engines.

[ACPI 6.5 thermal management, chapter 11](https://uefi.org/sites/default/files/resources/ACPI_Spec_6_5_Aug29.pdf)
is a takeover prerequisite: evaluate valid zones, trips and required fan/
passive-cooling methods; retain firmware policy if unsupported. Lid close
does not imply suspend. Soft-off follows desktop application closure, native
flush/quiesce and evaluated S5 preparation/entry; a failed shutdown retains
safe diagnostics and uncertain leases. No S3/S4 promise is made here.

[Intel/Microsoft APM 1.2](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF),
§§4.3/4.6, is an exclusive alternative for some early machines. Probe version,
entry points and capabilities without connecting. A later reviewed firmware
broker may negotiate the protected-mode interface and poll events once per
second; notify processing at least every five seconds and reject ordinary
suspend while unsupported. Critical suspend cannot be rejected; connecting
requires an emergency quiescence policy first. APM offers less detailed
device/thermal/lid information than ACPI; unsupported status stays unknown.
Do not connect both or silently fall back to APM after partial ACPI takeover.

The owner's read-only Windows 98 Resource Kit, `wrkc30_4.htm:54–129`, was
checked: Windows 98 distinguishes OS-directed ACPI from BIOS-directed APM
and qualifies ACPI BIOSes. `wrkc05_3.htm:42` describes firmware-resource and
hardware-profile discovery. These are period-model context, not a license to
reuse Microsoft implementation. No Resource Kit text is copied into this tree.

### Audio and storage

Extend Ciuki's existing PCM backend contract, kernel-owned staging, codec
restore, IRQ budget and DMA stop/quarantine rules. ALSA's PCM/control/TTY/
libata/driver-core infrastructure is **not** being ported wholesale. Borrow
bounded register/descriptor algorithms and exact ID/revision quirks, replacing
upstream platform services with the existing Ciuki adapters.

| Engine/codec gap | Chosen reference and reason | Admission limit |
| --- | --- | --- |
| Existing ICH/SiS7012/nForce; ALi M5455 | Linux `sound/pci/intel8x0.c`; existing engine is close and already has SiS/nForce branches. Reuse audited codec files below. | Qualify each layout, IRQ link and codec; SiS/nForce are partial existing coverage. Later NVIDIA IDs and M5455 are not enabled by matching a name. |
| ALi M5451 | Linux `sound/pci/ali5451/ali5451.c` | Dedicated voice/interrupt engine, 31-bit DMA. Do not confuse with M5455/Intel AC'97. |
| ESS Maestro1/2/2E | Linux `sound/pci/es1968.c` | Needed for E500's `125d:1978`; APU/wavecache, 28-bit DMA and window constraints, not Intel BDLs. Early non-AC'97 codecs need separate evidence. Maestro3 firmware is outside this choice. |
| EMU10K1/10K2, SB Live!/Audigy | Linux `sound/pci/emu10k1/{emu10k1_main,emupcm,emufx,irq,memory,io,voice,emumixer}.c`, `include/sound/emu10k1.h` | Stereo PCM first, chip-specific DMA/page maps and generated DSP routing. E-MU professional FPGA firmware paths excluded. Audigy LS `1102:0007` is a different engine, not covered by this port. |
| CMI8338/8738 | Linux `sound/pci/cmipci.c` | Dedicated native mixer/DMA; legacy SB/FM/MPU/game-port decode stays disabled until separately leased. |
| Yamaha YMF724/740/744/754 (DS-XG) | Linux `sound/pci/ymfpci/{ymfpci,ymfpci_main}.c` and `ymfpci.h` | Requires `yamaha/ds1_dsp.fw` plus `ds1_ctrl.fw` or `ds1e_ctrl.fw`. Driver GPL permission does not license firmware. The inspected ALSA v1.2.4 `ymfpci/hwmcode.c` has a Yamaha all-rights-reserved header; redistribution terms remain unresolved, so firmware packaging is blocked pending a specific grant. |
| Trident 4DWave DX/NX; SiS7018 | Linux `sound/pci/trident/trident_main.c` and `trident.h` | 30-bit DMA, voice engine; SiS7018 differs from SiS7012. No hardware proof from ES137x or AC'97 emulation. |
| AC'97 codec identification and quirks | Linux `sound/pci/ac97/{ac97_codec,ac97_patch}.c`, local/patch headers and `include/sound/ac97_codec.h` | Read 7Ch/7Eh IDs under the proper engine lease; apply masked codec/revision/subsystem quirks, with bounded ready/power/rate/mute restoration. |

The initial codec census from `ac97_codec.c` includes Analog Devices
AD1819/1881/1881A/1885/1886/1887/1888/1980/1981/1985; Cirrus
CS4297/4297A/4298/4299/4201/4205; Realtek ALC100/200/202/650/655/658;
SigmaTel STAC9700/9701/9704/9708/9721/9744/9750/9752/9756/9758;
Wolfson WM9701/9703/9704/9705/9707/9708/9710; AKM AK4540/4542/4543/4544A;
C-Media CMI9738/9739/9761; Yamaha YMF743/752/753; and TriTech TR2802x.
This is a representative list to qualify, not a whitelist based on a PCI
label. Exact IDs/masks and anomalous aliases come from the audited table;
unrecognized codecs start with verified generic stereo capability only.
PCM, codec response, analog output and speaker wiring are separate results.

For storage, choose Linux `ata_piix.c` for PIIX/ICH, `pata_via.c`,
`pata_sis.c`, `pata_ali.c`, and `pata_amd.c` (also NVIDIA PATA) for vendor
timing/cable rules. Existing BMIDE remains the baseline transport. Do not
increase UDMA solely because IDENTIFY advertises it: controller revision,
cable, shared-channel timing, clock and errata must agree. Quiesce channel
and outstanding requests before a mode change; verify programmed state and
disk selection. Preserve safe PIO only when the DMA engine is proven stopped;
uncertain DMA stays quarantined, without BIOS retry.

Promise splits into `pata_pdc202xx_old.c` and `pata_pdc2027x.c`; HighPoint into
`pata_hpt366.c` and `pata_hpt37x.c`. Their PCI task-file layouts, timing/PLL
and clocks need exact model selection. Support plain disks first; RAID
metadata, array assembly and proprietary option-ROM behavior require later
contracts. SATA378 in the owner's names is not authorization to treat it as
a PDC2027x. AHCI is a separate native SATA interface outside the current
hardware baseline, even on a late-period board; no AHCI directive is proposed.

## Port size, host adapters and tests

Estimates below are engineering ranges for **new/adapted Ciuki code**, not
measured binary sizes or approved memory allocations. Source sizes/line
counts in the appendix are measured text inventory. KiB figures are likely
resident code and data order of magnitude, excluding normal shared kernel
services. Allocate only for detected/admitted devices. Measure actual usable,
reserved and free memory on target profiles before approving any budget or
changing current limits.

| Gap / source set above | Estimated adaptation / resident addition | Required Ciuki host API | Host model and later test path |
| --- | --- | --- | --- |
| PS/2 wheel/Explorer | 0.5–1.5 kLOC; 4–12 KiB, bounded packet state | Existing i8042 command sequencer, input queue, generation/resync | Byte-exact commands/ID/ACK/RESEND/timeouts and 3/4-byte traces; QEMU default PS/2 wheel/Explorer; physical wheel, KVM and E500 policy. |
| Report keyboard, consumer keys and USB controllers | 3–6 kLOC; 32–64 KiB plus bounded per-interface layouts | Common HID parser, USB interrupt transfers, normalized input capabilities, LED output | Descriptor corpus/fuzzing, overflow, rollover, detach releases, hats/axes; QEMU boot keyboard/mouse only covers a subset; real composite/consumer/gamepad hardware. |
| Synaptics/ALPS optional | 2–5 kLOC per family; 24–64 KiB | i8042 query/transaction lease, absolute input/calibration, pass-through identity | Captured/synthetic packet/query fixtures; hardware only for mode changes, palm/tap and pass-through. |
| UART, ISA PnP and PCI serial | 3–6 kLOC; 24–64 KiB plus bounded RX/TX rings per port | Port/IRQ claims, PnP resource discovery, shared-PCI or aggregated-legacy interrupt dispatch, waitable character I/O and per-open configuration | Register models for all six UART generations, bad FIFO/clock/stride, cancellation, overflow and logger conflict; QEMU ISA COM and `pci-serial`; real 16650/16750/multiport. |
| Serial mouse and DOS COM | 2–4 kLOC; 16–40 KiB plus per-VM UART state | Exclusive mouse/COM leases, input queue, virtual UART/IRQ3/4, INT14, virtual BDA | Wire/PnP parser and DLAB/FIFO/modem-status models; two-VM detach/isolation; QEMU chardev-driven bytes; real mouse electrical and timing qualification. |
| Parallel SPP/EPP/ECP and guest LPT | 2–5 kLOC; 24–64 KiB; optional bounded DMA bounce | Split port/IRQ/DMA claims, mode/direction transactions, character/raw policy, virtual LPT/INT17 | Busy/ACK/errors/direction/EPP timeout and ECP FIFO model; QEMU SPP output; real EPP/ECP/printers/dongles. |
| Analog game port and DOS joystick | 1–3 kLOC; 12–32 KiB plus calibration/state | Timed sampling worker, normalized joystick API, virtual 201h/INT15 | RC timing model, stuck axes, active-low buttons and VM clock; hardware only for physical sampling. USB descriptor tests reuse common HID set. |
| FDC read/write and DOS media | 4–8 kLOC; 40–96 KiB plus ≤64 KiB per in-flight ISA bounce proposal | DMA2 allocator/pins, IRQ6, deadlines/workers, block request queue, media generation/exclusive provider, virtual FDC/DMA/INT13 | Command/result/DMA boundary/motor/media-change/error/write-protect models; QEMU `isa-fdc` with disposable data media; physical read/write/CRC/no-media. |
| EHCI and chipset quirks | 6–12 kLOC; 64–160 KiB plus 128–512 KiB/controller working estimate | Existing USB host ops extended for HS, periodic/split scheduling, constrained DMA, IRQ/workers, ownership and companion topology | Fake RAM/register schedule walker, W1C and unlink/stop fault injection; QEMU `usb-ehci` and explicit companions; physical SMM/port mapping/TT/errata. |
| ACPICA + generic ACPI services | 164 upstream C units + Ciuki OSL/classes about 5–10 kLOC; roughly 300–700 KiB runtime code, dynamic AML/namespace memory measured separately | Full `acpiosxf.h` OSL, region arbitration, EC transport, SCI/GPE workers, resource/PCI routing, power event API and ordered shutdown | Fake OSL and AML fixtures for tables/regions/EC/GPE/battery/lid/thermal/S5, allocation and timeout faults; QEMU tables/button/S5 subset; real firmware/thermal/battery required. |
| APM 1.2 fallback | 1.5–3 kLOC; 16–40 KiB plus firmware workspace | Exclusive bounded firmware-call broker, entry descriptors, event worker, emergency quiesce | Negotiation/event/critical-suspend model; actual APM BIOS hardware; do not count QEMU ACPI off as APM conformance. |
| Existing AC'97 variants + codecs | 1–4 kLOC incremental; 16–48 KiB plus existing stream buffers | Existing `audio_backend`, codec leases, constrained DMA, qualified IRQ routes | Mock codec/register/BDL/stop tests; QEMU `AC97` baseline only; actual SiS/nForce/ALi/codec/analog paths. |
| ESS Maestro | 3–6 kLOC; 32–64 KiB, 64–256 KiB constrained buffers estimate | PCM adapter, APU/wavecache indexed register lock, 28-bit/window-constrained DMA | Register/voice/window/stop model; E500 and other identified boards, hardware only. |
| EMU10K1/10K2 | 6–12 kLOC; 64–160 KiB, page tables + 128–512 KiB buffers estimate | PCM, generated DSP program, per-chip address mask, page-map pinning, IRQ stop proof | DSP routing/page-map/interrupt model; SB Live!/real Audigy hardware only. |
| CMI8738 | 2–4 kLOC; 24–48 KiB + 64–128 KiB buffers estimate | PCM/native mixer/DMA, independent legacy-function leases | Register/rate/stop model; real card only. |
| Yamaha DS-XG | 3–6 kLOC; 32–80 KiB + firmware + 64–256 KiB DMA estimate | PCM, verified firmware loader, endian-safe control banks and DMA | Firmware length/hash/license checks, bank/mixer/stop model; hardware only and no redistributed firmware until terms resolved. |
| Trident and ALi M5451 | 3–6 kLOC each; 32–80 KiB + 64–256 KiB DMA estimate | PCM, indexed voice access, 30-/31-bit masks respectively | Address/voice/interrupt/stop models; hardware only. |
| Chipset ATA timing/native task files | 1–3 kLOC/family; 8–32 KiB/family plus existing queues | ATA request/flush/generation service, PCI BAR/clock/cable facts, transactional mode change and DMA quiesce | Table/mode/cable/clock/PRD fault fixtures; QEMU PIIX functional subset; each real chipset/revision. |
| Promise / HighPoint PATA | 2–4 kLOC/family; 24–64 KiB plus bounded transfer buffers | Same ATA API, model-specific task files/BMIDE/timing, qualified IRQ and boot-provider binding | PLL/timing/native-BAR/timeout/flush models; real plain-disk cards only. No QEMU Promise/HPT device assumed. |

FreeBSD's audited UART (1,129 text lines) and FDC (2,109), NetBSD's FDC
(1,694), and Haiku's standard PS/2 mouse (517) are useful comparative
sources. BSD bus_space/bus_dma/newbus, NetBSD autoconf and Haiku's C++ device
manager would still need adapters. Choose Linux algorithms for consistency
with existing Ciuki drivers and broader matching tables, rather than carry
multiple foreign driver frameworks. Their permissive licenses permit
selective future reuse with notices; they do not make porting free of work.

## Device model, claims and guest boundaries

The existing [ownership](device-firmware-ownership.md),
[Device Manager](device-manager.md) and unified DOS contracts remain in
force. The following extensions need a written directive before code:

1. **Discovery facts before activation.** Retain BDA/PnP/ACPI/PCI origin,
   compatible IDs, revision, parent, resources, routing proof and reason.
   Add read-only PnP/ACPI discovery records; do not activate from a name
   lookup, UI refresh or ID-table match. ISA PnP isolation/config writes,
   if required, are a separate claimed operation with restore/quiesce rules.
2. **Existing registry is the authority.** It has PORT/IRQ/DMA/MMIO resources,
   half-open extents, generations, activation, quiesce, retained quarantine
   and explicit takeover proof. Add logical device/function/VM lease IDs
   above it, rather than make overlapping independent physical claims.
   The inspected inventory copy has 64 resources/32 PCI functions; ABI v1
   exports 64 devices/16 drivers/64 leases. Measure occupancy/truncation
   before selecting capacities. No silent enlargement or memory-limit change.
3. **Parent/child and discontiguous resources.** i8042 owns keyboard/AUX
   transport; HID interfaces and game-port/audio child functions do not
   claim it again. FDC excludes ATA's 3F6h. ECP claims its high range; EC
   and ACPI regions may overlap parent-owned functions only through an
   explicit access broker. Claims must precede every probe that writes.
4. **Interrupts.** Qualified level PCI routes may use the current shared
   chain; handlers test/ack their own source within the existing budget.
   ISA IRQ3/4/6/7 remain edge/exclusive unless a new aggregate legacy owner
   has a reviewed dispatcher. UART logger keeps its current claim. Do not
   reuse the PCI level chain for ISA just because two COM ports list IRQ4.
   SCI outside the supported PIC/routing model is refused explicitly.
5. **Stop is not timeout.** Mask local sources, stop DMA, drain queued
   completions, invalidate generations, detach callbacks and prove idle
   before release. An uncertain controller retains buffers and claims in
   quarantine; clients receive bounded errors. A quarantined child must not
   reset a controller with live peer input/storage. No new `panic()` paths.
6. **Firmware and OSL arbitration.** Power starts with read-only table and
   SCI_EN inventory. Existing F4+ ACPI/APM prohibition is not amended by
   f1-102. Lead approval must identify the replacement thermal/event/EC
   owner and qualified routes before ACPI_ENABLE, APM connection or SMI
   changes. Reject unsupported address widths without truncating 64-bit
   ACPI addresses into the registry's 32-bit fields.
7. **Device Manager visibility.** Keep v1 ABI bytes/numbers stable. Existing
   PORT, KEYBOARD, MOUSE, DISK, SOUND and SYSTEM classes cover initial
   records. Propose a separately versioned/paged inventory extension for
   ACPI/PnP identifiers, stable parent/child locator, capability bits,
   joystick class, current logical/VM owner and explicit discovered,
   unsupported and routing-unqualified states. Do not reuse USB VID/PID
   fields for ACPI strings or mark discovered hardware READY. Until the
   extension lands, retain truthful generic records and truncation flags.
   Hardware name lookup stays side-effect-free. This also addresses the
   existing EHCI route-only record being distinguishable from HS readiness.
8. **Native device API.** Propose waitable, cancellable character devices
   for COM/LPT, with typed fixed-width configuration/status operations,
   checked buffers, per-open rights, explicit exclusive mode, bounded rings
   and generation-aware close/detach. Naming (`COM1` alias versus `/dev/com1`)
   and ioctl ABI belong to the lead's interface directive. Logger, mouse,
   native app and VM cannot independently configure one UART. Gamepad input
   needs a versioned capability/state API; do not overload mouse events.
9. **DOS services are virtual frontends.** Add bounded `dosvm_port_register`
   handlers plus INT14/INT17/INT15 and virtual BDA data as appropriate.
   “Passthrough” means a VMM-managed exclusive physical service behind a
   virtual UART/LPT/FDC, never unrestricted I/O bitmap access. Virtual IER,
   DLAB/divisor, FIFOs, line/modem status and IRQs are per VM. DMA2 and
   controller reset operate on guest state; VMM owns physical requests.
   Close/kill removes guest ownership while in-flight physical objects
   remain pinned until stopped. Two VMs cannot acquire the same raw port or
   removable medium. Native file service remains authoritative for mounted
   volumes; unmounted removable block ownership is an explicit alternative.

| New class | Physical claims / special quarantine | Device Manager and DOS result |
| --- | --- | --- |
| PS/2/absolute input | Parent input ranges 60h/64h, IRQ1/12; preserve firmware lease and original failure stage | Protocol/capability child records; existing virtual keyboard/mouse projection |
| USB HID / EHCI | Existing host BAR/IRQ/DMA lease plus per-interface generations and companion parent relation | Class records and actual speed/protocol/route-only status; normalized guest input, no guest HC ownership |
| UART/serial mouse | Eight-byte proven base (or exact PCI extent/stride), qualified IRQ; log UART excluded from reconfiguration | Logical COM resources/open owner; virtual COM and optional mouse projection |
| LPT | SPP/EPP and separate ECP extents, IRQ7/5 if proved, optional DMA channel; retain uncertain DMA | PORT modes and guest lease; INT17/virtual registers, physical dongle caveat |
| Game port | Proven 201h or PCI BAR extent; optional sound-card enable belongs to parent; sampling is bounded | Game-controller capability/calibration record; virtual 201h and INT15 |
| FDC | Split register ranges, IRQ6, DMA2; pinned ISA buffers/media generation | Controller/drive/media children; virtual FDC/INT13 with exclusive provider |
| ACPI/APM/EC | Table/NVS reservations, validated PM/EC regions, SCI and region broker; exclusive backend | SYSTEM power/AC/battery/thermal records with unknown values; DOS cannot power off host directly |
| Audio/ATA variants | Exact BARs/routes/DMA masks, existing request accounting; retain all uncertain DMA | Existing SOUND/DISK records plus chip/revision/codec/mode/reason; existing virtual audio/native disk services |

## Proposed directive order and user-visible milestones

Numbers below are proposals for lead allocation, not newly issued directives.
The order favors generic interfaces shared by many boards and reuses common
parsers/services before less common card-specific work. It is an engineering
priority estimate; the pack cannot establish prevalence. Dependency and
physical-device availability may change the order during cross-review.
Every row needs host evidence, source/license manifest and explicit failure
acceptance before integration. F4/power subjects retain their phase gate
even when tracked by an F1 census follow-up number.

| Proposed ID | Deliverable / dependency | Result visible to the user |
| --- | --- | --- |
| f1-103 | PS/2 ID 0/3/4 negotiation, packet/resync tests; preserve E500 policy | Standard wheel and five-button mice work; failure retains a diagnosed supported input path. |
| f1-104 | Bounded common HID report parser, report keyboard/consumer keys and joystick capabilities; versioned input API | More USB keyboards and gamepads work; media keys have explicit supported actions. |
| f1-105 | Controller/companion inventory, chipset-specific ownership/quirk census and truthful route-only status | Device Manager explains which USB ports/controllers can serve a device and why activation is refused. |
| f1-106 | EHCI control/bulk/interrupt engine, unlink/stop, companion and TT support; depends on 105 | High-speed storage and mixed-speed input work on qualified USB 2.0 ports. Unsupported TT topology is reported until implemented. |
| f1-107 | Existing AC'97 SiS/nForce/VIA/ICH routing/codec qualification; codec-name/mask data; additional M5455 only after layout tests | Onboard sound works on more desktops; codec identity and failure reason are visible. |
| f1-108 | PIIX/ICH then VIA/SiS/ALi/NVIDIA ATA timing/cable/native-mode resource contracts, in separately reviewed substeps | Qualified controllers get a safe negotiated transfer mode; PIO fallback remains explicit. |
| f1-109 | ACPI/PnP read-only inventory, device hierarchy/paged ABI, ACPICA OSL and region/SCI/EC tests | Device Manager shows firmware-described devices and power capabilities before takeover. |
| f1-110 | Generic ACPI button/lid/AC/battery/thermal/S5 backend; requires 109 and explicit F4+ ownership amendment | Power button and shutdown work; battery/lid status is accurate on qualified firmware; suspend stays disabled. |
| f1-111 | UART generations, BDA/PnP/PCI resources, native COM ABI and logger arbitration | Native applications can open qualified serial ports without corrupting kernel logs. |
| f1-112 | Serial PnP/mouse protocols and virtual UART/INT14 leases; depends on 111 | Legacy serial mice and DOS COM applications use an assigned port with accurate errors/status. |
| f1-113 | Analog game-port driver, calibration and virtual 201h/INT15; reuse 104 joystick state | Analog and USB controllers can drive DOS joystick input without exposing physical I/O. |
| f1-114 | LPT SPP/bidirectional first, EPP/ECP next, INT17/raw lease and DMA stop evidence | DOS printer output and explicitly qualified parallel peripherals work; timing-sensitive dongles remain device-specific. |
| f1-115 | FDC read/write, removable-media ownership, virtual FDC/INT13; separately specify native FAT12 if needed | DOS can access an assigned physical floppy; native raw read/write works. Explorer mounting is not claimed before FAT12 support. |
| f1-116 | ESS Maestro1/2/2E PCM, codec and E500 routing qualification | Armada E500 can use its native audio engine. |
| f1-117 | EMU10K1/10K2 stereo PCM and mixer, generated DSP routing | SB Live!/original Audigy provide native PCM sound. |
| f1-118 | CMI8338/8738 PCM/mixer; independently leased legacy functions | Common inexpensive PCI sound cards play native and virtual DOS audio. |
| f1-119 | Yamaha firmware rights decision before DS-XG port/packaging | YMF7xx sound only when the required microcode can be lawfully supplied and tested. |
| f1-120 | Trident 4DWave and ALi M5451 as separate engine subdirectives | More late-1990s PCI/onboard sound engines work. |
| f1-121 | APM 1.2 broker/backend after emergency-quiesce decision; exclusive alternative to 110 | Some pre-ACPI PCs gain qualified battery/power-off status without pretending to provide ACPI detail. |
| f1-122 | Optional identified Synaptics/ALPS absolute protocols | Touchpad scrolling/tap/absolute behavior improves on qualified laptops; compatibility mode remains available. |
| f1-123 | Promise/HighPoint plain-disk PATA backends; depends on 108 resource/mode contract | Qualified add-in PATA disks become usable; RAID and AHCI remain separate work. |

## Verification boundary and acceptance

No QEMU, systemd scope, image build or hardware probe is part of f1-102.
The commands below are **future lead validation recipes**, not executed
results. Host-test results for this research change are recorded in the
worktree report `build/codex/f1-102-report.md`.

QEMU **v9.2.0 source** was checked for the names below because the baseline
retains `pc-i440fx-9.2`; this is not an assertion about the installed QEMU 11
binary's compiled device set. The lead must record that binary's version,
firmware and `qemu-system-i386 -device help` output before selecting models.
Use one canonical image, runtime test selection and disposable overlays;
do not build per-controller images. The current QEMU suspension/allowed
exceptions still apply; this contract does not authorize peripheral runs.

| Proposed source-confirmed QEMU setup | What it can establish | What remains physical or host-model-only |
| --- | --- | --- |
| Default i8042 PS/2 mouse, QMP relative movement/buttons/wheel | ID 0/3/4 state machine and packet delivery (`hw/input/ps2.c`) | Real AUX timing, KVM switches, E500 firmware, Synaptics/ALPS; no invented `-device ps2-mouse` recipe |
| `-device usb-kbd`, `usb-mouse`, `usb-tablet`, `usb-hub` | Basic HID/descriptor, detach and routing behavior | Consumer/composite/NKRO/gamepad descriptor corpus needs host fixtures and hardware; tablet is absolute, not a wheel-mouse substitute |
| `-device usb-ehci,id=ehci` plus explicit `ich9-usb-uhci1/2/3` using `masterbus=ehci.0,firstport=...`, or the upstream ICH9 config | HS transfers and companion topology/state, regression of UHCI/OHCI; `pci-ohci` supplies OHCI model | Real BIOS/SMM semaphore/port pairing and chipset errata. Stock full-speed `usb-hub` does not establish HS-hub TT behavior. |
| Existing ISA COM, `-device pci-serial,chardev=...` with socket/file character backend | Basic 16550 interface, fake-wire RX/TX, virtual COM/IRQ isolation | 8250/16650/16750 detection uses host register models and actual cards; serial mouse power/identification uses actual devices |
| `-device isa-parallel,chardev=...` with controlled default LPT duplication | SPP output and guest API/register behavior | EPP paths delegate to host parallel ioctls; ECP emulator reads are FFh and writes ignored. No pure-emulation ECP/dongle timing qualification. |
| `-device isa-fdc` with disposable floppy **data** media and controlled default-controller duplication | Read/write/IRQ/DMA/error paths and guest media isolation | Physical motor/seek/rate tolerance, CRC/media-change/write protection; no floppy-profile build or boot |
| Existing PC ACPI/PIIX4 PM, controlled power-button injection and S5 | ACPICA table/AML/fixed-event/software shutdown subset | OEM EC/GPE, thermal/fan, battery/lid, broken firmware, APM 1.2 and actual loss of power |
| `AC97`, `ES1370`, `sb16`; `ich9-intel-hda` explicitly excluded | Regression of existing emulated audio transport | No assumed EMU10K1/CMI/YMF/ESS/Trident/ALi model. HDA is another engine and proves none of these. |
| Existing PIIX IDE | Native PIO/BMIDE behavior and generic ATA error/flush paths | VIA/SiS/ALi/NVIDIA timing/cable/PLL and Promise/HighPoint hardware; synthetic table fixtures are not disk qualification |
| No stock generic analog game-port/gamepad model assumed | Host RC/descriptor/virtual-port models only | Physical game port, USB gamepad and proprietary digital controllers. USB host-device passthrough still requires real hardware. |

QEMU references: [USB documentation](https://github.com/qemu/qemu/blob/v9.2.0/docs/system/devices/usb.rst),
[companion config](https://github.com/qemu/qemu/blob/v9.2.0/docs/config/ich9-ehci-uhci.cfg),
[PCI serial](https://github.com/qemu/qemu/blob/v9.2.0/hw/char/serial-pci.c),
[parallel](https://github.com/qemu/qemu/blob/v9.2.0/hw/char/parallel.c),
[ISA FDC](https://github.com/qemu/qemu/blob/v9.2.0/hw/block/fdc-isa.c),
[PS/2](https://github.com/qemu/qemu/blob/v9.2.0/hw/input/ps2.c),
[EHCI models](https://github.com/qemu/qemu/blob/v9.2.0/hw/usb/hcd-ehci-pci.c),
[OHCI model](https://github.com/qemu/qemu/blob/v9.2.0/hw/usb/hcd-ohci-pci.c),
[USB HID models](https://github.com/qemu/qemu/blob/v9.2.0/hw/usb/dev-hid.c).
These are test references; no QEMU implementation is proposed for the kernel.

For each later driver require: successful enumeration and correct resources;
absence/malformed identity without writes; conflict and stale-generation
rejection; finite timeouts; start/stop/detach/close loops; concurrent peer
progress; DMA stop proof and retained quarantine under injected faults;
read-only inventory with honest reasons; native API bounds; two-VM isolation
and reset/exit cleanup; measured memory/IRQ cost. Acceptance distinguishes
source reviewed, host modeled, QEMU exercised and physical qualified for each
exact PCI/subsystem/revision/BIOS/codec/media configuration. Record image
SHA-256, serial/disk evidence and limitations; never generalize one successful
T23 or emulator path into “all retro PCs”.

Lead decisions still required are the sequence/IDs and interface cross-review,
the inventory/COM/joystick ABI, ACPI/APM phase and thermal/emergency ownership,
firmware redistribution for Yamaha, native FAT12 scope and available physical
qualification matrix. None prevents completion of this research-only census;
all precede their respective implementation or distribution.

## Lead decisions (2026-10-22 23:48)

The lead reviewed this contract and the source audit; both are accepted as binding with these decisions:

1. **Order and IDs** f1-103–f1-123 are allocated as proposed.
   - **Wave 1 starts now in parallel:** f1-103 (PS/2 wheel and five-button), f1-104 (HID report parser, consumer keys, joysticks, versioned input API), f1-106 (USB controller inventory of f1-105 plus the EHCI engine), f1-109 (ACPI/PnP read-only inventory with ACPICA and its OSL) and f1-111 (UART generations and the native COM ABI).
   - **Wave 2 follows their merges:** f1-107, f1-108, f1-110, f1-112–f1-118, f1-120–f1-123.
2. **ABIs.** Each implementation directive first writes its versioned ABI into this contract: input, inventory/PNP/ACPI, COM, LPT, joystick, and the native-versus-VM lease. Host ABI tests come with it, and the lead reviews them at merge. Measure memory before any growth; no limit changes without measurement.
3. **Power ownership.** ACPI takeover is approved as the generic path (f1-109 read-only, f1-110 events, thermal prerequisite first). APM 1.2 (f1-121) is the exclusive alternative for pre-ACPI machines.
   - **APM emergency policy:** on a critical suspend or low-battery event, flush and quiesce storage and DMA within a bounded budget, then acknowledge. Ordinary suspend stays rejected.
   - **ThinkPad EC:** stays a quirk routed through the generic EC service.
4. **Yamaha DS-XG (f1-119)** is deferred: no microcode is shipped without an explicit redistribution grant.
5. **Native FAT12 mounting** is approved as a separate follow-up, f1-115b, so floppies open in the file manager.
6. **QEMU.** Peripheral probes in QEMU wait for the owner's approval under the current suspension. Until then the evidence is host models plus the hardware trials.
