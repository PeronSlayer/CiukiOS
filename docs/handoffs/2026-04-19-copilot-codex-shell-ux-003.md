# Handoff: SHELL-UX-003 — Path-Aware Execution, Editing Shortcuts, Resolver Introspection

**Date:** 2026-04-19
**Branch:** `feature/copilot-codex-shell-ux-003`
**Base:** `origin/main` (bc9bdd9)

## Context and Goal

SHELL-UX-003 is the third shell enhancement task pack, building on UX-001 (basic shell
input) and UX-002 (cursor editing, tab completion, history/which). This pack adds:

- D1: Path-aware command execution and tab completion
- D2: Advanced editing shortcuts (Esc, Ctrl+A/E/U/K/L)
- D3: Enhanced `which`/`resolve` introspection with serial markers
- D4: Deterministic resolver selftest (`stage2_shell_selftest_resolver`)

## Files Touched

| File | Changes |
|------|---------|
| `stage2/src/keyboard.c` | Ctrl key tracking (Left Ctrl scancode 0x1D, Right Ctrl E0+0x1D). When Ctrl held + letter, returns `base & 0x1F` (Ctrl+A=0x01, Ctrl+E=0x05, Ctrl+K=0x0B, Ctrl+L=0x0C, Ctrl+U=0x15). Ctrl check before Shift check for priority. |
| `stage2/include/shell.h` | Added `int stage2_shell_selftest_resolver(void);` declaration |
| `stage2/src/shell.c` | All four deliverables (see below) |
| `stage2/src/stage2.c` | Selftest call: `stage2_shell_selftest_resolver()` with PASS/FAIL serial output |

## D1: Path-Aware Execution and Tab Completion

### Execution
- `shell_try_direct_exec`: detects path separator (`/` or `\`) in token
- Serial markers: `[shell] direct-exec name=X resolve=path-aware` or `resolve=bare-name`
- `DIRECT_EXEC_TRY` macro: split catalog vs FAT probes with distinct serial markers
  including `class=catalog suffix=X` or `class=fat path=X suffix=X`
- Path resolution uses existing `build_canonical_path()` for both `/` and `\` separators

### Tab Completion
- `shell_fat_complete_ctx_t`: added `const char *dir_prefix` field
- `shell_fat_complete_cb`: prepends `dir_prefix` to candidates, appends `\` for directories
- `shell_do_tab_complete`: detects path separators in typed prefix:
  - **With path**: splits into `user_dir_prefix` (up to last sep) + `name_part` (after sep),
    resolves directory via `build_canonical_path`, lists that directory, uses `user_dir_prefix`
    as candidate prefix for correct common-prefix calculation
  - **No path**: original flat completion (builtins + catalog + FAT CWD)
- Serial marker: `[complete] path-aware dir=X name_part=X`

## D2: Advanced Editing Shortcuts

All implemented as `if (ascii == 0xXXU)` checks in the shell input loop, after the Tab
handler and before the printable-ASCII check:

| Shortcut | Code | Behavior |
|----------|------|----------|
| Esc | 0x1B | Clear entire line (move to end, then BS-space-BS all) |
| Ctrl+A | 0x01 | Move cursor to start of line |
| Ctrl+E | 0x05 | Move cursor to end of line |
| Ctrl+U | 0x15 | Clear from cursor to start (shift buffer, redraw tail) |
| Ctrl+K | 0x0B | Clear from cursor to end (`line_len = cursor`, redraw tail) |
| Ctrl+L | 0x0C | Clear screen (`shell_cls()`), redraw prompt + line with cursor positioning |

### Keyboard Driver Changes
- `CTRL_LEFT_BIT 0x10` added to shift state bits in `keyboard.c`
- Left Ctrl (scancode 0x1D) and Right Ctrl (extended 0xE0+0x1D) tracked
- When Ctrl held and letter key pressed: returns `base & 0x1F` (standard ASCII control codes)
- Ctrl processing placed before Shift processing for priority

## D3: Enhanced `which` / `resolve` Command

- `shell_cmd_which`: detects path component in name
  - Builtin/catalog checks skipped for path-containing names
  - FAT check uses `suffixes_path[]` (includes empty suffix for exact match)
    vs `suffixes_bare[]` for non-path names
  - Shows `[suffix=X]` in output when suffix was appended
- Rich serial markers: `[which] probe name=X mode=X`,
  `[which] resolved class=X target=X`, `[which] not-found name=X`
- `resolve` added as alias: dispatched in `shell_execute_line`
- `resolve` added to tab-complete builtins list and which builtins list

## D4: Resolver Selftest

`stage2_shell_selftest_resolver()` — 7 test cases for `build_canonical_path`:

1. Bare name → `/EFI/CIUKIOS/HELLO.COM`
2. Backslash relative path → `/EFI/CIUKIOS/SUBDIR/APP.COM`
3. Dot-slash → `/EFI/CIUKIOS/APP.COM`
4. Parent traversal → `/EFI/APP.COM`
5. Absolute path → `/EFI/TEST.COM`
6. Empty input → `g_shell_cwd`
7. Forward slash relative → `/EFI/CIUKIOS/SUB/FILE.EXE`

Each failure emits serial diagnostic with actual value. Called from `stage2_main()` after
dosrun status path, with `[selftest] resolver PASS` or `FAIL count=N` serial output.

## Decisions Made

1. **Ctrl key priority**: Ctrl checked before Shift in keyboard decoder — if both are held,
   Ctrl wins (standard terminal behavior)
2. **No conflict Esc vs Ctrl+A**: Esc is scancode 0x01 → ASCII 0x1B; Ctrl+A is scancode
   0x1E with Ctrl held → ASCII 0x01. Different code points.
3. **Path-aware tab completion**: `dir_prefix` field in `shell_fat_complete_ctx_t` ensures
   full-path candidates are produced for correct common-prefix calculation
4. **Catalog lookup for path tokens**: harmlessly falls through (catalog has simple names only)

## Validation Performed

- **Build**: `make clean all` — zero errors, zero warnings
- **QEMU tests**: `make test-stage2` attempted but BLOCKED by host infrastructure
  (serial capture unavailable on CachyOS Wayland host — known limitation from UX-002)
- **Code review**: grep-verified all 4 deliverables present with correct patterns

### Deterministic Evidence (non-QEMU)
- All COM/EXE binaries built successfully
- Selftest function compiles and is linked into stage2
- Serial marker strings grep-verified in compiled source
- Help text updated with all new shortcuts and resolve alias

## Risks and Next Steps

### Risks
- Ctrl+L (clear screen) depends on `shell_cls()` and prompt redraw — no QEMU confirmation
  that cursor positioning after clear is pixel-perfect
- Path-aware tab completion for deeply nested directories not tested beyond compile

### Next Steps
- Commit and push branch
- QEMU runtime validation when serial capture is available
- Consider UX-004: piping, redirection, or batch file support
