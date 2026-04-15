# Handoff - M3 FAT I/O Hardening (Claude branch)

## Context and Goal
Branch: `feature/claude-m3-fat-io-hardening`

Parallel M3 workstream alongside Codex M1 (COM loader + PSP semantics).
Goal: harden DOS-like FAT/path/file behavior so that `dir`, `type`, `copy`, `del`
behave predictably against realistic edge cases.

## Files Touched
1. `stage2/src/fat.c` — new write-path helper functions + `fat_write_file`
2. `stage2/include/fat.h` — added `fat_write_file` declaration
3. `stage2/src/shell.c` — `write_decimal`, improved `dir` output, `copy` command,
   better error messages in `type`/`del`
4. `stage2/src/stage2.c` — boot banner updated with `copy`
5. `scripts/test_stage2_boot.sh` — updated required pattern for new banner
6. `scripts/test_fat_compat.sh` — new FAT compatibility test script
7. `Makefile` — added `test-fat-compat` target + `.PHONY`

## Decisions Made

### fat_write_file (fat.c)
New public function and 6 private helpers:
- `is_valid_83_char(u8)` — validates a single DOS 8.3 filename character.
- `fat_normalize_83_name(name, out11)` — converts "FILE.TXT" → 11-byte
  space-padded on-disk 8.3 form. Rejects names with invalid chars, name
  part > 8, extension > 3.
- `fat_find_free_cluster()` — walks FAT table from cluster 2 looking for
  value 0 (free). O(total_clusters) per call; acceptable for small images.
- `fat_alloc_chain(count, *first)` — allocates `count` linked clusters.
  Marks each as EOC immediately so subsequent find-free calls skip it,
  then links prev → cur. On failure, frees any partial chain.
- `fat_write_cluster_data(start, data, size)` — writes bytes into cluster
  chain; zero-pads the last partial sector to the sector boundary.
- `fat_find_free_dir_slot(dir, *slot)` — scans directory (fixed root or
  cluster chain) for a 0x00 or 0xE5 slot. Does not expand directories
  (acceptable for M3 scope).
- `fat_write_dir_entry_slot(slot, name83, attr, cluster, size)` — writes
  a full 32-byte FAT directory entry using the R/W cache.
- `fat_write_file(path, data, size)` — top-level public API: parse path,
  validate 8.3 name, resolve parent dir, refuse if name exists, allocate
  cluster chain, write data, create dir entry. Returns 0 on any failure.

Non-goal: `fat_write_file` does NOT overwrite existing entries; callers
must `fat_delete_file` first (as `shell_copy` does). This keeps the write
path simple and correct.

### shell.c improvements
- `write_decimal(u32)` — new helper used by `type` (file size in error
  messages) and `copy` (confirmation line).
- `shell_dir_fat_cb` rewritten: file sizes shown decimal, right-aligned in
  a 10-char field. Directories shown as `<DIR>`. Name column padded to 15
  chars for readability.
- `shell_copy(args)` — `copy <src> <dst>`. Reads source into the 128 KB
  shell file buffer, deletes destination if it exists (overwrite
  semantics), writes via `fat_write_file`. Distinct error messages for:
  source not found, source is dir, dest is dir, dest is read-only.
- `shell_type` — error messages now use `write_dos_path` (A:\-prefix),
  distinguish "not found" from "is a directory", show decimal file size in
  "too large" message.
- `shell_del` — pre-check via `fat_find_file` before calling
  `fat_delete_file`: gives distinct messages for "not found", "is a
  directory", "read-only".

### Boundary with Codex M1
Codex had already landed PSP-related changes (`SHELL_RUNTIME_PSP_SIZE`,
`shell_prepare_psp`, `ciuki_dos_context_t`, etc.) onto `main` before this
branch diverged. M3 changes do not touch any of those symbols.

No shared struct changes were needed; `fat_write_file` is a standalone
addition to the FAT layer with no impact on the loader ABI.

## Validation Performed
1. `make test-stage2` → **PASS** (all 19 required patterns matched, 5 forbidden absent)
2. `make test-fallback` → **PASS**
3. `make test-fat-compat` → **PASS** (7/7)

## Risks / Open Points
1. `fat_find_free_cluster` searches from cluster 2 on every call → O(n²)
   chain allocation. Acceptable now (images < 64 MB). Add a free-cluster
   hint or bitmap if performance becomes an issue.
2. Directory expansion not implemented: if the root directory or any
   subdirectory is completely full, `fat_write_file` returns 0. Not an
   issue for current image sizes.
3. No timestamp is written in the directory entry (bytes 14-21 remain 0).
   Fine for CiukiOS; add when real-time clock is available.

## Suggested Next Steps (M3 branch)
1. Merge Codex M1 branch into `main` first (as agreed in worksplit plan).
2. Rebase this branch onto updated `main`, resolve any ABI drift, merge.
3. Next M3 task: `fat_write_file` overwrite mode (skip delete step) or
   FAT32 directory expansion (add new cluster when dir is full).
4. Consider adding `rename` shell command backed by in-place dir-entry
   update (no cluster chain change needed).
