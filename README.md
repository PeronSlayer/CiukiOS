![CiukiOS boot splash](misc/CiukiOS_SplashScreen.png)

# CiukiOS

**Classic computing, one woof at a time.** An open-source operating system for playing the games
of the 1990s on the computers of the 1990s. Dedicated to **Ciuki**, the dog
in the boot splash. Code under [GNU GPLv2](LICENSE).

**Version: 0.9.0 pre-alpha** on `main` (the Ciuki VMM line, built on a new
32-bit kernel), next to the frozen **0.8** line on branch `legacy-0.8`. 0.9.0
is not released: its results are QEMU evidence plus partial ThinkPad T23
sweeps, hardware qualification is open and publication is suspended
([Changelog](CHANGELOG.md)).

## What CiukiOS wants to be

CiukiOS is for people who own a late-1990s PC or laptop and want to play on
it with a system that is open, understandable and still maintained.

It is **not a Windows clone** and it does not try to run every Windows
program: Windows 98 already exists and does that. CiukiOS aims at something
narrower and more useful today:

- a system that boots quickly, plays reliably, and whose source you can read;
- **DOS games** running natively, with sound, full screen or in a window;
- the classic games whose engines have public source code, **built for
  CiukiOS itself**: Doom, Quake, Half-Life (through Xash3D), Descent, Duke
  Nukem 3D and others; the game data stays the player's own copy;
- **LAN play**, for DOS games and native games alike, with the network cards
  those machines have;
- later, the original **Windows 95/98 games** through a Win32 layer based on
  Wine, and **3D acceleration** on a chosen set of graphics chips.

"Modern" means modern engineering, not a modern look: isolated processes,
a kernel that owns the hardware, automated tests, and evidence for every
claim. Completed features need recorded tests on QEMU and on the supported
machines (today an IBM ThinkPad T23 and a Compaq Armada E500); unfinished
work stays clearly marked as such.

The project is small and is meant to stay active for years. Each step of
the plan adds capabilities that can be tested and shown, so there is
always something concrete to bring to a retro computing fair.

## Where the project is (October 2026)

There are two lines of code:

| Line | Branch | What it is | State |
| --- | --- | --- | --- |
| **0.8** | [`legacy-0.8`](https://github.com/PeronSlayer/CiukiOS/tree/legacy-0.8) | The DOS-based system in the screenshots below: CiukiDOS kernel, graphical desktop, DOS games in windows, CiukWeb browser. | Frozen. Last release: prerelease **0.8.3 build 849** with a [Windows portable bundle](docs/windows-portable-release.md). |
| **Ciuki VMM** | `main` | **0.9.0 pre-alpha.** A 32-bit protected-mode kernel with paging, scheduling and fault containment; ATA, FAT32, keyboard, mouse and framebuffer drivers; native processes with a POSIX-subset libc and Lua; a ring-3 desktop (Ciuki menu in the top bar, taskbar with tray, resizable windows, three themes, wallpapers, Files, CiukNote, Task Manager, Control Panel and accessories); DOS virtual machines (V86, XMS/EMS/UMB, trapped VGA in a desktop window, a kernel DPMI host, Sound Blaster 16 and OPL3 emulation); AC'97 audio, ATAPI/ISO 9660 and NTFS read-only. DOOM and Wolfenstein 3D shareware are pre-installed. | Unreleased. QEMU only: the F0 to F2 suites, DOS VM boots, DPMI, DOS/4GW, DOS audio, DOOM (five timedemo loops, with sound) and Wolfenstein 3D pass on integration images; the newest image (`8435a16a…`) has had no QEMU campaign yet. Hardware: only the ThinkPad T23 has run this line (unattended sweeps, failures remain open) and a new image was handed over for a direct T23 test; the E500 and generic PCs have not run it. Main prereleases are suspended. |

The 0.8 line ran a real-mode DOS kernel under a third-party V86 monitor and
a 16-bit desktop. It worked on emulators but not reliably on the real
laptops it targets. On 9 October 2026 the owner decided to rebuild on a
kernel that owns the machine, with DOS as a guest. The decision, the review
by a second AI agent and the design contracts are in the
[development diary](dev_diary/README.md) and in [`docs/design/`](docs/README.md).

## The plan, one step at a time

Nine steps, F0 to F8. Each one adds capabilities that can be tested and
demonstrated. A step is done only when its tests pass on QEMU **and** on
the real laptops.

| Step | What you get | Status |
| --- | --- | --- |
| F0 Foundations | Kernel boots, isolates processes, survives faults, reports its own tests | QEMU: `f0-core` 92/92 on five profiles. T23: the nine non-fatal probes pass in the sweeps. E500 and generic PCs: open |
| F1 Drivers and disk | Keyboard, mouse, display, hard disk, FAT32, safe mode | QEMU: suites pass. T23: sweeps ran, input and safe-mode criteria to re-run. Not done until hardware passes |
| F2 Native programs | POSIX-compatible API, a desktop that survives application crashes | QEMU: process, runtime, crash-isolation and desktop-shell cases pass. Files, CiukNote, Task Manager, window resizing, the Computer view and the retro folder names are merged; their QEMU cases wait for the next campaign. Not done until hardware passes |
| F3 DOS | DOS programs and games in virtual machines, with DPMI (DOOM, Wolfenstein 3D…) | QEMU: VMs boot to the shell prompt; XMS/EMS/UMB, devices, video, DPMI (16/32-bit and DOS/4GW), Sound Blaster 16/OPL3 state, DOOM and Wolfenstein 3D pass on integration images; the newest image has not been through the campaign. No hardware evidence |
| F4 Sound and media | Sound Blaster for DOS games, AC97/ESS audio, CD-ROM, NTFS read-only | QEMU: native AC'97 audio, ATAPI/ISO 9660, NTFS read-only and DOS VM audio (Sound Blaster 16 and OPL3 through the AC'97 mixer) pass. ESS: not started. No hardware evidence |
| F5 Native games | SDL and the open-source engines: Quake, Half-Life (Xash3D), Doom, Descent, Duke Nukem 3D | Not started |
| F6 Network | TCP/IP, DHCP, network cards, IPX for DOS games: **LAN parties** | Not started |
| F7 3D | OpenGL on chosen graphics chips (SuperSavage, Rage Mobility, Voodoo, TNT…): Quake 3 natively | Not started |
| F8 Windows games | Original Windows 95/98 games and applications through Wine | Not started |

