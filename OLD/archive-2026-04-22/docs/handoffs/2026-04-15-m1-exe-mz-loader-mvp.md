# Handoff - M1 EXE MZ Loader MVP (Codex)

## Context and Goal
Branch: `feature/codex-m1-exe-mz-loader-mvp`

Continue Codex M1 scope after COM/PSP runtime work by adding an initial DOS `.EXE MZ` load path (parse + relocation + staging) without touching Claude's filesystem feature stream.

## Files Touched
1. `stage2/include/dos_mz.h` (new)
2. `stage2/src/dos_mz.c` (new)
3. `stage2/src/shell.c`

## What Changed
1. Added dedicated MZ module API (`dos_mz`):
- `dos_mz_parse(file, size, out)`
- `dos_mz_build_loaded_image(file_buf, file_size, load_segment, info_out, loaded_size_out, reloc_applied_out)`

2. MZ parser validates:
- `MZ` signature
- header size and bounds
- relocation table bounds
- entry offset inside module image

3. MZ load transform in-place:
- applies relocation entries with chosen load segment
- strips EXE header by moving module bytes to buffer base
- returns loaded image size and relocation count

4. Integrated into `run` flow in `shell.c`:
- `run` still executes COM directly via current runtime path.
- if input image is `MZ`, stage2 now:
  - parses + relocates + builds loaded image
  - computes runtime entry linear address
  - prints load diagnostics (`bytes`, `reloc`, `load_seg`)
  - exits with explicit message: runtime dispatch pending (16-bit execution path)

5. UX updates:
- `help` text now states `run` can load EXE.
- `ver` moved to `v0.3` with MZ loader MVP label.

## Validation Performed
1. `make -j4` -> PASS
2. `make test-stage2` -> PASS
3. `make test-fallback` -> PASS

## Boundaries Respected
1. No edits to FAT write/dir command scope owned by Claude.
2. No loader ABI break for existing COM path.
3. No merge of unrelated files.

## Risks / Open Points
1. This MVP does not execute 16-bit MZ runtime yet; it only performs loading and relocation prep.
2. Stack/segment register runtime emulation for EXE dispatch is still pending.
3. EXE relocation correctness should be validated with a deterministic MZ sample in future tests.

## Next Step
1. Implement 16-bit-compatible dispatch contract for loaded MZ entry (`CS:IP`, `SS:SP` semantics) or an explicit bridge layer.
2. Add a targeted test artifact to assert MZ relocation/output invariants automatically.
