# f1-45 — USB on QEMU: the UHCI controller is ready but no root-port device is ever enumerated

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-45` from `main`. Host-testable; the lead re-runs the `f1-usb` suite.

## Evidence (QEMU 9.x, `qemu-desktop-2002`, image 21106e99, suite `f1-usb` case `usb-input-uhci`, 2026-10-13 21:40)

QEMU command topology (runner evidence): `-machine …,i8042=off`, `-device piix3-usb-uhci,id=usb0`, `-device usb-kbd,bus=usb0.0`, `-device usb-mouse,bus=usb0.0`. Serial transcript (copy in `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-uhci-fail/serial.log`, with `qmp.log`, `stderr.log`, `result.json`):

```
[i8042] step=flush index=1 command=ad reply=ff … result=-110
[init] input result=failed error=-110 reason=native activation_seq=3 tick=102
[usb] controller=uhci ports=2 pci=00:03.0 mode=poll reason=ready_polling
[work] budget exceeded site=accel_work+0 us=252722 pending=0 violations=1
… the desktop starts (crash-isolation probe), no key or click ever arrives, the desktop stops, END status=FAIL reason=desktop_contract
```

No `[usb] device …` record, no `hid keyboard/mouse attached`, no port-change record: the two root ports with the keyboard and mouse were never enumerated. The controller record shows the polling worker reached readiness. The OHCI case did not run.

## Task

1. Root cause from `src/kernel/usb/uhci.c`, `usb_core.c`, `usb_hub.c` and the UHCI 1.1 design guide: root-port connect detection (PORTSC CCS/CSC, port reset and enable sequence, the 50 ms reset and 10 ms recovery of USB 1.1 §7.1.7.5), the polling worker's cadence and whether it ever reaches the port scan (the 253 ms work overrun above hints at a blocking wait inside a work item), the legacy-support hand-off on QEMU's PIIX3 UHCI (LEGSUP register 0xC0), the frame list start (USBCMD RS, FRNUM, FLBASEADD) and whether the control transfer of the first GET_DESCRIPTOR retires. Compare with QEMU's `hw/usb/hcd-uhci.c` behaviour (port status bits after reset, SOF timing) and with the Linux `uhci-hcd` root-hub polling; cite them.
2. Add verbose records for every root-port transition (`[usb] port=N status=… event=connect|reset|enable|disconnect`), for the first control transfer of each device (`[usb] addr0 get_descriptor … result=`), and for the worker cadence (first scan tick, scans per second), so the next QEMU run is conclusive even if the fix is incomplete.
3. Fix, with a host model of QEMU's root-port status behaviour in `tests/host/usb/` reproducing the stall (fail before, pass after). Keep the firmware-first and safe-mode guards of f1-42.
4. If the worker is the culprit (work-queue budget), move the port scan and enumeration into the dedicated P_DEVICE thread as the contract prescribes and keep work items short.

## Acceptance

Host suites green (`tests/host/usb/run.sh`, kernel host tests); i686 kernel compiles. The lead runs `f1-usb` (UHCI and OHCI cases) on the rebuilt image.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-45-report.md`.
