# F1 USB host, boot HID and mass storage contract

## F1-96b: drained TD ring, QH element erratum, recovery ladder, re-plug (2026-10-22)

Evidence (T23, image 5a10eef4, serial `t23-5a10eef4-serial.txt` lines
4444–4537): a 64 KiB READ(10) (tag 155) ended its data stage after 44 ms
(`frames=45`, deadline 10 s later) with `td=388007ff`: Active, C_ERR=3, no
NAK/error bit, ActLen unwritten, i.e. a TD the HC never executed;
`packets=273 index=272 offset=17408 stop=packet result=-110`. The BOT class
reset then timed out (`reset addr=2 result=-110`, 500 ms packet budget),
`quarantine addr=2 reason=reset_recovery`, the port was disabled and, by the
F1-53 rule, stayed blocked (`reason=quarantined`) for the rest of the boot.

Root cause (driver, reproduced on the host model): the f1-96 ring loop kept
`stopped` = the status of the last TD it walked. When the HC had executed
every armed TD before the driver woke (the walk reached `armed`), `stopped`
held that TD's inactive full-ACK status; the refill then armed new TDs and
`progress<armed && !(stopped&Active)` ended the stage as if a short packet
had arrived, unlinking the QH under the TD the HC was about to run. The
T23 index 272 means the HC drained 127 TDs (145–271) between two driver
polls, at least 7 frames at the 19-packet full-speed maximum. The host model
with a driver waking every 9 frames reproduces the exact record
(`td=388007ff … stop=packet … result=-110`, then reset and quarantine).

Research before implementation:
[UHCI 1.1 §3.4.1.3, §3.3.2, Table 8](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf):
the HC fetches the QH element; an inactive element sends it to the
horizontal link without advancing (step 2→10); a successful TD is marked
inactive, then its link is written to the element (steps 6, 9); a queue is
never advanced on error or NAK; element bit 2 may vary as a side effect of
the update. [ICH3-M §5.16.2.2](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)
repeats the order for the 82801CAM (status update step 10, element write
step 11, inactive element step 2→12) and states UHCI 1.1 compliance; it
lists no queue-advance erratum. Linux [b761d9d867bc "UHCI: Work around old
Intel bug"](https://github.com/torvalds/linux/commit/b761d9d867bcc29e8de3e62d1d72b27e75078ca6)
(Alan Stern, 2006-05-12, seen on PIIX3 "Neptune"): `uhci_advance_check` in
[uhci-q.c v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/uhci-q.c)
— "Early Intel controllers have a bug which causes qh->element sometimes
not to advance when a TD completes successfully. The queue remains stuck on
the inactive completed TD" — sets `qh->element = qh->post_td->link` when
the element equals the last completed TD after `QH_WAIT_TIMEOUT` (200 ms,
uhci-hcd.h). Linux links TDs breadth-first (`LINK_TO_TD`, no `UHCI_PTR_DEPTH`
since v2.6.17; v2.6.16 set depth-first on every few TDs only after an FSBR
timeout) and relies on FSBR for several transactions per frame; it
appends by activating an inactive dummy TD (`uhci_submit_common`). We keep
depth-first links without FSBR (f1-96). Linux
[usb-storage transport.c v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/transport.c)
`usb_stor_reset_common` sends the Bulk-Only reset with a 5 s timeout, then
clears both halts; a failed reset escalates to `usb_stor_port_reset`
(USB device reset). [BOT 1.0 §5.3.4](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf):
Reset Recovery (class reset, clear HALT IN, clear HALT OUT) is the only
way to restore framing after the host abandons a phase. No upstream code is
imported.

Decision:

- The ring walk records `stopped` only for the TD that ended it; a walk that
  reached `armed` leaves it Active, so a drained ring keeps streaming once
  the refill re-arms the slot where the HC waits.
- Early-Intel workaround: when the QH element still points at the newest
  full-ACK TD after `USB_UHCI_ADVANCE_FRAMES` (4) frame boundaries without
  progress, the driver writes that TD's link to the element (the value the
  HC should have written; the slot is not re-armed before its successor
  completes). The HC writes an element only after executing an active
  element TD, so it cannot race this write; 4 frames instead of Linux's
  200 ms because the HC writes the element right after the status, not
  frames later. A fix of the final TD retires the queue as usual. Record
  `[usb] uhci_advance controller=… addr=… ep=… tag=… packet=… element=…
  link=… frames=… fixes=N` (fixes 1–4, then every 64th).
- Same-chain retry, once per data stage: after `USB_BULK_STALL_MS` (200 ms,
  Linux `QH_WAIT_TIMEOUT`) without progress, if the next TD was never
  executed (Active, C_ERR=3, no status bit, ActLen unwritten) or the element
  is off the chain, the QH is unlinked, retired for two frames, the
  completed prefix re-read and the element pointed at the first incomplete
  TD again: same TDs, toggles and buffers, no new CBW. A NAKing device keeps
  its bounded wait. Record `[usb-storage] data_retry addr=… tag=… packet=…
  offset=… element=… td=… stalled_ms=… frames=… relink=… result=…`.
