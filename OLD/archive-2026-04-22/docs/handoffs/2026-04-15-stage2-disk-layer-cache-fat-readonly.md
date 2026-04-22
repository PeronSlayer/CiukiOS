# HANDOFF - stage2 disk layer v0 (loader cache + FAT readonly)

## Date
`2026-04-15`

## Context
After UI customization, user requested to continue with the disk layer. Since stage2 has no AHCI/IDE runtime driver yet, we implemented a first practical layer: loader-side raw disk cache passed via handoff, consumed by a stage2 FAT readonly parser.

## Completed scope
1. Extended `handoff_v0_t` with disk cache metadata:
   - `disk_cache_phys_base`
   - `disk_cache_byte_size`
   - `disk_cache_lba_start`
   - `disk_cache_lba_count`
   - `disk_cache_block_size`
   - `disk_cache_flags`
   - plus constant `HANDOFF_DISK_CACHE_MAX_BYTES` (8 MiB)
2. Added UEFI loader raw block caching:
   - open `EFI_BLOCK_IO_PROTOCOL` from current boot device
   - allocate below 4 GiB
   - read first N LBAs (up to 8 MiB) with `ReadBlocks`
   - populate handoff disk cache fields
3. Added stage2 disk cache module:
   - `stage2/include/disk.h`
   - `stage2/src/disk.c`
   - API to init/read/cache-check block data from handoff
4. Added stage2 FAT readonly module:
   - `stage2/include/fat.h`
   - `stage2/src/fat.c`
   - BPB parse + FAT type detection (FAT12/FAT16/FAT32)
   - directory iteration (short 8.3 entries, LFN skipped)
   - path traversal and `fat_list_dir` / `fat_find_file`
5. Wired stage2 boot flow:
   - `stage2_disk_init(handoff)`
   - `fat_init()`
   - serial markers for disk/FAT readiness
6. Updated shell `dir` behavior:
   - primary: list real files from `/EFI/CIUKIOS` via FAT layer
   - fallback: old COM catalog listing when FAT unavailable
7. Updated stage2 test expectations:
   - require disk cache + FAT mounted markers

## Touched files
1. `boot/proto/handoff.h`
2. `boot/uefi-loader/loader.c`
3. `stage2/include/disk.h` (NEW)
4. `stage2/src/disk.c` (NEW)
5. `stage2/include/fat.h` (NEW)
6. `stage2/src/fat.c` (NEW)
7. `stage2/src/stage2.c`
8. `stage2/src/shell.c`
9. `scripts/test_stage2_boot.sh`

## Technical decisions
1. Decision: cache raw disk blocks in loader before ExitBootServices.
   Reason: stage2 currently lacks a runtime storage driver.
   Impact: stage2 can still parse a real filesystem image using readonly cached sectors.

2. Decision: cap cache at 8 MiB (`HANDOFF_DISK_CACHE_MAX_BYTES`).
   Reason: safe memory footprint while covering FAT metadata + early data regions for this project image.
   Impact: not full-disk; future runtime I/O must fetch uncached LBAs.

3. Decision: FAT parser supports only short 8.3 names right now; LFN entries skipped.
   Reason: keep first disk layer small and robust.
   Impact: long filenames are invisible to `dir` until LFN support is added.

4. Decision: `dir` is FAT-first with COM-catalog fallback.
   Reason: immediate DOS-like behavior without losing previous working path.
   Impact: smoother migration from loader-side COM catalog to true runtime FS.

## ABI/contract changes
1. `handoff_v0_t` extended with disk cache metadata fields (appended).
2. New stage2 internal APIs:
   - `stage2_disk_*` in `disk.h`
   - `fat_*` in `fat.h`

## Tests executed
1. `make test-stage2`
   Result: PASS
2. `make test-fallback`
   Result: PASS

## Current status
1. Stage2 now mounts a readonly FAT layer from cached boot disk blocks.
2. Shell `dir` can list `/EFI/CIUKIOS` from actual FAT structures (not only COM handoff list).
3. Regression suites remain green.

## Risks / technical debt
1. Disk cache is partial (first 8 MiB), not an on-demand block driver.
2. No LFN support yet (8.3 only).
3. FAT writes are not implemented (readonly layer).
4. `HANDOFF_V0_VERSION` remains `0` despite struct growth (project policy so far, but worth revisiting).

## Next steps (recommended order)
1. Add `run <name>` load path from FAT file lookup (using `fat_find_file`) instead of only preloaded COM catalog.
2. Add file-content read API on top of FAT clusters (start readonly) to support commands like `type`.
3. Add LFN decoding for better external usability.
4. Replace cache-only disk model with true runtime block I/O layer in stage2.

## Notes for Claude Code
- Keep loader disk cache setup strictly before `ExitBootServices`.
- Preserve COM catalog fallback while FAT runtime path matures.
- If you increase cache size, re-check memory pressure and low-4G allocation behavior.
