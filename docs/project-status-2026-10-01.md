# CiukiOS project status — 1 October 2026

The current development version is **pre-Alpha 0.8.0**. It boots a CiukiOS
DOS kernel and a graphical desktop from a FAT16 image. M4 is implemented for
the tested QEMU scope: a desktop DOS window runs its program in a separate
forked V86 machine, with private DOS memory, virtual video and input, and
focus-dependent audio. QEMU runs on the development host to test the image;
CiukiOS does not run QEMU inside the guest. This is a development build, not
a claim of broad PC or Windows compatibility.

## Working today

| Area | Implemented and demonstrated | Scope of the evidence |
| --- | --- | --- |
| Desktop and M4 | Files can stay open while DOOM runs in a DOS window; two DOS windows receive independent focus and close; Task Manager can inspect and end VMs. | The [M4 record](validation/2026-09-30-m4/README.md) and the [current-image gates](validation/2026-10-01-native-app-gl/README.md) are QEMU results. |
| Files and identity | VFAT long names, desktop folders, Recycle Bin, context menus, font/settings applets, the approved Ciuki portrait and Tango icons are integrated. | The [long-name design](long-file-names-2026-09-30.md) and earlier focused gates cover those features. |
| Native applications | CiukNote, CiukPaint, Files, Task Manager, Control Panel and CiukWeb are C/OpenWatcom `.APP` modules. A held CiukPaint stroke is visible before mouse release. | The [native application record](native-apps-and-opengl-2026-10-01.md) and focused QEMU reports cover the latest changes. |
| Network and devices | IPv4/DHCP and advanced adapter settings, a hardware-gated DOS driver catalog, packet networking and a simple HTTP page in CiukWeb. | The [network](network-settings-2026-10-01.md) and [driver catalog](../assets/drivers/README.md) state supported devices and exceptions. |
| Graphics API | A TinyGL-based static library and DOS/4GW demo draw a coloured triangle inside an M4 window. | This is software rendering of an OpenGL-style subset, without guest GPU acceleration or full OpenGL conformance. |
| Windows preparation | A persistent CiukiOS Settings Registry, a `C:\PROGRAMS` destination and free PE32 probes exist. | The [compatibility record](windows-compatibility-and-layout-2026-09-30.md) confirms that PE32 programs are detected and rejected safely, not executed. |

The Application Library lists system applications and opens installed programs
from `C:\PROGRAMS`. `C:\DESKTOP\TestGames` has launchers for the existing DOS
game tests without duplicating their data. The image's project version and
About panel report 0.8.0 and the approved dedication to Ciuk.

## Current image and validation boundary

The current FAT16 image is `build/full/ciukios-full.img`, SHA-256
`a1a2ab8d5dd1b1d304b8ea65739d93ec180984d4679ad7547a7dc50dfe17f042`.
The final build succeeded and `fsck.fat -n` reported a clean filesystem.
The 26 VM-window profile gates have each passed on this exact image. A host
reboot interrupted the automated profile after 17 gates, so the remaining
gates were run serially with one QEMU process at a time. The checked-in
[combined result](validation/2026-10-01-native-app-gl/serial-summary.json)
lists each gate and its evidence path. It is **26/26 combined gate results**,
not one uninterrupted invocation of the profile script.

Focused QEMU gates on this image also passed live CiukPaint drawing, CiukWeb
retrieving and rendering an HTTP page, the software OpenGL triangle, and
visible cursor movement during DOOM level 1. The DOOM cursor check measured
0.185 s with the game active versus 0.058 s on the desktop in its QEMU run;
those numbers are host-specific, not physical-hardware performance claims.
The 36-icon loader/sprite suite passed after the palette was stabilized.

The long-name, driver-pack, registry, desktop-polish and advanced-network
feature gates passed on their earlier integration images. They have not all
been repeated as one suite on the current image. Keep their dated validation
records separate from the current 26 VM-window gates.

## Open work and limits

1. **DOS compatibility and M5:** Wolf4GW reaches its sign-on screen in an M4
   window but can lose desktop mouse input and resist normal close. M5 still
   needs broader protected-mode concurrency and lifecycle qualification.
   Phase 6's external DOS corpus and full-CD matrix are incomplete; Phase 7
   audio compatibility is not closed.
2. **Windows 95/98 applications and installers:** No PE32 loader, Win32 DLL
   runtime, registry API bridge, GUI/GDI stack, installer service or game API
   layer exists. The current registry file and PE detector are groundwork.
   Windows 3.1 support was removed from the current image in September.
3. **Third-party native programs:** C/OpenWatcom is the chosen language, but
   `.APP` modules are still registered at build time. A stable installable
   module manifest, loader, SDK and independent sample application are open.
4. **Browser and graphics:** CiukWeb renders simple HTTP text and absolute
   links through the bundled mTCP `HTGET.EXE`; it has no native TCP service,
   HTTPS, CSS, JavaScript or image rendering. TinyGL is a static software
   subset; modern OpenGL and guest GPU acceleration are open.
5. **Hardware and storage:** The driver catalog covers selected PCI packet
   adapters, with several entries disabled or unverified. CD redirector and
   native USB hot-plug support remain open. Files is close to its 64 KiB
   module-data ceiling, and some old root directories still await a safe
   migration. No 2004-era physical PC has been qualified by these QEMU runs.
6. **Release discipline:** The 0.8.0 work is a development snapshot, not a
   finished release. A serial, resource-bounded complete regression on a
   stable host, redistributable-media audit, broader DOS corpus and real-PC
   testing remain before a release claim.

## Next order of work

First isolate the Wolf4GW input/close failure and extend M5's VM lifecycle
tests. Then publish an installable native `.APP` contract and move CiukWeb's
HTTP transport into a native network service. Treat Win32 execution as a
separate staged compatibility project, beginning with the two free PE probes;
do not equate recognizing a `.EXE` with running it. Expand graphics and
hardware support against named free workloads and physical test machines.

For the dated implementation history and screenshots, see the
[README](../README.md), [changelog](../CHANGELOG.md) and
[milestone ledger](current-milestones.md).
