# f2-74 — Removable media: the device-removal / CD notification renders wrongly; safe removal of mass-storage devices (owner request 2026-10-15)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-74` from `main`. Build the desktop and the kernel before reporting.

## Request

1. The popup shown when a device is removed or a CD is inserted/ejected is not displayed correctly on the T23 (owner's report; the notification panel of f2-64 is the path: `apps/desktop`, tray notifications for removable media and optical events from `[storage]`/`[usb-storage]`/`[optical]` records). Reproduce with the host replay (stick attach/detach, CD insert/eject with the existing media events), fix the layout, timing and dismissal (text clipped or overlapping, wrong position, stays or vanishes wrongly), and re-record the goldens with PNGs under `build/host/f2-74/`.
2. **Safe removal.** A tray action (icon of the removable device, context menu "Safely remove <name>" and the same entry in the file manager) that flushes the volume's pending writes, publishes its metadata, unmounts it, releases the USB device (storage stage teardown, port kept enumerated) and shows the "safe to remove" notification; while a file on the device is open the action refuses with the list of programs; the kernel side exposes one request through the existing storage/devices interface (`src/kernel/fs/mount.c`, `src/kernel/usb/usb_storage.c`, `src/kernel/core/devices.c`) with the reason records; for the CD the action ejects the tray through the ATAPI driver after unmounting. A yanked device (removed without the action) still produces the existing warning and the volume repair at the next insertion.
3. Host tests: replay of attach → write → safe removal → detach, refusal with an open file, CD eject; kernel host tests for the flush/unmount/release sequence and the ATAPI eject.

## Acceptance

Desktop suite, gate and kernel host suite green; QEMU `f4-media` cases (stick and CD) extended with the safe-removal action; the owner judges the popup and the action on the T23.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; plain prose, no names of other operating systems. Report `build/codex/f2-74-report.md`.
