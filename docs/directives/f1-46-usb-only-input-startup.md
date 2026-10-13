# f1-46 — USB-only input: the desktop must survive a late or absent input device, and the mouse behind QEMU's hub

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-46` from `main`. Host-testable; the lead re-runs `f1-usb`.

## Evidence (QEMU `qemu-desktop-2002`, i8042 off, UHCI with usb-kbd and usb-mouse; suite `f1-usb`, case `usb-input-uhci`, image fb9387ee, 2026-10-13 22:40; full transcript `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-uhci-fail2/serial-clean.log`, QMP log beside it)

After f1-45 the USB stack works: `[init] input result=failed error=-110 reason=native` (no i8042), then `[usb] worker service=started tick=4302`, port 1 connect/reset/enable, `GET_DESCRIPTOR` in 10 ms, `[usb] device addr=1 vid:pid=0627:0001 class=00`, `[usb] hid keyboard attached addr=1`; port 2 gives `[usb] device addr=2 vid:pid=0409:55aa class=09` (QEMU attached the mouse behind an auto-inserted hub). Then `[supervisor] desktop stopped; console ready pid=3 status=0 deaths=1` and the probe ends with `END status=FAIL reason=desktop_contract`; the runner sent no QMP input (0 `input-send-event`) because the terminal record arrived first.

Two problems:
1. The desktop (spawned by the supervisor right after boot) exits with status 0 when the kernel input subsystem reports failed/absent at its start, instead of waiting for devices that attach later (USB enumeration here began at 4.3 s; on real PCs USB keyboards and mice are also hot-plugged). Find the exact exit path in `apps/desktop` (input grant, `input` open, the `[init] input` gate) and in `src/kernel/proc/supervisor.c`.
2. Whether the hub-attached mouse was enumerated: the records stop after the hub device; check `usb_hub.c` downstream port handling with QEMU's hub (0409:55aa, 8 ports) and add port records for hub ports like the root ones.

## Task

1. Input readiness semantics: the kernel exposes "input present" as a dynamic state (PS/2 ready, USB HID attached, none yet); the desktop starts, draws and waits for input devices without exiting, showing nothing special; when the first device attaches, events flow (the merged USB/PS/2 queue of f1-42). The supervisor's spawn does not wait for input. The `[init] input` record keeps its meaning (i8042 result) and a new `[input] devices=… source=…` record is logged on every attach/detach.
2. Hub path: enumerate HID devices behind one hub level on UHCI and OHCI with records; QEMU's hub as the host model fixture.
3. Host tests: desktop harness case "no input at start, keyboard attaches later, keys arrive"; kernel USB test with the hub fixture; the f1-usb cases unchanged (they must now pass).

## Acceptance

Host suites green; i686 kernel compiles; the lead runs `f1-usb` (UHCI and OHCI) and `f2-desktop` on QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-46-report.md`.
