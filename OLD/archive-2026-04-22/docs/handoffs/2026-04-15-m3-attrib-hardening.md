# Handoff - M3 Attribute Support + C1 Filesystem Hardening

## Context and Goal
Branch: `feature/claude-m3-fat-io-hardening`

Two briefs executed: **Task C1** (filesystem hardening edge cases) and **Task C2**
(DOS-like `attrib` command + read-only/archive enforcement).

## Files Touched
1. `stage2/src/fat.c` — added `fat_set_attr(path, attr)`
2. `stage2/include/fat.h` — declared `fat_set_attr`
3. `stage2/src/shell.c`
   - `shell_attrib` — new command (display and toggle R/H/S/A bits)
   - `shell_rename` — added cross-directory rename guard
   - `shell_copy` — added same-src/dst detection
   - `shell_move` — added same-src/dst detection
   - Updated `shell_print_help` and `shell_execute_line` dispatch
4. `stage2/src/stage2.c` — boot banner updated with `attrib`
5. `scripts/test_stage2_boot.sh` — updated required pattern
6. `scripts/test_fat_compat.sh` — updated pattern for new commands

## Decisions Made

### fat_set_attr
- Reads the current attribute byte to preserve DIRECTORY and VOLUME_ID bits
  (caller cannot corrupt the directory tree structure through this API).
- Uses `fat_locate_path_entry` for the R/W slot pointer — same mechanism as
  `fat_rename_entry` and `fat_delete_file`.

### shell_attrib
**Display mode** (`attrib <path>`):
- Shows four-character flag string: `R H S A` (space if clear, letter if set).
- Uses existing `fat_find_file` — no new FAT call needed.

**Modify mode** (`attrib +r|-r|+a|-a <path>`):
- Parses sign (`+`/`-`) and flag letter (`r`, `a`, `h`, `s`) from the first token.
- Applies bitmask to current attr byte, then calls `fat_set_attr`.
- DIRECTORY and VOLUME_ID bits are protected in `fat_set_attr` (silent mask).

### C1 edge-case fixes
| Case | Fix | Error message |
|---|---|---|
| `ren FILE.TXT ../OTHER` | Pre-check: `/` or `\` in new name | "Cross-directory rename not supported." |
| `copy A.TXT A.TXT` | `str_eq_nocase(src, dst)` before any I/O | "Source and destination are the same." |
| `move A.TXT A.TXT` | Same check before directory expansion | "Source and destination are the same." |

### Existing enforcement (unchanged, confirmed correct)
- `del` already checks `FAT_ATTR_READ_ONLY` → "Access denied (read-only)"
- `copy` destination already checks `FAT_ATTR_READ_ONLY` → "Destination is read-only"
- `move` destination already checks `FAT_ATTR_READ_ONLY` → "Destination is read-only"
- `fat_set_attr` does not add a read-only guard on itself (root attribute changes must
  be reachable; the shell `del`/`copy`/`move` guards are the right enforcement layer).

## Validation Performed
1. `make test-stage2` → **PASS** (all 19 patterns)
2. `make test-fallback` → **PASS**
3. `make test-fat-compat` → **PASS** (7/7)

## Risks / Open Points
1. `attrib` supports only one flag modifier per invocation (e.g. `attrib +r +a FILE.TXT`
   needs two calls). Multi-flag chaining (`attrib +r+a FILE.TXT`) is not parsed. Low
   priority given DOS ergonomics.
2. Hidden/system attribute display is implemented but there is no enforcement policy
   for `H`/`S` beyond display (brief marks this optional).

## Suggested Next Steps
1. Merge this branch into `main`.
2. Codex: complete Task X1 (EXE MZ dispatch) and X2 (INT 21h subset).
3. Next M3 optional: wildcard support for `dir`/`del` (`*`, `?`) if Codex M1 INT 21h
   surface stabilizes first.
