# F1-66 USB bind and mouse reports

## Research and decision (2026-10-15)

Sources checked before implementation:

- [UHCI 1.1](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf),
  sections 3.2–3.4: depth-first TD links advance after ACK; NAK retains the
  current element; short packet detection stops an IN queue. Unlink and
  two-frame retirement remain necessary before reusing DMA.
- [BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 5–6: a data phase can contain many packets; status validation and
  recovery remain separate. A shorter data phase must not consume its CSW.
- [FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
  FAT extent follows cluster count; allocated padding is not cluster data.
  Free-space hints do not establish ownership or permit skipping validation.
- [HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf), sections
  6.2.2, 7.2.6 and appendix B: report IDs, bit fields, global push/pop and
  per-report input offsets determine report layout; boot mouse data excludes
  wheel. [Receiver reference descriptor](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-logitech-dj.c)
  provides a numbered mouse report with 16 button bits, packed 12-bit X/Y,
  signed wheel and a separate pan field.
- [QMP input enums](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#enum-InputButton):
  vertical wheel input uses wheel-up/down button pulses. These pulses do not
  produce public button edges, so runner pacing keeps the existing motion and
  ordinary-button counters and requires separate signed wheel evidence.

Repository findings: UHCI submits and retires each 64-byte packet separately.
FAT preparation bypasses the shared cache and rereads a sector for each
cluster and directory entry, in addition to comparing every allocated FAT
sector. Stage logging tries a disk drain at both boundaries and synchronizes
terminal records. The mouse recognizer accepts only a fixed four-byte layout.

Decision: retain ownership and copy validation. During unpublished preparation,
use temporary bounded metadata windows, read FAT sectors in runs of up to
128 sectors, and exclude FAT padding beyond the last cluster entry. Repeated
cluster/entry reads use those windows. UHCI large bulk phases use bounded TD
chains with short-IN detection, ACK-only toggles and one retirement per batch.
Keep the existing packet path for small control/BOT phases. Stage records go
to the normal asynchronous logger without a durability operation. Successful
namespace publication ends binding before diagnostic output; a subsequent
budget overrun is recorded without revoking the volume.

Mouse report parsing remains bounded: descriptor size, collections, global
stack, report count and report length are checked. Recognized relative mouse
fields select report protocol when wheel or extra buttons are present;
unsupported layouts retain boot protocol. The input ABI currently exposes
three buttons, so additional button bits are parsed for offsets but do not
become new public button codes. Wheel keeps its existing public event route.

The first serial boot has damaged fragments, including a mount timeout at
tag 983 and a 21,311 ms worker interval. The clean disk log is a later boot
without storage bind records. Neither supplied file contains descriptor bytes;
the receiver reference fixture is not a physical descriptor capture. A bounded
attach dump supplies those bytes on the next boot. Exact physical stage times
and a successful mount cannot be reconstructed from the supplied fragments.

## Qualification

Host results and commands are recorded in `build/codex/f1-66-report.md`.
The lead must qualify one canonical image on `f1-usb`, including
`usb-wheel-uhci`, and on the T23. Require mount elapsed time below 5 seconds
for the 4 GiB stick, continued input, readable E:, no post-publication deadline
quarantine, descriptor dump retention and wheel delivery/scrolling in desktop.
Host timing models establish software costs only. The 4 GiB regression uses
32 KiB clusters and an otherwise small directory tree. Smaller clusters and
larger directory trees require separate measurement; the parser retains its
full integrity scan rather than trusting free-space hints. The supplied T23
memory record has 523,260 KiB usable RAM, 2,274 free DMA pages and 126,816 free
NORMAL pages. The change adds one pinned page per host and a temporary
49-page FAT preparation allocation; memory limits are unchanged.
