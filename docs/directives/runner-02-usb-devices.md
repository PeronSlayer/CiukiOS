# runner-02 — QEMU runner: USB host controllers and HID devices in profiles, i8042 off

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-runner-02` from `main`. Host-testable (`tests/host/test_runner*.py`, `tests/host/test_desktop_gate.py`); the lead runs the QEMU evidence.

## Why

f1-42 (merged) adds UHCI/OHCI and HID input. Its acceptance needs a QEMU boot of `qemu-desktop-2002` with USB keyboard and mouse only. `scripts/test/run.py` (`qemu_args`, line ~1275, and the evidence `devices` record at line ~1443) hard-codes `i8042: True` and offers no USB device option.

## Task

1. Profiles and cases accept `usb`: a list of `{"controller": "uhci"|"ohci", "devices": ["kbd", "mouse", "tablet"?]}` → `-device piix3-usb-uhci,id=usbN` or `-device pci-ohci,id=usbN` plus `-device usb-kbd,bus=usbN.0`, `-device usb-mouse,bus=usbN.0`; and `i8042: false` → machine option `i8042=off` (check the installed QEMU version supports it; otherwise refuse with a clear `Refusal`). The evidence record lists the USB devices and the i8042 state; hashes and the T1 checks unchanged.
2. The firmware input path of the runner (`fwinput`/QMP `input-send-event`) must work with USB devices: the desktop-shell and crash-isolation cases type and click through QMP `input-send-event`, which targets the active input device; verify with the host tests that the request path is unchanged, and document which device QMP drives when both exist.
3. A new suite `tests/suites/f1-usb.json` (prerequisites: `f0-smoke`) with two cases on `qemu-desktop-2002`: `usb-input-uhci` (i8042 off, UHCI kbd+mouse, the desktop-shell contract: keys and clicks reach the shell) and `usb-input-ohci` (same with OHCI); expected `[usb]` records (`controller=`, `device … class=`, `hid keyboard attached`, `hid mouse attached`) in the serial transcript.
4. Host tests for the argument builder (both controllers, refusal on unknown entries, i8042 option) and the suite schema.

## Acceptance

Host runner tests green; the lead runs `f1-usb` once on the current image.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble. Report `build/codex/runner-02-report.md`.
