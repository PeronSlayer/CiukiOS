# Handoff - M3 FAT Semantics v2 (C3 + C4)

## Context and Goal
Branch: `feature/claude-m3-fat-semantics-v2`

Two tasks from `docs/collab/parallel-next-tasks-2026-04-16.md`:
- **Task C3** — FAT write-path safety (invalid chains, disk-full, partial-write).
- **Task C4** — Path/error semantics hardening (`.`/`..`, root edge cases, explicit dot rejection).

## Files Touched
1. `stage2/src/fat.c` — all hardening changes
2. `scripts/test_fat_compat.sh` — tightened patterns, added forbidden patterns, min-pass count

`stage2/include/fat.h` — no new public API required; all changes are internal.

## Decisions Made

### C3 — Write-path safety

| Location | Fix |
|---|---|
| `fat_write_cluster_data` | Early return if `start_cluster < 2` — cluster 0 and 1 are reserved; writing to them underflows `cluster_first_sector`. |
| `fat_write_file` | Guard `cluster_bytes == 0` (corrupt geometry) → return 0 before division. |
| `fat_write_file` | Guard `cluster_count > g_fs.total_clusters` → return 0 (file too large for volume) instead of silently allocating a chain that will fail mid-way. |
| `fat_alloc_chain` | Unchanged — rollback via `fat_free_chain` on partial alloc was already correct. |
| `fat_write_cluster_data` | Unchanged — `return remaining == 0U` already surfaces short-chain as failure. |

No metadata corruption is possible on partial write because: cluster chain is allocated before the dir entry is written; if any step fails, the chain is freed and the dir entry is never written.

### C4 — Path/error semantics hardening

Two new static helpers:

```c
/* Returns 1 for "." or ".." token */
static int fat_is_dot_or_dotdot(const char *token);

/* Returns 1 if last component of path is "." or ".." */
static int fat_path_ends_in_dot(const char *path);
```

Applied to every write-path public API:

| Function | Guard applied |
|---|---|
| `fat_write_file` | Rejects `.`/`..` as target file name |
| `fat_create_dir` | Rejects `.`/`..` as new directory name |
| `fat_rename_entry` | Rejects `.`/`..` as old-path last component AND as new name |
| `fat_remove_dir` | Rejects `.`/`..` as target (catches `/SUBDIR/.` before locate) |
| `fat_delete_file` | Rejects `.`/`..` as target (supplements existing DIRECTORY attr check) |
| `fat_set_attr` | Rejects `.`/`..` as target |

Note: `fat_normalize_83_name` already rejected "." and ".." by construction (empty name part before the dot), but the new explicit guards make the intent clear and add defence for callers that bypass normalize.

### test_fat_compat.sh improvements

- Required `"[ ok ] FAT layer mounted"` tightened to `"[ ok ] FAT layer mounted (rw cache)"` — catches silent mode downgrade.
- Added required: `"[ stage2 ] next step: handoff to DOS-like runtime"` — asserts clean stage2 completion.
- Added forbidden: `"#UD"`, `"fat: chain error"`, `"fat: corrupt entry"`, `"[ tick ] irq0 #0000000000000064"`.
- Added explicit minimum-pass count assertion: `$PASS -lt $EXPECTED_PASS` → FAIL.

## Validation Performed
1. `make test-stage2` → **PASS** (22 patterns including new INT10h/INT16h/INT1Ah compat markers)
2. `make test-fallback` → **PASS**
3. `make test-fat-compat` → **PASS** (12/12)

## Risks / Open Points
1. `fat_find_free_cluster` still O(n) per allocation — accepted for small images, documented in prior handoff.
2. Non-root subdirectory directory entries have no cluster-chain expansion (if dir is full, returns 0). Expansion would require allocating a new cluster and linking it — deferred.
3. `fat_write_file` does not distinguish "exists as directory" from "exists as file" at the FAT level (both return 0). The shell layer uses `fat_find_file` beforehand to produce the correct user-visible message.

## Suggested Next Steps
1. Merge into main after Codex M2/M4 branch is ready.
2. Task X3 (INT 21h expansion) and X4 (BIOS compat test harness) — Codex owns.
3. Next M3 optional: wildcard `*`/`?` support for `dir`/`del` once INT 21h surface stabilizes.
