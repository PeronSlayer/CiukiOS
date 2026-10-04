# CiukiOS 0.8.3 status — 4 October 2026

This is the current status page; dated earlier reports remain historical evidence.
The active profile is the full FAT16 HDD image and its sanitized Windows bundle.

## Changes in this release

| Area | Implementation | Validation |
| --- | --- | --- |
| Presentation | Retained VirtIO staging, actual pixel damage, independent VM clocks; eliminate the desktop's periodic full-scene repaint. | 73 GPU protocol checks pass. Doom new game/exit pass; 34.78 loops/s, residual pacing spikes remain. |
| Mouse | IntelliMouse negotiation, physical and virtual four-byte PS/2 packets, INT 33h wheel ABI and native window scrolling. | 1,285 peripheral-model assertions pass with ASan/UBSan; QEMU Files scroll down/up pixel check passes. |
| Display | Native graphical preview, 12-second rollback, transactional profile save, adapter/driver/EDID tabs. | 32-bit timeout, 16-bit banked rollback, confirmation and saved mode across reset pass. |
| Network/Web | Automatic resident bridge startup behind a detected packet driver; cooperative native IPv4/DNS/TCP/HTTP browsing. | Real example.com DNS/HTTP 200 and reload pass without a DOS launch. |
| Desktop | About replaces the startup library; persistent startup checkbox; fitted active/inactive titles. | Startup preference survives reset; About reopens at 640×480; screenshots record rendered titles. |

## Boundaries

Original DOS Doom runs its software renderer on the guest CPU at a nominal
35 game tics per second. Average rate alone does not establish smooth frame
spacing. The October 4 baseline reproduced software presentation gaps above
100 ms while the game averaged 34.78 loop starts/s. Measurements distinguish
engine timing, capture, GPU commands and actual monitor presentation.
At matched 800×600, the desktop-damage correction reduced submission gaps over
42 ms from six in the earlier 9.9-second sample to one and two in the two corrected
samples. The final maximum remained 67.73 ms, so zero visual stutter is not claimed.

VirtIO uses the 2D resource protocol. Host GL presents its texture; there is
no general guest accelerated OpenGL/Direct3D implementation. ATI/NVIDIA base
backends support a checked list of older adapters; detection is not physical
hardware qualification. HTTPS/TLS, modern web rendering, general PE32/Win32
execution and broader DOS compatibility remain work in progress.

All runtime evidence in this release is Linux QEMU/KVM on the supported
single-vCPU profile. Windows portable packaging is checked for integrity and
absence of private payloads; Windows execution is not claimed.

## Evidence

- [Final runtime results and cadence](validation/2026-10-04-release-runtime/README.md)
- [Actual release screenshots](screenshots/0.8.3/README.md)
- [Graphical transitions and presentation](validation/2026-10-04-desktop-transitions.md)
- [Mouse wheel](validation/2026-10-04-mouse-wheel.md)
- [Startup and window titles](validation/2026-10-04-desktop-startup.md)
- [Web and network](validation/2026-10-04-web-network.md)
- [Native GPU architecture](validation/2026-10-03-native-gpu-presentation.md)
- [Build and run](build-and-run.md) · [Windows release](windows-portable-release.md)
