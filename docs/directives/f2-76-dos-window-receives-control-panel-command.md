# f2-76 — a DOS window receives "control --panel=sounds" typed at its prompt

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-76` from main `d5383378`+.
- **Authority:** `apps/desktop/` (shell.c menus/actions, desktop.c launch and media/autoplay paths, client.c launch arguments, settings_catalog.h), `apps/dosbox/` (typed command), their host tests and goldens.

## Evidence (image a34118c1, case `volume-resolution-cd-usb-qemu-t23` of `tests/suites/f4-media.json`, selector `f3:vm-window`, fixtures: Joliet CD + FAT16 USB stick; run `build/test-runs/zz-media/c1458ec4/`)

The probe starts the desktop with `--launch=dosbox` (`src/kernel/probes/f3_probes_video.c:229`); the window opens and the DOS shell prompt appears, then, before any runner stimulus (the ARM `window_input` was emitted but no key was injected yet), the guest screen reads:

```
CiukiOS SHELL C:\APPS> control --panel=sounds
command: not found
CiukiOS SHELL C:\APPS>
```

The probe then fails (`END status=FAIL error=-5`, VM terminated by its cleanup, `data_loss=1`). The same `vm-window` probe without fixtures (`vm-window-qemu-desktop-2002`) passes on the same image. `control --panel=sounds` is the launch line of the Sounds applet in the new settings catalog (`apps/desktop/settings_catalog.h`, `apps/desktop/shell.c` around line 590, `act()` case `SH_ACT_CONTROL`). The dosbox client types its non-option `argv` words at the DOS prompt (`apps/dosbox`). `desk_task_launch`/`desk_task_launch_args`/`run_command` reset `launch_arg`/`launch_argv` after each spawn, and the `--launch=` path (`apps/desktop/desktop.c` around line 1428) launches with no argument, so the text reached the DOS window through another path: candidates are the f2-74 media notification / autoplay handling on USB insertion (`desktop.c` `media_read`, `autoplay_preference`, `media_sound`), the f2-75 menu/catalog rework (tray and Start settings items, `DESK_SETTINGS_*` requests, the "same controller for both Display entry points"), the volume icon click mapping (`shell.c:2085` `CP_SOUNDS`), or a keyboard/launch replay. This case passed when f1-63 was merged (`docs/validation/f1-63-volume-resolution.md`). On the T23 the owner keeps a USB stick inserted: the first game window would receive this text instead of its game command.

## Required

1. Reproduce in the desktop host harness (`tests/host/desktop`): desktop started with `--launch=dosbox` while a USB volume is present (media state with a removable drive, generation and the f2-74 notification), with the probe's gate disabled as the kernel probe does. Identify the exact path that types `control --panel=sounds` (file:line) and why it is triggered without user input.
2. Fix it: no desktop action may be executed without a user event; launch arguments must never leak from one launch into another; a DOS window started by `--launch=`, a menu click or a game entry receives only its own command.
3. Regression tests: the reproduction above (no text typed, dosbox argv exactly as expected), plus the media-notification and settings-menu paths asserting that no launch is issued without a click/key. Run `make CAP= desktop`, the desktop suite (`python3 -B -m unittest discover -s tests/host/desktop -t .`) and the gate (`test_desktop_gate.py`). Regenerate goldens only if pixels change and say which.
4. Report in `build/codex/f2-76-report.md` with cause, fix (file:line) and verbatim results; the lead reruns `volume-resolution-cd-usb-qemu-t23`, `vm-window-qemu-desktop-2002` and `dos4gw-qemu-t23` on the integrated image.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
