# f2-58 — Desktop shell test with a USB keyboard: keys delivered, stages do not complete

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-58` from `main`. Host-testable; the lead re-runs `f1-usb`.

## Evidence (QEMU `qemu-desktop-2002`, UHCI, i8042 off; `f1-usb` case `usb-input-uhci`, main 59d66440, 2026-10-14 07:10; transcript `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/f1-usb-keys0/serial-clean-final.log`)

The USB keyboard's reports are translated and delivered (`[input] usb key usage=45 set1=058 down=1 …`, 'a' as set-1 0x1E) and the mouse behind QEMU's hub moves and clicks (`motion=36 buttons=12`), yet the probe's interaction record ends with `keys=0`: `gate_report` in `apps/desktop/desktop.c` counts keys only once the shell test reaches stage 6, and with USB input the shell-test scenario (selected by the post-fault F12) does not reach it, while the same case with the PS/2 keyboard (`desktop-shell-qemu-t23`) passes. The runner then fails the case ("desktop did not consume every input kind"). f1-51 proved the kernel path with a host regression through the real input grant.

## Task

1. Find which shell-test stage stalls with USB input and why: timing (USB polls at 10 ms, reports arrive in batches 1.4–2.5 s apart in the transcript — check the runner's batch pacing against the desktop's stage deadlines), the set-1 translation of the scenario keys (F12, 'a', Logo keys, Enter/Escape used by the stages), key repeat/typematic emulation of USB keys versus the PS/2 make/break the stages expect, or the desktop's `keys=` accounting itself. Reproduce in the desktop host harness with USB-sourced key events (source=usb, the translated set-1 codes) driving the shell test.
2. Fix at the cause (desktop/shell-test or probe), keeping the PS/2 case unchanged; if the runner's pacing is the cause, say so and the lead adjusts the suite.
3. Host test: the shell-test stages complete with USB-sourced keys.

## Acceptance

Desktop host suites green; the lead re-runs `f1-usb` (UHCI and OHCI) and `desktop-shell-qemu-t23`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-58-report.md`.
