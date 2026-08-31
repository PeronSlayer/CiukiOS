# Costa v1.8.0 Integration

## Scope

Costa is integrated as an optional external DOS application. It is not linked into CIUKIDOS and does not change the kernel license boundary.

## Pinned upstream release

- Project: <https://github.com/jacobpalm/costa>
- Release: <https://github.com/jacobpalm/costa/releases/tag/v1.8.0>
- Archive: `costa180.zip`
- SHA-256: `254e79b7617bd96722d228731883ea2aeac982ee22e236d30fa0f9987430ee88`
- Upstream license: MIT, retained as `C:\APPS\COSTA\LICENSE` in the image

`scripts/fetch_costa.sh` refuses an incomplete installed directory, validates the archive before extraction, and checks the required executables, font data, and license. The verified payload is stored under ignored build output at `build/external/costa/v1.8.0`.

## Build and launch

```bash
bash scripts/build_run_full.sh
```

The wrapper fetches Costa by default, builds `build/full/ciukios-full.img`, verifies the Phase 5 ownership boundary, and launches QEMU. At the CiukiOS prompt, enter:

```text
costa
```

The shell changes to `C:\APPS\COSTA` only for the launch and restores the previous directory when Costa returns. `costa.exe` is accepted as an equivalent command.

## Automated evidence

```bash
make qemu-test-full-costa
```

The gate uses a snapshot of the canonical image, launches Costa through the normal shell, rejects DOS/runtime/EXEC errors, and validates three separate 640x350 captures. It requires the multi-color desktop, moves the QEMU PS/2 mouse and checks the small before/after cursor delta, then selects Calculator through the stock desktop and requires the complete gray/white `CALC.EXE` interface. It also requires the three-process MZ chain (`COSTA.EXE` → `DESKTOP.EXE` → `CALC.EXE`). The 2026-08-31 regression passes all observations with the shared mouse path also used by DOSNavigator and Windows 3.1.

The Calculator workflow exercises the QuickBASIC `RUN.DAT` handoff and its legacy read/write open mode, FAT16 metadata updates above LBA 65,535, deletion from a non-kernel caller data segment, nested EXEC, INT 33h mouse motion, and EGA `SCREEN 9` cursor rendering. QuickBASIC updates the BIOS data-area video-mode byte without necessarily issuing a hooked INT 10h mode-set call, so INT 33h reset synchronizes CIUKIDOS from that canonical byte. The planar cursor renderer also saves and restores every VGA graphics-controller register around its XOR sprite.

This gate proves packaging, DOS filename/path handling, the Calculator launch workflow, EGA/VGA rendering, mouse movement, and stable QEMU control through the observation point. It does not count as a full-CD result or prove every other Costa application and input workflow.
