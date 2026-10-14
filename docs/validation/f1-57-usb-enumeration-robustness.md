# F1-57 USB enumeration and live interrupt pipes

## Research and implementation decision (2026-10-14)

Checked before implementation:

- [USB 2.0 specification, April 2000](https://sinelab.net/pdf/usb-20-specification.pdf)
  (original specification mirror; [USB-IF distribution](https://www.usb.org/document-library/usb-20-specification)):
  §§7.1.7.3/7.1.7.5 require connection debounce and reset recovery;
  §9.2.6.3 allows 2 ms after SET_ADDRESS status completion. §9.4.5 and
  endpoint GetStatus specify DATA0 after CLEAR_FEATURE(ENDPOINT_HALT),
  including when the endpoint was not halted. Root reset retains the
  existing 50 ms signaling and at least 10 ms recovery.
- [Linux v6.12 hub.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/core/hub.c):
  `get_bMaxPacketSize0`, `hub_port_init`, and debounce use bounded descriptor
  operations, reset between series, and 100 ms stable connection. Linux
  also waits 10 ms after address assignment; this directive retains the
  USB minimum 2 ms. Ciuki implements the requested two series of three
  eight-byte requests, rather than adopting Linux's complete enumeration
  scheme or 64-byte initial request.
- [Linux UHCI root hub](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/uhci-hub.c)
  and [register/TD definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/uhci-hcd.h):
  ordinary PORTSC writes mask W1C and reserved bits; reset release clears
  CSC/PEC and explicitly sets PE. Intel extensions include OC/OCC. TD NAK
  with Active set is pending; babble and CRC/timeout differ from STALL.
  The Intel 1.1 PDF mirrors linked in the contract timed out in this session;
  register behavior was checked against upstream definitions and code.
- [HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf): boot
  input reports and idle suppression explain idle interrupt NAKs. Appendix B
  explicitly permits boot mouse extensions up to eight bytes; only the first
  three bytes define boot input. Requesting exactly three bytes on an
  eight-byte endpoint can cause legal extensions to retire as babble.
- [QEMU 9.2 OHCI](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-ohci.c)
  and [USB HID](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/dev-hid.c):
  a NAK does not retire an OHCI TD; completed TD NextTD becomes done-list
  linkage. Skipped EDs cancel pending asynchronous packets. HCCA periodic
  heads are revisited each frame. HID returns NAK while idle. Root reset
  sets PRSC (bit 20), independently of CSC (bit 16).

Repository comparison: enumeration issued one GET_DESCRIPTOR(8), then
retained failures permanently. Root scanning detached on CSC even with
CCS set. The logged `event=enable reason=state` is emitted by normalized
PE differences (either direction), not proof that software enabled a port.
UHCI `port_write` already masked CSC/PEC; reset's final W1C write preserved
PE. It did not itself demonstrate the receiver's later babble cause.
The private HID engine unlinked pending TDs after two frames and failed
immediately on STALL/babble. OHCI re-arm already rebuilt HeadP and TD. Its
reset code incorrectly waited for CSC as reset completion; the host model
incorrectly set CSC on reset, masking that defect. Correct both to PRSC
before attributing a batch stall to scheduling or changing runner pacing.

Decisions: keep one device record throughout bounded descriptor retries;
retry initial descriptor operations 100 ms apart, with one additional
physical reset before the second series. A live child with CCS set survives
change acknowledgements and receives PE recovery without reset. Observe
disconnect before performing endpoint recovery. The input worker requests
one halt-clear/toggle-reset recovery; only the serialized general owner
issues that control request. A successful report renews the recovery budget.
The composite replay also exercises babble disabling its shared root port:
another interface can retire with CRC/timeout before the general owner
restores PE. Permit one retry with the existing toggle only when CCS is
still set and PE is clear, with raw TD CRC/timeout (UHCI) or device-not-
responding (OHCI). This port-enable retry does not halt-clear the unaffected
interface. Enabled-port CRC/timeout and other transport/DMA errors remain
terminal. Root recovery precedes re-arm and endpoint halt-clear.
F1-52's private interrupt request used three bytes for a boot mouse (four
for the recognized wheel layout), unlike the older endpoint-sized request.
Restore up to eight bytes, bounded by endpoint MPS and the existing private
buffer; parse only the three-byte boot prefix or recognized wheel layout.
The supplied three-byte babble length is compatible with this defect, but
does not prove how many additional bytes the real receiver sent. No generic
HID parser or receiver-specific quirk is introduced.
Idle NAK leaves the private TD linked indefinitely; only a stalled HC frame
clock triggers the watchdog. Retired errors and ACKs still wait two frames
before DMA reuse. Keep the existing bounded HID trace prefix.

Operation diagnostics are separate from the F1-53 coalesced state records:
a 32-operation initial prefix preserves raw reset/enable/disable writes during
enumeration; subsequent repeated operations have a one-second budget and
cumulative count. Enumeration attempt records are inherently bounded to six
per device record. Host findings and lead validation requirements are
recorded below after testing.

## Reproductions and saved-run provenance

The three-byte boot mouse request was restored in a scratch copy of the
current HID source, keeping the rest of the engine unchanged. An eight-byte
boot report from the composite receiver fixture reproduced
`requested=3 td=18500002 clear_halts=1 reason=hid_transfer input=0`.
The final request accepts it and ignores its suffix, without halt-clear,
wheel interpretation or detach. Genuine injected babble still gets one
endpoint recovery, including a disabled shared port and another interface's
timeout. The matching TD shape explains a software mechanism for the T23
regression; the real receiver's report length still needs hardware evidence.

The unmodified HEAD OHCI source was compiled against the corrected model:
`reason=root_port_reset devices=0 input=0 resets=1 raw=00100003`.
The current driver completes five PRSC-qualified pulses, enumerates, then
consumes F12 down/up, batch-2 motion, repeated motion and button release.
No periodic re-arm defect or runner pacing change was needed for that case.
Both scratch source copies and their binaries were removed; short output
records remain in `build/codex/f1-57-{mouse,ohci}-regression.log`.

The saved lead run `f1-usb/ebe4b735` linked by the directive has image SHA-256
`2727313208d90f0aa18d54ab9c0d311c7bf919401450be6545697cd55dc4a155`,
revision `c367510fa36f35f8d9b3cd0ef9615ddbeaf2a33a`, QEMU 11.1.1.
Its serial log records OHCI `root_port_reset` before any USB device is
enumerated, followed by zero input counters. Its result records three sent
batches and no consumption waits, then the batch-2 failure. This is saved
evidence only, not a QEMU run performed in this worktree, and it is not
image `fb3147dc`. The register-bit defect exists in this worktree's base too.

## Host verification

All builds and suites ran sequentially with `nice -n 19`:

- `bash tests/host/usb/run.sh`: PASS with ASan/UBSan. Covers BIOS-addressed
  devices ignoring address zero before physical reset, first SETUP timeout,
  each retry position through six failures, reset/recovery/address timing,
  stable debounce, live CSC/PEDC, guarded reset/disable, permanent quarantine,
  >1 s NAK on the same TD, bounded traces, halt-clear failure, persistent
  STALL/babble and enabled-port CRC rejection. Composite receiver coverage
  preserves keyboard state/toggle across endpoint-2 recovery, accepts legal
  boot suffixes, and repeats with shared PE loss. OHCI batch-2 and repeated
  re-arm pass under independent and delayed clocks. Existing latency coverage
  delivers all 1,344 reports through the production input queue within its
  ACK-to-delivery bound under concurrent BOT and scan/reset work.
- `python3 -m unittest discover -s tests/host -p test_runner_usb.py -v`:
  PASS, 15 tests; runner code unchanged.
- `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`:
  PASS, including final USB and storage suites. ASan/UBSan remain enabled;
  LeakSanitizer is disabled for the sandbox. The native `-m32` runtime probe
  was unavailable (status -31); target ABI checking still ran. The optional
  DOS guest fixture reported its existing missing-dependency/payload skip.
- `PYTHONDONTWRITEBYTECODE=1 python3 scripts/build_kernel.py`: PASS, i686
  compile/link, frame-size checks, undefined-symbol check and FPU/SIMD audit.
  Produced `build/f0/VMM.ELF` (4,452,536 bytes). No full disk image was built.
- `git diff --check`: PASS.

This is host evidence against base `f63f6d1b28cb2f473297f63e9c8895a6d24bbd35`
plus this worktree's diff. Logs and the implementation report are in
`build/codex/`. No QEMU, systemd scope, commit, push or diary was performed.

The lead must build the integrated canonical image and record its SHA-256,
keep prior gates passing, and run both `f1-usb` controllers. On the T23,
verify the boot-present Lexar binds storage and appears in Files; verify
the receiver's keyboard/mouse, held state, buttons and idle-to-input recovery
throughout the session. Inspect port-action writes, attempt numbers and
endpoint recovery methods in the disk log. Hardware determines whether the
receiver actually emits an extended boot report and whether the stick needs
the second reset/descriptor series. No host fixture qualifies either device.
