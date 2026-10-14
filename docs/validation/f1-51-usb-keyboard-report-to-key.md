# F1-51 — boot keyboard reports through the input grant

## Sources and implementation decision

Research and repository inspection on 2026-10-14, before implementation:

- [USB HID 1.12](https://www.usb.org/sites/default/files/hid1_12.pdf),
  appendices B.1 and F.4–F.5: byte 0 carries modifiers, byte 1 is reserved
  and must be ignored, and bytes 2–7 carry the six keyboard usages. Compare
  successive reports to produce press/release events; rollover retains the
  last valid key state.
- [USB HID Usage Tables 1.22](https://usb.org/sites/default/files/hut1_22.pdf),
  Keyboard/Keypad page: usage 0x04 identifies A and usage 0x45 identifies F12.
- [Linux v6.12 boot keyboard driver](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/hid/usbhid/usbkbd.c):
  the driver compares modifiers and the six key slots, without validating
  the reserved byte, and reports unsupported usages separately.

The existing CiukiOS table already maps these usages to public set-1
positions 0x1e and 0x58. The wheel descriptor check applies only to mice.
`input_usb_event` merges USB transitions into the same queue that
`grant_input_read` drains, without requiring active PS/2 hardware.

The current file at the directive's cited `serial-clean-after-f1-50.log` contains the
four translation records at lines 617, 621, 625 and 629 (F12 down/up, A
down/up). Its `keys=0` summary is therefore not evidence of a decoder
failure. In `apps/desktop/desktop.c`, F12 selects the shell scenario;
`gate_report` publishes its key count only after shell stage 6 (launch,
drag, close, reap). The cited run ends before that contract succeeds.
Whether scenario timing explains the incomplete interaction needs the
lead's validation; no QEMU or hardware evidence is produced here.

Keep the correct mappings, modifier handling and mouse decoder. Correct
the reserved-byte check, add verbose/test discard reasons, and exercise
the production HID poll/decoder, merged queue and input grant together.
Test the exact supplied reports before the decoder change, then separately
test the reserved-byte case and diagnostics. Do not claim that an exact
sequence already passing before the change is a reproduced failure.

## Host validation

- The exact F12 press/release and A press sequence reaches a real grant
  reader before and after the change, with PS/2 absent. A release also
  reaches the reader; source, generation, sequence and timestamps match.
- A nonzero reserved byte previously quarantined the keyboard; after the
  fix, modifier and F12 press/release events reach the grant.
- `tests/host/usb/run.sh` and `scripts/test/host_kernel_tests.sh` pass with
  ASan/UBSan. The latter includes 1,248,503 i8042 checks with zero failures.
  Its optional DOS guest workload is skipped without the built payload.
- `scripts/build_kernel.py` compiles and links the i686 kernel with the
  canonical flags; its FPU/SIMD audit passes. No image build or QEMU run.

Commands, artifact hashes and remaining limitations are recorded in
`build/codex/f1-51-report.md`.
