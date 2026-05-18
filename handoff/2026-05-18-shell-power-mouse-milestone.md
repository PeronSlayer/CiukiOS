# SHELL.COM power and mouse milestone - 2026-05-18

## Scope completed

- SHELL.COM visual polish is complete.
- SHELL.COM command parsing now accepts `SHUTDOWN` and the longer power-command surface.
- SHELL.COM now provides a scheduled `REBOOT` / `SHUTDOWN` queue with `status` and `cancel`.
- The scheduler polls BIOS tick count locally inside SHELL.COM and reuses the existing reboot/halt primitives already present in the COM program.
- `MOUSE.COM` was added as a minimal real `INT 33h` utility.

## MOUSE.COM status

- `MOUSE.COM` probes the existing `INT 33h` service.
- `MOUSE.COM` supports `SHOW` and `HIDE`.
- `MOUSE.COM` reports current buttons and coordinates.
- This is not a full PS/2 mouse backend, TSR, or CiukiOS mouse driver replacement.
- Current scope is a shell-visible utility layered on top of whatever `INT 33h` service is already available at runtime.

## Packaging status

- `MOUSE.COM` is injected into `C:\SYSTEM\MOUSE.COM` by `scripts/build_full.sh`.
- Full-CD builds inherit the same payload because `scripts/build_full_cd.sh` delegates the partition image build to `scripts/build_full.sh`.
- Result: `MOUSE.COM` is present in both full-HDD and full-CD images.

## Test coverage status

- `scripts/qemu_test_full_shell_com.sh` now covers:
  - `where MOUSE`
  - `MOUSE`
  - `shutdown status`
  - `reboot /t 5`
  - `shutdown /t 5`
  - `cancel`
  - invalid timer argument handling
- `scripts/qemu_test_full_cd_shell_com_boot.sh` now covers the same SHELL.COM-visible `MOUSE.COM` and power-queue behavior on the direct full-CD boot path.
- The `SHELL.COM missing` fallback boot lane intentionally drops into the Stage1 shell, so it cannot validate SHELL.COM-only power queue or `MOUSE.COM` behavior. That limitation is structural, not a skipped test.

## Validation

- `bash scripts/build_shell_com.sh` PASS
- `bash -n scripts/qemu_test_full_shell_com.sh` PASS
- `bash -n scripts/qemu_test_full_cd_shell_com_boot.sh` PASS
- `bash -n scripts/build_full_cd.sh` PASS
- `make build-full` PASS
- `make build-full-cd` PASS
- `make qemu-test-full-shell-com` PASS
- `make qemu-test-full-shell-com-boot` PASS
- `make qemu-test-full-shell-com-boot-fallback` PASS
- `make qemu-test-full-cd-shell-com-boot` PASS

## Left untouched

- Unrelated workspace deltas were not included in this milestone.
- Confirmed unrelated files left out of the milestone commits:
  - `scripts/build_full_cd.sh`
  - `AGENTS.md`
  - `Makefile`

## Follow-up boundary

- DOOM taxonomy and DOOM audio regressions remain separate from this shell milestone.
- A real PS/2-backed mouse driver / resident `INT 33h` provider remains future work.
- A true ACPI/APM shutdown backend remains future work; current `SHUTDOWN` behavior uses the existing local halt primitive.