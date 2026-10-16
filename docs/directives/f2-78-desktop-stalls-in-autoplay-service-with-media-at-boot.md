# f2-78 — the desktop stalls in the AutoPlay service when a USB stick is present at boot

- **Phase:** F2+ (desktop). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f2-78` from main `2d6990cf`.
- **Authority:** `apps/desktop/autoplay.c`, `apps/desktop/desktop.c` (media polls, stall observer), `apps/desktop/shell.c` (internal dialogs), host tests.

## Evidence (T23, image a79f840c, second boot: menu L with the stick inserted before power-on; `legacy/local/physical/a79f840c/serial.log` lines 1092–1923, read-only)

The stick mounts in 306 ms (`[usb-storage] volume drive=E … mode=ro`, `bind … published=1`); the system then shows the desktop but no input is processed and the owner sees it frozen; the stall observer reports for more than 30 s:

```
[user 20] [desktop] stall stage=shell worker=idle/settings-read no_frame_ms=33529 input_pending=0 work_pending=0 wait=autoplay-service wallpaper_wait=mailbox-wait
```

`[kernel] stretch_max_ms` stays small and the USB workers keep scanning, so the kernel is alive: the desktop's main loop is waiting inside `desk_autoplay_service` (f2-76 made newly discovered media wait while a foreground window, menu or initial launch frame owns input; f2-74 added the notification and the AutoPlay prompt). The third boot without the stick is normal. The same image on QEMU passes `safe-remove-usb-qemu-t23` and the desktop gates, which start the stick after the first frame.

## Required

1. Reproduce in the desktop host harness: media present before the first frame (mounted during setup, before `desk_shell_*` is ready), with the wallpaper decode and settings read in flight; find the wait (`autoplay-service` names the stall stage) and why it never returns: a wait on a condition that only the main loop itself can satisfy, a queued media entry that keeps the prompt from opening while the initial launch frame "owns input", or the settings/wallpaper mailbox.
2. Fix: the AutoPlay service never blocks the frame loop; queued media is serviced by state, not by waiting; media arriving before the first frame is handled after the shell is ready; the stall observer's `wait=autoplay-service` cannot exceed one frame.
3. Host coverage: media present at setup, media arriving during the first launch frame, media plus an open menu; the three f1-usb cases and the desktop gate unchanged. Run `make CAP= desktop`, the desktop suite, the gate. Report in `build/codex/f2-78-report.md`. The lead runs the desktop and media gates on QEMU and the owner boots the T23 with the stick inserted.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
