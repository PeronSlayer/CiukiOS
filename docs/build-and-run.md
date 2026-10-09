# Build and run CiukiOS

This guide is for the **pre-Alpha 0.8.3** development image on Linux. The
main build creates a FAT16 disk image at `build/full/ciukios-full.img`.
QEMU is the external test machine; it is not part of CiukiOS.
All canonical GUI, recording and headless launch profiles attach an NE2000
adapter to QEMU user NAT by default. CiukiOS has outbound Internet access
when the host does; the boot sequence starts the resident network service
automatically after detecting its packet driver. Set `QEMU_NETWORK_MODE=off` only for an explicit
isolated run. Headless smoke runs omit host FTP port forwarding by default.
The full HDD and CD runners also attach the QEMU audio devices by default;
the automated full-display profile records their output to WAV. The CD runner
uses the same outbound NAT default. The standalone floppy
profile is retired from ongoing main-branch builds and tests.

## Requirements

| Needed for | Tools |
| --- | --- |
| Main build | `nasm`, `mtools`, `make`, `patch`, `ffmpeg`, a host C/C++ toolchain and the `ia16-elf-gcc`, `ia16-elf-ld`, `ia16-elf-objcopy` cross-tools. |
| Desktop modules | OpenWatcom under `/opt/watcom`, or set `WATCOM` to its installation directory. |
| HTTPS/JavaScript worker | DJGPP cross compiler `i586-pc-msdosdjgpp-gcc` (GCC 12.2.0 tested), installed under `build/external/djgpp`, or set `CIUKIOS_DJGPP_ROOT` / `DJGPP_CC`. |
| Build scripts | Python 3.12+ with Pillow. The selected development environment uses Python 3.14. |
| Boot and recording | `qemu-system-i386` or `qemu-system-x86_64`, KVM access; recording also needs GTK, X11/XWayland and `xdotool`. |
| Live/install CD | `xorriso`; Syslinux BIOS files support the diagnostic ISO path. |

Verified downloads for optional Costa and the mTCP/Crynwr network stack are
prepared by `scripts/build_run_full.sh`. Commercial DOS game data and other
proprietary third-party binaries are not in the repository; keep local copies
untracked. A base image can be built without them.

The full build compiles `WEBWORK.EXE` from the vendored BearSSL and MicroQuickJS
sources, and installs the pinned Mozilla-derived CA bundle with its notices.
The HDD/CD and portable Windows launchers attach a transitional VirtIO RNG for
TLS entropy. HTTPS fails closed without that source, a valid UTC guest clock,
or a matching trusted certificate. The current protocol is TLS 1.2; physical
machines need a supported entropy source before HTTPS is available. Browser
HTML, CSS and DOM limits are listed in the current status.

## Windows portable ZIP

The canonical full build also creates
`build/releases/CiukiOS-0.8.3-Windows-portable.zip`. Extract it on 64-bit
Windows and run `Start-CiukiOS.cmd`; the archive contains QEMU, its required
Windows files, the FAT16 disk, and a short README. The packager removes local
commercial game payloads and their shortcuts from its image copy. It checks
the ZIP structure here; the Windows launcher has not been run on Windows.
Run `python3 scripts/package_windows_portable.py` to refresh the ZIP from an
already built full image. See [Windows portable release](windows-portable-release.md)
for provenance and the local push-to-release hook; GitHub Actions are not used.

## One-command desktop

```bash
bash scripts/build_run_full.sh
```

This fetches the verified optional packages, builds the image, verifies the
CiukiDOS loader/kernel boundary and opens QEMU. To use an image already
built:

```bash
bash scripts/qemu_run_full.sh --no-build
```

For a lighter host run, `QEMU_MEMORY_MB=128` selects 128 MiB of VM RAM. Keep
one QEMU process active at a time. `Ctrl+Alt+G` releases the host mouse grab;
shut down CiukiOS from the guest before closing QEMU.

## VirtIO GPU presentation (Linux/QEMU)

To use CiukiOS's native CVSESSION VirtIO 2D driver with QEMU VirGL host
presentation, launch the existing image explicitly:

```bash
QEMU_VIDEO_DEVICE=virtio-gl bash scripts/qemu_run_full.sh --no-build
```

The guest driver creates a 2D texture and uploads completed RAM-backed desktop
pages for host GL presentation. Doom still uses its CPU software renderer; this
path accelerates presentation, not Doom's rendering. The default VBE/standard
VGA profile remains available when VirtIO is disabled or unsupported. Compiled
base drivers for selected ATI Rage 128/Radeon R100–R200 and NVIDIA TNT/GeForce
through GeForce 4 preserve the firmware mode and program the scanout address;
their hardware 2D/3D engines are not implemented. See the native GPU validation record
for scope, sources and current results.

Keep builds and runtime tests sequential and resource-capped. The full build
uses one job, a 3 GiB memory cap and 1 GiB swap cap:

```bash
systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G \
  -p CPUQuota=100% -- env CIUKIOS_BUILD_JOBS=1 make build-full
```

The Linux runner automatically applies a 768 MiB memory cap, no swap, a two-core
CPU quota and at most 128 tasks. The VM still has one virtual CPU. The checked
Linux SDL/OpenGL run entered a new Doom game, turned, exited and released the
GPU for the DOS console. The final 0.8.3 run measured 34.78 game tics/render-loop
iterations per second, not physical display FPS. Its integrated wheel, web,
graphical mode-change and saved-preference checks passed; see the
runtime and pacing record.
Physical ATI/NVIDIA cards and Windows GL have
not been runtime-tested. These results do not establish zero visual stutter.

Open **Control Panel → Display** (or press F3 and enter `DISPLAY`) for screen
properties. Screen lists the available resolutions and colour depths. Apply
starts a 12-second preview: Enter keeps it, Esc or timeout restores the previous
setting. Close DOS windows before changing modes. Adapter, Monitor and Advanced
show the detected PCI device, EDID identity, active display driver and its
capabilities, with shortcuts to Device Manager and Drivers. In QEMU, the monitor
identity belongs to the virtual screen. Custom refresh timings and a guest 3D
API are not currently implemented. See the display settings record.

## Build or test a specific image

| Purpose | Command |
| --- | --- |
| Build FAT16 full image | `make build-full` |
| Check FAT16 structure | `fsck.fat -n build/full/ciukios-full.img` |
| Check generated icons | `python3 scripts/build_ui_icons.py --check` |
| Check generated fonts | `python3 scripts/build_fonts.py --check` |
| Build Live/install CD | `make build-full-cd` |

| Boot smoke test of the built image | `make qemu-test-full` |

Tests use the single canonical image and never rebuild it or copy it; see
[test architecture](design/test-architecture.md). The old focused QEMU gates
are archived in `legacy/CiukiOS-scripts-legacy-2026-10-09.zip`.

## Image profiles and guides

| Profile | Role |
| --- | --- |
| `full` | Main FAT16 development image and desktop. |
| `full-cd` | Live/install CD; also exercises `SETUP.COM`. |
| `floppy` | Minimal historical loader scaffold, not the main desktop. |

For network commands, FTP and CiukWeb, see network settings
and recording and web. For physical
hardware and release limits, start with the project status.
