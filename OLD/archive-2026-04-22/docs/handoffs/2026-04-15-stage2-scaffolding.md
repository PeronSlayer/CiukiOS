# HANDOFF - stage2 scaffolding + loader handoff

## Date
`2026-04-15`

## Context
First concrete scaffolding for high DOS-compatibility path: introduce separate `stage2` from kernel, with explicit handoff from UEFI loader.

## Completed scope
1. Created new `stage2` target with dedicated entry point.
2. Integrated multi-target build (`kernel.elf` + `stage2.elf`).
3. Updated UEFI loader to load `stage2.elf` when present.
4. Implemented automatic fallback to `kernel.elf` when `stage2.elf` is missing.
5. Extended handoff ASM to pass two arguments to target (`boot_info`, `handoff_v0`).
6. Defined shared `handoff_v0_t` header.
7. Updated run script to include `stage2.elf` in FAT image.

## Touched files
1. `Makefile`
2. `run_ciukios.sh`
3. `boot/uefi-loader/loader.c`
4. `boot/uefi-loader/handoff.S`
5. `boot/proto/handoff.h`
6. `stage2/linker.ld`
7. `stage2/include/types.h`
8. `stage2/include/serial.h`
9. `stage2/src/serial.c`
10. `stage2/src/entry.S`
11. `stage2/src/stage2.c`

## Technical decisions
1. Decision: keep `stage2` separate from current kernel.
Reason: isolate DOS bootstrap work without breaking legacy kernel flow.
Impact: loader now selects target dynamically.

2. Decision: explicit handoff v0 with `magic/version`.
Reason: stabilize ABI between loader and stage2.
Impact: simple backward-compatible evolution.

3. Decision: fallback to kernel when stage2 is missing.
Reason: robustness and gradual debugging.
Impact: boot remains non-blocking during transition.

## ABI/contract changes
1. New `handoff_v0_t` in `boot/proto/handoff.h`.
2. `efi_handoff` now receives 4 arguments:
   - `new_cr3`
   - `entry_point`
   - `boot_info`
   - `handoff_v0*`
3. Output convention toward target:
   - `RDI = boot_info`
   - `RSI = handoff_v0`

## Tests executed
1. `make clean && make` (project root).
Result: `kernel.elf` and `stage2.elf` build OK.

2. `make clean && make` (`boot/uefi-loader`).
Result: `BOOTX64.EFI` build OK.

3. `timeout 45s ./run_ciukios.sh`.
Result: boot OK, loader loads `stage2.elf`, stage2 prints:
- `[ stage2 ] scaffolding started`
- `[ ok ] boot_info is valid`
- `[ ok ] handoff v0 is valid`

## Current status
1. Stage2 scaffolding is operational.
2. Loader -> stage2 handoff is stable.
3. No CPU exceptions in tested path.

## Risks / technical debt
1. `kernel_phys_*` in `boot_info` currently reflects the loaded image (stage2 when active).
2. Formal automated pass/fail checks for boot checkpoints were still missing (at that time).
3. No `stage2 -> DOS-like runtime` chain yet.

## Next steps (recommended order)
1. Confirm shared ABI header strategy for stage2 (`stage2/include/handoff_abi.h` vs direct include from `boot/proto`).
2. Add script-based automatic validation of key boot lines.
3. Implement stage2 substructure for next CPU transition (minimal dedicated GDT/IDT setup).
4. Define first interrupt dispatcher stub in stage2.

## Notes for Claude Code
Immediate priority: consolidate stage2 boot test automation before moving to interrupts/memory manager. Avoid adding new DOS features until ABI/checkpoints are automatically tested.
