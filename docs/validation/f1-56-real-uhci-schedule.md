# F1-56 UHCI schedule traversal

## Research and implementation decision

Checked on 2026-10-14 before changing the controller code:

- [Intel UHCI Design Guide 1.1](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf),
  sections 1.2.3, 3.2, 3.3 and 3.4: schedule order is interrupt, control,
  bulk. Frame/horizontal termination ends the frame; element termination
  skips an empty queue. Successful queued TDs advance the element pointer.
  Breadth-first execution then follows the horizontal QH link; depth-first
  follows the TD link. Errors/NAKs do not advance the queue. Element bit 2
  is ignored, not a depth selector. Inactive TDs do not transact; IOC can
  still set completion status. NAK leaves Active set.
- [Linux UHCI schedule description](https://github.com/torvalds/linux/blob/master/drivers/usb/host/uhci-hcd.h)
  and [queue implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/uhci-q.c):
  interrupt queues precede asynchronous queues; private QHs use horizontal
  links and only successful transfers advance their element pointers.
- [QEMU UHCI frame walker](https://raw.githubusercontent.com/qemu/qemu/master/hw/usb/hcd-uhci.c),
  `uhci_process_frame`: QEMU also starts at the frame list and honors TD
  depth/breadth selection. Its walker masks low address bits without
  rejecting reserved bits. Passing QEMU does not establish silicon
  behavior, but it does not justify assuming every link form is accepted.

Repository comparison: F1-52 links `frame -> shared control/BOT QH ->
64 HID QHs -> terminate`. Control is reachable; each HID TD has
`T=1,Vf=0`, a valid single-element breadth-first queue. Active is set and
IOC is clear, consistent with polling and USBINTR=0. The old host model
discarded frame flags, did not implement TD-link traversal, replaced every
completed element with terminate, and advanced even on errors.

The supplied hardware value `188807ff` includes Active and NAK. Since the
driver initially writes `188007ff`, this observation indicates a controller
visit and a NAK response, rather than proving that the HID TD was unreachable.
The original schedule must be replayed with the stricter model; a synthetic
broken link is evidence for the model, not proof of the T23 root cause.

Decision within F1-56: explicitly link `frame -> independent HID QHs ->
control QH -> bulk QH -> terminate`, using the unused aligned QH space
before the general TD. Control and BOT retain one reserved TD/buffer and
two-frame retirement. Keep F1-52 service cadence and F1-53 port/quarantine
rules. Add a bounded once-per-controller verbose/test dump after the first
enumeration scan, including frame entries, the actual reachable QH chain
and private TD fields. Hardware confirmation belongs to the lead.

## Host evidence

The full USB suite was run **before changing production USB sources**, with
the new strict walker and corrected modeled USBSTS W1C behavior. It passed:
enumeration, HID input, bulk/control coexistence, late ACK retirement,
native worker lifecycle and the F1-52 latency cases. Thus the unchanged
F1-52 schedule does not reproduce the T23 regression under the specified
traversal rules. This is a material limit of F1-56, not evidence of a
confirmed hardware fix.

The independent schedule fixtures exercise frame/QH/TD terminate and type
bits, breadth-first versus depth-first TD chains, nested QH context changes,
unlinked queues, ignored element bit 2, reserved bits, inactive TDs, IOC,
NAK, STALL and short-packet detection. Both the HID register model and the
BOT/storage model now start at the actual frame entry and share this walker.
Successful completion copies the TD link back; failed/inactive TDs leave the
element pointer unchanged. A previously fetched late ACK is the explicit
exception to following current links, and its DMA remains pinned.

The production regression fixture verifies all 1024 frame entries and the
64 interrupt/control/bulk chain, quiet versus verbose/test logging, four
frame entries plus the 66-QH chain exactly once, and bounded diagnostic reads
on invalid pointers/cycles. Fault injection terminates the interrupt tail
before control: HID TDs receive NAKs (`188807ff` for the full-speed fixture),
and an unreachable address-zero SETUP times out without an ACK or NAK
(`188007ff`). Restoring that edge permits control completion and keyboard/
mouse delivery. This distinguishes visited-but-NAKing TDs from unvisited TDs;
the missing edge is injected, not present in the original production code.

The updated USB and storage host suites passed sequentially with ASan/UBSan
under `nice -n 19`. The F1-52 latency fixture still delivers all 1,344 reports
within its existing controller-ACK-to-queue bound; F1-53 port-flood and
quarantine tests still pass. The complete kernel host suite also passed with
`ASAN_OPTIONS=detect_leaks=0`; its optional DOS guest runner retained the
missing-libx86emu/payload skip. The canonical i686 kernel compiler/linker
script passed with `--no-undefined`, stack-frame checks and the FPU/SIMD audit.
Commands, logs and kernel artifact identity are in
`build/codex/f1-56-report.md` in the implementation worktree.

No QEMU or hardware evidence is claimed. The lead must check the new dump,
keyboard, mouse and stick on the T23 and run `f1-usb`. The first scan's
one-time dump shows a live observation: TD/element fields may change while
the running controller completes packets. The traversal model does not
simulate PCI arbitration or certify frame bandwidth on physical controllers.
