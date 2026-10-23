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

#### f1-103 implementation contract (2026-10-22)

Research checked Microsoft's linked PS/2 packet tables and
[Linux v6.12 `psmouse-base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/psmouse-base.c)
(`intellimouse_detect`, `im_explorer_detect`,
`psmouse_process_byte`, `psmouse_receive_byte`) against Ciuki's current
ID-0-only activation, three-byte decoder and merged input queue before code.
Use the two Microsoft rate sequences below while AUX reporting is disabled.
Accept an already negotiated ID 4 directly; F6 is not proof of ID 0. After
each complete sequence, a fresh supported ID 0/3/4 proves the packet format;
ID 0 or 3 is a supported fallback when the mouse declines an upgrade.
ACK errors, exhausted RESEND, unknown IDs and timeouts retain the existing
quarantine and failure stage, without resetting either device or controller.
The existing 200 ms reply / 500 ms total setup budgets remain unchanged.
The canonical i686 layout measures 12,524 to 12,532 bytes per controller
(8 bytes for the packet gap clock, including alignment); stats remain 76
bytes. The native and private fixture together grow by 16 static bytes.
Queue, registry, heap and memory limits are unchanged.

Existing input event ABI v1 layouts and numbers remain unchanged: native
button codes 0..4 become desktop button numbers 1..5; RESYNC retains bits
0..4 of the button bitmap. `INPUT_WHEEL` is positive upwards, matching USB;
negate the signed PS/2 wheel once in the native decoder. No horizontal mode
sequence, new input ABI, resource claim, or firmware-owned activation is added.
The f1-104 versioned input ABI remains a separate directive.

Reject invalid headers and Explorer reserved fourth-byte bits. Preserve
possible suffix headers after a malformed Explorer packet, and discard an
incomplete packet after an inter-byte gap greater than 500 ms (Linux's
half-second lost-synchronization precedent), or on a controller parity/timeout
status. This is bounded stream recovery, without controller writes. PS/2 has
no checksum or unique delimiter: byte loss that still forms a legal packet
cannot always be detected, particularly with ID 3's unrestricted wheel byte.
Host traces cover detectable corruption, lost bytes and gap recovery; physical
KVM/disconnect timing and peripheral qualification remain lead validation.

#### f1-104 input extension, version 1

The existing 40-byte `ciuki_input_event`, its types 1–6, input grant,
single-consumer queue and physical resource claims stay byte compatible.
`ciuki/input_abi.h` defines an independently versioned extension. New event
types 7–12 are keyboard HID usage, consumer action, joystick axis, hat,
button and device lifecycle. Their `value2` is a stable logical interface
locator (1–256), `generation` is the USB device generation, and `lost_count`
is the extension version (1). Keyboard usages are page 07h (0–255), with
0/1 transitions; existing translated key/text events remain available.
Consumer actions are volume up/down, mute, play/pause, next and previous
(1–6), with 0/1 transitions and one action on each press. They never become
scan codes. The desktop uses its existing mixer worker for volume and sends
explicit transport actions to the running native Music Player.

Axes X/Y/Z/Rx/Ry/Rz/slider/dial/wheel (0–8) are normalized to -32767..32767
using per-interface minimum/centre/maximum calibration, initially derived
from descriptor logical limits. Hats (0–3) are -1 neutral or 0–7 clockwise
from north; four-way hats occupy even directions. Buttons (1–32) are 0/1.
No joystick data is projected onto mouse events or physical guest I/O.

`INPUT_HID_GET_CAPS` (4901h), `INPUT_HID_GET_STATE` (4902h) and
`INPUT_HID_CALIBRATE` (4903h) use `ciuki_devctl` on the existing input grant.
All structures start with uint32 version, size, device and generation;
exact size/version and reserved zeros are checked before writing. GET_CAPS
accepts generation zero for discovery; state/calibration require an exact
generation. Locators encode host/device/interface record slots, not VID/PID,
and are never reassigned before reboot. Capabilities retain keyboard usages,
the six consumer actions, axes, hats, buttons, logical/physical limits and
units. State queries recover held state after queue RESYNC. Detach publishes
releases, neutral axes/hats, then lifecycle value 0; capabilities remain
queryable with attached=0. Calibration requires the input grant's existing
exclusive authority and minimum < centre < maximum within logical limits.
Queries and publication use the existing UP synchronization boundary.
Publication runs under the existing host transfer mutex/preemption hold;
queries copy stable records inside the short UP IRQ exclusion. These ioctls
do not grant an application, DOS VM or interface its own physical HC lease.
Boot fallback preserves its legacy event stream and allocation footprint;
the versioned keyboard state query still exposes its held HID usages.

| Version-1 record | Bytes | Selected fixed offsets |
| --- | ---: | --- |
| `ciuki_hid_caps` | 340 | keyboard bitmap 56; axis metadata 88 |
| `ciuki_hid_state` | 116 | keyboard bitmap 32; normalized axes 64; hats 100 |
| `ciuki_hid_calibration` | 32 | axis 16; minimum/centre/maximum 20/24/28 |

Wire alignment is four bytes on host and i686. Invalid size/version/reserved
fields return EINVAL; missing devices and stale generations return ENODEV;
invalid user ranges return EFAULT. Discovery metadata retains descriptor
limits; calibration changes normalized state, not those discovery facts.

Parser limits are explicit: 512 descriptor bytes, eight report IDs, 32 data
main items, 64 usage spans, four global pushes, eight nested collections,
and 64-byte input/output/feature reports (including the ID, extended by
f1-104b). Oversize or unsupported layouts fail transactionally without
modifying the destination.
Long items and alternative usage delimiters are refused. Input, Output and
Feature offsets are independent. Unknown application/vendor collections are
ignored while their offsets and global state are retained. Only one joystick
application per interface is supported. Boot-capable keyboards/mice retain
their proven boot fallback. Report-only devices receive no SET_PROTOCOL.
LED reports use their descriptor's Output offsets and ID through SET_REPORT.
Output reports with unrelated writable fields are skipped instead of zeroing
vendor state. Unsupported report-only interfaces retain an explicit
unsupported flag and diagnostic; compatible peer interfaces remain active.
Descriptor allocations are split into a 1,868-byte layout and a 900-byte
i686 state object (2,048/1,024-byte heap classes), respecting the existing
2,040-byte heap payload maximum. Allocation failure keeps a boot keyboard's
fallback. Successful layouts remain with monotonic device records after
detach; failed attachment frees both allocations.

Research decision: [HID 1.11](https://www.usb.org/sites/default/files/documents/hid1_11.pdf)
§§6.2.2, 7.2, 8 and Appendix B govern parsing, arrays, LEDs and fallback;
[HUT 1.12](https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf)
§§4, 10, 11, 12 and 15 govern usages, hats and explicit consumer actions.
Linux v6.12 [hid-core.c](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-core.c)
and [hid-input.c](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-input.c)
were consulted for range validation and usage/hat mapping, not copied.
The old mouse parser's signed relative fields and first-three-button
projection remain the compatibility policy. F1-104 used the existing 48-byte
DMA slot's 16 payload bytes at offset 32. F1-104b expands UHCI/OHCI slots
to 96 bytes (64 payload bytes), moves the bulk ring, and rounds the shared
controller arena from five to six pages: +4096 bytes per controller, +16384
at the four-controller bound. EHCI retains its existing 224-byte slots and
64-byte payloads. Allocation, registry claims and storage pins retain their
existing accounting; no global memory limit changes. The current DMA map
and primary-source decisions are in [usb-contract.md](usb-contract.md#f1-104b-64-byte-hid-reports).
Implementation measurements and host evidence are recorded in
`build/codex/f1-104-report.md` and `build/codex/f1-104b-report.md`; the lead
reviews the ABI at integration. Hardware qualification remains separate.

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

#### f1-122 implementation contract (2026-10-23)

Primary references checked before implementing the protocol logic:
[Synaptics Interfacing Guide 510-000080-A, second edition](https://ccdw.org/~cjj/l/docs/ACF126.pdf),
sections 2.3–2.5 and 3.4–3.6;
[Linux v6.12 synaptics.c](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/synaptics.c),
[synaptics.h](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/synaptics.h),
[alps.c](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/alps.c),
[alps.h](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/alps.h),
and the [versioned ALPS protocol description](https://www.kernel.org/doc/html/v6.12/input/devices/alps.html).
The existing i8042 engine has one-byte command replies and only relative
three/four-byte decoders. Extend its existing transport rather than claiming
new ports or IRQs. Collect ACK and all query bytes in one armed transaction.

Start with identified Synaptics firmware 3.2–6.x old/new absolute packets and
ALPS table-identified v1/v2. Other signatures remain compatibility devices.
No branding selects a protocol. E500 firmware ownership returns before probes.
Use the donor's sliced Synaptics queries/mode command and ALPS E6/E7 reports,
tap-disable sequence, absolute knock and pass-through selection. Host gestures
handle taps and right-edge scrolling; Synaptics capability-proven multifinger
reports also permit two-finger scrolling. Pressure hysteresis and palm rejection
are explicit policy, with movement/timing thresholds documented in the adapter.

Internal input event values 1–5 and all public ABI layouts remain unchanged.
Append kernel-only INPUT_ABS=6 with X, downward Y, pressure, fingers and width
codes 0–4. Each pad packet emits one such ordered sample; the existing native
REL/BTN events carry its pointer projection to desktop/DOS. Stick packets never
enter the pad stream or change its coordinate baseline. A read-only internal
snapshot exposes identity, protocol, ranges, capabilities and optional error.
No new syscall, heap allocation, registry slot or memory limit is introduced.

Optional probe work must leave recovery time inside the existing 500 ms setup
budget. On failure restore compatibility before negotiating ID 0/3/4. Synaptics
uses explicit mode zero (F6 alone preserves other mode bits); ALPS leaves its
pass-through path before defaults. A three-byte remote poll must prove the
relative framing before enabling a fallback. Missing ACKs or an unproven
rollback retain the existing quarantine and original setup stage; they cannot
justify parsing an unknown stream as PS/2. Hardware timing, ALPS rollback and
pass-through qualification remain lead validation; host fixtures are no claim
of physical support. Per-file donor notices are retained in the adapter and
the credits notice manifest.

Host measurement with the production i686 flags: controller storage grows from
12,532 to 12,628 bytes, or 192 static bytes across native and private fixture
storage. The input ring capacity, registry capacities, heap and memory limits
are unchanged. Supported-hardware usable/reserved/free memory has not been
measured in this host-only worktree. Firmware-first, ID 0/3/4, all 21 selected
ALPS signatures, both Synaptics formats, optional-command refusals, query
timeouts, rollback, interleaving, gestures, corruption and bounded fuzz fixtures
pass the focused UBSan host harness. Sanitizer leak checking requires a host
outside the current sandbox's ptrace restriction; physical qualification and
canonical image evidence belong to the lead.

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

#### f1-114 LPT v1 implementation interface (2026-10-23)

Discovered ports create exclusive `/dev/lpt1` through `/dev/lpt8` character
devices. Missing paths return ENOENT; unavailable retained ports return ENODEV. Existing syscalls and Device Manager v1 bytes stay
unchanged. `CIUKI_SYS_DEVCTL` accepts LPT_GET_CONFIG=0x4c500101,
LPT_SET_CONFIG=0x4c500102, LPT_GET_STATUS=0x4c500103 and
LPT_INIT=0x4c500104. Records in abi.h use uint32_t fields, alignment four,
version=1, exact size, reserved=0; no pointers. Config (32 bytes): version,
size, mode, reverse, timeout_ms, dma, address, reserved. Modes are SPP=0,
PS2=1, EPP=2, ECP=3; reverse/dma/address are booleans. Address cycles are
EPP-only, DMA is ECP forward-only. Timeout is 0..60000 ms for the whole
operation, zero means try once. Init (12 bytes): version, size, reserved.
Status (64 bytes): version, size, generation, number, base, base_hi,
origin, capabilities, flags, status, transferred, fifo_depth, dma_channel,
owner_id, owner_generation, reserved. Origins BDA/PNP/PCI=1/2/3;
capabilities SPP/PS2/EPP/ECP/DMA=1/2/4/8/16, flags READY/OPEN/VM/RAW/
QUARANTINED=1/2/4/8/16. dma_channel=0xffffffff means none. Status contains
the unmodified status-register byte. Generation changes at every acquisition;
owner_id/generation identify the DOS VM, zero for native or unowned.

Read/write pin checked user buffers before touching hardware, retain the file
description across waits, return partial completed bytes, and honor signals,
O_NONBLOCK and the generation. Config/init require a writable description;
config changes are serialized with transfers. Close cancels an in-flight
operation, stops DMA, checks FIFO idle, restores saved data/control/mode, and
invalidates the logical owner. Unproved stop retains bounce memory and claims
in quarantine. Limits and registry capacities do not grow.

Late PnP discovery upgrades an idle BDA port through registry quiesce/reclaim,
preserving its number. An open port stores the assigned facts and applies
them at its next open, after restoring the old lease. A bounded discovery
worker waits up to 30 seconds for the read-only inventory; namespace refresh
only copies cached facts and creates newly discovered nodes. It never probes.

Firmware-assigned PNP0400/PNP0401 resources are consumed without ISA isolation
or configuration. BDA consumes exactly three words at 408h (never 40Eh).
PCI admission uses exact card layouts, separate low/high claims and no BAR
sizing writes. Polling masks local IRQ sources; no guessed IRQ claim is made.
An assigned 8237 channel is claimed independently, uses a one-page below-16-MiB
bounce, single mode without auto-init, and never reads shared terminal status.
DMA also requires an exclusive assigned ISA IRQ5/7 claim, kept PIC-masked:
ECR serviceIntr must be cleared to enable DMA, which also enables its terminal
interrupt independently of DCR's ACK mask (PC87338 §4.4.12). A conflicting or
unqualified DMA IRQ leaves ECP PIO available without DMA capability. PCI cards
use PIO; no shared PCI interrupt line is masked for this service.

DOS INT17 AH=00/01/02 provides write/init/status through the same exclusive
owner; AH is `(status ^ 0x48) & 0xf8` plus bit0 on timeout/unavailable,
AL and FLAGS preserved. Three virtual BDA entries and equipment count reflect
the logical map. Trusted VMM policy may explicitly replace a printer lease
with a raw lease; guest code receives no acquisition syscall or I/O bitmap
permission. Raw accesses are trapped byte cycles for data/status/control/EPP;
physical IRQ enable and ECR DMA/service requests are blocked. ECP FIFO/DMA
remain mediated native operations, not guest 8237 passthrough. VM stop revokes
the lease; pending transfers keep resources until stop proof. Timing-sensitive
dongles and IRQ-driven raw devices require device-specific qualification.

Research/decision: National Semiconductor PC87338/PC97338, November 1998,
[parallel-port chapter](https://pccomponents.com/datasheets/NATI-PC87338.pdf),
defines direction, EPP timeout and ECR/FIFO/DMA behavior. Linux v6.12
[parport_pc.c](https://github.com/torvalds/linux/blob/v6.12/drivers/parport/parport_pc.c)
corroborates separate base_hi, exact PCI BAR layouts, bounded FIFO drain and
masked DMA residue accounting. Its companion
[ieee1284_ops.c](https://github.com/torvalds/linux/blob/v6.12/drivers/parport/ieee1284_ops.c)
is the contract's protocol reference; retrieval availability is recorded in
the implementation report. SeaBIOS rel-1.16.3
[serial.c](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/serial.c)
provides the INT17 status inversion and BDA conventions. Resource Kit local
`wrkc24_1.htm:205–208`, `wrkc24_3.htm:706–713` corroborate negotiated
parallel modes and protected-mode port arbitration. These are behavioral
references; no donor implementation or Microsoft text is imported.

Intel [8237A 231466-005](https://www.pcjs.org/documents/datasheets/intel/INTEL_8237A_DMA.pdf),
Figure 6, makes the mask registers write-only. Stop issues the channel mask,
reads ECR DMA-disable back and compares counts across a bounded 5-us delay;
it reads neither the mask nor shared terminal-count status. DMA pages carry
MM_DMA ownership and a generation-qualified pin until proven stopped.

Virtual VGA registration is split into 3B0h–3BBh and 3BFh–3DFh so LPT3
owns 3BCh–3BEh. VGA batching excludes that printer range. The default
PIT/keyboard/audio registrations use nine entries; three LPT, two VGA and
one optional network handler use fifteen of the existing sixteen slots.
Broad physical VGA claims may still conflict with a physical 3BCh LPT;
that controller is reported unavailable, without stealing the display lease.

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

The f1-110 implementation, its versioned call-73 status interface, admission
limits and primary-source decisions are specified in
[Generic ACPI events and power](acpi-events-power.md). Unsupported thermal
control retains firmware ownership; inventory capability bits remain facts,
not activation evidence.

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
§§4.3/4.6, is an exclusive alternative for some early machines. Probe version
and capabilities without connecting. Under f1-121 Amendment 1, connect the
real-mode interface through the contained V86 broker and poll events once
per second; protected-mode firmware far calls are excluded. Reject ordinary
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

F1-116 supplies the [Maestro engine and host-model contract](f4-maestro.md):
native stereo PCM, copied private capture, common native/DOS mixer transport,
E500 firmware routing and standard EAPD readback. This adds host protocol
coverage; E500 analog, actual rate/latency and physical memory qualification
remain required. It neither qualifies every AC'97 codec nor changes the
playback-only public and virtual DOS recording capabilities.

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

#### f1-108 ATA mode/resource contract v1 (2026-10-23)

The private kernel boundary is `ciuki/ata_chipset.h`, ABI version 1. It
does not change the Device Manager v1 layout or the two-channel capacity.
The five separately tested modules select exact PCI/bridge IDs and revisions;
an unrecognized or concealed bridge is a recorded PIO fallback. Names never
authorize timing writes. Resources are decoded before claims: compatibility
ports/IRQ14–15, or native I/O BAR0–3 (control at BAR+2), BAR4 BMIDE,
and a firmware-qualified shared level PCI IRQ. Reject MMIO, overflow,
misalignment, overlapping apertures, ambiguity, disabled decode, ICH3
native mode and unqualified routing without probing legacy aliases.
The driver never switches PCI programming-interface mode or sizes BARs live.

Negotiation is boot-only, under the channel mutex, before publishing either
channel's request worker. No exported runtime mode-setting service exists.
Refuse changes with an active command, armed/started DMA, stopped/quiescing
channel, or either PCI master active/START. Read-modify-write plans preserve
PIO/command timings, peer fields, cable evidence and reserved bits. Thus the
shared PIO timings and firmware PCI clock remain unchanged; only independent
UDMA timing fields are negotiated. No MWDMA/SWDMA or ATAPI DMA is introduced.
The VIA/NVIDIA encodings use the donor's 33.333 MHz PCI assumption; physical
qualification must confirm a standard, non-overclocked clock. Unsupported
clock/firmware setup retains PIO rather than inventing timing values.

The selected UDMA is the intersection of valid IDENTIFY words 49/53/88,
controller/revision ceiling, host cable evidence and valid drive word 93.
Unknown/40-wire cable is capped at UDMA2. NVIDIA additionally limits high
modes to the saved BIOS timing per drive (no ACPI optimism). PIIX3 stays
PIO; PIIX4 caps at UDMA2; ICH0/ICH/ICH2+ cap at 2/4/5. Intel 450NX
errata disable DMA, and ICH3 native mode is refused. VIA uses the ISA bridge
revision, rejects unsafe FIFO/clock setup and the 586/TS64GSSD25-M pair;
bad-AST parts preserve address timing. SiS uses the visible host bridge's
old/66/100/133 layout, checks compatibility setup, and refuses concealed
IDs requiring a separate remapping transaction. ALi pre-C2 stays PIO
(WDC/ATAPI/LBA48 errata); C2–C4 require the firmware bridge fixup and use
PIO for LBA48 disks (this transport always uses EXT commands for them);
C5+ supports LBA48 DMA. ALi cable detection must
already be enabled. No short-cable/DMI override raises a cable limit.

Every plan is checked by masked PCI readback before SET FEATURES 03h/40h+n.
Then IDENTIFY must confirm exactly the selected mode, support/cable and disk
identity before DMA is enabled. A programming/readback mismatch or uncertain
issued command quarantines the channel. Cleanly unsupported negotiation
records `mode=pio reason=...`; it leaves the safe firmware PIO timing intact.
Existing stop/revoke/pin/quarantine rules remain binding; no BIOS retry and
no new panic path. Record family, PCI revision, resource mode, cable/mode
and fallback reason through existing DISK diagnostics.

Sources checked against the existing firmware-only `ata_dma.c` / `identify`
path: Linux **v6.12**
[`ata_piix.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/ata_piix.c),
[`pata_via.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_via.c),
[`pata_sis.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_sis.c),
[`pata_ali.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_ali.c),
[`pata_amd.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_amd.c),
and [`libata-pata-timings.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libata-pata-timings.c)
(UDMA period table only).
[`libata-sff.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/libata-sff.c)
`ata_pci_sff_init_host` and `ata_pci_sff_activate_host` were checked for
BAR pairing, control offset and shared native IRQ semantics (research only).
[Intel ICH2 datasheet 290687-002](https://www.intel.com/content/dam/doc/datasheet/82801ba-i-o-controller-hub-2-82801bam-i-o-controller-hub-2-mobile-datasheet.pdf)
§5.15 and chapter 10 separate PIO timing from UDMA clock/dividers.
[Intel ICH3-S specification update 290739-014](https://www.intel.com/content/dam/doc/specification-update/82801ca-io-controller-hub-3-specification-update.pdf)
erratum 18 requires refusing the whole function if either channel is native.
Erratum 15 remains a BIOS workaround prerequisite, as in the donor; its
platform qualification is not established by the host register model.
[T13 ATA-6 d1410r3a](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf)
§8.15 (words 88/93), §8.46 and §9.4 define the drive half of the transaction.
The owner's Resource Kit `wrkc10_6.htm` and `wrkc26_11.htm` confirm that
bus-master capability alone is insufficient: drive IDENTIFY and controller
compatibility must agree. Those private texts are not copied or redistributed.
Host register/IDENTIFY/failure fixtures are engineering evidence only;
per-family lead review and physical timing/cable qualification remain required.

Promise splits into `pata_pdc202xx_old.c` and `pata_pdc2027x.c`; HighPoint into
`pata_hpt366.c` and `pata_hpt37x.c`. Their PCI task-file layouts, timing/PLL
and clocks need exact model selection. Support plain disks first; RAID
metadata, array assembly and proprietary option-ROM behavior require later
contracts. SATA378 in the owner's names is not authorization to treat it as
a PDC2027x. AHCI is a separate native SATA interface outside the current
hardware baseline, even on a late-period board; no AHCI directive is proposed.

#### f1-123 add-in PATA contract

The f1-108 boot-only transaction and failure rules also apply to Promise and
HighPoint. Exact PCI IDs/revisions select a backend, including RAID/other-storage
class functions with native resources; no option-ROM array is assembled. SATA378
and unrelated IDs remain excluded. HPT366/368 exposes one channel per function;
HPT374 exposes two per function. Inventory is bounded at eight channels (six
additional native channels), with sixteen storage slots. Excess functions are
reported and left untouched. ISA compatibility aliases remain unique.

PDC20246/62/65/67 and HighPoint use native I/O BARs. PDC20268/69/71/75/76/77
uses the donor's BAR5 16-KiB MMIO aperture, sparse task-file offsets and MMIO
BMIDE; it must never be probed as SFF I/O. Claim before mapping, map uncached,
reject invalid decode, wrap, alignment, unqualified PCI IRQs and resource aliases.
Both I/O task-file apertures are claimed before a shared clock can change.
Releasing a shared BMIDE/MMIO claim requires every channel to prove quiescence.
Clean MMIO release revokes the shared mapping and makes retained adapters inert;
uncertain DMA retains the mapping and claims in quarantine.
Do not size BARs live. Serialize controller-wide clock work and commands before
publication and at runtime. PDC20265 behind an Intel i960 RAID bridge is refused.

Promise old chips use the donor's DMA timing masks and 33/66 MHz transfer clock
switch with PIO clock restoration; EXT DMA receives its direction/word count.
PDC20246 uses PIO for EXT commands. PDC2027x measures its split down-counter,
checks a 5–70 MHz PLL input, programs 100/133 MHz and waits for stabilization.
133-MHz chips repair both PIO and UDMA timing after SET FEATURES hardware
autotiming; slave UDMA6 is conservatively capped at 5. HighPoint selects 25/33/40
MHz HPT36x tables or measured 33/50/66 MHz HPT37x tables with bounded DPLL
calibration; N variants use the DPLL clock and donor timing masks. Preserve
command/PIO fields unless a clock change requires a complete PIO0 rebase.
Drive model errata, valid IDENTIFY and both cable endpoints constrain UDMA.
Unknown clocks retain firmware PIO only before any clock mutation; a failed
clock/programming/readback transaction quarantines the function. Never retry
uncertain DMA through PIO or BIOS. No new panic path.

Before block publication, read metadata using PIO only: dmraid Promise end
offsets (including secondary copies and the large-disk absolute location),
HighPoint sector 9 and capacity minus 11. Recognized signatures, including
broken/stale metadata, quarantine that unit and remove all single-disk write,
flush and emergency-write entry points. Failed metadata reads quarantine the
channel; an unreadable/too-small disk cannot qualify as plain. Every disk on
these add-in controllers is screened, regardless of PCI storage subclass.
Plain disks use the existing partition/mount path. Setup installation to these
controllers remains forbidden until the lead records later physical qualification.

Primary donors are Linux v6.12
[`pata_pdc202xx_old.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_pdc202xx_old.c),
[`pata_pdc2027x.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_pdc2027x.c),
[`pata_hpt366.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_hpt366.c),
[`pata_hpt37x.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_hpt37x.c),
[`pata_hpt3x2n.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/ata/pata_hpt3x2n.c)
and `include/linux/pci_ids.h`. Metadata locations/signatures are checked against
dmraid [`pdc.h`](https://github.com/Distrotech/dmraid/blob/master/lib/format/ataraid/pdc.h),
[`pdc.c`](https://github.com/Distrotech/dmraid/blob/master/lib/format/ataraid/pdc.c),
[`hpt37x.h`](https://github.com/Distrotech/dmraid/blob/master/lib/format/ataraid/hpt37x.h),
[`hpt45x.h`](https://github.com/Distrotech/dmraid/blob/master/lib/format/ataraid/hpt45x.h).
The compatible FreeBSD signature is checked against
[`md_promise.c`, releng/14.2](https://github.com/freebsd/freebsd-src/blob/releng/14.2/sys/geom/raid/md_promise.c).
The local Resource Kit `wrkc10_6.htm` and `wrkc26_11.htm` confirm the need for
controller-specific drivers in addition to drive DMA capability. Host models
verify software/register sequencing; PLL/cable/electrical and installation
qualification remains the lead's responsibility.

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
| APM 1.2 fallback | 1.5–3 kLOC; 16–40 KiB plus firmware workspace | Exclusive bounded real-mode V86 broker, event worker, emergency quiesce | Negotiation/event/critical-suspend model; actual APM BIOS hardware; do not count QEMU ACPI off as APM conformance. |
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

## Native COM ABI v1 (f1-111)

The implementation interface is `ciuki/com.h` in the SDK and `ciuki/com.h`
in the kernel. Native names are `/dev/com1` through `/dev/com8`; BDA slots
retain their numbers and additional assigned PnP/qualified PCI ports take
unused slots. Zero/FFFF, wrapping and overlapping resources are rejected.
Enumeration never probes the live logger. Its existing `uart` claim and
IRQ callback remain pinned; the native aggregate dispatcher invokes that
callback once on its line. The boot record identifies the reserved base.
An application opening that port gets `EBUSY`, including read-only opens.

`open`/`close`/`read`/`write` and existing `devctl(fd, op, buffer, size)` are
used, without new syscall numbers. Opens are exclusive per file description;
dup/fork share it, final close stops local interrupts, invalidates the open
generation and discards queued bytes. Close flushes the hardware holding
FIFO, then gives the shift register at most 20 one-millisecond pauses with
interrupts enabled. Idle failure retains the port in quarantine; another
open cannot enter during that stop. Process exit follows the same close.
Native opens grant no raw I/O privilege. Native/VM/mouse handoff follows
the additive f1-112 rules below. `O_APPEND`, create/truncate/directory flags are rejected.
Access mode controls read/write rights; configuration needs write access.

All v1 records contain only 32-bit fields, with leading `version=1` and
`size=sizeof(record)`. Unknown versions, sizes, enum values, flags and
reserved fields return `EINVAL`; unrelated controls return `ENOTTY`.
`COM_GET_LINE`/`COM_SET_LINE` use baud, data bits (5..8), stop bits (1/2;
5-bit/two-stop means the hardware's 1.5), parity (none/odd/even/mark/space),
RX trigger selector (0..3) and read/write timeout milliseconds (0..60000).
The default is 38400 8N1, trigger 0, immediate I/O. Trigger selectors 0..3
map to 1/4/8/14 bytes on a 16550A, 8/16/24/28 on the 32-byte 16650, and
1/16/32/56 in 16750 64-byte mode; single-byte operation ignores the selector.
Only exactly representable divisors of the proven clock are accepted. A line
change requires empty software/hardware RX and TX; otherwise `EBUSY`.
`COM_SET_MODEM` sets DTR/RTS and transmit break, without exposing LOOP/OUT2.
`COM_GET_STATUS` returns generation, port/base/IRQ/origin, detected generation,
verified FIFO depth, modem inputs/deltas, sticky line errors, queue occupancy
and capacities, and receive/drop counters. Status reads do not clear errors.

Each open allocates 1024 RX and 1024 TX bytes; at most eight metadata slots
exist, with no registry/inventory limit increases. Reads/writes return a
bounded short transfer as soon as progress occurs. Zero timeouts and
`O_NONBLOCK` return `EAGAIN` when empty/full; finite waits return `ETIMEDOUT`,
signals/termination return `EINTR`, stale generations return `ENODEV`.
IRQ work is bounded per port/pass and uses the wait queue's deferred wake.
RX overflow drops new bytes and increments a counter; LSR overrun, parity,
framing and break are sticky status bits. Caller buffers are checked/pinned
before consuming or publishing bytes. Seek/positioned I/O returns `ESPIPE`.

Research and implementation decisions: [TI TL16C550D, register descriptions
and interrupt priorities](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf),
[TI TL16C750, FCR5/DLAB and trigger table](https://www.ti.com/lit/ds/symlink/tl16c750.pdf),
[Exar ST16C650A rev 5.0.3, EFR and FIFO](https://www.maxlinear.com/ds/st16c650a_503_061610.pdf),
and Linux v6.12 [8250 detection](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_port.c),
[PCI layouts](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_pci.c)
and [assigned PnP resources](https://github.com/torvalds/linux/blob/v6.12/drivers/tty/serial/8250/8250_pnp.c).
Detection restores LCR/IER/MCR/SCR/divisors/EFR; because FCR is write-only,
an inherited enabled FIFO is refused rather than claiming to restore an
unknown trigger. Inactive FIFO-off ports have a known restore baseline.
EFR alone is insufficient to claim 32 bytes: loopback measures depth, with
unknown clones falling back to the verified lower capability.
The four BDA words are confirmed by [SeaBIOS rel-1.16.3
`bda.h`](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/std/bda.h).
The local Windows 98 Resource Kit, chapter 24 (`wrkc24_1.htm`, ISA devices
and ESCD), confirms separating assigned resources from device names. The
read-only f1-109 inventory provider must supply assigned PnP resources;
f1-111 does not perform ISA isolation/configuration writes. Unsupported PCI
IDs/MMIO layouts and unqualified routes stay visible and cannot be opened.
Assigned PnP submission runs after `com_init()` and before the initial native
namespace is attached. `com_pnp_assigned()` preserves an existing BDA logical
number and can qualify its previously unknown IRQ without probing a live port.
Before an open, assigned routing supersedes the BDA IRQ convention; the logger
keeps its original reservation. Registry invalidation prevents further native
access and is reflected in the read-only COM inventory.
The f1-109 provider was absent from the f1-111 base; f1-112 now imports its
assigned inventory after read-only discovery, without an ISA isolation scan.
PCI activation currently admits Oxford 1415:9501 (four packed
ports at 1.8432 MHz; SIIG subsystem excluded) and QEMU 1b36:0002/0003/0004
(one/two/four packed ports, same clock), from exact layouts. QEMU's
[PCI serial device specification](https://www.qemu.org/docs/master/specs/pci-serial.html)
documents the offsets and IRQ pin. No QEMU run is evidence for this change.

## Serial peripheral and DOS lease interface (f1-112)

The additive COM controls are `COM_GET_OWNER` (v1, 24-byte
`ciuki_com_owner`: version, size, kind, id, generation, reserved) and
`COM_SET_MOUSE` (v1, 16-byte `ciuki_com_mouse`: version, size, protocol,
reserved). Existing COM records and syscall numbers keep their bytes.
Owners are NONE=0, NATIVE=1, VM=2, MOUSE=3, ENUMERATOR=4, LOGGER=5.
Owner IDs identify the native description's opener PID or the VM ID; zero
denotes a kernel-held mouse/enumerator/logger. The generation is the
physical COM open generation, never a guest pointer.
Mouse protocols are OFF=0, MICROSOFT=1, LOGITECH=2, MOUSE_SYSTEMS=3,
WHEEL=4. A writable exclusive native description can select a non-PnP
mouse explicitly; final close releases its buttons and its lease. Raw reads
are refused while that description is driving the input decoder. Automatic
enumeration only binds a mouse after a validated known PnP ID, never from
an arbitrary byte stream. The trusted kernel attach/detach interface also
supports retained mouse ownership without a native description.

`dosvm_uart_bind(key, slot, native_index, reason)` is a trusted monitor
interface, not a guest syscall. Slots are zero-based COM1–COM4; -1 selects
the null sink. Trusted `dosvm_config.serial[4]` selects 0=null or 1..8=native
COM1..8 at creation, before boot; refusal unwinds the whole creation.
Rebinding requires an unstarted VM. No physical I/O
bitmap is granted. Native COM generations and the full VM key prevent stale
release. Default VM UARTs use null sinks at 3F8/2F8/3E8/2E8 with IRQ4/3/4/3.
IRQ lines aggregate the two virtual UARTs sharing each edge. Each VM has
independent 16-byte FIFOs, modem/line state, timeout and interrupt latches.
INT14 functions 00–03 use these same registers and bounded BDA timeouts.
BDA 47C–47F hold BIOS tick counts (65536/1193182 seconds per tick), with
ten ticks by default and zero selecting immediate I/O in this virtual BIOS.
Null output drains at the programmed character rate; null input has no
fabricated carrier or incoming bytes. Physical bindings use the native
queues, configuration and stop/quarantine path; unsupported divisors return
line-error status rather than reconfigure a peer. Stop revokes guest work;
destroy closes leases only after the worker is quiescent.

Firmware `_CRS` COM resources are consumed after inventory completes.
IRQ resource flags retain EDGE=1, EXCLUSIVE=2 and ACTIVE_HIGH=4; only a single 8-byte I/O
extent and a single high/edge/exclusive IRQ3/4 qualify. `_STA` must permit
decoding (or be absent, as specified by ACPI). Partial/unsupported,
ambiguous, wide or conflicting records do not trigger hardware probes.
BDA logical numbers and the pinned logger remain authoritative at their bases.

Research decisions: Microsoft/Hayes [External COM 1.00, §§2–3](https://citeseerx.ist.psu.edu/document?doi=f48af868c7395b5a8357780c5663011f5ec572fc&repid=rep1&type=pdf)
defines exclusive acquisition, 1200/7N1, DTR/RTS/DSR stages, two response
phases and bounded ID collection. Validate both encodings, delimiters,
field lengths and wire-byte checksum before selecting a decoder.
[TI TL16C550D, register descriptions](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf)
defines DLAB, FIFO triggers, four-character timeout, error/modem read-clear
and interrupt priority. Virtual IRQ assertion honors OUT2 and loopback.
[Linux v6.12 `drivers/input/mouse/sermouse.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/input/mouse/sermouse.c)
(blob 218c8432a13bc9addfe54a8818ee3701f7dd6795) supplies the mouse wire
equations; the adaptation emits complete packets without prediction.
[SeaBIOS rel-1.16.3 `src/serial.c`](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/serial.c)
and [`src/clock.c`](https://github.com/coreboot/seabios/blob/rel-1.16.3/src/clock.c)
corroborate INT14 register results and BDA timeout units. These firmware facts
do not introduce a foreign BIOS implementation.
The local Windows 98 Resource Kit `wrkc24_3.htm:704–742` establishes VCOMM
as the protected-mode communications owner. The Microsoft NT serenum sample
is a protocol reference only, never a V86 architecture donor.
ACPI [device configuration](https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html)
defines `_STA` decoding permission, current resources and IRQ electrical
properties. Variable or disabled resource descriptions grant no COM activation.
FreeBSD [release/14.2.0 `usr.sbin/moused/moused.c`](https://github.com/freebsd/freebsd-src/blob/release/14.2.0/usr.sbin/moused/moused.c)
confirms the PnP serial mouse protocol IDs, including `MSH0001` wheel mice.

## Device model, claims and guest boundaries

### f1-109 inventory ABI and read-only interpreter boundary

Call 76 retains its v1 layout and numbers. A distinct version 2 request,
`ciuki_firmware_page` in `ciuki/firmware_abi.h`, uses the same call with its
exact smaller byte extent. All records use pack(4), fixed-width integers,
terminated strings and no pointers. Input is version=2, size=sizeof(page),
cursor (zero for the first page), generation (zero only on the first page),
and reserved=0. Output has at most four records, total, next cursor (zero at
end), immutable boot generation, flags and ACPICA status. Invalid headers or
cursors return EINVAL, stale generation EAGAIN, inaccessible memory EFAULT.
Queries only copy cached facts; they never evaluate AML or access hardware.

Records retain a boot-local ID, nearest device parent ID, absolute namespace
path, HID, up to four CIDs, eight half-open 64-bit resource extents, capability
bits, origin (ACPI or PnP BIOS), and explicit discovered/unsupported/routing
unqualified states. IDs are stable within the returned generation. Overflow
of records, paths, IDs, resources or unsupported resource descriptors is
explicit. Capabilities describe object presence, never current battery,
thermal, power or routing readiness. Logical owner and VM lease are zero:
firmware discovery grants neither. Device Manager consumes pages and displays
firmware identity, hierarchy, resources and capabilities separately from v1.

The retained inventory is bounded by the existing 64-device ceiling; no
registry, v1, heap or physical-memory limit grows. ACPICA has a separate
one-MiB allocation admission budget (including allocator rounding), with
high-water accounting. Table length retains power.c's 256-KiB ceiling and
the validated table set is bounded at 64 entries. Memory exhaustion is a
reported incomplete inventory. NVS remains excluded by PMM's existing
non-type-1 E820 policy; the OSL never frees/reclaims firmware memory.

Research decisions: [ACPI software model §5](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html)
permits control methods to access hardware, including during table load.
[ACPICA AcpiLoadTables](https://github.com/open-acpica/acpica/blob/20260930/source/components/tables/tbxfload.c)
installs default region handlers and executes module-level AML. Therefore
every OSL region read needs a live generation-qualified `acpi-inventory`
registry lease; there is no implicit access to an existing driver's region.
Writes, SCI installation, global-lock firmware writes, EC transport, sleep
entry and event work are refused. No ACPI_ENABLE, AcpiEnableSubsystem,
AcpiInitializeObjects or _INI/_REG activation is performed. SCI_EN remains
unknown without a qualified read lease. EC-dependent methods report denied
access; f1-110 supplies the EC/event owner and broker.

The exact audited vendor files remain unchanged. The build produces Ciuki
platform headers and derived psloop.c, psargs.c and nsaccess.c. The loop
checks an opcode/time/cancellation budget and preserves parser errors. The
package decoder's truncated-length clamps and invalid package extents become
sticky errors. The predefined-name load uses a byte-safe copy. Direct pointer
casts and offsetof use defined integer/builtin operations; ACPICA's portable
unaligned moves are selected on both host and i686. Expiry uses ACPICA's
existing CompleteOp/error unwind, with sticky transaction and whole-run errors
because upstream AcpiLoadTables and optional lookups can mask failures. A thread
checkpoint yields every 64 parser steps; waits and stalls are bounded by the
same transaction deadline. This seam applies to nested calls and module AML.
The default transaction is 100,000 steps/250 ms; neither limit is reset by
nested methods. Inventory initialization runs once on its own device thread,
outside shared IRQ/EC/GPE work. The OSL callback queue excludes event types
in this phase. PnP BIOS installation-structure discovery is read-only;
real-mode PnP node calls and ISA isolation/configuration are unsupported here.
ACPI HID/CID PnP IDs are enumerated through the interpreter.

[ACPI device objects](https://uefi.org/specs/ACPI/6.5/09_ACPI_Defined_Devices_and_Device_Specific_Objects.html)
and [power-source objects](https://uefi.org/specs/ACPI/6.5/10_Power_Source_and_Power_Meter_Devices.html)
define the advertised capabilities. The read-only Resource Kit references
wrkc30_4.htm:54–129 and wrkc05_3.htm:42 corroborate discovery before power
policy and firmware hardware-profile origin; no Microsoft text/code is copied.

Build selection retains all 207 audited files but excludes debugger-only
rsdump.c and rsdumpinfo.c from compilation; the unguarded dump implementation
otherwise references disabled debug tables. No debugger, disassembler,
application mode or foreign OSL is enabled. The optional upstream C library
is symbol-prefixed to avoid replacing Ciuki/libc routines. The interpreter
is terminated after the one-shot copy; page queries read the retained cache.
The inventory root also records FADT fixed-button description and _S5 object
presence without evaluating sleep methods. These bits never mean power-off
or event readiness. Native/VM owner and SCI state stay unknown/unassigned.

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
| f1-106b | EHCI FS/LS TT splits, shared periodic admission and completion-driven HID workers | Host models cover LS keyboards, FS mice and FS storage behind HS hubs; zero periodic HID wakes with working IRQs. Hardware qualification remains required; topology scans retain their separate deadline. |
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

## Lead decisions (2026-10-22 23:46)

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

### f1-121 implementation boundary (Amendment 1)

Amendment 1 supersedes the protected-mode connection described above.
[Intel/Microsoft APM 1.2](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF),
sections 4.1, 4.2, 4.3, 4.6.2, 4.6.5, 4.6.8, 4.6.11, 4.6.12 and
4.6.15 define the real-mode cooperative interface, disconnect, status and
notification protocol. Ciuki connects with INT15/5301 and negotiates 1.2
with 530E. Every operation uses the serialized V86 BIOS worker. No BIOS
GDT descriptors or ring-0 firmware calls are installed. The kernel-only
broker permits 5300–530F and 5380; protected-mode connect 5302/5303 is
explicitly refused by this real-mode owner. The generic input and DOS
allowlists remain unchanged. Section 4.7.1 defines 5380/BH=7F as the OEM
installation query. Only that query is admitted; section 4.7.2's other BH
values select vendor operations and are refused. The backend itself issues
no OEM calls.

A fault/timeout makes the backend unavailable. Disconnect is attempted once
through the same contained broker, never by resurrecting a quarantined VM.
A failed disconnect retains the exclusive reservation. A BIOS refusal of
connection never authorizes disconnecting another owner's connection. Any
ACPI RSDP excludes APM, including failed or partial ACPI takeover.

The common kernel-only `ciuki/power_source.h` interface reserves exactly one
of ACPI/APM before connection and exposes cached AC, battery percentage,
remaining seconds, notifications and orderly off. UI consumers never call
firmware. The lead reconciles this interface with parallel f1-110; no public
ABI or SDK record changes here. Global DMA stop/resume is an explicit
platform service: an absent service cannot be represented as successful
quiescence, and APM connection requires that owner to be installed.

`storage_sync_until` flushes mounted volumes without unmounting or setting
clean bits, using a task-scoped microsecond VFS request deadline. A retained
worker (`storage_sync_bounded`) bounds its caller even when cancellation
must wait for transport retirement/bookkeeping cleanup. It never abandons
caller-stack pointers or frees in-flight buffers. Pending flushes leave the
APM event worker runnable. Ordinary shutdown uses existing `storage_sync`;
APM Off is issued only after the platform's orderly-shutdown proof.
Emergency allocation remains 2 seconds for flush, 2.5 seconds total for DMA
quiescence, followed by a 2-second firmware acknowledgement. Repeated events
do not extend it. Expiry/error is recorded and critical suspend is still
acknowledged, never Processing or Rejected. Ordinary suspend is rejected.
A stopped DMA owner must recover devices/time before admitting workloads.

Reconciled with f1-110 at staging integration (2026-10-23), recorded in
`acpi-events-power.md`, "One power-source interface": ACPI reserves the same
interface before its ownership writes, call 73 STATUS reads only
`power_source_status` (ACPI record, or APM facts while APM holds the
reservation), and the orderly-shutdown proof for both ACPI S5 and APM Off is
call 73's stopped latch. The platform service keeps only DMA stop and
device/time resume. Its owner is `core/power_platform.c` (lead decision
2026-10-23; `acpi-events-power.md`, "APM suspend-service owner"), so APM
connects on pre-ACPI machines; devices stopped for a critical suspend stay
stopped until the next boot.

[Linux v6.12 `arch/x86/kernel/apm_32.c`](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/apm_32.c)
is reference only; no donor code, notices or library objects are imported.
The host model measures per-call overhead in microseconds; that is not
physical BIOS latency or evidence of hardware suspend/power removal.
Runtime qualification and f1-110 UI/syscall integration remain lead work.

## Owner decision on licences (2026-10-22 23:55)

The owner removed licence-compatibility gating: "integra tutte le licenze GPL varie versioni, MIT ecc non mi interessa che siano diverse tu metti tutto e basta".
- **Yamaha DS-XG (f1-119)** is no longer deferred. Its microcode is included from the best available source, with its notice.
- **Notices.** Every imported file keeps its notice and origin line in the credits.
- **Redistribution.** The owner redistributes regardless (2026-10-22 23:55); no release-time licence list is kept.

## f1-115 kernel-private floppy interface (2026-10-23)

This implementation adds no syscall, SDK header or exported record layout.
The private service in `src/kernel/drivers/floppy.h` uses the existing
`blkdev` callbacks, 512-byte sectors, monotonically advancing media generations
and cache-disabled durability. Standard geometry/rate selection covers 720 KB,
1.2 MB, 1.44 MB and 2.88 MB (perpendicular mode); 360 KB in its own drive is
also represented. Non-primary or unqualified FDC routing is refused.

The private lease identity is controller + drive + owner + owner generation +
monotonic lease serial. A native mount holds it from before FAT preparation
until cache/namespace detach. A DOS owner is its immutable VM ID/generation.
Acquisition refuses an existing owner; stale lease copies cannot transfer or
release a replacement lease. Controller requests serialize behind one bounded
gate. No physical guest DMA address or physical I/O bitmap permission is used.
Stop masks DMA2, gates the controller and proves readback; failed proof retains
the buffer and all resource claims in quarantine and masks/detaches IRQ6.

`storage_add_floppy` publishes whole media at A:/B: through the existing FAT
driver; the FAT12 implementation remains f1-115b. Surprise removal expires the
cache, identities and drive generation before releasing native ownership.
`floppy_mount`/`floppy_unmount` are trusted kernel mount/eject entrypoints.

`dosvm_floppy_assign` and `dosvm_floppy_image` assign media only to a created,
not-yet-started VM. Image files use the VM's existing VFS table and deny-all
sharing. Virtual DOR/MSR/FIFO/DIR/CCR and INT13 00/01/02/03/04/05/08/15/16
share that provider. Reset remains virtual; seek/recalibrate can acknowledge a
changed physical medium through the VMM. IRQ6 and the existing virtual 8237
state are per VM. Diskette parameter tables use spare bytes following the
existing INT1E/1F synthetic BIOS stubs; their code bytes and the entire DFVM
and DPMI ROM page stay unchanged. Unsupported command families return errors.
Research and verification are in [f1-115 validation](../validation/f1-115-floppy-controller.md).

## f1-105/f1-106 implementation interface (2026-10-22)

The existing Device Manager v1 layout and state numbers remain unchanged.
An EHCI controller with only companion routing is ABSENT (native high-speed
service unavailable), with a `route_only_*` reason, never READY. Companion
relationships are PCI bus/device/function locators, not USB VID/PID aliases.
The kernel-only `usb_controller_inventory` v1 snapshot is 72 bytes: eighteen
32-bit words (`version`, `size`, `pci_locator`, `parent_locator`, `flags`,
`quirks`, `ports`, `companions`, `companion_functions`, `ready_functions`,
then eight `port_companion` locators). Locator bit 31 means valid, bits
23:16/15:8/7:0 contain PCI bus/device/function. Zero means unknown. Flags
are OWNED=1, ROUTE_ONLY=2, HIGH_SPEED=4, TT_UNSUPPORTED=8, QUARANTINED=16.
Quirks are NVIDIA_31BIT=1, AMD8111_REFUSED=2, VIA_SLEEP=4. The snapshot is
an internal, size/version checked interface; it creates no new syscall.
The Device Manager v1 reason includes the companion parent locator when known.
Ready-function snapshots recheck the pinned companion records and register/DMA
leases; a failed companion cannot receive another port hand-back.

Research and resulting implementation decision: Intel EHCI 1.0
[specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
sections 2.2–2.3, 3.5–3.6, 4.2, 4.8.2, 4.10 and 5.1 define routing,
ownership, QH/qTD and stop/unlink. Keep `usb_core.c:ehci_route` the sole
firmware handoff owner; initialize companions before switching CF to one.
Same-slot companion groups with one EHCI and a complete HCS routing map
are admitted; ambiguous or missing groups stay route-only. A low/full-speed
root port goes to its companion; a low/full-speed child of a high-speed hub
gets an explicit unsupported record, without issuing unsupported split DMA.
High-speed bulk MPS is 512; control MPS is 64. Boot HID remains bounded to
64-byte endpoints with one transaction per microframe (larger/high-bandwidth
endpoints refused). Supported interrupt intervals are scheduled in hardware;
intervals above the 1024-frame list period are explicitly refused.

The existing 20,480-byte arena is sufficient (including zeroed 64-bit buffer extensions): frame list 0–4095, async head
4096–4191, general QH 4192–4287, qTD 4288–4351, control buffer 4352–4415,
64 periodic slots of 224 bytes at 4608–18943, and 1024 bytes of bulk bounce
data at 19456–20479. No capacity or memory limit grows. General transfers
use asynchronous advance and schedule-stop fallback; periodic reuse requires
unlink plus two complete frames, and uncertainty retains DMA and leases.

Linux v6.12 [ehci-pci.c](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-pci.c)
and [pci-quirks.c](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/pci-quirks.c)
are behavioral references: honor NVIDIA's 31-bit QH limit (already satisfied
by the existing 768 MiB DMA bound), refuse AMD8111 high-speed activation,
and apply the VT6212 sleep-time bit with a byte config access. No forced BIOS
semaphore clear is permitted. Linux's
[queue](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-q.c),
[periodic](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-sched.c),
and [timer](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-timer.c)
implementations corroborate cached-QH retirement and polling after lost IRQs.
The implementation is written against the EHCI specification, without
transplanting Linux code. The Windows 98 Resource Kit, chapter 30, “Using
USB”/“USB Topology” (`wrkc30_2.htm`, local research media), supplies the
host/hub/device enumeration model, not USB 2.0 register semantics.

## f1-113 analog and virtual joystick interface

Research before implementation checked the upstream Linux game-port files
[`ns558.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/ns558.c),
[`analog.c`](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/analog.c),
[`gameport.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/gameport.c)
and [`emu10k1-gp.c`](https://github.com/torvalds/linux/blob/master/drivers/input/gameport/emu10k1-gp.c).
The audit above pins the first three to v6.12; fetching those tag URLs failed
in this sandbox, so the browsable upstream master was a corroborating reference,
not a claimed v6.12 source download. No donor code is imported. RC bits 0–3
fall after a trigger, buttons 4–7 are active-low, and a 3 ms deadline bounds
sampling. Probe only assigned PCI analog game-port functions and ISA 201h,
claiming before writes. PCI names alone never authorize audio/MIDI configuration.
Creative 1102:7002–7005 uses BAR0; other PCI class 09:04, programming interface
10h analog functions use their first assigned I/O BAR (PCI-SIG class 09h, note 1). Disabled or unknown layouts
remain unsupported. Do not enable an audio device's legacy decode here.
The PIT-derived monotonic counter supplies actual elapsed clocks; reject
falling edges across a sampling gap above 100 us rather than invent timings.
A normal-priority dedicated worker samples with interrupts enabled (20 ms when
connected, 1 s when disconnected; full-axis rediscovery once per second).
Three consecutive uncertain samples neutralize the source; disconnected
axes expire and publish neutral/release state. No memory limits grow.

The Win98 Resource Kit wrkc28_3.htm:97 and wrkc30_3.htm:19–75 describe HID
mapping into trapped legacy joystick interfaces. This is the selected V86
model. IBM's [PC/AT Technical Reference BIOS listing](https://www.bitsavers.org/pdf/ibm/pc/at/1502494_PC_AT_Technical_Reference_Mar84.pdf)
(JOYSTICK_SUPPORT, INT15/AH=84) is the original register-contract reference;
its PDF fetch was blocked. The upstream [DOSBox-X BIOS implementation,
lines 5713–5748](https://dosbox-x.com/doxygen/html/bios_8cpp_source.html)
corroborates the two subfunctions, register ordering and CF on absent input.
No BIOS/emulator code is copied. Each VM has private RC deadlines, reads cached normalized native
state only, and never changes the physical PIT. Absent axes stay high after
trigger (disconnected resistance); BIOS positions are zero for missing axes.
No connected axes sets CF (positions zero, switches open); unsupported DX
returns CF/AH=86h. Virtual resistances map -32767..32767 to
24..1432 PIT clocks (about 20..1200 us); BIOS positions map to 0..416.

Analog locators 257–264 extend the logical namespace after the unchanged 256
USB locators: at most four retained physical game ports with two logical sticks
each. No USB host/device, registry or VM memory ceiling changes. A port defaults
to one four-axis/four-button stick (X/Y/Z/Rx); two-stick configuration exposes
X/Y and two buttons per child. Mode change invalidates both logical generations.
Existing f1-104 capabilities/state/calibration sizes and commands are unchanged.
Analog capabilities use host=FFFFFFFFh, address=the I/O base and interface=the
logical child (0/1); applications distinguish this transport by its locator
namespace rather than treating these fields as a USB address.
`INPUT_JOY_CONTROL` (4904h) on the input grant takes the 100-byte pack(4)
`ciuki_joystick_control` version 1. QUERY=0 returns axes/buttons/attached plus
first four normalized/raw axes; CALIBRATE=1 forwards minimum/centre/maximum
for axis to the existing calibration API; ROUTE=2 selects the default DOS
source(s); CONFIGURE=3 selects four-axis=0/two-stick=1 wiring; GET_ROUTE=4
returns the current route; NEXT=5 finds the next attached joystick after device.
QUERY/NEXT never probe. All unused request outputs and reserved words must be
zero. State/calibration/configuration require exact generations; discovery can
use generation zero. ROUTE validates both live identities transactionally.
Device zero disables DOS joystick input. In two-stick mode, second_device and
second_generation optionally select stick B. Disconnect/stale identity yields
no input, never an automatic substitution. VMs snapshot the default route at
creation; these are read-only logical subscriptions, not additional physical
leases. Multiple VMs may subscribe without owning or configuring the hardware.

The target layout adds 4,360 bytes of static port/route storage and 64 bytes
of private state per VM; the latter fits the existing page-rounded VM record.
The worker uses the existing task stack and admission limits. These are compiled
layout measurements, not physical-machine free-memory evidence.

Control Panel uses authenticated desktop channel requests, brokered by the
existing input-grant owner, for discovery, live test and three-point calibration.
Device Manager links to this same panel. Per-game file writes run in a bounded
Control Panel job, leaving its input/event thread responsive. Calibration stays
per session; no physical device is configured by UI refresh.
Per-game joystick properties persist
separately from DOS memory/network settings. CREATE's formerly reserved words
1–6 (offsets 88–108) carry device/generation/second-device/second-generation/mode/session;
all zero selects the desktop default and device FFFFFFFFh explicitly disables.
Unused fields in other operations retain their zero requirement. Older callers stay compatible.

Saved explicit selections include a nonzero 32-bit boot-session discriminator,
returned at offset 96 of the common control record. The existing entropy service
supplies it in the sampling worker, outside IRQ exclusions and RC loops. It is
an accidental-rebinding guard, not an authorization credential. An unavailable
entropy service leaves it zero and explicit per-game persistence returns EAGAIN;
the native API and global DOS default still work. CREATE rejects a mismatching
session with ENODEV. Default/off preferences contain zero session and survive
reboot; explicit controller preferences require reselection after reboot rather
than automatically trusting a boot-local locator. The six-word settings grammar
also rejects incoherent tuples and integer overflow. No entropy policy changes.

### f1-113b persistent identity amendment

This supersedes the session-only persistence policy above. Research checked the
[USB-IF USB 2.0 specification entry](https://www.usb.org/document-library/usb-20-specification)
and Microsoft's [USB serial-number explanation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-faq--introductory-level)
and [string descriptor request documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-string-descriptors)
(USB 2.0 sections 9.6.1/9.6.7). VID/PID alone does not distinguish identical
controllers. Retain the full serial descriptor as a SHA-256 fingerprint; no
truncation or display-name matching. With no serial, use PCI bus/device/function,
the root/hub port chain and HID interface. Serial identities include VID/PID and
interface but exclude the port. Unreadable advertised serials cannot persist.
Game-port identities use I/O base and logical child, since analog hardware has
no serial. Identical serial-less controllers remain tied to their sockets.
The owner's Resource Kit wrkc28_3.htm:97 and wrkc30_3.htm:19–75 continue to
define the V86 cached-input mapping; no physical VM I/O or NTVDM is introduced.

Use the existing `/system/settings` configuration directory and INI pattern.
Private fixed-width calibration and route records have a version, sequence and
checksum, encoded as hex INI values. Alternate two committed files so a torn
write preserves the previous valid record. A worker owns all storage operations.
The storage worker is separate from the RC sampler (250 ms service cadence), so
disk admission/completion cannot stall analog sampling. Control requests only
update bounded pending state under the existing UP input
boundary. Log rejected/corrupt records and use defaults (or the previous valid
slot). Old boot-local explicit preferences cannot be migrated to identities:
log their rejection; default/off records can migrate without guessing hardware.
Saved calibration is reapplied to each new generation of the matching controller.
Store wiring and the global DOS route as well as catalog games 1–3.
The bounded calibration cache covers the full existing 264-locator namespace;
there is no smaller controller admission limit. Wiring has one authoritative
record per I/O base, in logical child 0, to avoid conflicting child settings
after an interrupted save. File handles and VFS table reservations are released
after each record operation. Existing task/stack and registry limits remain.

The 100-byte v1 control layout and existing operation numbers remain unchanged.
SAVE_GAME=6 takes axis=catalog game, plus the existing route tuple. Device zero
means use global default; FFFFFFFFh disables. SAVE_STATUS=7 takes zero inputs and
returns buttons=pending writes and minimum=last negative storage error. Changes
are acknowledged as queued; the Control Panel polls until storage completion.
CREATE with an all-zero explicit tuple uses the saved catalog route, resolved
against live identities at creation. Missing saved A/B falls back to the first
attached controller; CREATE's `changed` output is JOYSTICK_FALLBACK=1 and the
launcher shows a note. Running VMs retain their generation-qualified snapshot.
No implicit rebinding takes place inside VM reads. Direct explicit session
tuples keep their original validation. Limits, IRQ/timing and physical-port
ownership are unchanged. Newly authored code only; no donor implementation.

### f2-125 per-game properties integration

The shared joystick/COM/LPT panel, next-start snapshot, persistent port format,
missing-device fallbacks and joint BIOS equipment word are specified in
[Game properties](game-properties.md). The joystick ABI adds GET_GAME=8 in the
existing control record; public record sizes and existing operation values stay
unchanged. Analog game-port inventory IDs use 72000h onward and LPT IDs
71000h onward (the integ-periph Device Manager assignment), so both classes
keep distinct IDs.
