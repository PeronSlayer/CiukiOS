# CiukiOS 1.0 Beta Release Notes

- **Version:** 1.0.2 beta — unreleased.
- **Date:** 2026-10-29.
- **Release channel:** beta; tags use `v1.0.2-beta.<n>`.
- **Architecture:** i686 PCs with CMOV, BIOS/E820, VBE 2.0+ video, parallel ATA and PS/2 input; late-1990s and early-2000s desktops and laptops. Current acceptance requires at least 128 MiB RAM; VM packages use 512 MiB.
- **Media:** one canonical 512 MiB FAT32 disk image, a Setup CD ISO and VM packages.
- **Qualification:** publication requires the release image to pass automated acceptance and the unattended self-test on the reference QEMU profile. Those results and the approved SHA-256 must be recorded at release time. These notes do not certify an image or completion of hardware qualification.

## Overview and contents

CiukiOS is a modern retro gaming OS dedicated to Ciuki. *Classic computing, one woof at a time.* It combines a 32-bit protected-mode kernel, Windows 98/ME-class resource management, native applications, DOS virtual machines and a compatible Win32 API. It is intended for classic PC software; compatibility depends on the program and hardware.

### User security and accounts

The beta introduces kernel-enforced Administrator, Standard user and Guest roles, inherited process credentials, private profiles and file ownership. Native applications, Win32 programs and DOS VMs retain their user's authority. A SYSTEM account service manages password verifiers and account transactions; separate services mediate privileged disk tools, machine settings and registry operations.

Ready disk and VM images automatically log on as **CiukiUser**, an administrator without a password. Set its password before enabling another account in Control Panel > User Accounts; setting that password permanently retires the bootstrap automatic-logon path. Guest is disabled by default. The hidden **CiukiAdministrator** recovery account is used in Safe Mode; on ready images it cannot authenticate until an administrator sets its password.

Setup installations create a named first administrator instead of CiukiUser and require a recovery password for CiukiAdministrator. Welcome logon, Log Off, Ctrl+Alt+Del, account pictures, password changes, screen-saver locks and resume locks are included. Power timers and logs follow the active profile.

### Windows programs and installers

The Win32 subsystem includes dialogs, controls, file and registry APIs, shell shortcuts, media playback and reused Wine 10.0 components. Installer wizard painting, window lifetime and focus are improved. Unresponsive programs and shutdown use bounded waits with End Task and Force choices.

Supported Windows CD AutoPlay entries can start installers. A native bootstrap handles supported InstallShield 5 NE stubs without requiring the deferred Win16 subsystem. GDI paths and metafiles, serial communication and game-port joystick interfaces expand compatibility. The Wine component set now includes DirectPlay, oledlg and wininet; WinINet TLS is unavailable.

### OpenGL and graphics

Software OpenGL supports the Quake III Arena demo's rendering path, including multitexturing, depth/stencil and full-screen presentation. Native SDK OpenGL and Win32 OpenGL share the rendering engine.

A common 3D service provides guarded command validation, resource accounting and software fallback. Probe-gated backends cover S3 Savage, ATI Radeon R100/R200, ATI Mach64, Matrox G200/G400 and VMware SVGA3D. Admission depends on the adapter, mode and state. NVIDIA NV04–NV20 uses software rendering. Matrox's incomplete deeper mipmapping sends Quake III to software. Background qualification and VRAM-resident targets improve the Savage path; BIOS VBE mode setting and palette/state restoration improve full-screen transitions.

### Networking and browser

lwIP provides TCP/IP, DHCP and DNS; NetSurf 3.11 provides a graphical browser with TLS 1.2 and JavaScript enabled through Duktape. New CSS support includes custom properties, modern colours, sRGB color-mix() and flexbox gaps; image reuse and layout scheduling reduce rendering work.

Win32 IPX/SPX and DOS networking share the kernel IPX hub. A frame-type setting supports legacy LAN framing, and Wine DirectPlay provides TCP/IP and IPX providers and lobby support. Multi-guest LAN tests exist; their presence does not certify every game or physical network card.

### DOS games, sound and desktop

The unified FreeDOS-based DOS kernel supplies XMS, EMS, upper memory, Sound Blaster/OPL emulation and a virtual network adapter. DOS games start full screen for graphics by default; Alt+Enter returns to a window, and screen choices are remembered per program. Mouse callbacks, IRQ timing, windowed presentation and full-screen audio paths are improved.

Bundled titles include approved shareware/demo distributions of DOOM, Quake, Wolfenstein 3-D, Warcraft II and Duke Nukem 3D, plus the Quake III Arena demo installer. The larger DOS acceptance catalogue contains separate test-media entries: it is not a list of bundled or redistribution-approved games. Sound setup uses the VM's BLASTER configuration, including IRQ 7.

The desktop includes Files, Control Panel, WordPad, Paint, Music, Classic Games, Clock, Task Manager, CiukIDE and System Information. Application launch feedback, executable and shortcut icons, persistent empty-drive letters, display preference recovery and ACPI soft-off fallback are improved. Tango remains the official icon family and the approved Ciuki portrait remains the system identity.

The original Half-Life installer, main menu and t0a0 map run in the host runtime harness through the Win32 libraries with software and OpenGL renderers. This is host evidence; it does not certify the release image on hardware. **Half-Life and commercial game data are not included.**

