# Handoff: SHELL-UX-001 — Shell UX Improvements

**Date:** 2026-04-18
**Branch:** `feature/copilot-codex-shell-ux-001`
**Task Pack:** `docs/copilot-task-codex-shell-ux-001.md`

## Context and Goal

Implement three shell UX improvements for CiukiOS stage2 shell:

1. **D1 — Direct Execution by Name:** Users can type a program name (e.g., `HELLO` or `CIUKEDIT`) without the `run` prefix, and the shell resolves it automatically.
2. **D2 — Command History:** UP/DOWN arrow keys navigate a 32-entry ring buffer of previously entered commands.
3. **D3 — Visual Readability:** Blank line before each prompt for visual separation; startup tip text properly spaced.

## Files Touched

- `stage2/src/shell.c` — All changes (+223 lines, -2 lines)

## Implementation Details

### D1: Direct Execution (`shell_try_direct_exec`)

- Resolution order: builtins → exact suffix (if user typed `.COM`/`.EXE`/`.BAT`) → `.COM` → `.EXE` → `.BAT`
- Extracts uppercase first token, uses `get_arg_ptr()` for argument tail
- Probes COM catalog via `shell_find_com()`, then FAT via `fat_find_file()`
- On match, delegates to `shell_run()` (for `.COM`) or `shell_run_from_fat()` (for `.EXE`)
- Emits serial markers: `[shell] direct-exec resolved=NAME` and `[shell] direct-exec notfound name=NAME`
- Not-found message changed from "Unknown command" to "Bad command or file name" (DOS-like)

### D2: Command History Ring Buffer

- `SHELL_HISTORY_MAX = 32`, each entry up to `SHELL_LINE_MAX` (128) chars
- Ring buffer with `g_shell_history_head` (next write slot) and `g_shell_history_count`
- `shell_history_push()`: skips empty/whitespace-only lines, coalesces consecutive duplicates
- UP arrow: saves current in-progress line on first press, then recalls older entries
- DOWN arrow: recalls newer entries or restores saved in-progress line
- LEFT/RIGHT arrows: silently ignored (no cursor movement yet)
- History navigation resets on Enter

### D3: Visual Readability

- `video_putchar('\n')` before prompt after each command execution
- Startup tip text gets trailing `\n` for spacing

### Help Text

- Added 3 lines documenting direct-exec and UP/DOWN history

## Decisions Made

1. Direct-exec probes catalog + FAT existence before calling `shell_run`, rather than letting `shell_run` fail and produce its own error message. This gives cleaner control over the not-found message.
2. BAT file resolution via direct-exec checks FAT only (no catalog entry for .BAT).
3. History uses oldest-first indexing (0 = oldest). When ring buffer is not full, `idx = hist_nav` directly. When full, `idx = (head + hist_nav) % MAX`.
4. Not-found message follows DOS convention: "Bad command or file name".

## Validation Performed

| Test | Result | Notes |
|------|--------|-------|
| `make clean all` | **PASS** | Zero warnings with `-Wall -Wextra` |
| `make all` (incremental) | **PASS** | shell.c compiled and linked cleanly |
| `make test-stage2` | **INFRA** | Serial capture unavailable on host — not a code regression |
| `make test-dosrun-simple` | **INFRA** | Same serial capture issue — infrastructure limitation |

## Risks and Next Steps

1. **Serial capture:** QEMU tests cannot validate runtime behavior on this host. Should be validated on a host with working serial capture or interactively via `run_ciukios.sh`.
2. **LEFT/RIGHT arrows:** Currently ignored. Future task to add cursor movement within the input line.
3. **Tab completion:** Natural follow-up feature for direct-exec.
4. **PATH-like search:** Currently probes root directory only. Could add directory search path later.
