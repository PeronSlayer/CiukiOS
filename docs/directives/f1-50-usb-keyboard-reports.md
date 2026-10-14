# f1-50 — USB keyboard attached but no key ever reaches the desktop (QEMU, i8042 off)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-50` from `main`. Host-testable; the lead re-runs `f1-usb`.

## Evidence (QEMU `qemu-desktop-2002`, UHCI, `usb-kbd` on root port 1, `usb-mouse` behind QEMU's hub on port 2, i8042 off; suite `f1-usb`, case `usb-input-uhci`, main ed85dbda, 2026-10-14 05:00; transcript `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-keys0/serial-clean.log`, QMP log beside it)

```
[usb] device addr=1 vid:pid=0627:0001 class=00 speed=full
[input] devices=1 source=usb event=attach ps2=0 usb=1 firmware=0
[usb] hid keyboard attached addr=1 interface=0 wheel=0
[usb] device addr=2 vid:pid=0409:55aa class=09 speed=full   (hub) … hub port 1 connect/reset → mouse
… case=interaction stage=after presents=3610 input_events=53 keys=0 motion=36 buttons=12
FAIL: usb-input-uhci — desktop did not consume every input kind: keys=0 motion=36 buttons=12
```

The runner sent four QMP key events (`f12` down/up twice, `input-send-event` without a device id) and the mouse moves/clicks through the hub-attached `usb-mouse`; the desktop counted 36 motions and 12 buttons but **no key**. The keyboard enumerated and attached its boot-protocol interface. Nothing in the transcript shows an interrupt-IN report from addr 1 (there is no per-report record).

## Task

1. Root cause: QEMU routes keyboard events to the `usb-kbd` (verify in QEMU's input routing: with `i8042=off` the only keyboard is the USB one). Check on the guest side: the interrupt IN endpoint of the keyboard interface is scheduled (UHCI interrupt QH in the frame list with the right period, data toggle, `MaxLen` 8), `Set_Protocol(boot)` and `Set_Idle` sent, reports polled by the P_DEVICE service for both devices (the mouse behind the hub works, the root-port keyboard does not: compare the two pipes — toggles, QH ordering, the hub's interrupt pipe stealing the slot, NAK handling), report diffing not dropping the first report, usage→set-1 translation of `f12` (0x45 → 0x58) and the merged queue delivering to the input grant.
2. Diagnostics: verbose records for the first N interrupt reports per HID device (`[usb] hid report addr=… len=… bytes=…`), keys translated (`[input] usb key usage=… set1=… down=…`), and interrupt pipe state when a poll fails.
3. Host test: the UHCI/OHCI HID fixture with a root-port keyboard and a hub-attached mouse at the same time, keys and motion both delivered (fails before, passes after).

## Acceptance

`tests/host/usb/run.sh` and kernel host suites green; i686 kernel compiles; the lead re-runs `f1-usb` (UHCI and OHCI cases).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-50-report.md`.