## Installation and VM packages

Verify `SHA256SUMS` before use. Packages are named `ciukios-1.0.2-*`; use the files attached to the same beta tag. Back up any destination disk before installation or raw writing, which can erase its contents.

### Setup CD

Burn `ciukios-1.0.2-setup.iso` to a CD, or attach it as an IDE CD-ROM in a BIOS VM, then boot the disc and follow Setup. Select the destination disk and components, and create the required administrator and recovery passwords. The CD carries system services, default machine registry hives, required sources and licence notices.

The ISO uses El Torito hard-disk emulation. It is **not a hybrid USB image**; use the raw disk package for direct USB/disk writing.

### Raw disk image

Extract `ciukios-1.0.2-disk.zip` and write `ciukios.img` to a whole destination disk using Rufus in DD mode, balenaEtcher or `dd`. This overwrites the destination. For development and readback verification, follow [the physical-machine procedure](https://github.com/PeronSlayer/CiukiOS/blob/main/docs/build-and-run.md#physical-machines).

The ready image uses the CiukiUser account described above. Device support depends on the detected controller; one image serves desktops and laptops.

### Virtual machines

- **QEMU:** extract `ciukios-1.0.2-qemu.zip` and run `run-qemu.sh` or `run-qemu.cmd`. Install QEMU separately. Launchers prefer host acceleration and retry with TCG when needed; Apple Silicon uses x86 TCG.
- **VirtualBox:** extract `ciukios-1.0.2-virtualbox.zip` and run `create-vm.sh` or `create-vm.cmd`, with VirtualBox installed. The package uses a VDI, BIOS, IDE PIIX4, ICH AC97, PCnet-FAST III, VBoxVGA, 512 MiB and one CPU.
- **VMware:** extract `ciukios-1.0.2-vmware.zip` and open `ciukios.vmx`. It uses an IDE VMDK, ES1371 sound, E1000 NAT and VMware SVGA.
- **OVA:** import `ciukios-1.0.2-vm.ova` as an alternative. Inspect the imported configuration: BIOS, IDE, supported audio/video, NAT, 512 MiB and one CPU. Importers can map device hints differently.

Conversion and integrity tests do not establish Windows/macOS launcher or VirtualBox/VMware runtime qualification. Consult the evidence attached to the published release.

## Known limitations

- Hardware qualification remains incomplete. The beta is a prerelease; adapter support, installers and applications need qualification on the actual configuration. Use the unattended self-test boot to collect hardware diagnostics.
- Accelerated 3D requires successful qualification and supported states. Software fallback may be slow. NVIDIA 3D is software-only and Matrox cannot accelerate the full Quake III state set.
- Direct3D through wined3d and the remaining Win16 subsystem are deferred until after the beta. Supporting an InstallShield 5 NE bootstrap does not imply general Win16 compatibility.
- The Win32 API and device support remain incomplete. WinINet has no TLS; NetSurf uses its separate BearSSL TLS 1.2 implementation.
- NetSurf is an ES5-era browser with partial modern CSS support. Modern JavaScript applications and some current websites will not work.
- NTFS support is read-only; volumes requiring journal replay or with active hibernation are refused. Installation uses FAT32.
- LAN acceptance cases, host runtime harnesses and merged GPU drivers are separate from physical-hardware qualification. No claim of universal game or network compatibility is made.

## Licences and corresponding source

CiukiOS kernel code is GPL-2.0, the unified DOS fork is GPL-2.0-or-later under its recorded grant, and the SDK has its own MIT licence. Component versions, licences and notices are listed in [Credits](https://github.com/PeronSlayer/CiukiOS/blob/main/docs/credits.md). Tango 0.8.90 is Public Domain; the Ciuki portrait is a separate project asset.

### Wine 10.0 LGPL source offer

The reused Wine 10.0 libraries are LGPL-2.1-or-later, without warranty. Recipients may modify, rebuild and replace them under that licence. Complete corresponding source accompanies each release in `ciukios-1.0.2-sources.zip` and the Setup CD's `SOURCES` directory: the exact Wine archive (`WINE100.TXZ`), CiukiOS patches, pins, recipes and helper sources (`CIUKIOS.ZIP`). Rebuild with `make upper`; see `third_party/wine/SOURCE-OFFER.txt` for component-specific recipes. Notices accompany the release in `ciukios-1.0.2-notices.zip` and the CD's `LICENSES` directory. These sources must be available from the same release location; upstream links alone do not replace them. See [Wine 10.0's LGPL text, sections 4 and 6](https://github.com/wine-mirror/wine/blob/wine-10.0/COPYING.LIB).

### Game data policy

**Commercial game data is never included.** Release media may contain only approved shareware, demo and freeware distributions with their required notices and redistribution terms. A demo label alone is not permission to redistribute. Test-media catalogue entries are excluded unless separately approved for bundling. Users supply their own lawfully obtained commercial software; private test media and keys are excluded from public packages and sources.

## Release evidence

The release manifest and notes record the approved image SHA-256, source commit and approval record. Acceptance and self-test results must refer to that image before publication. See the published release assets for the final evidence; no beta image is qualified by this document alone.