- Recovery ladder for READ(10): any recoverable transport fault of a
  multi-sector command (NAK timeout after the same-chain retry, data/CBW
  STALL, invalid or STALLed CSW, Phase Error, short data with residue,
  ILLEGAL REQUEST) performs Reset Recovery and re-issues the range with the
  next size: more than 8 sectors → 8, otherwise → 1, each with
  `[usb-storage] max_sectors … from=128 to=8 …` / `from=8 to=1`; the device
  keeps the lowered size and its queue limit (4096, then 512 bytes) until
  re-plug. A one-sector READ keeps the F1-74 reset-qualified single retry;
  quarantine (with terminal Reset Recovery) follows only when that fails, or
  when the device no longer answers: a failed Reset Recovery, disconnect,
  host/DMA error or CRC/babble (still fail-closed). Writes are never resent;
  a failed multi-sector WRITE(10) records the next ladder size with
  `resend=0` and quarantines. This supersedes the F1-96 one-sector fallback
  and the F1-74 same-size retry of multi-sector READ(10).
- The Bulk-Only reset request waits up to `USB_STORAGE_RESET_MS` (5 s) for
  its status stage (`usb_device.control_timeout_ms`, bounded by the bind
  deadline); halt clears keep the 500 ms packet budget.
- Re-plug: a quarantined root or hub port stays blocked while the device is
  connected (F1-53/F0-40 unchanged); an observed disconnect (CCS=0) releases
  it (`[usb] release port=… controller=… reason=disconnect`, `[usb] release
  hub=… port=…`), and the next connection is debounced, reset and enumerated
  as a new device generation with a new address and record. A persisting
  overcurrent blocks it again before enumeration. This supersedes F1-53's
  "Quarantine remains permanent for this boot" for device-level port
  quarantine; controller quarantine is unchanged. Device records, addresses
  and drive letters are still not recycled (16 records per controller).

Host model (`tests/host/usb/f1_96b_test.c`, FAT16 fixture, 16 transactions
per frame): 8 × 128-sector READ(10) with the driver waking every 9/20/40
frames complete (the base code fails the first one with the T23 record);
an element stuck after packet 0, 127, 272, 1022 or 1023 costs 72–75 ms per
64 KiB READ(10) instead of the 10 s deadline; a lost element or a wedged
queue is relinked once after 200 ms; a 1.5 s NAK is waited out without
retry; the ladder 128 → 8 → 1, quarantine only after the one-sector retry
and on a reset not answered within 5 s; a 2 MiB copy from E: at wake 9 with
an element stuck every 700 packets completes without reset or fallback.
`usb_test.c` port flood: disconnect releases root and hub ports (UHCI and
OHCI, device and storage-block quarantine) and a re-plug enumerates a new
device. T23 qualification belongs to the lead.

## F1-96: negotiated 64 KiB transfers, UHCI TD ring, pinned queue limit (2026-10-22)

Evidence (T23, image b453bc92): `[files] job=copy done ms=62239
bytes=5458058 rate_kbs=85`, `[storage] queue name=usb-storage … split=1295
… limit=4096` with service buckets `svc=0/0/1365/38` (8–32 ms per command).
Every READ(10) carried 8 sectors: the f1-83 adaptive split (C3) shrinks a
queue whose command exceeds 10 ms and grows it only after commands of at
most 5 ms, which a full-speed device can never meet for 4 KiB, so the limit
stayed on the one-page floor of 7c5b9878. The bind field `max_sectors=1`
was the largest bind-time read (the FAT mount reads single sectors), not a
transfer limit. Each 4 KiB command also paid TEST UNIT READY, CBW, two
32-TD data batches and CSW at about three frames per phase.