Details and gates: [Roadmap](Roadmap.md).

## What the 0.9.0 pre-alpha does today

- **Boot.** A quiet normal boot: the loader menu (N normal, S safe mode, L
  serial log, P probe), the kernel-drawn splash with the original Ciuki
  photograph, then the desktop. Verbose boots keep the console.
- **Desktop.** The Ciuki (Start) button at the left end of the top bar opens
  the menu (Programs, Documents, Settings, Help, Run..., Shut Down...); the
  taskbar has the window buttons and a tray with volume and clock; desktop
  icons have readable labels. One Tango 0.8.90 icon family, three themes
  (Platinum by default, Ciuki Blue, Classic 98) with uniform 24 px title bars,
  resizable windows, and the owner's wallpaper scenes (Ciuk2 by default) with
  procedural wallpapers and tiles.
- **Programs.** Files (a "Computer" view with the drives `C:`, `D:`...; the
  folders Programs, Documents, System and Games), CiukNote, Task Manager,
  Control Panel (Display, Mouse, Keyboard, Sounds, Date & Time, System),
  accessories (Calculator, Character Map, Clock, CiukiOS Help, Volume Control,
  Image Viewer), DOS Box, Lua and developer tools.
- **DOS games.** DOOM 1.9 and Wolfenstein 3D 1.4, shareware releases,
  are pre-installed on `C:\GAMES\CIUKIGAM.IMG` and start from Programs >
  Games in a DOS window with DPMI, Sound Blaster 16/OPL3 emulation and a fast
  path for VGA mode 13h. The shareware licence texts are in
  `third_party/games`; whether Wolfenstein 3D may stay in a published image is
  an open owner decision.
- **File names.** Programs see a POSIX view of the drives (`/` is `C:\`,
  `/Programs`, `/Documents`, `/system`, `/tmp`, synthetic `/dev` and `/mnt`);
  users see `C:\Programs`, `C:\Documents`, `C:\System` and `C:\Games`.

## 0.9.0 pre-alpha in pictures

QEMU captures (Pentium III profile, TCG) of the integration image `8435a16a…`
of 2026-10-12 ([capture details](docs/screenshots/0.9.0/README.md)). The two
game frames show the games starting up, not play.

| Desktop | Ciuki menu |
| --- | --- |
| ![Desktop](docs/screenshots/0.9.0/01-desktop.png) | ![Ciuki menu](docs/screenshots/0.9.0/02-ciuki-menu.png) |

| Programs | Files, Computer view |
| --- | --- |
| ![Programs](docs/screenshots/0.9.0/03-programs.png) | ![Files, Computer view](docs/screenshots/0.9.0/04-files-computer.png) |

| CiukNote | Task Manager |
| --- | --- |
| ![CiukNote](docs/screenshots/0.9.0/05-ciuknote.png) | ![Task Manager](docs/screenshots/0.9.0/06-task-manager.png) |

| Control Panel and Calculator | Games menu |
| --- | --- |
| ![Control Panel and Calculator](docs/screenshots/0.9.0/07-control-panel-calculator.png) | ![Games menu](docs/screenshots/0.9.0/08-games-menu.png) |

| Wolfenstein 3D (shareware) | DOOM (shareware) |
| --- | --- |
| ![Wolfenstein 3D (shareware)](docs/screenshots/0.9.0/09-wolfenstein-3d.png) | ![DOOM (shareware)](docs/screenshots/0.9.0/10-doom.png) |

## The 0.8 line in pictures

Actual Linux QEMU captures of 0.8.3 ([capture details](docs/screenshots/0.8.3/README.md)).

| Desktop | DOS game in its own window |
| --- | --- |
| ![CiukiOS 0.8.3 desktop](docs/screenshots/0.8.3/desktop.png) | ![Doom in a DOS window](docs/screenshots/0.8.3/doom.png) |

| CiukWeb over HTTPS | Display and monitor settings |
| --- | --- |
| ![CiukWeb](docs/screenshots/0.8.3/web.png) | ![Display Properties](docs/screenshots/0.8.3/display.png) |

## Build and test the new kernel (Linux)

```bash
make build-full        # kernel build/f0/VMM.ELF + 512 MiB FAT32 image build/f0/ciukios.img (config/version.json: 0.9.0 pre-alpha)
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

## How it is made

The project is developed by its owner with two AI agents: Claude Code leads
implementation and integration, OpenAI Codex writes bounded parts and
reviews every substantial change; disagreements are recorded and decided by
the owner ([AGENTS.md](AGENTS.md)). Nothing is claimed without a recorded
test, and a QEMU result never qualifies a physical PC.
