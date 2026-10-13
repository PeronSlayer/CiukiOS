# F1 USB host, boot HID and mass storage contract

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
No xHCI, isochronous or general HID report parser. F1-43 adds the bounded
bulk/storage extension below.
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

One P_DEVICE service thread owns enumeration, including address zero. The
host transfer mutex also serializes synchronous storage callers against HID
and enumeration; all controller schedule/bounce-buffer access takes it.
The service sleeps after each bounded poll and each device, keeping IF enabled and permitting scheduler progress. USB does not block the common
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


## F1-43: USB mass storage (2026-10-13)

Implementation directive: `docs/directives/f1-43-usb-mass-storage.md`.
This extends F1-42 without adding a controller family or IRQ routing. Cross-review,
canonical link, QEMU file-copy and physical-device evidence belong to the lead.

### Research and repository comparison

- [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 3–6: interface 08/06/50, Get Max LUN stall allowance, 31-byte CBW,
  13-byte CSW, tag/residue/status validation, endpoint halt and reset recovery.
- [T10 SBC-3 working draft](https://www.t10.org/ftp/t10/document.05/05-344r0.pdf),
  READ/WRITE(10), capacity and SYNCHRONIZE CACHE(10): USB ACK or successful
  WRITE status does not by itself certify stable-media durability.
- [QEMU 9.2 USB storage](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/dev-storage.c)
  and [SCSI disk](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/scsi/scsi-disk.c):
  full-speed bulk endpoints, BOT state transitions, fixed-format sense,
  capacity and synchronous cache completion (reference, no imported code).
- The UHCI/OHCI manuals above define bulk scheduling: UHCI uses its existing
  nonperiodic QH, OHCI uses BulkHeadED/BulkCurrentED, BLE and BulkListFilled.

Repository findings: `blkdev_durable()` admits enabled caches only with a
qualified flush callback. `fat_enable_write()` can recover the dirty flag,
so removable orchestration explicitly refuses that transition. `blkpart`
and the counted storage view must forward media generations. Native Files
already lists `/mnt/<letter>` and displays drive letters; its old 16-bit
predecessor in `src/apps/` is outside this implementation. Namespace/cache/host
lock order is preserved by dropping the USB service's host lock before mount
or detach. Storage records, views and letters remain allocated for the boot.

### Admission and command transport

One alternate-zero transparent SCSI BOT interface per full-speed device,
exactly one bulk IN and one bulk OUT endpoint, legal 8/16/32/64-byte packet
sizes. LUN zero only; other LUNs remain unmounted. Low-speed, UAS, CBI,
additional BOT interfaces, READ CAPACITY(16) and non-512-byte media are
unsupported. Existing controller/device/address bounds still apply.

`usb_bulk` splits at endpoint packet size and advances DATA toggles only
on ACK, including short/ZLP packets. Caller buffers use the existing DMA
bounce buffer; no user/stack pointer becomes a bus address. OHCI's endpoint
max packet field uses descriptor size even on the 13-byte CSW. Retirement
retains F1-42's two-frame rule. Each bulk stage is bounded at 64 KiB/10 s;
each packet retains the controller's 500 ms limit and sleepable polling.

Commands: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10),
READ(10), WRITE(10); SYNCHRONIZE CACHE(10) qualifies and implements
flush; a four-byte MODE SENSE(6) header observes write protection. Initial
power-on unit attention may be consumed with at most three readiness probes,
before publishing a mount or issuing writes. Capacity sentinel FFFFFFFFh and
any block length other than 512 are rejected. Block requests are at most
128 sectors; range and READ/WRITE(10) arithmetic are checked before issuance.

CBW/CSW are encoded bytewise with correct little-endian wrappers and
big-endian SCSI fields. Validate exact wrapper lengths, signatures, matching
tag, bounded residue and status. Data-stage STALL clears the affected halt
and reads CSW; CSW STALL permits one halt-clear/CSW retry. Phase errors,
malformed wrappers and uncertain transfers perform class reset then clear
IN then OUT and quarantine storage. This restores framing only: never resend
a failed CBW, never retry an issued write, never recover a writable mount
from a transport error. Command-failed status obtains fixed-format sense.
Published-media unit attention or medium absence revokes the old medium.

### Mounts, hot-plug and removal

Each admitted disk is a `blkdev` beside ATA. Scan the existing MBR/EBR parser
and make `blkpart` views; validated whole-disk FAT is also admitted. FAT12,
FAT16 and FAT32 use the existing read-first validation/cache/native mount path.
USB volumes have `VFS_DRIVE_REMOVABLE` and receive E: onward, skipping existing
letters; exhausted letters/record slots fail explicitly and are never recycled.

Clean validated FAT may enable writes only after a successful synchronous
cache probe. Unsupported flush or write-protected media remain read-only.
Dirty FAT stays read-only with its diagnostic reason, including calls to
`storage_enable_write`; this step supplies no confirmation/recovery override.

The USB service polls readiness once per second; physical block I/O probes
readiness before READ/WRITE. Root disconnect/change/overcurrent is also checked
between packets; child detach follows the hub scan. A media-change or uncertain
I/O condition discards cached blocks/names and detaches all the device's FAT
partitions without clean flags or flushes. Old native descriptions and directory
cursors fail rather than reaching replacement media. F1-42's no-runtime-recovery
policy remains: absent media at admission or changed media require unplug/replug
(or reboot if the controller/port is quarantined). A new device gets a fresh
retained record and generation. This intentionally excludes live card-reader
media reinsertion without reconnecting the USB device in this step.

`/dev/media` exposes copied, versioned `MEDIA_GET_STATE` (120 bytes: version,
size, mounted-removable mask, reserved=0, 26 generation values) and
`MEDIA_SAFE_REMOVE` (16 bytes: version, size, drive index, media generation)
through existing devctl call 69. Existing syscall numbers/record sizes stay
unchanged. The removal generation prevents a stale menu from removing another
medium. Requesting one partition removes the entire physical USB device.
All partitions are preflighted for native and legacy open descriptions before
any commit/clean flag. Busy handles return EBUSY. Commit caches, synchronize
media, then detach/clean FAT and invalidate cache under the namespace lock.
Any failure keeps the action unsuccessful and reports an error; no UI claims
that unplugging is safe. After success, block I/O is disabled until reconnect.

Files Computer view shows mounted removable letters with the Tango removable
icon/type. The File menu and drive context menu provide “Safely remove”; the
tray has a Tango removable icon and one removal row per mounted letter.
Keyboard Up/Down scroll long tray lists; Escape dismisses them. Both report
busy/errors and only successful removal displays the safe-to-unplug message.
Verbose lifecycle, SCSI sense, transport faults and mount/eject records begin
`[usb-storage]`. No QEMU or physical qualification is claimed by host fixtures.
