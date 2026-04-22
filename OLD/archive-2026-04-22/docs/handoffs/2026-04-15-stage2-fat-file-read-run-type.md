# HANDOFF - stage2 filesystem integration step 2 (file read + shell type + run FAT fallback)

## Date
`2026-04-15`

## Context
User asked to start integrating a real filesystem workflow. Previous step already mounted FAT readonly and used it for `dir`. This step adds actual file-content reads and shell features that consume files from FAT.

## Completed scope
1. Extended FAT API with file read primitive:
   - `fat_read_file(path, out, out_capacity, out_size)`
2. Implemented cluster-chain file reads in `fat.c`:
   - uses `fat_find_file`
   - validates regular file (not directory)
   - copies sector data cluster-by-cluster from disk cache
   - guards against truncated chains / invalid clusters
3. Extended shell help and command set:
   - new command `type <file>`
4. Implemented `type` command:
   - FAT-backed file open/read
   - default path base `/EFI/CIUKIOS/` when relative name is used
   - supports absolute `/...` or `\...` input too
   - basic printable rendering (non-printables shown as `.`)
5. Upgraded `run` command with FAT fallback loading:
   - existing behavior unchanged: if COM is in handoff catalog, runs as before
   - new fallback: load `/EFI/CIUKIOS/<name>.COM` from FAT into fixed runtime addr `0x600000`, then execute
   - `run` without args: tries default preloaded COM, otherwise tries `INIT.COM` from FAT
6. Updated stage2 ready marker and test expectation to include `type` command.

## Touched files
1. `stage2/include/fat.h`
2. `stage2/src/fat.c`
3. `stage2/src/shell.c`
4. `stage2/src/stage2.c`
5. `scripts/test_stage2_boot.sh`

## Technical decisions
1. Decision: keep FAT reads readonly and in-memory via loader cache.
   Reason: no runtime block device driver yet.
   Impact: reliable first iteration, but bounded by cache window.

2. Decision: execute FAT-loaded COM at fixed runtime address `0x600000`.
   Reason: current COM binaries are linked for this base (flat binary, no relocations).
   Impact: simple compatibility with existing COM toolchain; long-term dynamic allocation/relocation still needed.

3. Decision: `type` uses a fixed shell buffer (`128 KiB`).
   Reason: avoid dynamic allocation and keep stack small.
   Impact: files larger than buffer are rejected with user-facing message.

4. Decision: path normalization is DOS-friendly but minimal.
   Reason: keep parser simple in this phase.
   Impact: no quoting/escaping or wildcard support yet.

## ABI/contract changes
1. No new boot ABI fields in this step.
2. New internal FS API function:
   - `int fat_read_file(const char *path, void *out, u32 out_capacity, u32 *out_size);`

## Tests executed
1. `make test-stage2`
   Result: PASS
2. `make test-fallback`
   Result: PASS

## Current status
1. Filesystem is now functionally integrated beyond listing:
   - `dir` lists FAT entries
   - `type` reads and prints FAT file content
   - `run` can execute COM from FAT even if not preloaded in catalog
2. Boot and fallback regression checks are still green.

## Risks / technical debt
1. FAT cache is still bounded (first 8 MiB from loader), so files outside cached region cannot be read yet.
2. COM runtime load address is fixed (`0x600000`).
3. No signature/validation on COM payload before execution.
4. No LFN support; 8.3 only.

## Next steps (recommended order)
1. Add a small file command set (`pwd`, `cd`, `type`, `dir` with optional path).
2. Add FAT cluster-chain streaming API (so large files can be consumed progressively).
3. Add explicit safety checks for COM header/size and isolate COM stack.
4. Replace cache-only disk model with runtime block read abstraction.

## Notes for Claude Code
- Keep FAT fallback in `run` as secondary path after catalog lookup.
- Preserve `0x600000` COM runtime base unless linker strategy changes.
- If you increase `type` capabilities, ensure non-printable handling remains safe for framebuffer output.
