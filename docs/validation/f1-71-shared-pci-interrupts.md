# F1-71 shared PCI INTx

Research before implementation, 2026-10-16. Host evidence is recorded in
`build/codex/f1-71-report.md`; QEMU and T23 qualification belong to the lead.

## Sources and decisions

- [PCI Local Bus Specification 3.0, §2.2.6](https://e2e.ti.com/cfs-file.ashx/__key/communityserver-discussions-components-files/639/1016.PCI.Local.Bus.Specification.Revision.3.0.pdf):
  INTx is level sensitive and drivers must support chaining. Keep ISA
  reservations exclusive; only validated level routes may join the chain.
- [Linux v6.12 PCI driver guide, §1.4.6](https://www.kernel.org/doc/html/v6.12/PCI/pci.html)
  and [generic IRQ handling](https://www.kernel.org/doc/html/v6.12/core-api/genericirq.html):
  each shared handler checks its own source. Retain the existing bounded
  mask/service/defer/unmask chain and dispatcher-owned single EOI.
- [Intel ICH3-M 290716-001](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf),
  §§6.1, 9.1.16–17, 11.1.14/16, 11.2, 13.2: validate the firmware route and ELCR,
  never program them. D29:F0/F1/F2 use PIRQA/D/C, not a generic slot
  swizzle; audio uses PIRQB and integrated LAN PIRQE on the D30 bridge's
  firmware-assigned secondary bus. Restore USB_LEGKEY's native PCI gate
  (bit 13) after handler attachment, with all SMI/trap enables off. USBSTS and AC'97
  channel status are device-local W1C causes. Codec access failure does
  not itself establish DMA failure: a pre-DMA codec failure may release
  resources after disabling and checking every engine and PCI BME.
- [Intel UHCI 1.1, §§2.1.2–3, 3.2, 4](https://ftp.yz.yamagata-u.ac.jp/pub/NetBSD/misc/blymn/uhci11d.pdf):
  IOC also works on an inactive TD. A pinned inactive IOC tail provides a
  frame interrupt without a USB transaction, including during idle/NAK and
  retirement. Enable completion/error causes only after handler attachment;
  acknowledge observed USBSTS causes, preserve fatal status for the worker.
- [OpenHCI 1.0a, §§5–6](https://www.cs.usfca.edu/~cruse/cs698s10/hcir1_0a.pdf):
  SOF/WDH/UE/RHSC have device-local status and enable bits. Frame interrupts
  wake USB service; HCCA done-head retirement remains worker-owned.
- [QEMU v9.2 UHCI source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-uhci.c)
  confirms inactive-TD IOC handling before PID decoding; the
  [Linux v6.12 OHCI register definitions](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/ohci.h)
  independently confirm the frame/completion/error enable masks.
- [Intel 8255x developer manual, §§6.3.2, 6.5.2, 7.4](https://www.intel.com/content/dam/doc/manual/8255x-10-100-mbps-ethernet-controller-software-dev-manual.pdf):
  SCB STAT/ACK is W1C and interrupts remain asserted until pending causes
  are acknowledged. Preserve the existing source-mask/worker-rearm handler.

The read-only T23 logs for image a395ffa8 show a quarantined AC'97 IRQ11
claim after `codec_reset/cas_timeout`, three USB claim conflicts, and e100
polling. Successful audio startup on another boot still leaves USB polling.
The registry and shared chain already exist; USB lacks an attached handler
and NIC route admission is QEMU-only. Root scans are 250 ms, while the
existing HID task already polls at 1 ms: scan frequency alone does not prove
mouse latency. Preserve bounded scan work, but service IRQ hosts on frame
notifications independently of that scan deadline.

The local driver pack's `UIU/e100b325.inf` identifies Intel LAN functions including
IBM subsystems and installs the NDIS PCI driver. `UIU/Alcwdm.inf` identifies
8086:2485 and installs the WDM AC'97 driver. Neither contains an IRQConfig,
LogConfig or explicit sharing declaration; these files establish device
matching, not interrupt electrical semantics. No vendor driver code is imported.

## Host outcome

The full kernel host script passed. The final focused USB rerun also passed
with an exact assertion that the original fatal USBSTS bits survive IRQ W1C.
The three-device registry fixture covers interleaved sources, a late assertion
after its handler, single modeled EOI, and stopped-device removal/release
without disturbing peers. USB worker replay retains 1 ms notification/HID
cadence during a 500 ms bind on another controller. The audio fixture produces
a reset-write CAS timeout and releases 12 leases/nine pages only after idle
proof; unsafe input-channel and prior-BME cases retain quarantine. All eight
NIC models pass, including firmware-table admission and polling fallback.

The kernel ELF linked and passed its FPU/SIMD audit (5,251,552 bytes,
SHA-256 `b9b32a17e2b042586a67ec0abd8f167a38af4841b43e8a3955ef5f417e6741a7`).
This is a kernel artifact, not image or QEMU evidence. All host tests ran
sequentially with `nice -n 19`.

Broader Python discovery ran 705 tests with three failures, two errors and
41 skips. Building the kernel supplied the missing stack-test payload; all
four stack tests then passed. Remaining failures concern unchanged desktop
geometry and loader-title fixtures, plus an unchanged storage-task replay
missing its `blackbox_flush` declaration. Those source/fixture inputs match
the branch base. The broad Python run is not green; exact cases and logs
are recorded in the implementer report. No QEMU, OS/DOS image build, diary,
commit or push was performed.

## Review fixes: source check and decisions

Research repeated before the review corrections, 2026-10-16:

- [QEMU v9.2 `uhci_update_irq()`](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-uhci.c)
  tests HSE/HCPE independently of USBINTR. Keep acknowledging real status
  after retaining the first fatal cause. Before removing the callback, check
  the interrupt mask, stopped command, HCHalted, raw W1C status and PCI BME readbacks;
  on UHCI also clear and verify USB_LEGKEY's native gate and SMI enables.
  An unproved isolation retains the attached IRQ lease in quarantine, which
  masks the shared chain through `registry_quarantine()`.
- [ICH3-M §§9.1.16/18 and 11.1.16](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)
  define PIRQ bit 7 as disable, bits 3:0 as IRQ, bits 6:4 as reserved, and
  USB_LEGKEY bit 13 as USBPIRQEN. Decode those route fields on ICH;
  [QEMU PIIX's route implementation](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/isa/piix.c)
  instead rejects the entire byte when it is >=16, so retain strict PIIX
  equality. Register access remains read-only for route qualification.
- USB reset/recovery/debounce delays must consume elapsed PIT time, rather
  than count notification returns. Retain one absolute deadline across USB
  frame wakeups in the native sleep and in the portable delay loop. Only
  optional waits (outer worker/HID notifications and TUR retry backoff) may
  complete early. Keep TUR backoff finite for its existing frozen-clock
  readiness replay; it has no protocol minimum spacing. Replay
  PIT ticks at integral milliseconds and USB frames at a 0.5 ms offset.
- Require `mode=irq reason=ready_irq` in all three qualified `f1-usb` cases.
  Retain explicit host admission-refusal cases requiring the emitted
  `mode=poll reason=ready_polling` record for unqualified routes, IRQ lease
  conflicts and chain attachment refusal. No runner field is necessary.

## Review corrections: host outcome

All requested focused suites passed again: USB (including the independently
phased 0.5 ms IRQ/PIT replay, mandatory delay checks and eight fatal-isolation
cases), storage (including frozen-clock readiness), audio, networking and PCI
routing. The USB runner replay passed 18 tests; JSON validation passed.
The full kernel host script passed with exit 0 under
`ASAN_OPTIONS=detect_leaks=0 nice -n 19` (240.38 seconds).

The kernel-only build linked without undefined symbols and passed its
FPU/SIMD audit: 5,252,248 bytes, SHA-256
`cfe5bede3087595991212d348459eee0a7a25dee98b613bcf1fa7d25fe4f5a0a`.
Verbatim results and logs are in the “Review fixes” section of
`build/codex/f1-71-report.md`. No QEMU, image build, diary or Git mutation
was performed; canonical-image and hardware qualification remain with the lead.

## F1-72: completion interrupts and PIC mask writes

Research repeated before implementation, 2026-10-16. The preceding F1-71
results remain historical evidence; this amendment supersedes its heartbeat
and per-pass mask/rearm choices.

- [QEMU v9.2 UHCI](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-uhci.c),
  `uhci_handle_td()` and `uhci_complete_td()`, and
  [ICH3-M §5.16.6.1](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)
  confirm that an inactive IOC TD raises an interrupt whenever encountered.
  Clear IOC on the pinned tail; retain it on actual transfer TDs.
- [Linux v6.12 OHCI register definitions](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/ohci.h)
  and [QEMU v9.2 OHCI](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-ohci.c)
  distinguish SF (bit 2) from WDH/UE/RHSC (bits 1/4/6). Enable only the
  latter and MIE; ignore latched disabled SF during peer dispatch.
- [PCI 3.0 §2.2.6](https://e2e.ti.com/cfs-file.ashx/__key/communityserver-discussions-components-files/639/1016.PCI.Local.Bus.Specification.Revision.3.0.pdf)
  requires level-sensitive INTx to remain asserted until its request is
  cleared. [ICH3-M §§5.7.4.6–9](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)
  describe ELCR and removing a level request before EOI. The existing
  trap dispatcher owns EOI with IF clear: successful bounded chains need
  no PIC mask transaction. Deferred/quarantined chains mask before EOI;
  only a completed healthy deferral unmasks once, in the worker.

Repository comparison: `uhci.c` installed the inactive IOC tail,
`usb_core.c` enabled OHCI SF and acknowledged it as an owned cause, and
`registry.c` masked/queued/unmasked every pass. The read-only supplied
`zz-usb/7fd10c05/serial.log` (image 79df4293) confirms IRQ admission and
cumulative PIC writes 4133, 6271, 8454 and 10550. Their two-second deltas
are 2138, 2183 and 2096 writes. At cycle 28 it records handles=19/20 and
`desktop_contract` failure. A heartbeat race is a hypothesis, not a proved
cause of that separate handle observation. Keep scan/deadline/retirement
rules and qualified IRQ/refusal polling gates; measure completion wake and
idle fallback separately in host fixtures. QEMU write limits and hardware
qualification remain with the lead.

### F1-72 host outcome

The focused USB, storage and registry host suites pass. The full kernel
host script also passes (exit 0), including the dependent production shadow
registry probe, audio, NICs, storage power cuts and all new completion cases.
Commands run sequentially with `nice -n 19` and ASan/UBSan; full logs and
verbatim results are in `build/codex/f1-72-report.md`.

With real production HID/TD/IRQ code and independently phased PIT/HC events,
both UHCI and OHCI measure 0 µs completion-to-first-service with delivered
IRQ and 500 µs with a lost IRQ or unqualified polling. Each idle HID replay
records 1000 wakes/s and zero interrupts; the separate worker orchestration
replay records 4 host scans plus 1000 HID turns = 1004 idle wakes/s and zero
interrupts. Immediate scheduler dispatch is modeled; these are not measured
hardware scheduling bounds. Input delivery retains two-frame retirement.

Claiming chain passes perform zero mask writes and queue no continuation,
including with a full work ring. A healthy deferred pass masks once and
unmasks once after modeled EOI. Quarantine during deferral and continuation
overflow mask only once and never rearm; the 1000-unclaimed-pass threshold,
shared peer removal, late assertions and spurious counters remain covered.
The shared dispatcher header and the embedded `registry_probe.c` fixture
are updated to the same EOI/mask contract; no live IRQ fixture is unmasked.

`tests/suites/f1-usb.json` and `scripts/test/run.py` remain unchanged.
Qualified cases still require `mode=irq reason=ready_irq`; host admission
refusals still require `mode=poll reason=ready_polling`. No QEMU/image build
or hardware result is claimed. The lead must measure the directive's idle
and moving-mouse PIC-write limits on its canonical image.

The kernel-only build also passes: link has no undefined symbols and the
FPU/SIMD audit passes. `build/f0/VMM.ELF` is 5,439,432 bytes, SHA-256
`e92604a63a267a260473dab8b9a19ab3f013cb001f27b661c332f4a764b9030a`.
This artifact is host build evidence, not an OS image or a QEMU result.

### F1-72 review: fatal containment, RHSC and done-head ownership

Research checked again on 2026-10-16 before the review fixes:

- [QEMU v9.2 `uhci_update_irq()`](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-uhci.c)
  asserts HSE/HCPE independently of USBINTR. The current handler only
  disabled USBINTR and W1C'd status, leaving isolation to a starvable worker.
  Perform one bounded register/configuration isolation attempt in IRQ context:
  clear USB_LEGKEY enables (including USBPIRQEN on
  [ICH3-M §11.1.16](https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf)),
  USBINTR, Run/Stop, status and PCI BME, then read all back, including raw
  USBSTS/HCHalted. An uncertain result quarantines the attached IRQ lease
  before EOI. Device detach and any sleeping cleanup remain worker work.
- [Linux v6.12 OHCI IRQ handling](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/host/ohci-hcd.c)
  disables RHSC because implementations can hold it until port changes clear.
  The current handler leaves it enabled after status W1C. Disable/read back
  RHSC locally, retain a separate root-work flag until the existing 250 ms
  scan, clear observed port/hub changes, then acknowledge/re-enable RHSC.
  Peer dispatch must filter against the current interrupt-enable set.
- [OpenHCI 1.0a §§6.4.5, 7.1.4 and 7.4.3](https://dl.xkwy2018.com/downloads/documents/hcir1_0a.pdf)
  hold HccaDoneHead stable until WDH clears; software must save it first.
  The current IRQ, HID worker and packet setup acknowledge independently.
  Use one IRQ-serialized capture/clear/barrier/ack path and a bounded queue
  of saved heads; the worker consumes every saved chain before TD reuse.
  IRQ capture never takes the sleeping transfer mutex. Queue overflow fails
  closed without acknowledging an uncaptured head. Hub status acknowledgement
  writes only OCIC (bit 17); bit 16 is SetGlobalPower on write, not W1C.

Regression fixtures must model fatal reassertion after every W1C without
worker service, level RHSC until port/hub changes clear, and two HCCA head
publications before worker service, including polling/IRQ interleaving.
No heartbeat, common-pass PIC mask writes or timing-policy changes are made.

Review-fix host outcome: USB, storage, the registry fixtures and the complete
ASan/UBSan kernel host script all pass (exit 0). The persistent-fatal replay
masks before EOI in one pass with zero worker turns. The level-RHSC replay
ignores 16 peer passes while disabled and scans/clears/rearms at the 250 ms
deadline; sticky changes retain polling. Both done-head publications survive
until worker consumption, both HID reports arrive, the native polling/IRQ
interleave consumes each list once, and queue overflow preserves HCCA/WDH
while masking the lease. Existing idle/completion, absolute-delay, shared
IRQ mask-write, audio and NIC checks still pass.

The kernel-only build passes its FPU/SIMD audit and has no undefined symbols:
5,445,228 bytes, SHA-256
`75e07eeb7cdd1794cfc18d35b89b214cac14d8dd46f6e6c60145fcc3645438fd`.
Commands, exact file/line references and verbatim results are appended under
“Review fixes” in `build/codex/f1-72-report.md`. No QEMU, OS image, hardware,
diary or Git mutation evidence is claimed.
