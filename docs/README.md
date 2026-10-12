# CiukiOS documentation

`main` is the Ciuki VMM line (0.9.0 pre-alpha, steps F0 to F4 merged); the
0.8 line lives on branch `legacy-0.8`. See the [README](../README.md) for the
overview.

- [Build and run](build-and-run.md)
- [Boot modes and what the screen shows](boot.md)
- [Desktop, window protocol and applications](desktop.md)
- [SDK and libc](sdk.md)
- [Windows portable release](windows-portable-release.md) (0.8 line)
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
9. [Network foundations (F6)](design/network-foundations.md) — decision record, contract to be written

F2 contracts (revised after cross-review 2026-10-10; lead approval pending):

- [Execution and ABI — F2 extension](design/execution-abi.md#f2-extension-native-abi-version-1) — syscall numbers, binary layouts and process model
- [POSIX subset, libc and SDK](design/posix-subset.md) — newlib decision, paths, supported interfaces and application gate
- [F2 acceptance](design/f2-acceptance.md) — native-process, desktop and upstream-application evidence

F3 and F4 documents:

- [F3 acceptance](design/f3-acceptance.md) — DOS virtual machines, DPMI, video, audio state and workloads
- [F4 audio, media and shared volumes](design/f4-audio-media-contract.md) and [F4 acceptance](design/f4-acceptance.md)

Validation records: [F0 kernel, loader and runner on QEMU](validation/2026-10-09-f0/README.md),
[F0 runner implementation](validation/f0-runner.md), [F0 loader](validation/2026-10-09-f0-loader.md),
[integrated images, one section per image SHA-256](validation/2026-10-11-integrated-image-f0-regression.md).

The development diary is in [`dev_diary/`](../dev_diary/). Earlier design
notes, validation records and history are archived in
[`legacy/CiukiOS-docs-legacy-2026-10-09.zip`](../legacy/).

CiukiOS is dedicated to Ciuki. The approved portrait and its provenance are in
`assets/brand/`; the photograph remains the boot splash. Icon and sound licenses
are preserved in `assets/icons/` and `assets/sounds/`.
