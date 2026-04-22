# HANDOFF - stage2 COM loader (INIT.COM execution)

## Date
`2026-04-15`

## Context
User requested implementing the `.COM` loader — the first step toward a DOS-like executable runtime from the stage2 shell.

## Completed scope
1. Defined `ciuki_services_t` service table in `boot/proto/services.h` — function pointers the COM uses to call stage2 video routines without knowing stage2's internal layout.
2. Extended `handoff_v0_t` with `com_phys_base` and `com_phys_size` fields.
3. UEFI loader now tries to open `\EFI\CiukiOS\INIT.COM` from the ESP. If found, allocates it at fixed physical address `0x600000` (6 MB) using `AllocateAddress` and records the address in the handoff.
4. Added `run` command to the shell: builds a `ciuki_services_t` pointing to stage2's `video_write`, `video_write_hex64`, `video_cls`, casts `com_phys_base` to `com_entry_t`, and calls it.
5. Created `com/hello/hello.c` — minimal hello-world COM using only the service table.
6. Created `com/hello/linker.ld` — flat binary linker script at `0x600000`.
7. Updated `Makefile` to build `build/INIT.COM` via `llvm-objcopy -O binary`.
8. Updated `run_ciukios.sh` to copy `INIT.COM` into the FAT image if present.

## Touched files
1. `boot/proto/services.h` (NEW)
2. `boot/proto/handoff.h` — added `com_phys_base`, `com_phys_size`
3. `boot/uefi-loader/loader.c` — INIT.COM load block before `build_bootstrap_paging`
4. `stage2/src/shell.c` — `run` command + `shell_run()` + updated help
5. `com/hello/hello.c` (NEW)
6. `com/hello/linker.ld` (NEW)
7. `Makefile` — `COM_HELLO_BIN` target, `all` dependency
8. `run_ciukios.sh` — INIT.COM mcopy block

## Technical decisions
1. Decision: fixed physical load address `0x600000` for all COMs (for now).
   Reason: no dynamic allocator in stage2 yet; hardcoding avoids collisions with stage2 (at 0x300000) and the framebuffer (at 0x80000000).
   Impact: only one COM can be loaded at a time; future work must add a COM region allocator.

2. Decision: service table (`ciuki_services_t`) instead of direct symbol calls.
   Reason: COMs are flat binaries with no access to stage2's symbol table; the service table is the stable ABI boundary.
   Impact: adding new services requires bumping the struct but is backward-compatible if new fields are appended.

3. Decision: `unsigned long long` for `print_hex64` in services.h instead of `uint64_t`.
   Reason: stage2 defines `u64` as `unsigned long long`; on LP64 Linux, `uint64_t` is `unsigned long`, causing a function pointer type mismatch under `-Wincompatible-function-pointer-types`.
   Impact: services.h is explicit about the width and avoids silent ABI mismatches.

4. Decision: INIT.COM is optional — loader prints a warning but does not halt if not found.
   Reason: automated tests run without a COM; the shell shows "No COM loaded." on `run`.
   Impact: test_stage2_boot.sh requires no changes.

5. Decision: COM binary extracted with `llvm-objcopy -O binary` from a linked ELF.
   Reason: allows using the C toolchain + linker script to position COM code at 0x600000 without writing raw assembly.
   Impact: COMs can be written in C; the ELF intermediate is kept in build/ for debugging.

## ABI/contract changes
1. `handoff_v0_t` extended: `com_phys_base` (u64), `com_phys_size` (u64) appended after `flags`. Old stage2 builds reading `handoff_v0_t` will read zero for these fields (safe, `HANDOFF_V0_VERSION` is still 0 — future version bump needed if breaking change is made).
2. New public protocol: `boot/proto/services.h` defines `ciuki_services_t` and `com_entry_t`.
3. COM calling convention: `void com_main(void *boot_info, void *handoff, ciuki_services_t *svc)`.

## Tests executed
1. `make clean && make`
   Result: PASS — zero warnings, `build/INIT.COM` produced.

2. `make test-stage2`
   Result: PASS — all 14 required patterns found, all 4 forbidden patterns absent.

## Current status
1. Shell has a `run` command; typing it calls INIT.COM via the service table.
2. INIT.COM clears the screen and prints a hello banner using stage2's framebuffer console.
3. After COM returns, the shell prompt reappears — no hang, no crash.
4. Boot tests unaffected (COM is optional; test runs without it).

## Risks / technical debt
1. Single fixed load address (0x600000): cannot load two COMs simultaneously.
2. No size check before jumping: if COM binary is corrupt, stage2 will fault.
3. `HANDOFF_V0_VERSION` is still `0` despite struct extension — should be bumped to `1` when a breaking ABI change is made.
4. COM stack is stage2's own stack — no isolation; a misbehaving COM can corrupt stage2.

## Next steps (recommended order)
1. Add `dir` command backed by FAT12 reader to list files on the boot device.
2. Implement `run <name>` with a name-to-sector map or FAT12 path lookup.
3. Add COM size guard: reject if `com_phys_size == 0` or unreasonably large.
4. Bump `HANDOFF_V0_VERSION` to 1 and add a version check in stage2_main.

## Notes for Claude Code
- The INIT.COM load block in loader.c must stay **before** `build_bootstrap_paging` and `acquire_memory_map` — both UEFI Boot Services calls (`read_file`, `AllocatePages`) must happen before `ExitBootServices`.
- Do not move the `AllocateAddress` call for INIT.COM inside the QEMU boot test; the test runs without a COM file on the image and must continue to pass.
- If the COM load address (0x600000) is changed, update the linker script `com/hello/linker.ld` and the loader constant together.
