# f1-51 — USB keyboard reports arrive, no key event is produced

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-51` from `main`. Host-testable; the lead re-runs `f1-usb`.

## Evidence (QEMU `qemu-desktop-2002`, UHCI, i8042 off; `f1-usb` case `usb-input-uhci`, main 313b62fb with f1-50, 2026-10-14 06:10; transcript `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-keys0/serial-clean-after-f1-50.log`)

After f1-50 the interrupt reports of the root-port keyboard (addr 1) are received and logged:

```
[input] devices=1 source=usb event=attach ps2=0 usb=1 firmware=0
[usb] hid keyboard attached addr=1 interface=0 wheel=0
[usb] hid report addr=1 len=8 bytes=00 00 45 00 00 00 00 00 interface=0   (F12 pressed)
[usb] hid report addr=1 len=8 bytes=00 00 00 00 00 00 00 00 interface=0   (released)
[usb] hid report addr=1 len=8 bytes=00 00 04 00 00 00 00 00 interface=0   ('a' pressed)
[usb] hid report addr=3 len=4 bytes=00 81 81 00 interface=0               (mouse, works: motion=36 buttons=12)
… case=interaction stage=after presents=3609 input_events=53 keys=0 motion=36 buttons=12
```

No `[input] usb key usage=… set1=…` record follows any keyboard report, and the desktop never counts a key. The mouse reports on the same controller are translated and delivered. The polls themselves show `result=-11` (EAGAIN) most of the time with occasional reports, as expected.

## Task

1. Follow one keyboard report from `usb_hid.c` to the merged input queue (`src/kernel/drivers/i8042.c` queue / `input.h`, the grants in `src/kernel/proc/grants.c`) and find where it stops: the boot-protocol keyboard decoder (report layout, the `wheel=0` layout check applied to keyboards by mistake, the "report diffing" that must detect 0x45 appearing), the usage → set-1 translation table (F12 = usage 0x45 → set-1 0x58; 'a' = 0x04 → 0x1E), the modifier byte handling, the per-source enable (`devices=1 source=usb`), or the delivery into the queue the desktop's grant reads. Add the `[input] usb key` record at the translation point and a `[input] usb drop reason=…` record for every discarded report so the next capture is conclusive.
2. Fix; host test with the exact QEMU report sequence above (F12 press/release, 'a' press) asserting the set-1 events reach a grant reader (fails before, passes after). Keep the mouse path unchanged.

## Acceptance

`tests/host/usb/run.sh`, `tests/host/i8042_test.c` and the kernel host suites green; i686 kernel compiles; the lead re-runs `f1-usb`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-51-report.md`.
