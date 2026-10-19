# CiukiOS documentation

`main` is the Ciuki VMM line (0.9.2 pre-alpha: steps F0 to F4 merged, the
Windows 98/ME-class resource management and the DOS file API in the kernel
merged, the F5 Win32 personality in progress); the 0.8 line lives on branch
`legacy-0.8`. See the [README](../README.md) for the overview and the
[Roadmap](../Roadmap.md) for the state of each step.

- [Build and run](build-and-run.md)
- [Boot modes and what the screen shows](boot.md)
- [Desktop, window protocol and applications](desktop.md)
- [Terminal](terminal.md)
- [SDK and libc](sdk.md) · [Developing native applications](developer-guide.md) · [API reference](developer-api.md)
- [Windows portable release](windows-portable-release.md) (0.8 line)
- [0.9.2 screenshots](screenshots/0.9.2/README.md)
- [0.9.0 screenshots](screenshots/0.9.0/README.md)
- [0.8.3 screenshots](screenshots/0.8.3/README.md)
- [Directives](directives/README.md) — the written tasks behind each merged change
- [Test architecture](design/test-architecture.md)
- [Generic PC hardware baseline and compatibility matrix](design/hardware-baseline.md)

Ciuki VMM foundation contracts (approved 2026-10-09):

1. [Foundations transition](design/foundations-transition.md)
2. [Boot and memory](design/boot-memory.md)
3. [Execution and ABI](design/execution-abi.md)
4. [VFS and storage](design/vfs-storage-contract.md)
5. [DOS VMs and DPMI](design/dos-dpmi-contract.md)
6. [Device and firmware ownership](design/device-firmware-ownership.md)
7. [F0 acceptance](design/f0-acceptance.md)
8. [F1 acceptance](design/f1-acceptance.md) — drivers and disk
9. [Network foundations (F6)](design/network-foundations.md) — decision record

Windows 98/ME-class resource management (owner decision 2026-10-19):

- [Resource management contract](design/win98-class-resource-management.md) — disk cache, asynchronous I/O, FAT publication, memory ledger, memory objects, page replacement, swap file, priorities, interrupts and timers, DOS VM memory; accepted after cross-review 2026-10-19 ([audit](validation/f0-42-resource-management-audit.md))

F0 and F1 design records:

- [Boot hardware check](design/hardware-check.md) — the H boot entry
- [Boot log publication and throughput](design/boot-log-publication.md)
- [USB host, HID and mass storage](design/usb-contract.md)
- [Optical drive binding](design/optical-drive-binding.md)
- [Framebuffer write combining](design/framebuffer-write-combining.md)
- [Savage 2D presentation](design/savage-2d.md)
- [Hardware ID database coverage](design/hardware-id-coverage.md)
- [Kernel latency research](design/research/kernel-latency-2026-10-14.md)

F2 contracts (approved 2026-10-10, dev diary 2026-10-10-05) and desktop records:

- [Execution and ABI — F2 extension](design/execution-abi.md#f2-extension-native-abi-version-1) — syscall numbers, binary layouts and process model
- [POSIX subset, libc and SDK](design/posix-subset.md) — newlib decision, paths, supported interfaces and application gate
- [F2 acceptance](design/f2-acceptance.md) — native-process, desktop and upstream-application evidence
- [Desktop shell bars and the Start orb](design/desktop-shell.md), [desktop folder](design/desktop-folder.md), [interaction audit](design/desktop-interaction-audit.md), [transient UI](design/ui-transients.md)
- [Desktop redraw pipeline](design/desktop-redraw-performance.md) and [rendering architecture](design/desktop-rendering-architecture.md)
- [Scalable fonts](design/fonts-contract.md), [system registry](design/registry-contract.md), [Terminal](design/terminal-contract.md), [Device Manager](design/device-manager.md)
- [CiukPaint](design/ciukpaint-native.md), [Graphics Demo](design/graphics-demo.md), [legacy 0.8 feature inventory](design/legacy-0.8-feature-inventory.md)

F3 and F4 documents:

- [F3 acceptance](design/f3-acceptance.md) — DOS virtual machines, DPMI, video, audio state and workloads
- [Native SuperSavage DOS display provider](design/supersavage-fullscreen.md), [Quake shareware](design/quake-shareware.md), [DOS VM video research](design/research/dos-vm-video-2026-10-14.md)
- [F4 audio, media and shared volumes](design/f4-audio-media-contract.md) and [F4 acceptance](design/f4-acceptance.md)
- [Second audio transport set](design/f4-audio-second-set.md), [audio levels and ThinkPad volume](design/f4-18-audio-levels.md), [system sounds](design/system-sounds.md), [system event sounds](design/f4-ui-sounds.md), [Music Player](design/music-player.md)

F5 documents (contract cross-reviewed 2026-10-14 with the owner's decisions; directives in execution):

- [Win32 personality](design/win32-personality-contract.md) — Windows games of 1996–2004: PE loader, kernel objects, memory, registry, DirectX 2D/sound/input, installers, acceptance titles and directives f5-02–f5-18
- [Win16 subsystem](design/win16-subsystem-contract.md)
- [Win32 layer research](design/research/win32-layer-2026-10-14.md), [PE tools and host evidence](design/win32-pe-host-evidence.md), [provisional compatibility matrix](design/win32-compatibility-matrix.md), [census](design/win32-census/README.md), [Wine 10.0 dependency audit](design/win32-wine-dependency-audit.md)
- [Presenter and display lease](design/f5-presenter-display.md), [user32 continuation boundary](design/f5-user32-continuation.md), [GDI engine](design/f5-gdi32-engine.md), [DirectDraw](design/f5-directdraw.md), [compatibility and diagnostics](design/f5-16-diagnostics.md), [acceptance tooling](design/f5-17-acceptance-tooling.md)

F6 and F7 drafts (not yet approved):

- [F6 network contract](design/network-contract.md) and [NDIS miniport host](design/ndis-miniport-host-contract.md) (shelved)
- [F7 native Savage 3D](design/f7-3d-acceleration-contract.md) and [software OpenGL subset](design/opengl-software.md)

Validation records: [F0 kernel, loader and runner on QEMU](validation/2026-10-09-f0/README.md),
[F0 runner implementation](validation/f0-runner.md), [F0 loader](validation/2026-10-09-f0-loader.md),
[integrated images, one section per image SHA-256](validation/2026-10-11-integrated-image-f0-regression.md),
[reliability audit](validation/f0-40-reliability-audit.md); the other
records, one per directive, are in [`validation/`](validation/).

The development diary is in [`dev_diary/`](../dev_diary/). Earlier design
notes, validation records and history are archived in
[`legacy/CiukiOS-docs-legacy-2026-10-09.zip`](../legacy/).

CiukiOS is dedicated to Ciuki. The approved portrait and its provenance are in
`assets/brand/`; the photograph remains the boot splash. Icon and sound licenses
are preserved in `assets/icons/` and `assets/sounds/`.
