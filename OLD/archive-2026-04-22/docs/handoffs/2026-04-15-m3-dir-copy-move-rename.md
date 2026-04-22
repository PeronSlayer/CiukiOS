# Handoff - M3 Directory Management + Copy/Move/Rename

## Context and Goal
Branch: `feature/claude-m3-fat-io-hardening`

Two new briefs from Codex landed on the branch:
- `docs/collab/claude-branch-brief-m3-directories.md` → `mkdir`/`rmdir`
- `docs/collab/claude-branch-brief-m3-copy-move-rename.md` → `ren`/`move` (copy already done in prior commit)

This commit delivers all four features.

## Files Touched
1. `stage2/src/fat.c`
   - `fat_list_cb`: now filters `.` and `..` entries so `dir` does not show them
   - `fat_rename_entry(old_path, new_name)` — rename in same directory
   - `fat_create_dir(path)` — allocate cluster, init `.`/`..` entries, write dir entry
   - `fat_is_not_empty_cb` (static helper) — stops iteration on first non-dot entry
   - `fat_remove_dir(path)` — empty-check then free chain + mark deleted
2. `stage2/include/fat.h` — added `fat_rename_entry`, `fat_create_dir`, `fat_remove_dir`
3. `stage2/src/shell.c`
   - `shell_rename` — `ren`/`rename <old> <new>` (same-directory rename)
   - `shell_mkdir` — `mkdir`/`md <path>`
   - `shell_rmdir` — `rmdir`/`rd <path>` (refuses non-empty dirs)
   - `shell_move` — `move <src> <dst>` (copy+delete; if dst is a dir, src appended into it)
   - Updated `shell_print_help` and `shell_execute_line` dispatch
4. `stage2/src/stage2.c` — boot banner updated
5. `scripts/test_stage2_boot.sh` — updated required pattern for new banner
6. `scripts/test_fat_compat.sh` — updated pattern for new commands

## Decisions Made

### fat_create_dir
- Allocates exactly 1 cluster regardless of `sectors_per_cluster`.
- Zeros all sectors in the cluster, then writes `.` (cluster=self) and `..`
  (cluster=parent, 0 for root) as the first two 32-byte entries.
- Parent cluster for `..`: fixed-root dirs use cluster 0 (DOS convention for FAT12/16).
- Fails if any parent does not exist (no multi-level implicit create).
- Uses existing `fat_alloc_chain`, `fat_find_free_dir_slot`,
  `fat_write_dir_entry_slot` — no new I/O primitives needed.

### fat_remove_dir
- Iterates the directory with `fat_is_not_empty_cb`; stops on first
  non-dot entry and returns 0 (the shell maps this to "not empty" message).
- Uses existing `fat_free_chain` and `fat_locate_path_entry`.
- Does not handle cluster-0 root dirs (returns 0 immediately).

### fat_rename_entry
- Updates the 11-byte name field in the existing dir entry in-place.
  No cluster movement — O(1) aside from FAT table walks.
- Cross-directory rename is rejected (new_name must contain no path separators;
  validated implicitly by `fat_normalize_83_name`).
- Case-insensitive collision check uses `fat_find_in_dir` with the
  uppercased new name.

### shell_move
- Implemented as `fat_read_file` + `fat_delete_file(src)` + `fat_write_file(dst)`.
- If dst is an existing directory, appends the source filename into it.
- Cannot move directories (would require updating `..` in the moved subtree —
  deferred to a future step).

### fat_list_cb dot-entry filter
- Added guard: `entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.')`
- Applies only in subdirectory listings (root dirs on FAT12/16 never have
  dot entries); harmless for root.

## Validation Performed
1. `make test-stage2` → **PASS** (all 19 patterns)
2. `make test-fallback` → **PASS**
3. `make test-fat-compat` → **PASS** (7/7)

## Risks / Open Points
1. `fat_move` (shell_move) reads the entire source into the 128 KB shell
   buffer. Files larger than 128 KB cannot be moved (same limitation as
   `copy`). Increase `SHELL_FILE_BUFFER_SIZE` if needed.
2. No timestamp written in new directory entries. Acceptable until RTC is
   available.
3. Root directory cannot be expanded on FAT12/16: if the root dir is full,
   `fat_create_dir` returns 0 with a generic error. Not an issue for current
   test images.

## Suggested Next Steps
1. Merge this branch into `main` following the worksplit merge order.
2. Rebase on any new Codex M1 commits that may have landed on `main`.
3. Next M3 optional tasks:
   - `fat_write_file` overwrite-in-place mode (avoid delete+create round-trip)
   - Timestamp write using RTC (when available)
   - `move` for directories (requires `..` cluster update in moved subtree)
