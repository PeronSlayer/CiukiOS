# Build and run CiukiOS

This guide is for the **pre-Alpha 0.8.0** development image on Linux. The
main build creates a FAT16 disk image at `build/full/ciukios-full.img`.
QEMU is the external test machine; it is not part of CiukiOS.

## Requirements

| Needed for | Tools |
| --- | --- |
| Main build | `nasm`, `mtools`, `make`, `patch`, `ffmpeg`, a host C/C++ toolchain and the `ia16-elf-gcc`, `ia16-elf-ld`, `ia16-elf-objcopy` cross-tools. |
| Desktop modules | OpenWatcom under `/opt/watcom`, or set `WATCOM` to its installation directory. |
| Build scripts | Python 3.12+ with Pillow. The selected development environment uses Python 3.14. |
| Boot and recording | `qemu-system-i386` or `qemu-system-x86_64`, KVM access; recording also needs GTK, X11/XWayland and `xdotool`. |
| Live/install CD | `xorriso`; Syslinux BIOS files support the diagnostic ISO path. |

Verified downloads for optional Costa and the mTCP/Crynwr network stack are
prepared by `scripts/build_run_full.sh`. Commercial DOS game data and other
proprietary third-party binaries are not in the repository; keep local copies
untracked. A base image can be built without them.

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

## Record a fixed window

```bash
bash scripts/qemu_record_full.sh
```

The default client area is 1280×800. `--size 1440x900` changes that size;
`--software-display` disables **host** OpenGL presentation. Neither setting
gives the CiukiOS guest GPU acceleration. See [recording and first web page](qemu-recording-and-web-2026-10-01.md).

## Build or test a specific image

| Purpose | Command |
| --- | --- |
| Build FAT16 full image | `bash scripts/build_full.sh` |
| Check FAT16 structure | `fsck.fat -n build/full/ciukios-full.img` |
| Check generated icons | `python3 scripts/build_ui_icons.py --check` |
| Check generated fonts | `python3 scripts/build_fonts.py --check` |
| Build Live/install CD | `make build-full-cd` |

Run focused QEMU gates **sequentially**, each with a fresh `--output` directory.
For example:

```bash
python3 scripts/qemu_test_ciukpaint.py \
  --image build/full/ciukios-full.img \
  --output build/tests/local-ciukpaint
```

The full `scripts/test_vm_window_profile.sh` runs some gates concurrently
and puts much more load on the host. Use it on a dedicated, stable test host;
the [current validation record](validation/2026-10-01-native-app-gl/README.md)
explains the last serial recovery run and its exact scope.

## Image profiles and guides

| Profile | Role |
| --- | --- |
| `full` | Main FAT16 development image and desktop. |
| `full-cd` | Live/install CD; also exercises `SETUP.COM`. |
| `floppy` | Minimal historical loader scaffold, not the main desktop. |

For network commands, FTP and CiukWeb, see [network settings](network-settings-2026-10-01.md)
and [recording and web](qemu-recording-and-web-2026-10-01.md). For physical
hardware and release limits, start with the [project status](project-status-2026-10-01.md).
