![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

CiukiOS is a personal open source retro-computing project: a small legacy BIOS x86 operating system rebuilt from a clean baseline.

The long-term goal is to support DOS and pre-NT software progressively, without CPU emulation in the final runtime path. The current system is shell-first, MS-DOS/FreeDOS-inspired, and focused on the FAT16 `full` profile as the main compatibility lane.

CiukiOS is not a finished operating system. It is an active learning and research project, built in spare time with AI-assisted development workflows and a lot of low-level debugging.

## Current Milestone

Current public version: `CiukiOS pre-Alpha v0.7.1`.

The Phase 4 DOOM gameplay milestone is closed. The full FAT16 runtime can launch DOOM through DOS/4GW, load `doom.wad`, initialize the gameplay path, and reach a playable visual runtime.

Phase 5 is **COMPLETE; NORMAL DOS RUNTIME OWNERSHIP CLOSED**. In the active full and full-CD profiles, Stage0 reads 8 sectors from the 72-sector BPB-reserved Stage1 slot and transfers to a bounded 1,542-byte loader-only Stage1. That loader locates and validates `\SYSTEM\CIUKIDOS.SYS`, loads it at segment `0x0900`, and has no normal DOS interrupt, process, memory, handle, file/path, COM/MZ, or device ownership. The current kernel contract is `ABI=2`, 11 descriptors of 8 bytes, capability mask `0x003F`, and no Stage1 compatibility chain (`CIUKRTST CHAIN=0`). The current kernel artifact is 43,254 bytes with a hard maximum of `0xA900` (43,264 bytes); four EXEC snapshot frames occupy segments `0x1400-0x1457`, and Stage2 starts at `0x1480`. CIUKIDOS launches the external `\SYSTEM\SHELL.COM`; missing or invalid required components, or an unexpected shell return, still fail closed to the bounded fatal path.

Phase 6 is active, not closed. The external GPL CuteMouse workflow passes on `full`, and the official MIT-licensed Costa v1.8.0 lane validates reproducible fetch/package/launch, the 640x350 desktop, one moving cursor, and the complete Desktop → Calculator nested-EXEC workflow. COM and MZ children use one title-independent first-fit EXEC allocator: COM placement derives from PSP+file+stack, while MZ placement derives from the real copy extent, header, `minalloc`, live MCBs, and arena limit. Doom-vanille passes the focused DOS/4GW 256 KiB low-DOS allocation gate and reaches DMX initialization, but its direct gameplay capture has corrupted planar walls and a missing HUD. DOSNavigator now validates dual-pane startup, one-row arrow navigation, mouse input, the Colors/XMS workflow, `EXIT`, and shell return; it remains PARTIAL because its packaged loader contains a bounded compatibility hook and no broad file-operation/editor or full-CD workflow is recorded. The packaged WOLF3D copy passes resource loading, stable runtime, keyboard-driven menu progression, and correctly rendered first-level video; it remains PARTIAL for unmodified-binary, clean-exit, full-CD, and audio compatibility. The required external corpus and five-category full-CD matrix are incomplete. Phase 7 is not closed: controlled SB16/DOOMSFX playback passes, while both Doom integrations still lack validated non-silent playback.

Phase 8 is now active with its first bounded milestone complete. The `full` profile packages the GPL mTCP/Crynwr stack, exposes NE2000 plus configurable IPv4 under QEMU, and provides bidirectional FTP sharing through `C:\SHARE`. `NETSTART` installs a resident ARP/ICMP bridge independent of FTP, `NETCFG` persists IP/mask/gateway/DNS, and `IPCONFIG` reports live values and service status. Isolated gates verify inbound and Internet ICMP plus FTP persistence without mutating the canonical image. This does not yet imply SMB, encrypted transfer, arbitrary NIC, DHCP-server interoperability, or physical-hardware support.

Phase 9 has advanced early but is not closed. With optional local media, Windows 3.1 now reaches 386 Enhanced Mode through the normal `WIN` command on the canonical `full` image. The bounded acceptance workflow proves a single PS/2 pointer, Calculator launch and task-scoped `Alt+F4`, Program Manager survival, clean return to CiukiOS, and a second Windows launch. Windows 95/98, full-CD, broad application coverage, audio, printing, and hardware evidence remain open.

Validation snapshot (2026-09-01): the Phase 5 loader/kernel ownership boundary and ABI gate are green, together with the focused `make qemu-test-all` bundle and its full-CD read beyond LBA 65,535. The same checkout also passes the generic EXEC-memory policy guard, DOSNavigator mouse/navigation/Colors/exit workflow, the Costa desktop/cursor/Calculator gate, the Windows 3.1 Enhanced Mode/one-cursor/Alt+F4/exit/relaunch gate, the doom-vanille 256 KiB gate, controlled DOOMSFX playback, the focused shell COM/MZ/PSTACK/TSR return gate, and the bounded mTCP ICMP/FTP gates. The networking gate also proves that a normal COM process can shrink its own DOS block before nested EXEC: `NETSTART` starts `NE2000.COM` without any title-specific kernel rule. The doom-vanille allocation result does not override the direct gameplay capture that exposes incorrect planar rendering. `make qemu-test-setup-runtime-hdd-install` remains a separate long gate and is not included in `qemu-test-all`.

Current work is concentrated on:

1. hardening logical JFT/SFT, handle, process, and file semantics as Phase 6 compatibility work without reopening the closed Phase 5 ownership boundary
2. keeping the aggregate, runtime-negative, shell, and installer gates green on the same checkout
3. expanding the Phase 6 matrix with meaningful external-application workflows on both `full` and `full-cd`
4. removing workload-specific compatibility patches where a general DOS subsystem fix is possible
5. using controlled audio probes as groundwork without promoting unresolved DOOM SB16 behavior to product support
6. keeping the bounded Packet Driver/IPv4/FTP lane green while DHCP, other NICs, encrypted protocols, and physical networking remain explicit Phase 8 follow-ups
7. extending the bounded Windows 3.1 result without treating it as Windows 95/98 or broad Phase 9 closure

## Quick Start

Install the usual build and test tools for your platform. On Linux, the active lanes use `nasm`, `mtools`, `xorriso`, `qemu-system-i386` or `qemu-system-x86_64`, Syslinux BIOS files for the full-CD fallback ISO, `python3`, and Python Pillow for splash asset generation.

Fetch the verified Costa release, build the complete FAT16 image, verify the runtime boundary, and open QEMU:

```bash
bash scripts/build_run_full.sh
```

At the CiukiOS prompt, type `costa`. Set `CIUKIOS_FETCH_COSTA=0` only when intentionally building without downloading the optional payload.

To share files with the host, run `NETSTART` and `FTPSRV`, then connect from Linux or Windows to `ftp://127.0.0.1:8021/` with `ciukios` / `ciukios`. See the network section below for exact commands.

Build the main FAT16 disk image:

```bash
make build-full
```

Boot-test the main full profile in QEMU:

```bash
make qemu-test-full
```

Build the Live/install CD profile:

```bash
make build-full-cd
```

Smoke-test the Live/install CD profile:

```bash
make qemu-test-full-cd
```

Run the visual Live/install CD profile:

```bash
make qemu-run-full-cd
```

Run the active aggregate validation lane:

```bash
make qemu-test-all
```

Generated images are written under `build/full/`. The main full-profile disk image is `build/full/ciukios-full.img`; the primary Live/install CD image is `build/full/ciukios-full-cd.iso`; the ISOLINUX/memdisk fallback image is `build/full/ciukios-full-cd-isolinux.iso`.

## Active Profiles

| Profile | Status | Purpose | Main commands |
|---|---|---|---|
| `full` | Active default | FAT16 C: disk image, shell-first runtime, DOS compatibility, DOOM/WOLF3D work, driver helper probes | `make build-full`, `make qemu-test-full` |
| `full-cd` | Active install/live media | Bootable Live/install CD, D: shell profile, destructive HDD install flow through `SETUP.COM` | `make build-full-cd`, `make qemu-test-full-cd`, `make qemu-run-full-cd` |
| `floppy` | Legacy/minimal | 1.44MB loader-only bring-up scaffold; no CIUKIDOS DOS-runtime claim | `make build-floppy`, `make qemu-test-floppy` |

Default validation should use the `full` lane first. Use `full-cd` when the change touches Live/install media, D: shell behavior, setup, direct ISO boot, or real-hardware install paths. Do not treat the floppy profile as the default release gate unless a task specifically targets it.

## Validation Lanes

Common focused lanes:

```bash
make qemu-test-full
make qemu-test-full-cd
make qemu-test-full-dos-compat-smoke
make qemu-test-full-dos-taxonomy
make qemu-test-full-doom-taxonomy
make qemu-test-full-wolf3d-taxonomy
make qemu-test-full-costa
make qemu-test-full-network-ftp
make qemu-test-full-network-icmp
make qemu-test-full-cutemouse
make qemu-test-full-doomvan-memory
make qemu-test-full-windows31
make qemu-test-full-drvload-smoke
make qemu-test-full-shell-stability
make qemu-test-setup-runtime-hdd-install
```

Use `make qemu-test-all` for the active aggregate smoke bundle. These commands describe the intended gates; documentation or release claims require a fresh result from the same checkout. Focused taxonomy lanes classify runtime stages and should not be upgraded to release claims unless the requested minimum stage and observation window match the claim being made.

## DOS Software And Third-Party Payloads

The repository does not publish commercial DOS game data or proprietary third-party binaries. Local payload directories may be used for private validation only:

1. `third_party/Doom` can be packaged into `C:\APPS\DOOM` when present locally.
2. `third_party/WOLF3D` can be packaged into `C:\APPS\WOLF3D` when present locally.
3. `third_party/DOSNavigator` can be packaged into `C:\APPS\DOSNAV` when present locally.
4. `third_party/drivers` can be packaged into `C:\SYSTEM\DRIVERS` when present locally.
5. `make fetch-costa` downloads the pinned official Costa v1.8.0 archive, verifies SHA-256 `254e79b7617bd96722d228731883ea2aeac982ee22e236d30fa0f9987430ee88`, and packages it into `C:\APPS\COSTA`.
6. `make fetch-network-stack` downloads pinned GPL mTCP and Crynwr packages, verifies both SHA-256 values, and packages tools, licenses, and sources under `C:\NET`.
7. A complete local `third_party/windows31/disk01.img` ... `disk07.img` set is copied unchanged to `C:\MEDIA\WIN31` and merged into the directly accessible `C:\WIN31SET`; an optional local `third_party/windows31/installed` tree is copied to `C:\WINDOWS`.

Keep third-party payloads legally supplied, local, and untracked unless a license explicitly permits redistribution. DOSNavigator acknowledgement: "Based on Dos Navigator by RIT Research Labs."

## Windows 3.1 And Mouse

Windows 3.1 uses the canonical `full` image; there is no separate Windows
build. The normal one-command build/run validates the seven embedded IMG files
before opening QEMU:

```bash
bash scripts/build_run_full.sh
```

If the local installed tree is present, launch Windows 3.1 in 386 Enhanced
Mode from the default `C:\APPS>` prompt with:

```text
CD ..
CD WINDOWS
WIN
```

`WIN /3` explicitly requests the same Enhanced Mode. `WIN /S` remains useful
only as a diagnostic fallback to the less capable Standard Mode.

The normal launcher automatically selects SDL over a verified X11/XWayland
socket. This is QEMU's reliable relative-pointer path for DOS applications and
Windows 3.1 across video-mode switches, including Costa and DOS Navigator. It
is selected even if the desktop inherited a native-Wayland toolkit setting.
Click inside the QEMU window to capture the pointer; use `Ctrl+Alt+G` to release
it. GTK remains available explicitly with `--display gtk`, and
`QEMU_DISPLAY_TRANSPORT=native` is the explicit transport opt-out. The
frontend ignores host window-close requests so a guest `Alt+F4` cannot
terminate the complete VM; exit CiukiOS with its `SHUTDOWN` command.

The `full` and `full-cd` launchers now allocate 256 MiB of VM RAM by default,
while the DOS/Windows-compatible BIOS and XMS interfaces expose approximately
63 MiB of usable extended memory instead of 15 MiB. The compatible single
Pentium III vCPU remains unthrottled and requires KVM hardware acceleration by
default; the launcher fails clearly instead of silently selecting slow TCG.
DOS and Windows 3.1 do not benefit from additional virtual CPUs. Override RAM
and CPU with `QEMU_MEMORY_MB` and `QEMU_CPU_MODEL` when needed.

Windows applications can be closed normally with `Alt+F4` without terminating
Program Manager or the complete Enhanced Mode session. Windows itself may be
closed and launched again without rebooting CiukiOS. The XMS
hook window, A20 state, DOS Swappable Data Area, and file-handle set are
restored when the session ends, so a later DOS application or a second `WIN`
starts from a clean runtime state. The shell title bar and text cursor are also
redrawn after every external program, including Windows. The automated
acceptance test is:

```bash
bash scripts/qemu_test_full_windows31.sh --no-build --headless-smoke
```

It verifies two Enhanced Mode starts at 640x480, exactly one moving PS/2 pointer,
Calculator launch and task-scoped `Alt+F4`, Program Manager responsiveness,
the visible CiukiOS title bar after exit, and a clean relaunch.

The original images remain available under `C:\MEDIA\WIN31`, while
`C:\WIN31SET\SETUP.EXE` can access the merged installation media without
mounting floppies. CiukiOS installs its PS/2-backed standard `INT 33h` service
at every boot, before any application starts, and exposes the IBM-compatible
`INT 15h/AH=C2h` BIOS interface used by Windows 3.x. Device reset, data-reporting,
sample-rate, resolution, identity, status, scaling, and callback operations are
forwarded to the PS/2 controller rather than acknowledged as no-ops. `MOUSE
STATUS` and `MOUSE INFO` inspect the resident DOS service. The GPL CuteMouse binary is also packaged as
`C:\SYSTEM\DRIVERS\CTMOUSE.EXE` for optional replacement-driver testing; it is
not automatically layered on top of an already active mouse service.

DOS compatibility changes belong in standard DOS, BIOS, XMS, DPMI, graphics,
input, and device interfaces rather than executable-name rules. Files copied
later through FTP or another writable-media workflow therefore use the same
loader, memory allocator, filesystem, mouse, audio, and networking paths as
the applications present during the build. Booting an unrelated standalone OS
still requires an explicit boot/chain-load path; it is not a DOS `EXEC` action.

## Network File Sharing

The graphical full runner enables QEMU user networking by default. `C:\NET` is
in the shell search path, so at the CiukiOS prompt use:

```text
netstart
ipconfig
ping 1.1.1.1
ftpsrv
```

Static IPv4 values are configurable and persistent:

```text
netcfg static 10.0.2.15 255.255.255.0 10.0.2.2 1.1.1.1
```

From Linux or current Windows `curl.exe`, list the share with:

```bash
curl --noproxy '*' --ftp-skip-pasv-ip -u ciukios:ciukios ftp://127.0.0.1:8021/
```

Uploads and downloads are restricted to `C:\SHARE`. `NETSTART` installs a
resident ARP/ICMP service, so incoming ping does not depend on `FTPSRV`. QEMU
user NAT cannot route a host `ping` directly to the guest; on Linux, create a
TAP endpoint with Internet forwarding using `scripts/ciukios_tap.sh up-nat`,
launch with `bash scripts/build_run_full.sh --tap`, then run `ping 10.0.2.15`.
FTP is plaintext and the default credentials are only
suitable for localhost QEMU NAT or a trusted isolated LAN. Run
`make qemu-test-full-network-ftp` and `make qemu-test-full-network-icmp` for the
disposable gates. Exact Linux, Windows, FileZilla, security, TAP, and
configuration instructions are in [docs/network-file-sharing-2026-08-29.md](docs/network-file-sharing-2026-08-29.md).

## Costa Desktop

Costa v1.8.0 is integrated as a redistributable external application, not as CiukiOS kernel code. `scripts/fetch_costa.sh` pins and verifies the official release; `scripts/build_full.sh` installs the payload at `C:\APPS\COSTA`; and the shell accepts `costa` or `costa.exe` from any current directory while restoring that directory after the program returns.

Run `make qemu-test-full-costa` for the isolated graphical gate. It rejects DOS/runtime/EXEC errors and validates the 640x350 desktop, a real moving mouse cursor, and the complete Calculator UI launched through Costa's `RUN.DAT` handoff. See [docs/costa-integration-2026-08-29.md](docs/costa-integration-2026-08-29.md) for the exact source, checksum, and validation boundary.

CiukiOS project code is licensed under GNU GPLv2. FreeDOS kernel and FreeCOM source snapshots under `third_party/freedos` are GPLv2-compatible source material; selected behavior is ported into the CiukiOS runtime from source, and the active full/full-cd runtime environment exposes a compact DOS-style PATH for driver helpers, exports BLASTER for audio-aware apps, and the shell searches `APPS` after the current directory; upstream license files stay alongside the snapshots.

## Audio Status

The full profile now includes a narrow SB16 validation path. `SB16INIT.COM` probes Sound Blaster-compatible DSP bases, verifies the QEMU SB16 DSP at `0x220`, and plays a short DMA-backed SB sample and observes IRQ7 completion. `DRVLOAD.COM /AUDIO` runs that helper from `C:\SYSTEM\DRIVERS`.

QEMU full, full-CD, taxonomy, and DRVLOAD smoke runners support `QEMU_AUDIO_MODE=off|auto|on` and `QEMU_AUDIO_BACKEND=pipewire|pa|pulse|alsa|sdl|none`. The default is `on`, so local QEMU runs expose SB16 and PC speaker audio devices by default. Interactive runners prefer PipeWire, PulseAudio, ALSA, then SDL; focused tests may pin ALSA for reproducibility. `QEMU_AUDIO_MODE=off` is only for explicit silent runs.

The interactive `full` and `full-cd` runners default to `QEMU_ACCEL_MODE=kvm`,
so hardware virtualization is always active and missing `/dev/kvm` access is a
fatal configuration error. `QEMU_ACCEL_MODE=auto` is the explicit portable
fallback: it prefers KVM and otherwise enables the slower `tcg-safe` mode
(`one-insn-per-tb`) required by Wolf3D's self-modifying renderer on affected
QEMU 11.1 TCG builds. Plain `tcg` remains an explicit, faster but
Wolf3D-unsafe diagnostic mode. KVM accelerates CPU-rendered DOS graphics; the
standard VGA device remains intentionally compatible and does not expose a
modern guest 3D API to DOS or Windows 3.1.

This proves SB16 DSP detection and controlled DMA1/IRQ7 playback in project probes. `PMIRQSB.COM` also proves the narrower DOS/4GW protected-mode timer and SB IRQ delivery path, and `DOOMSFX` plays selected WAD lumps through the controlled SB16 harness.

The DOS child environment now has a valid owned MCB and exports `BLASTER=A220 I7 D1 H5 T6`; the conventional-memory arena also leaves room for DOS/4GW plus MultiVoc. Doom-vanille is packaged with an explicit SB16 SFX profile and reaches `DMX_Init() returned 8` with SFX device 3/code 8, but the final gameplay WAV contains only digital silence (`-91 dB`). This does not promote either game path: the proprietary `DOOM.EXE`/DMX SB16 branch remains an investigation lane, and doom-vanille still needs its DMA/mixer sample path fixed.

## Project Policy

1. Final runtime compatibility work must not rely on CPU emulation shortcuts.
2. Stage1 size and ownership pressure should be reduced through runtime/module ownership, not endless byte-level feature accretion.
3. Validation claims must name the lane and scope that proved them.
4. Local agent handoffs and transient operational notes belong under `handoff/`, not public docs.
5. Public documentation and changelog entries should stay concise, traceable, and in English.

## Links

1. Full changelog: [CHANGELOG.md](CHANGELOG.md)
2. Project roadmap: [Roadmap.md](Roadmap.md)
3. DOS compatibility matrix: [docs/dos-compatibility-matrix-v0.1.md](docs/dos-compatibility-matrix-v0.1.md)
4. Legacy audio bring-up plan: [docs/legacy-audio-bring-up-plan-v0.1.md](docs/legacy-audio-bring-up-plan-v0.1.md)
5. Setup stream notes: [setup/README.md](setup/README.md)
6. Network file sharing: [docs/network-file-sharing-2026-08-29.md](docs/network-file-sharing-2026-08-29.md)
7. Current milestone ledger: [docs/current-milestones.md](docs/current-milestones.md)
8. Costa integration: [docs/costa-integration-2026-08-29.md](docs/costa-integration-2026-08-29.md)
9. Automatic EXEC allocation: [docs/exec-memory-allocation-2026-08-29.md](docs/exec-memory-allocation-2026-08-29.md)
10. Donations and support: [DONATIONS.md](DONATIONS.md)

## Support

GitHub Sponsors is the primary support channel for CiukiOS: [github.com/sponsors/PeronSlayer](https://github.com/sponsors/PeronSlayer).

Non-monetary help is also useful: reproducible bug reports, focused pull requests, documentation improvements, and compatibility results from real DOS/FreeDOS software are all welcome.
