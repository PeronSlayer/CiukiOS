# F1 USB host and boot HID contract

Written for directive `f1-42`, 2026-10-13. Lead cross-review and QEMU/hardware
qualification remain required before integration; host fixtures are not boot evidence.

## Sources checked before implementation

- [Intel UHCI Design Guide 1.1](https://ftp.yz.yamagata-u.ac.jp/pub/NetBSD/misc/blymn/uhci11d.pdf),
  sections 2–5: 4 KiB frame list, 16-byte aligned QH/TD pointers, active/error
  status, low-speed bit, port reset and PCI C0 legacy support.
- [OpenHCI specification 1.0a](https://www.cs.usfca.edu/~cruse/cs698s10/hcir1_0a.pdf),
  sections 4–7, appendices A/B: 256-byte HCCA, ED/TD alignment, dummy tail,
  explicit toggles, ownership request, frame timing and root hub write semantics.
- [USB 1.1](https://ieeemilestones.ethw.org/File:Usb11.pdf) and
  [USB 2.0](https://www.usb.org/document-library/usb-20-specification), chapters
  8/9/11: address-zero serialization, reset/debounce/recovery, descriptor
  lengths, setup/data/status stages, configuration and hub port features.
- [HID 1.11](https://www.usb.org/sites/default/files/documents/hid1_11.pdf),
  sections 7.2 and appendices B/C: Set_Protocol, Set_Idle, Set_Report LED
  output, eight-byte keyboard state, rollover, three-byte boot mouse.
- [Linux v6.12 OHCI root reset](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/ohci-hub.c):
  five 10 ms hardware pulses meet USB 7.1.7.5’s 50 ms root reset requirement.
- [USB HID Usage Tables](https://www.usb.org/hid), Keyboard/Keypad page
  and Generic Desktop Wheel (0x38): usage-to-set-1 conversion and signed
  relative wheel units.
- [QEMU USB HID descriptor source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/dev-hid.c):
  its fixed mouse layout has five button bits and one combined X/Y/wheel input.
- [Intel EHCI 1.0](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
  sections 2.3.8/4.2 and legacy capability: CONFIGFLAG=0 routes to companions.
- [Linux v6.12 PCI USB handoff](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/pci-quirks.c):
  UHCI C0 RW/W1C masks, OHCI ownership and EHCI BIOS/OS semaphores.

Repository comparison: `i8042.c` owns the shared ordered input queue and
accepts repeats as KEY value 2. It has no software typematic timer: native
PS/2 devices supply repetition. USB supplies 500 ms initial delay and 33 ms
repeat interval through that same transition/text policy. `registry.h`
provides pinned leases and permanent quarantine; `devices.c` exports retained
observations only. The build enumerates source directories explicitly, so
`usb/` must be added. Device Manager already ships both PCI and USB ID JSON,
but only loads PCI IDs; USB lookup is added without changing record layout.

## Scope and admission

PCI class 0C/03 prog-if 00/10 selects UHCI/OHCI; prog-if 20 receives only
EHCI ownership/stop/companion routing, never an EHCI transfer schedule.
No xHCI, bulk, isochronous, storage or general HID report parser.
All native USB activation and EHCI routing return before config/register
writes, claims or allocations in safe mode or firmware-first input policy
(including INPUT_FORCED). This deliberately preserves the E500 BIOS lease.
UHCI disables C0 legacy SMI/PIRQ enables; OHCI requests ownership and waits
for IR to clear, then disables implemented emulation. EHCI requests OS
ownership before stopping; a failed handoff retains quarantine, never forces
the firmware semaphore. No i8042 ports or PIC masks are touched by USB.

Fixed bounds: four controllers, sixteen device records per controller, eight
root/hub ports, four boot HID interfaces per device, one external hub level,
512-byte configuration and 256-byte report descriptors, 64-byte packets
(eight at low speed), seven-bit addresses. Unsupported devices stay visible.
Addresses and record slots are retained for the boot, including disconnects
and quarantine; exhaustion is explicit, never aliases a live address.

## DMA and execution

Each host owns two contiguous physical pages below the direct-map limit,
claimed along with its port/MMIO aperture and valid PCI IRQ (shareable,
masked at the controller). DMA stays disabled until all schedule pointers,
sentinels and bounce buffers are initialized. Only offsets within these
pages become bus addresses; callers' pointers never do. PCI command writes
are 16-bit and never echo W1C status. Controllers poll with USB interrupt
enables off; no unqualified IRQ handler or routing is installed.

One P_DEVICE service thread serializes enumeration and transfers, including
address zero. It sleeps after each bounded poll and each device, keeping IF
enabled and permitting scheduler progress. USB does not block the common
work queue with enumeration. Every transfer uses at most one live packet TD;
control data stages split at endpoint-zero max packet size. NAK is normal for
interrupt polling, STALL/timeout/protocol errors quarantine the device. Host
system errors or failed retirement quarantine the controller and its leases.
No descriptor/buffer reuse until the schedule is unlinked/skipped and two
hardware frames have elapsed. A stalled frame clock fails closed. DMA and
claims remain pinned on uncertain retirement; there is no runtime recovery.

## Enumeration, hubs and input

Debounce connections 100 ms; root reset is held at least 50 ms with 10 ms
recovery. Set_Address is followed by 2 ms recovery. Validate every descriptor
boundary and endpoint before Set_Configuration. Ignore nonzero alternate
settings. Hub power and reset use class requests with bounded status polling;
reject nested hubs, excess ports and overcurrent. Disconnects recursively
release input state for that device and children, keeping inventory records.

HID boot interfaces receive Set_Protocol(0) and Set_Idle(0). Keyboard array
diffing ignores duplicates, holds the last valid state on rollover, tracks
modifiers and releases all held keys on detach/quarantine. Independent USB
and PS/2 key/button ownership is merged, so one device's release cannot
release another device's held key. LED output is a one-byte class Set_Report.
Wheel support is restricted to a validated fixed three-to-five-button/X/Y/wheel
report layout with signed eight-bit relative axes, no report IDs or push/pop;
this narrow four-byte layout uses Set_Protocol(1). Other mice stay in boot
protocol and their device-specific trailing bytes are ignored. General
report-protocol support remains deferred.

Input source/generation and sequence remain in the shared kernel queue.
Wheel has a distinct relative axis/public event type; desktop delivery must
never translate wheel into Y motion. Device Manager uses the existing record
shape, an added USB bus value, stable controller/address identities and USB
ID names. Driver-array capacity remains fixed: one aggregate `usb-host`
entry describes UHCI/OHCI/handoff and HID bindings.

## Evidence required

Host ASan/UBSan register/DMA models execute production UHCI/OHCI code,
enumeration, hub requests, HID transitions, retirement, DMA bounds, malformed
descriptors, NAK/STALL/timeouts, handoff and policy guards. Compile i686 units
with the production flags; run affected input/inventory/UI regressions
sequentially with nice 19. The lead supplies canonical kernel build/link,
QEMU USB-only desktop typing and hardware evidence before qualification.

Public additions retain all record sizes and prior constants: USB bus=3,
input source=3, WHEEL type=6 (signed detents in value; other fields zero).
The DOS window filters WHEEL because the current DOS VM mouse ABI has no
wheel consumer. Native clients receive it through the existing DESK_MOTION
message with its distinct input event type.
