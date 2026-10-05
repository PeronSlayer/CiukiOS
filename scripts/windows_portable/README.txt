CiukiOS 0.8.3 - Windows portable preview
========================================

1. Extract the entire ZIP to a writable folder on 64-bit Windows.
2. Choose your preferred startup script:
   - Start-CiukiOS.cmd: Interactive launcher (menu with automatic fallback to VGA).
   - Start-CiukiOS-VGA.cmd: Failsafe standard VGA mode (universal compatibility).
   - Start-CiukiOS-VirtIO.cmd: VirtIO-GPU mode (direct 2D presentation).
   No external QEMU installation is needed; all required binaries are bundled.
3. The disk image is writable; keep a copy of CiukiOS.img if you want to
   preserve your files before replacing this package with a newer release.

The window is configured for 1280 x 800 pixels with SDL display output. Ctrl+Alt+G
releases the mouse from QEMU. Use the CiukiOS shutdown command before closing the
window.
The emulated NE2000 adapter has outbound Internet access through QEMU user
networking when the Windows host is online. Start the guest network service
with NETSTART. This does not expose a server running in the guest to the host.

QEMU runs on Windows as the host emulator. It is not part of CiukiOS and
does not emulate DOS applications inside CiukiOS. Guest GPU acceleration and
Windows 95/98 program compatibility are not provided by this package.

This Windows launcher package provides both standard VGA and VirtIO modes.
The main full image is tested with QEMU on Linux; package structure and ZIP
integrity are checked during packaging. The locally supplied commercial DOS game
payloads and their desktop shortcuts are removed from this release image.

CiukiOS project: https://github.com/PeronSlayer/CiukiOS
QEMU project and Windows build: https://www.qemu.org/download/
QEMU Windows installer source: https://qemu.weilnetz.de/w64/2026/
Project and third-party notices are included in LICENSE.txt,
ICON-NOTICES.txt, DRIVER-NOTICES.txt, and qemu/COPYING*.
Exact package provenance and hashes are in MANIFEST.json.
