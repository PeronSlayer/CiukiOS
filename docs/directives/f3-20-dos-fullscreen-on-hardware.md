# f3-20 — Full-screen DOS sessions on the real VGA hardware (Windows 98 two-mode model; owner approval 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f3-20` from `main`. Host-testable; the lead runs QEMU (standard VGA and Cirrus) and the T23.

## Input

`docs/design/research/dos-vm-video-2026-10-14.md` sections 2, 4 and 5 (the Windows 98 model: full-screen DOS session driving the real VGA untrapped, windowed emulation otherwise; hotkeys; what it takes in CiukiOS), `docs/design/device-firmware-ownership.md` (lines 240-260: DOS VMs must not invoke host firmware; `biosvm_video_switch` accepts only VBE modes in `src/kernel/vm/biosvm.c:925`), the desktop display lease (`docs/desktop.md`, `apps/desktop/*`, the display service and `grants.c`), f3-19's windowed fast paths (keep them for the windowed mode), the owner's decision recorded in `dev_diary/2026-10-14-08-decisioni-notte-f5.md`.

## Task

1. **Amend `device-firmware-ownership.md`** (dated subsection): a DOS session in full-screen mode MAY call the host video BIOS through the mediated BIOS VM for standard VGA modes (13h, text modes 03h/01h, Mode X/Y set through direct VGA register programming by the guest) while the desktop's display lease is released; the kernel saves the complete VGA/VESA state before and restores it after; VBE modes in full screen are a later amendment; IOPL stays zero for ports not in the full-screen allowlist (the VGA port ranges 3B0h-3DFh and the A0000h-BFFFFh window are granted directly during full screen).
2. **Kernel**: a `dosvm_fullscreen_enter/leave` service: the desktop releases the lease (existing revocation path), the VM's VGA model state is pushed to the real hardware (registers, palette, memory image) and the A000 window and VGA ports are mapped/granted untrapped; on leave, the hardware state is captured back into the model (so the window shows the game's last frame), the desktop lease is restored and its surface repainted. Keyboard hotkeys handled by the kernel's input path regardless of the guest: Alt+Enter toggles, Alt+Tab returns to the desktop (full screen suspended, VM keeps running or pauses per a setting), Ctrl+Alt+Del untouched. Mouse: PS/2 and USB mouse reports routed to the VM's INT 33h path in full screen.
3. **Desktop**: a "Full screen" command in the DOS window's menu and the hotkey; the window shows "Running full screen — Alt+Enter to return" while released; the taskbar entry survives.
4. **Safety**: a watchdog returns to the desktop and restores the display if the VM exits, faults or hangs in full screen; the Savage/VESA state restore must leave the panel exactly as before (QEMU screendump equality before/after; T23 by eye and by the display records).
5. Host tests: the state save/restore model (VGA registers, DAC, memory), the lease hand-over sequence with the desktop harness, the hotkey routing; a QEMU case `vm-fullscreen-qemu-t23` (enter, run DOOM's demo, leave, screendump equality) in `f3-vm`.

## Acceptance

Kernel and desktop host suites green; the lead runs `f3-vm` (new case) on QEMU, then the T23: DOOM and Wolf3D full screen at native speed, Alt+Enter back to the window, panel restored. Files: `src/kernel/vm/*` (new fullscreen module; do not change f3-19's windowed paths), `biosvm.c`, display/grants, `apps/desktop` DOS window, tests.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research first (`--search`), record links and decisions in the relevant `docs/design/` document (append dated subsections; never rewrite earlier text), then implement and test. Report `build/codex/f3-20-report.md`: findings, changes, host tests, what the lead must verify on QEMU and the T23.
