# F2-58: USB shell stage replay (host evidence)

## Finding and decision

The supplied UHCI transcript is consistent with the shell scenario stalling at
**stage 0**, before opening Start. F12 selects the shell test and its release
counts; A also arrives. The first Start click lands at `(0,0)`, outside the
Ciuki button `(4,4,96,24)`. Thus `gate_report` correctly keeps `keys=0` despite
the consumed A edges. Do not relax that accounting or change desktop shortcuts.

The cause reproduced on the host is the runner sending relative moves faster
than the guest consumes USB mouse reports. QEMU merges adjacent unconsumed
motion with equal button state. A corner clamp followed by a positive move
requires the intermediate corner position to be consumed; merging them changes
the final absolute position after desktop clamping. Increasing the number of
corner-clamp moves cannot preserve that intermediate position by itself.

The directive assigns runner pacing changes to the lead. This worktree adds
the reproduction and positive USB stage regression; desktop, kernel, probe,
runner and suite declarations keep their existing behavior. The lead needs a
consumption barrier between corner clamping and the move onto Start, and must
allow each intended path segment to drain before the next segment cancels it.
Audit the return-to-corner before drag and the large multi-report moves too.
QMP command success alone acknowledges injection, not guest consumption.

## Research checked against this repository (2026-10-14)

- [QEMU 11.1.1 HID source](https://github.com/qemu/qemu/blob/v11.1.1/hw/input/hid.c):
  `hid_pointer_sync` combines pending same-button relative motion;
  `hid_pointer_poll` drains axes in signed 127-unit chunks, keeping remainders.
  Keyboard and mouse queues are independent. The model uses those rules and
  rejects queue saturation instead of modeling QEMU's overflow behavior.
- [QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event):
  the request injects events; it does not supply a desktop-stage acknowledgement.
- [USB HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf), Appendix B:
  boot keyboard and mouse report formats. Repository `usb_hid.c` maps keyboard
  usages to public set-1 positions, emits button edges before mouse axes and
  schedules polls using the endpoint interval. USB typematic is a separate
  500 ms delay / 33 ms repeat mechanism. F12 and A require no repeats here.
- `tests/suites/f1-usb.json` copies the shell script from
  `desktop-shell-qemu-t23`: F12 down/up, mouse menu/drag/close, A down/up.
  Logo, Enter and Escape are absent from this scenario.
- `scripts/test/run.py:Actions` schedules the next batch using host monotonic
  time and 20--100 ms declared pauses. `apps/desktop/desktop.c` selects F12
  in the production loop, queues input and holds it for a launched program's
  first frame. The desktop cannot recover motion already merged by QEMU.
- `shell.c`, `client.c`: stages progress on Start open, successful demo launch,
  window creation, drag, close request and successful child reap. No early
  stage deadline exists; the close timeout starts after a close request.

## Supplied transcript and reproduction

Read-only input: the directive's `serial-clean-final.log`, run `9b8fb480`.
The initial reports include five `00 81 81 00`, then `00 a9 91 00`, then
`01 00 00 00`. Signed deltas are five `(-127,-127)`, then `(-87,-111)`;
the first button press therefore occurs at the corner. The merged totals are:

```
x = 6 * -127 + 40 = -722 = 5 * -127 - 87
y = 6 * -127 + 16 = -746 = 5 * -127 - 111
```

The transcript shows guest report gaps around 1.4--2.5 s and ends with
`keys=0 motion=36 buttons=12`. This is existing evidence, not a new QEMU run.
A companion `result.json` has a different run ID (`96db3e1e`); its 44 observed
batches span 4.914 s of host time, including about 64 ms between corner-clamp
batches. Those timings are corroboration only and are not attributed to the
cleaned transcript's run or image hash.

The new `--usb-poll-ms` host model drives the unchanged production desktop
through its normal `ciuki_input_read` boundary. It supplies `source=INPUT_USB`
(public value 3) and already-translated set-1 keys, with independent keyboard
and pointer queues. The harness asserts the USB source on every consumed event
and counts the four scenario key edges.

- 2000 ms modeled polling, declared script pacing: reproduces the exact initial
  seven mouse reports, first click `(0,0)`, shell selected, stage 0, four key
  edges consumed, A gate count 2, final report keys 0, motion 36, buttons 12.
- 10 ms modeled polling, both UHCI/OHCI script declarations: stage 6, keys 4,
  first click `(40,16)`, no queued reports or keys left.
- 10 ms polling with an 8 s launched-program startup delay: both scripts still
  reach stage 6 and keys 4 via the existing desktop input hold/replay.

The UHCI/OHCI replays share a HID/grant model; they do not execute either
controller, the real HID decoder, typematic or real scheduling. Existing USB
host tests cover the real HID/grant path. The cause of the slow USB worker
itself remains outside this directive. The lead must re-run `f1-usb` for both
controllers and `desktop-shell-qemu-t23` after correcting pacing.

## Host validation completed

Suites ran sequentially, with `nice -n 19`:

- `python3 -B -m unittest discover -s tests/host/desktop -t .`: 94 tests,
  OK, 5 skipped (235.168 s). Existing PS/2 shell replays also pass.
- `bash tests/host/usb/run.sh`: PASS, including the production HID/input
  grant F12 and A regression and ASan/UBSan controller/HID harnesses.
- `git diff --check`: PASS.

Initial discovery without `-t .` failed two package-relative imports; Terminal
also required the pinned Lua archive. Package-aware discovery and a hash-verified
copy of the existing cached archive resolved those setup errors. No QEMU or
image build was performed for this work.
