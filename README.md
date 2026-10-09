![CiukiOS boot splash](misc/CiukiOS_SplashScreen.png)

# CiukiOS

**A modern Retro OS** — an x86 operating system for late-1990s PCs, dedicated
to **Ciuki**, the dog in the boot splash. Its identity uses the owner's
[approved portrait](assets/brand/ciuki-logo.png) and the
[Tango icon family](assets/icons/README.md). Code is [GNU GPLv2](LICENSE).

## Where the project is (October 2026)

CiukiOS is being rebuilt on new foundations. There are two lines:

| Line | Branch | What it is | State |
| --- | --- | --- | --- |
| **0.8** | [`legacy-0.8`](https://github.com/PeronSlayer/CiukiOS/tree/legacy-0.8) | The DOS-based system shown in the screenshots below: CiukiDOS kernel, graphical desktop, DOS games in windows, CiukWeb browser. | Frozen. Last release: prerelease **0.8.3 build 849** with the [Windows portable bundle](docs/windows-portable-release.md). Only critical fixes. |
| **Ciuki VMM** | `main` | A new 32-bit protected-mode kernel with Windows 95/98-class structure: native processes with private address spaces, FAT32, DOS programs in virtual machines, NTFS read-only later. | **Phase F0** (kernel foundations). Boots on QEMU and passes its ten acceptance probes; not yet usable as a desktop OS; not yet tested on real PCs. |

Why the rebuild: the 0.8 line ran a real-mode DOS kernel underneath a
third-party V86 monitor and a 16-bit desktop, and it was stable on emulators
but not on the real ThinkPad T23 and Compaq Armada E500 it targets. The
owner decided to move to a 32-bit kernel that owns the machine, with DOS as
a guest. The decision, the review by a second AI agent and the seven design
contracts are recorded in the [development diary](dev_diary/README.md) and
in [`docs/design/`](docs/README.md).

### Phases

| Phase | Content | Gate (QEMU, T23 and E500) | Status |
| --- | --- | --- | --- |
| F0 | Loader, kernel core, memory, scheduler, ring-3 isolation, FPU, syscalls, crash screen, test runner | 11 acceptance probes ([f0-acceptance](docs/design/f0-acceptance.md)) | QEMU: passes. Physical: open. |
| F1 | Resource registry drivers, PS/2, VBE framebuffer, ATA, VFS with FAT32, safe mode | FAT32 integrity, safe-mode boot, input on both laptops | Not started |
| F2 | ELF32 native processes, system calls, desktop as a ring-3 process, first ported application | An application crash leaves the desktop running | Not started |
| F3 | DOS virtual machines (V86) with the CiukiDOS personality, virtual devices, kernel DPMI, DOS/4GW games | Named DOS workload matrix | Not started |
| F4 | AC97 and ESS audio, DOS-to-VFS bridge, optional S3/ATI acceleration, ATAPI/ISO9660, NTFS read-only | Per-device qualification | Not started |

Releases on `main` resume after F1 ([release policy](config/release-policy.json)).

## The 0.8 line in pictures

Actual Linux QEMU captures of 0.8.3 ([capture details](docs/screenshots/0.8.3/README.md)).

| Desktop | DOS game in its own window |
| --- | --- |
| ![CiukiOS 0.8.3 desktop](docs/screenshots/0.8.3/desktop.png) | ![Doom in a DOS window](docs/screenshots/0.8.3/doom.png) |

| CiukWeb over HTTPS | Display and monitor settings |
| --- | --- |
| ![CiukWeb](docs/screenshots/0.8.3/web.png) | ![Display Properties](docs/screenshots/0.8.3/display.png) |

The 0.8 line includes Files with long names, CiukNote, CiukPaint, Task
Manager, Control Panel, DOS windows as separate V86 machines, a VirtIO GPU
2D presenter, IPv4/DHCP/FTP networking and a bounded HTML/CSS/TLS 1.2/
JavaScript browser. Its exact limits are in the 0.8.3 documentation on the
`legacy-0.8` branch and in `legacy/CiukiOS-docs-legacy-2026-10-09.zip`.

## Build and test the new kernel (Linux)

```bash
make build-full        # kernel build/f0/VMM.ELF + 512 MiB FAT32 image build/f0/ciukios.img
make test-host         # host unit tests (kernel library, loader statics, runner, fixtures)
make qemu-test-full    # boot smoke through the test runner (QEMU TCG, pentium3)
make qemu-run-full     # boot interactively; scripts/run_f0.sh "f0:all run=00000001" runs every probe
```

Tools: clang, ld.lld, nasm, mkfs.fat, mtools, qemu-system-i386 and a user
systemd with a memory controller (every QEMU runs in a capped scope). Details
and the physical-machine procedure: [Build and run](docs/build-and-run.md).

## Documentation map

- [Development diary](dev_diary/README.md) — every essential decision and
  change, one file each (Italian).
- [Design contracts](docs/README.md) — foundations transition, boot and
  memory, execution ABI, VFS and storage, DOS and DPMI, device and firmware
  ownership, F0 acceptance, test architecture.
- [Validation records](docs/validation/2026-10-09-f0/README.md) — what was
  measured, on which image, with which limits.
- [Changelog](CHANGELOG.md) · [Roadmap](Roadmap.md) · [Support the project](DONATIONS.md)

## Working method

The project is developed with two AI agents under the owner's direction:
Claude Code leads implementation and integration, OpenAI Codex writes
bounded parts and reviews every substantial change; disagreements are
recorded and decided by the owner ([AGENTS.md](AGENTS.md)). Nothing is
claimed without a recorded test, and a QEMU result never qualifies a
physical PC.
