# CiukiOS status — 1 October 2026

**pre-Alpha 0.8.0 · development image · QEMU-qualified scope**

CiukiOS boots its own DOS kernel and a graphical desktop from FAT16. M4 gives
each desktop DOS window a forked V86 machine with private memory, video and
input. QEMU tests the image on the host; CiukiOS does not run QEMU inside
itself. Physical PCs and general Windows compatibility remain unqualified.

## What works

| Area | Current result |
| --- | --- |
| M4 DOS windows | DOOM runs while Files stays open; two DOS windows have independent focus and close. Task Manager can inspect and end VMs. [M4 evidence](validation/2026-09-30-m4/README.md). |
| Desktop and files | Files, CiukNote, CiukPaint, Recycle Bin, Control Panel, desktop folders, VFAT long names, fonts and settings. [Long-name design](long-file-names-2026-09-30.md). |
| Network and drivers | IPv4/DHCP and advanced adapter controls, FTP, selected hardware-gated packet drivers, and a simple HTTP page in CiukWeb. [Network](network-settings-2026-10-01.md) · [drivers](../assets/drivers/README.md). |
| Native code and graphics | C/OpenWatcom `.APP` modules, DOS/4GW programs and a TinyGL software drawing demo. [Formats and limits](native-apps-and-opengl-2026-10-01.md). |
| Windows groundwork | CiukiOS Settings Registry, `C:\PROGRAMS` and free PE32 probes. PE32 programs are detected and rejected safely; they do not run. [Compatibility plan](windows-compatibility-and-layout-2026-09-30.md). |

The Application Library lists system applications and opens `C:\PROGRAMS` for
installed programs. `C:\DESKTOP\TestGames` groups DOS test launchers without
copying game data. The image and About panel report 0.8.0 and carry the
approved dedication to Ciuk.

## What was validated on the current image

The FAT16 image `build/full/ciukios-full.img` has SHA-256
`a1a2ab8d5dd1b1d304b8ea65739d93ec180984d4679ad7547a7dc50dfe17f042`.
The build succeeded and `fsck.fat -n` found a clean filesystem.

- **26/26 VM-window gates pass** on this exact image. A host reboot stopped
  the automated profile after 17 gates; the rest ran serially, one QEMU at a
  time. The checked-in [combined result](validation/2026-10-01-native-app-gl/serial-summary.json)
  is not one uninterrupted profile run.
- Focused QEMU gates pass live CiukPaint drawing, CiukWeb HTTP retrieval,
  the TinyGL triangle and visible cursor movement during DOOM level 1.
  The cursor check measured 0.185 s with DOOM active versus 0.058 s on the
  idle desktop on that host. [Details](validation/2026-10-01-native-app-gl/README.md).
- The 36-icon loader/sprite suite passed. Long-name, driver-pack, registry,
  desktop-polish and advanced-network gates passed on earlier integration
  images; they have not all been rerun together on this current image.

These are QEMU observations, not physical-hardware performance claims.

## What remains open

| Priority | Work |
| --- | --- |
| DOS compatibility and M5 | Wolf4GW can lose mouse input and resist normal close in a window. Broader protected-mode concurrency, Phase 6's external DOS corpus/full-CD matrix and Phase 7 audio coverage remain open. |
| Windows 95/98 | No PE32 loader, DLLs, Win32 API bridge, GUI stack, installer service or game API layer. Windows 3.1 was removed from the current image. |
| Third-party native apps | `.APP` modules still register at build time. An installable manifest, loader, SDK and independent sample are needed. |
| Browser and graphics | CiukWeb handles simple HTTP text/links via `HTGET`; HTTPS, native TCP, images, CSS and JavaScript are open. TinyGL is a static software subset, without guest GPU acceleration or full OpenGL support. |
| Devices and release | More licensed drivers, CD redirector and native USB hot-plug, safe migration of old root entries, a bounded full regression and physical-PC trials. Files is near its 64 KiB module-data limit. |

The next implementation order is the Wolf4GW/M5 lifecycle defect, a stable
native `.APP` contract and TCP service, then staged Win32 probes and wider
hardware/graphics testing. This remains a development snapshot, not a
finished release. The [roadmap](../Roadmap.md) and [changelog](../CHANGELOG.md)
retain the longer historical record.
