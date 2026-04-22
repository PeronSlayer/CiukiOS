# Handoff - M1 DOS-like COM Runtime (PSP Contract)

## Context and Goal
Implement the first M1 execution upgrade on branch `feature/codex-m1-com-loader-psp`: move from raw function-call COM execution to a DOS-like runtime contract with PSP-style memory layout and explicit program termination semantics.

## Files Touched
1. `boot/proto/services.h`
2. `stage2/src/shell.c`
3. `com/hello/hello.c`
4. `CLAUDE.md`

## What Changed
1. Extended COM ABI (`boot/proto/services.h`):
- Added `ciuki_dos_context_t` passed to COM entrypoints.
- Added exit reason model (`RET`, `INT 20h`, `INT 21h/AH=4Ch`, `terminate API`).
- Added service callbacks:
  - `int20(ctx)`
  - `int21_4c(ctx, code)`
  - `terminate(ctx, code)`
- Updated `com_entry_t` signature to:
  - `void (*)(ciuki_dos_context_t *ctx, ciuki_services_t *svc)`

2. Reworked `run` execution path in `stage2/src/shell.c`:
- Added PSP-style staging model:
  - Reserved first `0x100` bytes as PSP
  - COM payload loaded at `SHELL_RUNTIME_COM_ENTRY_ADDR = base + 0x100`
- Added command-tail extraction from `run <prog> <args...>` and mapping into:
  - `PSP:80h` length
  - `PSP:81h..` bytes
  - trailing `0x0D`
- Added metadata fill in runtime context:
  - `psp_segment`, `psp_linear`, `image_linear`, `image_size`, `command_tail`
- Added explicit exit reporting after COM returns.
- Added safe size checks for staged image.
- Added MZ detection guard (`"MZ"`): report `.EXE` loader not implemented yet.

3. Updated COM sample (`com/hello/hello.c`):
- Migrated to new context-based ABI.
- Prints PSP segment and optional command tail.
- Exits with `svc->int21_4c(ctx, 0x00)`.

4. Minor shell UX update:
- `help` now shows `run X A` with optional args.
- `ver` now reports `v0.2 (M1 DOS-like COM runtime)`.

## Decisions Made
1. Kept implementation intentionally DOS-like but still native x64 payload execution.
2. Chose PSP emulation and termination contract now, full 16-bit `.COM` CPU compatibility later.
3. Added explicit MZ reject path to avoid accidentally executing `.EXE` as raw COM.

## Validation Performed
1. `make clean && make` -> PASS
2. `make test-stage2` -> PASS
3. `make test-fallback` -> PASS

## Risks / Open Points
1. This is not yet true 16-bit instruction compatibility for arbitrary DOS COM binaries.
2. Real DOS `.COM` binaries (16-bit machine code) still need dedicated execution path/emulation/protected transition work.
3. `.EXE MZ` loader is explicitly pending (roadmap M1/M7 bridge).

## Immediate Next Step
1. Add a focused COM smoke test path that validates command-tail propagation and exit code/reason in automated logs.
2. Start `.EXE MZ` loader MVP planning (relocations + entry setup), while keeping this ABI stable.
