# F1-50 — interrupt-IN reports with a root keyboard and hub mouse

## Sources and implementation decision

Research on 2026-10-14, before implementation:

- [QEMU input routing, v10.1.0](https://github.com/qemu/qemu/blob/v10.1.0/ui/input.c):
  `qmp_input_send_event` without a device selects an unbound handler matching
  the event kind. The USB keyboard registers and activates the key handler in
  [hid_init](https://github.com/qemu/qemu/blob/v10.1.0/hw/input/hid.c).
  [pc_superio_init](https://github.com/qemu/qemu/blob/v10.1.0/hw/i386/pc.c)
  returns before creating i8042 when it is disabled. In the directive's
  topology the keyboard destination is therefore `usb-kbd`.
- [USB HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf), sections
  7.2.4, 7.2.6 and appendix F: boot protocol uses eight-byte keyboard reports;
  idle zero inhibits unchanged reports, with NAK as normal flow control.
- [QEMU USB HID](https://github.com/qemu/qemu/blob/v10.1.0/hw/usb/dev-hid.c):
  interrupt IN returns NAK without pending input and an eight-byte report for
  a keyboard event. F12 is usage 0x45 in QEMU's HID translation table.
- [QEMU UHCI frame processing, v9.2.0](https://github.com/qemu/qemu/blob/v9.2.0/hw/usb/hcd-uhci.c):
  frame processing fetches the QH/TD, retains an active TD after NAK and
  clears Active on ACK. Frame progress and software timer progress are
  separate observations. This is upstream source research, not a QEMU run.
- [OpenHCI 1.0a](https://www.cs.usfca.edu/~cruse/cs698s10/hcir1_0a.pdf),
  sections 4.3.1.3.7.1 and 7.3.3: NAK leaves the general TD unchanged;
  HcFmNumber is the HC's 16-bit timing reference. Check it rather than
  assuming that software sleeps produced periodic service opportunities.

Repository inspection: both HCs serialize one live packet through a shared
descriptor and bounce buffer, retiring it for two hardware frames before reuse.
UHCI's frame entries all lead to that QH; OHCI's HCCA entries all lead to its ED.
The hub uses control requests rather than an interrupt pipe, so it cannot take
a HID interrupt slot. The P_DEVICE service visits both active HID interfaces.
Boot/idle setup, initial report diffing, F12 usage 0x45 to set-1 0x58, and the
merged input grant are already implemented.

The defect to reproduce is the periodic packet's two **software** millisecond
deadline: it can remove the TD before the HC gets a frame to service it, then
return EAGAIN as though a poll had occurred. The existing fixture advances
both clocks together and cannot expose this. Extend it with independently
paced controller frames and the directive's simultaneous root keyboard/hub
mouse topology. Keep each periodic TD linked until ACK or two observed HC
frames, bounded by a 20 ms software/count watchdog. Preserve retirement and
advance a HID toggle only on ACK. Record poll frames separately from retirement
frames so the lead can distinguish an idle endpoint from an unserviced TD.

This is a host-reproducible scheduling defect; the supplied QEMU transcript has
no HID packet trace and cannot establish that it caused that specific failure.
The lead's UHCI/OHCI `f1-usb` rerun remains required to confirm that connection.

Verbose/test boots will log a bounded prefix of HID reports, translated keys
and failed interrupt polls per interface. Quiet boots will keep these
diagnostics disabled. Terminal transfer failures are still logged after the
NAK prefix has reached its limit.

## Host validation (2026-10-14)

- The new `delayed_frame_hid` fixture fails with the original UHCI engine:
  simultaneous mouse motion passes, but the root F12 delivery assertion fails.
  Replaying the original OHCI engine and its two-tick periodic deadline gives
  `F12 held=0 keyboard bytes pending=8 mouse x=3 y=5 result=-11` and fails
  the same assertion. These are synthetic host results, not QEMU evidence.
- `nice -n 19 bash tests/host/usb/run.sh`: PASS with ASan/UBSan. Both HCs
  deliver the first F12 report, its release and simultaneous hub mouse
  motion/buttons; idle keyboard NAKs preserve its toggle and allow mouse
  progress. The fixture checks boot/idle requests, endpoint ACK toggles,
  separate polling/retirement frame counts, frame counter wraparound,
  diagnostic limits, verbose/test admission and quiet suppression.
- `ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_kernel_tests.sh`:
  PASS, including the production merged USB/native queue, input grant tests,
  USB lifecycle/storage suites and the other kernel fixtures. The first run
  with default LeakSanitizer stopped because this sandbox disallows its
  process inspection. ASan/UBSan remained enabled in the passing run.
  The native `-m32` runtime probe is unavailable (status -31); the suite's
  i686 ABI JSON fallback passes. Its optional DOS guest runner is skipped
  for the missing headers or DOSBOOT.IMG prerequisite reported by the suite.
- `nice -n 19 python3 scripts/build_kernel.py`: PASS. The i686 kernel links
  with no undefined symbols, frame-size checks and FPU/SIMD audit passing.
  `build/f0/VMM.ELF`: 4,353,332 bytes;
  SHA-256 `05b5fa254468dca183e14bdf6f6d70c99085ff5ee8ce204fa6fedcd14d123cca`.
- Host suites ran sequentially. No QEMU, image build, commit or diary entry.
  Temporary baseline replay sources and executable were deleted.

The lead must rebuild the canonical image and run UHCI/OHCI `f1-usb`. Capture
the new HID report/key records and pipe state. An EAGAIN snapshot now reports
`poll_frames` separately from the frames elapsed during retirement; a missing
service opportunity hits the bounded watchdog rather than masquerading as
an ordinary idle poll. The ELF hash above identifies only the compiled kernel.