Research before implementation:
[BOT 1.0 §§5.1–5.3, 6.5–6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf):
dCBWDataTransferLength is a 32-bit byte count without a class limit;
dCSWDataResidue is the difference between expected and processed data and
never exceeds it; data starts on a packet boundary; an invalid CSW or Phase
Error requires Reset Recovery; a device may STALL the data pipe when host
and device disagree. [UHCI 1.1 §§1.3, 3.2.2, 3.4.1.3, 4.1.3](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf):
depth-first (Vf) queues execute TD after TD inside a frame and write each
successful TD's link pointer back to the QH element; SPD leaves a short IN
at the element; IOC is reported at the end of the frame; without bandwidth
reclamation the HC idles after the terminating QH until the next frame.
[USB 2.0 §5.8.4, Table 5-9](https://www.usb.org/document-library/usb-20-specification):
at most 19 full-speed 64-byte bulk transactions per frame (1216 bytes); the
host model keeps its conservative 16. [T10 SBC-3](https://www.t10.org/ftp/t10/document.05/05-344r0.pdf):
READ(10)/WRITE(10) carry a 32-bit LBA and a 16-bit TRANSFER LENGTH in
blocks. Linux [usb-storage scsiglue.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/storage/scsiglue.c)
limits USB mass storage to 240 sectors by default, notes that Windows 7
uses 128, keeps US_FL_MAX_SECTORS_64 for devices that fail above 32/64 KiB
and skips VPD pages for usb-storage. Linux [uhci-q.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/uhci-q.c)
appends to a running queue by filling an inactive dummy TD and activating it
after a write barrier (`uhci_submit_common`): the controller stops at an
inactive TD. `uhci_advance_check` documents early Intel controllers whose
QH element sometimes does not advance after a successful TD; Linux moves it
by hand after 200 ms. No upstream code is imported.

Decision:

- Transfer length is host policy, recorded as `[usb-storage] bind …
  read_max=<largest bind read> max_sectors=<negotiated>`: 128 sectors
  (64 KiB, the Windows value and our `BLKDEV_READ_SECTORS`) on a host with
  a chained bulk engine (UHCI), 8 sectors on a packet-at-a-time host (OHCI,
  about one packet per frame), never a VPD request. A WRITE(10) never
  exceeds the longest READ(10) that completed with a passed CSW on this
  medium; enabling writes scans the FAT with 64 KiB reads first.
- The usb-storage queue pins its limit (`blk_queue_set_limits`, max = floor
  = negotiated size). C3 already exempts USB source commands from the 20 ms
  same-device bound; the ATA queues keep the adaptive split and floor.
- A data stage longer than the 128-TD window streams through it as a ring:
  slot k%128 holds packet k, links are depth-first to the next slot, the
  final packet terminates the chain with IOC. The HC stops at a slot not yet
  re-armed and resumes in a later frame. Slots are harvested in order and
  re-armed with Active written last, only after the following packet
  completed, so no link changes under a pending QH writeback. A phase of at
  most 128 packets is the former single chain. Completion still requires
  every TD and the HC-written terminating element.
- Fallback: a multi-sector READ(10) that fails with a STALL or CSW error
  (data STALL, CBW STALL, invalid or persistently STALLed CSW, Phase Error,
  passed CSW with residue, ILLEGAL REQUEST) performs the existing recovery
  and re-reads the range one sector per command; the device then keeps one
  sector per command and its queue limit becomes 512 bytes:
  `[usb-storage] max_sectors addr=… cmd=28 lba=… sectors=… from=128 to=1
  reason=… resend=1 fallbacks=N`. A NAK timeout keeps the F1-74 single
  same-size retry. A failed multi-sector WRITE(10) is recorded with
  `resend=0` and quarantines; no issued write is retried. CRC/babble errors
  still fail closed without retry.
- DMA arena: `USB_DMA_BYTES` 20480 (five pages per controller; TDs at
  7680–9728, 64-byte buffers at 12288–20480).

Host model (fixture FAT16, 5,458,058 bytes in 64 KiB chunks as Files
copies, 16 transactions per frame; `wake` = frames per driver sleep, 3 is
the T23 cadence): copy from the stick 5795 ms (919 KB/s) at wake 1 and
6681 ms (797 KB/s) at wake 3, 89 READ(10) of up to 128 sectors; the
f1-83 adaptive floor on the same path needs 10035 ms and 24519 ms
(limit=4096, as on the T23). Copy to the stick 8761/15789 ms: per 64 KiB
the FAT transaction issues two 64-sector writebacks, four SYNCHRONIZE
CACHE and four to five single-sector metadata writes, each READ/WRITE
behind TEST UNIT READY. A 64 KiB data stage takes 64 frames at wake 1 and 66 at
wake 3 (the old 32-TD batches needed a service wake per 2 KiB).

Not changed: the TEST UNIT READY before each block request, the cache
writeback run size (64 sectors) and the OHCI packet path. The early-Intel
QH element workaround is not implemented; on such a controller a stuck
element still ends in the bulk deadline, as before (superseded by F1-96b:
workaround implemented, fallback ladder 128 → 8 → 1).

## F1-81: completion-based bulk retirement (2026-10-18)

Research before implementation: [UHCI 1.1 §§3.4.1.3/4.1.2](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf)
orders TD status before successful QH element advancement; IOC notifies at
the end of the completing frame. SPD, STALL and uncompleted TDs do not
advance the QH. [OpenHCI 1.0a §§6.4.4.6/6.4.5](https://dl.xkwy2018.com/downloads/documents/hcir1_0a.pdf)
orders ED dequeue before done-queue insertion and publishes the done list
through HCCA/WDH; a TD condition code alone is insufficient. DI=0 requests
the earliest done publication. [BOT 1.0 §§5.3/6.3–6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
still requires completion of data before CSW and the existing stall/reset
recovery sequence.

Repository comparison: small UHCI phases (including 512-byte metadata) used
one schedule submission and two retirement frames per packet; larger phases
used bounded 32-TD depth-first batches with the same retirement floor. The
native notification wait also consumed its full millisecond after an IRQ.

Decision: use the existing pinned 32-TD window for every nonempty UHCI bulk
phase, with IOC only on the last TD of each batch. A full successful prefix
and the HC-written terminating QH element prove completion, allowing reuse
without extra retirement frames. Polling checks this proof every millisecond;
the native wait may return on an IRQ so the caller can recheck DMA immediately.
OHCI bulk packets require both ED dequeue and consumption of that exact TD
from the saved HCCA done chain before reuse. IRQ delivery alone never proves
retirement. OHCI retains its packet-at-a-time data schedule.

This supersedes the blanket two-frame bulk rule below only for proven
completion. Short/error/timeout/cancelled UHCI batches and incomplete OHCI
packets still unlink/skip and wait two HC frames; late ACK accounting,
frozen-frame containment, F1-74 recovery and the 128-sector read limit remain.
Control, HID schedules and mandatory elapsed-time reset/recovery/debounce
delays retain their existing rules. No register or PIC programming is added.

## F0-39: IRQ loss preserves polling service (2026-10-18)

This amendment supersedes the whole-line stale-lease/fatal-member policy
below; see [device ownership](device-firmware-ownership.md#f0-39-shared-irq-fault-isolation-and-external-stalls).
IRQ lease revocation clears only its chain member. UHCI/OHCI retain their
valid register and DMA leases, schedule, frame clock, configured devices and
workers. They emit `[usb] irq_lost controller=... mode=poll` once and use the
existing frame polling/1 ms HID fallback and worker storage cadence.
Fallback reuses existing local interrupt-disable masks and readback, without
new register definitions, PIC/routing changes, controller reset or BME changes.
Lost IRQ leases remain pinned and cannot authorize resource/DMA release.
Register/DMA lease loss, retained fatal status and a failed local mask remain
controller faults. OHCI done-head capture and two-frame retirement are retained.

Research: [UHCI 1.1 §2.1.3](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf)
retains status for polling when interrupts are disabled. The existing
[OpenHCI 1.0a §§7.1.5–6](https://dl.xkwy2018.com/downloads/documents/hcir1_0a.pdf)
interrupt masks and [upstream OHCI completion path](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/ohci-hcd.c)
separate controller progress/done processing from interrupt delivery.

## F1-74: bulk completion snapshot and removable publication (2026-10-16)

Research before implementation: [UHCI 1.1 §§3.2/3.4/4.1](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf)
defines independently updated TD status, ACK/NAK, IOC and SPD (short IN
leaves the QH element unchanged). [BOT 1.0 §§5.3/6.3–6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
defines data before CSW, one clear-IN/CSW retry on status STALL, and reset,
clear-IN, clear-OUT recovery. Notifications are wakeups, never DMA retirement.

Repository comparison: the complete T23 e8dfd0cd trace reports tag 207's
READ(10) data timeout at 64576/65536 bytes, with `csw_available=0` because
CSW was never attempted. `retired=0` means successful retirement. In UHCI's
batch poll the prefix walker could observe an active TD, then a separate
Active-bit test could observe its completion and prematurely unlink the
unfinished suffix. This interleaving is legal independently of interrupt
delivery. Use the same TD status snapshot for both prefix progress and the
stop decision; only an observed short/error or complete batch ends polling.
Keep two-frame retirement before copying, toggle accounting or CSW submission.
The host fixture must inject completion between those observations through
the production F1-72 IRQ notification path; this reproduces the software
failure mechanism, not proof of the precise T23 bus event.

A READ(10) transport fault with proven retirement and a live controller may
perform BOT reset recovery and replay the whole command once with a fresh tag.
The retry retains the aggregate bind and diagnostic deadlines; each replayed
stage keeps its existing bound. Failed recovery, repeated
failure, disconnect, host/DMA errors and uncertain retirement quarantine.
Writes and flushes are never replayed. Existing data/status STALL handling
precedes reset recovery, and command-failed sense/media-change handling remains.

Removable FAT volumes publish read-only after bounded metadata reads; no
census or ownership traversal runs during attach. The explicit write-upgrade
path completes ownership validation before any dirty flag or repair write.
Fixed disks keep their policy. USB volume and hardware-check records report
mount time, ownership scan state and scan time separately. See the F1-74
amendment in [VFS/storage](vfs-storage-contract.md).

## F1-72: completion interrupts and shared-line EOI (2026-10-16)

This amendment supersedes F1-71's frame notifications below. The UHCI
inactive schedule tail has IOC clear; control, bulk and HID TDs retain IOC
on attached IRQ controllers. OHCI enables WDH, UE, RHSC and MIE
(`0x80000052`), with SOF disabled. Its handler ignores disabled SOF status
and acknowledges only observed completion, error and root-hub causes.
See the appended [research and validation](../validation/f1-71-shared-pci-interrupts.md#f1-72-completion-interrupts-and-pic-mask-writes).

Review corrections: UHCI fatal handling performs bounded source isolation
and readback in the handler; failure quarantines its attached IRQ lease
before EOI. OHCI disables RHSC locally until the 250 ms worker scan clears
port/hub changes, and ignores disabled causes on peer dispatch. One
IRQ-serialized path saves HccaDoneHead, clears it, issues a barrier and then
acknowledges WDH. Saved done chains are consumed before descriptor reuse;
the IRQ takes no sleeping mutex. See the
[review research and fixtures](../validation/f1-71-shared-pci-interrupts.md#f1-72-review-fatal-containment-rhsc-and-done-head-ownership).

Qualified PCI INTx handlers run with IF clear, acknowledge their own device
causes and return to the trap dispatcher's single `pic_eoi()` call. A complete
bounded pass leaves the PIC mask untouched and queues no continuation.
A source asserting after its member ran remains level asserted and is
delivered again after EOI. On budget exhaustion the chain masks once before
returning for EOI, then completes remaining members in the work thread and
unmasks once with IF clear. An unclaimed streak reaching the existing stuck
threshold, a stale lease or failed continuation queue quarantines the line:
mask before EOI and never rearm. Removing the last member also keeps the
line masked. Quarantine during deferral must not duplicate the mask or
permit the queued continuation to unmask. Exclusive ISA handlers and
spurious IRQ7/15 EOI rules retain their existing handling.

This ordering follows [PCI 3.0 §2.2.6](https://e2e.ti.com/cfs-file.ashx/__key/communityserver-discussions-components-files/639/1016.PCI.Local.Bus.Specification.Revision.3.0.pdf)
(level-sensitive source stays asserted until serviced) and
[ICH3-M §§5.7.4.6–9](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)
(ELCR level detection, source removal before EOI, cascaded PIC EOI).

Host workers wake on completion notifications independently of the unchanged
absolute 250 ms root/hub scan deadline. Mandatory control/bulk and USB
reset/recovery sleeps retain their absolute elapsed-time bounds across
notifications. HID service wakes on completion and retains its 1 ms timer
fallback for lost interrupts and two-frame retirement. Completion-to-first
service latency is distinct from queue delivery, which still requires safe
retirement; no DMA reuse or retirement rule is relaxed.

F1-71 amends the polling-only execution rules below: a qualified firmware
PIRQ/ELCR route uses the registry's shared INTx chain. See
[F1-71 research and validation](../validation/f1-71-shared-pci-interrupts.md).
UHCI enables the native PCI gate with SMI/traps disabled and uses a pinned
inactive IOC schedule tail for 1 ms notifications; OHCI uses SOF/WDH/UE.
Handlers acknowledge only their own controller status and wake the existing
workers. Fatal status survives W1C until thread cleanup. Root/hub scans keep
their bounded 250 ms cadence; frame notifications independently wake host and
HID service, including during bulk waits and two-frame retirement. Unqualified
routes, exclusive-line conflicts and chain-attachment refusal retain polling.
Controller faults detach a stopped/masked source before quarantining its
claims, preserving healthy shared peers when detachment succeeds. HSE/HCPE
remain serviceable after the first fatal snapshot: USBINTR alone does not mask
them. Detachment requires raw source/status, native gate and PCI BME readback
proof. Failed isolation retains the attached IRQ claim in quarantine, masking
the shared line and preventing queued rearm. Mandatory reset, recovery and
debounce sleeps use absolute elapsed-time deadlines preserved across IRQ
wakeups. Separate worker/HID notification waits and optional TUR retry backoff
may finish early. The TUR backoff retains its finite polling bound even when
the clock freezes; it cannot be used for reset/recovery/debounce.

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
512-byte configuration and report descriptors, 64-byte packets
(eight at low speed), seven-bit addresses. Unsupported devices stay visible.
Addresses and record slots are retained for the boot, including disconnects
and quarantine; exhaustion is explicit, never aliases a live address.

## DMA and execution

Each host owns three contiguous physical pages below the direct-map limit,
claimed along with its port/MMIO aperture and valid PCI IRQ (shareable,
masked at the controller). DMA stays disabled until all schedule pointers,
sentinels and bounce buffers are initialized. Only offsets within these
pages become bus addresses; callers' pointers never do. PCI command writes
are 16-bit and never echo W1C status. Controllers poll with USB interrupt
enables off; no unqualified IRQ handler or routing is installed.

One P_DEVICE service thread owns enumeration, including address zero; a
second P_DEVICE thread progresses HID without port or filesystem work.
The host mutex protects short controller/DMA/state operations. A `busy`
reservation serializes the general control/BOT slot and complete storage
commands, while every sleep drops the mutex. The owner also progresses
HID between 1 ms sleep slices. Interrupt pipes have private DMA slots and
do not wait for the scan's 250 ms deadline, general-slot ownership or
storage publication. USB does not block the common work queue with
enumeration. Each control/BOT transfer uses one live packet TD, independently
of the bounded interrupt TDs; control data stages split at endpoint-zero
max packet size. NAK is normal for
interrupt polling. F1-57 permits one endpoint halt-clear/DATA0 retry for
HID STALL or babble. A timeout retired while the connected root port has
PE clear gets one re-enable retry with its existing toggle; enabled-port
timeouts and other transfer/protocol errors quarantine the device. Host
system errors or failed retirement quarantine the controller and its leases.
No descriptor/buffer reuse until the schedule is unlinked/skipped and two
hardware frames have elapsed. A stalled frame clock fails closed. DMA and
claims remain pinned on uncertain retirement; uncertain DMA is never recovered.

F1-52 retains the existing 8 KiB allocation: 64 private interrupt slots at
offset 4608, 48 bytes each (16-byte QH/ED, TD and boot report buffer). HID
requests up to eight bytes bounded by endpoint MPS; boot mouse suffixes are
allowed by HID 1.11 Appendix B and ignored beyond the recognized prefix.
OHCI ED MPS remains
the advertised endpoint maximum. Each pipe has linked/retiring/idle states,
with two unlinked frames before DMA reuse. F1-57 leaves an idle NAK TD
linked until data or a retired error; the watchdog measures HC frame-clock
progress. NAK leaves the toggle unchanged; an ACK is decoded once after retirement.
The host mutex is free throughout waits, even while a general TD remains
live. OHCI PLE remains enabled during control/bulk service; DI=0 and WDH
acknowledgement flush done chains before retired TDs are reused. Slots are
never reassigned after detach. LED requests and quarantine requiring hub
control execute only through the general owner; unplug/change is checked
before permanently blocking a port for a deferred HID failure.

Verbose/test boots emit `[usb] hid latency addr=... ms=...` for the first
20 valid reports per pipe and a maximum for each nonempty one-second window.
`bound=upper` identifies the last pending TD observation to queue-delivery
bound; `observed_ms` measures observed completion to delivery. The exact
ACK frame is unavailable from these TDs. The fixture independently measures
availability, ACK and production queue timestamps under concurrent bulk and
scan/reset work. See [F1-52 validation](../validation/f1-52-usb-hid-poll-latency.md)
for sources, measurements and the distinction between frame and dispatch
clocks. QEMU and hardware qualification remain required.

F1-56 makes UHCI's static horizontal order explicit: all frame entries point
to the independent interrupt QHs, the final interrupt QH points to control,
control points to bulk, and bulk terminates. The two general QHs occupy
offsets 4096 and 4112; the reserved general TD remains at 4128 and only one
general QH can own it at a time. Interrupt and general TDs remain single
element breadth-first queues (`T=1,Vf=0`), with Active set and IOC clear
because USBINTR is masked and completion is polled. Retirement, allocation
size, the 1 ms HID service and port quarantine rules stay as specified above.
Verbose/test boots dump the first four frame entries and the shared reachable
QH chain once per UHCI controller after the first root/hub enumeration scan.
See [F1-56 validation](../validation/f1-56-real-uhci-schedule.md) for the strict
host traversal model and the unresolved T23 reproduction.

## Enumeration, hubs and input

Debounce connections 100 ms; root reset is held at least 50 ms with 10 ms
recovery. F1-57 samples root stability every 25 ms, bounded to 2 s, and
retries GET_DESCRIPTOR(8) in two series of three operations, 100 ms between
operations, with a new physical reset before series two. Only then retain
an initial descriptor failure in quarantine. OHCI reset completion is PRSC
(bit 20), independently of CSC (bit 16). Set_Address is followed by 2 ms recovery. Validate every descriptor
boundary and endpoint before Set_Configuration. Ignore nonzero alternate
settings. Hub power and reset use class requests with bounded status polling;
reject nested hubs, excess ports and overcurrent. Disconnects recursively
release input state for that device and children, keeping inventory records.
While CCS remains set, CSC/PEDC acknowledgement preserves a present child;
PE loss is recovered without reset or address/toggle loss. Reset and disable
callbacks reject a connected live child. Endpoint recovery is requested by
the input worker and executed by the serialized general owner. A successful
report renews its one-retry budget. See [F1-57 validation](../validation/f1-57-usb-enumeration-robustness.md).

HID boot interfaces receive Set_Protocol(0) and Set_Idle(0). Keyboard array
diffing ignores duplicates, holds the last valid state on rollover, tracks
modifiers and releases all held keys on detach/quarantine. Independent USB
and PS/2 key/button ownership is merged, so one device's release cannot
release another device's held key. LED output is a one-byte class Set_Report.
Mouse interfaces parse bounded report descriptors, including report IDs, global
push/pop, padding and signed relative fields up to 16 bits. Recognized layouts
with wheel or extra buttons use Set_Protocol(1); unsupported layouts and a
stalled protocol switch keep boot protocol. Input reports remain bounded to
eight bytes, including their ID. Extra buttons contribute to bit offsets;
the public input ABI still exposes three buttons. See the F1-66 amendment.

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
F1-47 makes each packet consume the remaining stage budget with sleepable polling.
F1-60 retains that whole-stage bound for published I/O. F1-62 gives binding
transfers a three-second wait; full ACK progress with packets remaining renews
that wait, capped by the twenty-second aggregate bind deadline and 100 ms finishing
reserve. NAK/failed packets never renew it. This gives the last packet an
opportunity after earlier progress without extending the complete bind.
The [F1-60 validation record](../validation/f1-60-usb-bulk-read.md) covers packet
renewal; [F1-62](../validation/f1-62-usb-bind-budget-per-stage.md) records the
T23 aggregate exhaustion, stage completion timings and the revised budgets.
Mount transfers get their own three-second waits within the same aggregate.

Commands: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10),
READ(10), WRITE(10); SYNCHRONIZE CACHE(10) qualifies and implements
flush; a four-byte MODE SENSE(6) header observes write protection. Initial
power-on unit attention may be consumed with at most three readiness probes.
Before publication, valid NOT READY/04/01 (becoming ready) responses permit
sleepable readiness polling until the aggregate bind deadline, with a finite
poll count for a frozen clock. Other readiness failures are not retried.
Capacity sentinel FFFFFFFFh and
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

### F1-47: real full-speed BOT data phase (2026-10-13)

Research before implementation: [USB 1.1 specification](https://doksi.net/en/get.php?lid=1971),
sections 5.8.3/5.8.4, 8.5.1/8.6 and 9.4.5, distinguishes temporary NAK flow control
from STALL and terminates bulk IN on short packets, including zero-length
packets. The allowed full-speed bulk packet sizes are 8/16/32/64 bytes.
[Intel UHCI 1.1 design guide](https://netwinder.oregonstate.edu/pub/misc/docs/29765002-usb-uhci%20design%20guide.pdf),
sections 3.2/3.3/4, defines TD MaxLen/ActLen, error-counter retries, NAK,
short-packet detection and QH writeback. [BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
sections 3.1/3.2, 5.1–5.3 and 6.3/6.7, requires byte-aligned 31-byte CBWs
and 13-byte CSWs starting on packet boundaries, and reset recovery in
class-reset, clear-IN, clear-OUT order. Class reset preserves toggles;
successful CLEAR_FEATURE(ENDPOINT_HALT) resets that endpoint to DATA0.

Compared with GPL-2.0 Linux [usb-storage transport.c, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/transport.c)
(`usb_stor_Bulk_transport`, `usb_stor_Bulk_max_lun`, `usb_stor_reset_common`)
and [UHCI queue implementation](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/uhci-q.c)
(`uhci_submit_common`, `uhci_map_status`). No upstream code is imported.
Linux accepts short IN, clears data-stage stalls before fetching CSW, and
allows one CSW halt recovery. Its last IN TD is capped at the remaining
length and need not set SPD when a short packet is allowed. Our single
terminated TD per packet already has that property: no chain can consume
the following CSW after short data. OHCI still uses descriptor MPS=64 even
for 36/13-byte transfers. Both controllers retire for two hardware frames
before descriptor/buffer reuse, then decode any completion racing unlink.

Repository findings: the T23 log lacks the packet return value and TD
status, so it cannot distinguish NAK timeout, exhausted CRC retries or
babble. The reset line is emitted by recovery **after** the data fault;
there is no class reset before tag 1. INQUIRY requests 36 bytes, so a
36-byte reply is short relative to MPS=64 but satisfies this CDB exactly.
The actual Lexar root cause remains unqualified until another hardware
capture. Two reproducible defects are corrected: the per-packet 500 ms
limit prematurely terminated legal bulk NAK flow control despite the
10-second stage budget, and UHCI's STALL-first decoding treated babble
(which also sets STALL) as a recoverable endpoint halt.

Decision: bulk packets consume the remaining existing 10-second stage
deadline; control and interrupt budgets stay 500/2 ms. A timeout still
unlinks and retires the TD, and an ACK during retirement is counted once.
No fresh TD or CBW is submitted after an uncertain result. INQUIRY keeps
its conservative 36-byte allocation, accepts a complete five-byte header
or more only with passed CSW and exactly consistent residue, and reads
only fields actually received. Fixed-size capacity, block I/O and writes
still require all requested bytes and zero residue. Short data and ZLP
advance the IN toggle once; NAK and STALL do not. Babble/buffer/CRC/bitstuff
errors fail closed and never take the halt-clear/CSW retry path.

Fault diagnostics preserve the CBW/data/CSW stage, stage and data lengths,
result, last TD/token/controller status, endpoint/PID/toggle, elapsed
hardware frames (accumulated across frame-number wrap), elapsed time and
retirement outcome before recovery overwrites the DMA buffer. Complete
CSWs include status, residue, signature and tag even when invalid.
Recovered data/CSW stalls also emit this snapshot so a subsequent capture
does not lose the original event to control transfers or REQUEST SENSE.

Linux tolerates more broken Get Max LUN responses, supports SINGLE_LUN
quirks, and waits six seconds after class reset. Its
[v6.12 unusual-device table](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/unusual_devs.h)
has a different Lexar CF reader (05dc:b002) with FIX_INQUIRY, but no
05dc:a81d entry. This stick answered Get Max LUN successfully; there is no
evidence for a matching no-Get-Max-LUN/reset-delay quirk. We retain the
specified STALL allowance and bounded recovery, without speculative
vendor exceptions or command resends. Hardware and QEMU qualification
belong to the lead; host fixtures demonstrate these mechanisms only.

### F1-53: quarantine scans and attach tolerance (2026-10-14)

Research checked before implementation:
[Intel UHCI 1.1](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf),
PORTSC bits 1/3 and TD status bits 18–23, requires explicit W1C change
acknowledgement and distinguishes active NAK from exhausted CRC, babble and
endpoint STALL. The existing packet engine already implements that distinction;
it remains unchanged. Root acknowledgement preserves port controls and writes
only observed CSC/PEC bits; OHCI clears only observed high-word change bits.
Hub changes use the existing class CLEAR_FEATURE requests.

[BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf), sections
5.3/6.3, specifies class-reset, clear-IN, clear-OUT recovery and validated
CSW framing. [Linux v6.12 transport.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/storage/transport.c)
provides the same recovery ordering and short-data handling.
[Linux v6.12 sd_read_write_protect_flag](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/scsi/sd.c)
treats unavailable MODE SENSE protection information as write enabled.
No upstream code is imported.

Repository comparison: root scans logged CSC before testing root_blocked,
then skipped acknowledgement forever. Hub blocked ports skipped status entirely.
Recovery diagnostics occupied four records; klog has a 256-byte buffer.
INQUIRY already requests 36 bytes and accepts a consistent short response;
READ CAPACITY(10) requires all eight bytes. Preserve both rules: capacity
responses shorter than eight bytes cannot safely describe the medium.

Decision: every root/hub per-port record shares a one-second budget. A deferred
event is coalesced and emitted on a later scan. Quarantined ports acknowledge
changes, emit one connect record with reason=quarantined, and stay silent until
disconnect. Storage-block quarantine also blocks its port. Quarantine remains
permanent for this boot; disconnect logging does not authorize enumeration.
(Superseded by F1-96b: an observed disconnect releases a device-quarantined
port and the next connection is enumerated as a new device.)

Only the first unpublished attach INQUIRY may be retried once after successful
BOT recovery for timeout, endpoint STALL or SCSI command failure. Failed
retirement, host errors, babble/CRC/buffer errors and malformed wrappers remain
terminal. No published command or write gains a retry. MODE SENSE data STALL
with a valid CSW skips the optional probe; persistent endpoint STALL requires
successful reset recovery before skipping. Other transport faults stay terminal.
Write admission still requires successful TEST UNIT READY, complete capacity
and qualified cache flush; reported write protection and dirty FAT remain read-only.

The first fault captures stage, tag, lengths, raw TD, toggle, CSW availability,
status/residue and raw root-port status in one bounded record before recovery.
Quarantine names the stage separately. Host models exercise the directive's
slow-first-INQUIRY/64-byte-packet/unsupported-MODE-SENSE/short-capacity scenarios;
they do not establish the actual controller inside Lexar 05dc:a81d or its
hardware root cause. The lead supplies T23 and QEMU qualification.

### Mounts, hot-plug and removal

Each admitted disk is a `blkdev` beside ATA. Scan the existing MBR/EBR parser
and make `blkpart` views; validated whole-disk FAT is also admitted. FAT12,
FAT16 and FAT32 use the existing read-first validation/cache/native mount path.
USB volumes have `VFS_DRIVE_REMOVABLE` and receive E: onward, skipping existing
letters; exhausted letters/record slots fail explicitly and are never recycled.
USB marks storage published only after successful namespace admission. Any
failed mount quarantines at stage `mount` without a second attempt; retained
`volume ... error=` records describe candidate slots, not visible drives.
Fault records preserve the first eight packets' indices, toggles and retired
TD snapshots, along with the current packet index/offset and deadline reason.
An expired deadline before submission records `valid=0`, rather than the
previous packet's successful TD.

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

`/dev/media` exposes copied, versioned `MEDIA_GET_STATE` (version 1: 120 bytes,
version/size, mounted-removable mask, reserved=0, 26 generation values;
version 2: 224 bytes, appending 26 uint32 type values: 0 absent, 1 USB,
2 optical; see [F1-48 optical binding](optical-drive-binding.md)) and
`MEDIA_SAFE_REMOVE` (16 bytes: version, size, drive index, media generation)
through existing devctl call 69. Existing syscall numbers and version 1 layouts
stay unchanged; version 1 requests remain supported. Optical letters from D:
are included and removal dispatches to optical eject with open-handle preflight.
The removal generation prevents a stale menu from removing another
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


### F1-66: bounded bulk chains and mouse report layouts (2026-10-15)

Research, evidence limits and qualification requirements are recorded in
[F1-66 validation](../validation/f1-66-usb-stick-budget-and-mouse-wheel.md).
This supersedes the earlier single-packet bulk schedule and bind-log durability
requirements. The aggregate 20-second budget and 100 ms retirement/publication
reserve remain. Stage records use asynchronous logging; binding requests no
logger drain or sync. Successful namespace publication ends binding before
volume/stage diagnostic output. Later overrun records preserve that volume;
transport, media-change and disconnect faults retain their quarantine rules.

UHCI bulk phases larger than 512 bytes use up to 32 depth-first TDs per
batch, with separate 64-byte bounce slots, within the 128-sector block request
limit. IN TDs enable SPD. NAK retains the current TD; ACK alone advances its
toggle. All TDs are unlinked and retired for two HC frames before examination
and reuse, including short transfers and errors. Only the completed prefix is
copied. The 12 KiB pinned window places bulk TDs at 7680 and data at 8192;
interrupt offsets stay fixed. Other controller paths retain their packet
engine. No packet or issued command is resubmitted after uncertainty.

Unpublished FAT preparation allocates three temporary 64 KiB windows (page
rounded allocation 196,608 bytes plus one page for descriptors). Two retain
FAT copies and one retains individual other metadata sectors. FAT windows read
at most 128 sectors, clipped to cluster entries and the validated FAT extent.
Padding sectors after the last cluster entry are excluded. Ownership, loops,
cross-links, copy equality, lost clusters and durability checks still run
before write admission. All temporary pages are freed on success or failure;
no shared cache/workspace mutex is held across the reads.

Mouse report parsing bounds collections at eight, global push/pop at four,
report IDs at eight records and total report size at eight bytes. Other IDs
are ignored without treating their payload as a boot mouse. Descriptor dumps
are limited to sixteen 32-byte chunks at attachment. The public wheel route
and three-button ABI remain unchanged. The QEMU wheel case requires both signed
kernel wheel records and corresponding desktop consumption records, in addition
to the existing desktop shell checks.

## F0-40 port quarantine isolation

A root-port reset timeout or exhausted device/address capacity blocks and
disables that port using existing HC operations and records controller,
one-based port, stage, result and `scope=port`. Other root-port devices keep
their schedules, generations and leases. The blocked port is not repeatedly
reset/re-enumerated. When a hub cannot disable a faulted child, quarantine
the hub and its root port rather than the entire HC; the other root ports
remain independent. A root hub still constitutes one physical domain.

Controller-wide quarantine remains necessary after loss of register/DMA
authority, fatal HC state, stopped frame clock or uncertain controller-level
DMA/interrupt isolation. IRQ lease loss retains the existing polling
fallback; neither port quarantine nor a detached shared IRQ member may
disable a peer's claims. No register sequence or transfer recovery policy
changes. Sources and both UHCI/OHCI failure-injection cases are listed in
the [F0-40 audit](../validation/f0-40-reliability-audit.md).
