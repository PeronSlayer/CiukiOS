# F2-69 shell scenario and window history

Research checked on 2026-10-15, before implementation:

- [QEMU QMP input events](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
  define `rel` as pointer motion, without window-relative targeting.
- [QEMU input dispatch](https://github.com/qemu/qemu/blob/v11.1.1/ui/input.c)
  dispatches those events to the input device. Window identity and geometry
  belong to the guest desktop, not to QMP.

Image `aae6a1704e2d2a8996f5e3c651b34afa0b87d5609c213383ae77305043bb26f4`,
desktop-shell run `d555dcef`, shows `shell stage 3 programs 22` in
`desktop-after.ppm`. The survivor is at `(20,52)` and the launched demo at
`(110,142)`. The serial interaction record has `keys=0 motion=47 buttons=12`;
both demos remain alive. The UHCI run `4b1ed0fa` has the same incomplete
scenario (`keys=0 motion=50 buttons=12`).

The new-window cascade advances for each accepted window, including all
100 fault victims before ARM. The shell demo therefore occupies slot
`101 % 8 = 5`. The old script's title press at `(60,80)` hits the survivor
instead. The host replay only skipped the victim PIDs, leaving cascade
slot 1 for the menu demo and concealing this error.

Decision: preserve cascade, activation and title dragging. Model the 100
victims' window creation/drop through the production desktop protocol in
the host shell replay. At the runtime-only F12 scenario's drag and close
presses, aim the pointer at the tested demo's actual title (stage 3) or close
box (stage 4), then dispatch the original mouse press. Targeting neither
focuses nor moves a window, requests a close, nor advances a scenario stage.
The unchanged QMP mouse/key events still traverse the PS/2 or USB path;
targeting avoids dependence on cascade history, frame position and
title-button geometry. Resolve targets when consuming the button, so
firmware keyboard/mouse reordering cannot separate a targeting key from
its click. The initial F12 pair and final A pair retain the existing
four-key gate contract.

The threaded host replay also needs separate copies of the demo's static
protocol-version variable for the v1 survivor and the v2 menu demo. Compile
the production source twice with separate entry points, preserving their
guest process separation without changing the application. Otherwise a
menu launch changes the survivor's expected protocol and it exits on its
next focus/input message; the old host replay did not check that survivor.

Host and QEMU validation results are recorded in
`build/codex/f2-69-report.md`. QEMU verification belongs to the lead.

Host validation on `wt/f2-69` from `0712bc59`, all commands serialized with
`nice -n 19`:

- Before the scenario correction, the extended shell replay failed in all
  six timing models at stage 3 with both demos alive and the same
  `keys=0 motion=47 buttons=12` as the T23 guest.
- After the correction, all six T23 shell models, the E500 firmware-order
  models, and the UHCI/OHCI shell models reach stage 6 with four keys.
- Desktop discovery: 101 tests in 304.554 s, OK, five built-payload checks
  skipped because the worktree has no built desktop payloads.
- `test_desktop_gate.py`: 10 tests in 47.158 s, OK.

QEMU evidence for the changed source is pending the lead's canonical image
and `desktop-shell-qemu-t23` / `usb-input-uhci` runs. No guest image was built
or run in this worktree.
