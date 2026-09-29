# Roadmap: DOS applications in windows, Windows 95/98 style — 28 September 2026

Goal set by the owner: CiukiOS is a standalone operating system. Its desktop
runs DOS applications natively in windows, as Windows 95/98 do, with several
DOS windows at once, plus a control panel, an Explorer-style file manager and
OpenGL. Windows 3.1 is removed.

Principle, as in Windows 95: the virtual machine manager (Jemm V86 monitor +
CVSESSION) is resident from boot. The desktop is the system VM. Every DOS
program is a VM in its own window with the virtual VGA, keyboard, mouse,
Sound Blaster 16/OPL3 and a DPMI host. Nothing is started by hand.

## Phase 1 — resident VM manager, seamless single DOS window

1. Remove Windows 3.1 from the build, the desktop and the tests.
2. Load Jemm386 + CVSESSION automatically at startup.
3. Keep the desktop fast in V86: CVSESSION maps the desktop's VBE bank window
   onto the linear framebuffer (the host-mode mapping, made permanent), so
   no bank switch reaches the firmware.
4. Any DOS program started from the desktop (Run, Programs, Games, a file)
   opens in the DOS window with the device model. The window always has a
   session-bound DPMI host, so DOS/4GW games need no launcher.
5. Alt+Enter switches window/full screen; closing a running program asks
   first.
6. Everything is in the main build; the complete test profile is the gate.

## Phase 2 — several DOS windows at once

One V86 context today. Needed:
- per-VM first-megabyte page tables (and HMA);
- per-VM virtual devices, PIC state and DPMI host;
- a time-slicing scheduler in the monitor;
- one DOSWIN window per VM;
- focus routing of keyboard, mouse and audio mixing across VMs.

This is a VMM redesign, planned in its own
[record](design-multi-vm-2026-09-28.md) with milestones M1-M5.

## Phase 3 — Control Panel

A separate application launched from the desktop (SHELL.COM has no room),
with:
- Devices: detected PCI/ISA devices and what drives them;
- Drivers: enable/disable/configure installed drivers;
- Audio: an AC'97 master/PCM/line mixer and the virtual SB16 mixer;
- Display: resolution, colour depth, refresh, wallpaper.

## Phase 4 — File manager

Explorer-style application:
- folder tree and file list (icons, details), address bar;
- copy/move/delete/rename, new folder, drag and drop, properties;
- open with the registered program; DOS programs open in a window.

## Phase 5 — OpenGL

There is no 3D-accelerated GPU path on QEMU or on the target machines, so
OpenGL is software rendering: Mesa built for 32-bit DOS clients, exposed as
a system library, with conformance tests. Complete API coverage is the goal;
speed is bounded by the CPU.

## Status

- 2026-09-28: DOS/4GW games with devices in the DOS window (launcher based)
  — [record](vm-dpmi-window-devices-2026-09-28.md). Phase 1 started.
- 2026-09-28: Phase 1 items 1-4 and 6 done and validated. The main build
  starts the VM manager at boot, the desktop opens DOS programs in a window
  by itself, and Windows 3.1 is gone. Profile: 20/20 —
  [evidence](validation/2026-09-28-desktop-dos/README.md). Item 5
  (Alt+Enter, close confirmation) is open.
- 2026-09-28: Phase 2 M1 (pre-emptive VMs, also while one waits for a key
  inside DOS) and M2 (file I/O from two VMs at once) pass on QEMU —
  [evidence](validation/2026-09-28-multi-vm/README.md). M3-M5 are open.
- 2026-09-29: Phase 2 M3 in progress (not complete). The DOS window session
  belongs to one VM, with per-VM hooks, video aperture and interrupts, and
  keyboard focus between the VMs; this part passes its gate. Still missing:
  SB DMA of a non-running VM, a clean IVT for forked VMs, clocks, kill and a
  focus hotkey (see the design record).
  The boot shows the splash and then the desktop, with the startup melody
  played by the desktop and no text screen. Profile 21/21 —
  [evidence](validation/2026-09-29-boot-and-m3/README.md). The disk-BIOS
  stack fixture still fails (see the record).
- 2026-09-29 (later): Phase 2 M3 complete on QEMU. Several VMs each own a
  DOS window session (their own VGA and device model). Keyboard and mouse
  follow the focus, and the AC'97 stream plays the focus VM's sound while
  the other sessions play muted in real time. Each VM has its own copy of
  the BIOS RAM in upper memory. Profile 21/21, VM gate 10/10 parts —
  [evidence](validation/2026-09-29-boot-and-m3/README.md), section 5. M4
  (the desktop paints each VM's session in its window) and M5 are open.
- 2026-09-29 (later): the kernel runs the disk BIOS on its own stack, so the
  firmware stack fixture passes; the suspected AH=48h overlap is not
  reproducible. Both are in the profile (23/23) — same record, section 6.
