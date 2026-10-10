# CiukiOS documentation

`main` is the Ciuki VMM line (phase F0); the 0.8 line lives on branch
`legacy-0.8`. See the [README](../README.md) for the overview.

- [Build and run](build-and-run.md)
- [Windows portable release](windows-portable-release.md)
- [0.8.3 screenshots](screenshots/0.8.3/README.md)
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

Validation records: [F0 kernel, loader and runner on QEMU](validation/2026-10-09-f0/README.md),
[F0 runner implementation](validation/f0-runner.md), [F0 loader](validation/2026-10-09-f0-loader.md).

The development diary is in [`dev_diary/`](../dev_diary/). Earlier design
notes, validation records and history are archived in
[`legacy/CiukiOS-docs-legacy-2026-10-09.zip`](../legacy/).

CiukiOS is dedicated to Ciuki. The approved portrait and its provenance are in
`assets/brand/`; the photograph remains the boot splash. Icon and sound licenses
are preserved in `assets/icons/` and `assets/sounds/`.
