# Handoff: SHELL-UX-002 — Inline Editing, Tab Completion, Discoverability

**Date:** 2026-04-18
**Branch:** `feature/copilot-codex-shell-ux-002`
**Based on:** `origin/main` (post SHELL-UX-001 merge, commit 258b576)

## Context and Goal

Implement the second shell UX task pack (SHELL-UX-002) with four deliverables:
- D1: Inline cursor editing (Left/Right/Home/End/Delete within the input line)
- D2: Tab completion for builtins, COM catalog programs, and FAT CWD files
- D3: `history` and `which`/`where` commands for discoverability
- D4: Updated help text documenting all new editing keys and commands

## Files Touched

| File | Change |
|------|--------|
| `stage2/include/keyboard.h` | Added `STAGE2_KEY_HOME` (0x84), `STAGE2_KEY_END` (0x85), `STAGE2_KEY_DEL` (0x86) |
| `stage2/src/keyboard.c` | Added extended scan code mappings: 0x47→HOME, 0x4F→END, 0x53→DELETE |
| `stage2/src/shell.c` | Major changes — see below |

### shell.c Changes

1. **Line editing helpers** (after history ring buffer):
   - `shell_line_redraw_tail()` — redraws from position to end, erases stale chars, repositions cursor
   - `shell_line_full_redraw()` — CR + prompt + full line + cursor repositioning
   - `shell_video_write_dec32()` — decimal number writer for video output

2. **`history` command** (`shell_cmd_history()`):
   - Prints numbered history entries (1-based) from oldest to newest
   - Handles both full and partial ring buffer cases

3. **`which`/`where` command** (`shell_cmd_which()`):
   - Resolution order: builtins → COM catalog (with .COM/.EXE suffix probing) → FAT files (with .COM/.EXE/.BAT probing)
   - Reports location type: "shell builtin", "COM catalog (memory-resident)", or "FAT file (path)"

4. **Tab completion engine**:
   - `shell_complete_ctx_t` — candidate collector (max 64 entries)
   - `shell_complete_builtins()` — matches ~30 builtin names
   - `shell_complete_catalog()` — matches COM catalog entries
   - `shell_complete_fat_cwd()` — matches FAT directory entries via `fat_list_dir` callback
   - `shell_complete_common_prefix()` — finds longest common prefix
   - `shell_do_tab_complete()` — main completion logic:
     - Single match or common prefix extension → inline expand
     - Ambiguous → list candidates + redraw prompt
     - Command position: searches builtins + catalog + FAT
     - Argument position: searches FAT only

5. **Rewritten input loop** (`stage2_shell_run()`):
   - New `cursor` variable tracks position within line (0..line_len)
   - LEFT/RIGHT: move cursor with `\b` / print-char-under-cursor
   - HOME: cursor to 0; END: cursor to line_len
   - DELETE: remove char at cursor, shift tail left, redraw
   - Backspace: remove char before cursor, shift tail left, redraw
   - Tab: invokes tab completion engine
   - Printable chars: insert at cursor (shift tail right if mid-line)
   - History recall: properly moves cursor to end before clearing

6. **Updated `shell_execute_line()` dispatch**: Added `history` and `which`/`where` handlers.

7. **Updated `shell_print_help()`**: Added `history` and `which` commands, added "Editing keys" section documenting Left/Right/Home/End/Delete/Backspace/Tab/Up/Down.

## Decisions Made

- Tab completion uses a static 64-entry candidate buffer (`g_complete_ctx`) to avoid dynamic allocation
- Completion in command position searches builtins + COM catalog + FAT; in argument position only FAT
- `which` checks builtins using a lowercase comparison (name is uppercased, builtins are lowercase)
- History command uses 1-based numbering for user-friendliness
- `where` is an alias for `which` (DOS convention)
- Extended key constants follow sequential numbering: HOME=0x84, END=0x85, DEL=0x86

## Validation Performed

- `make clean all` — zero warnings, zero errors
- QEMU tests (`make test-stage2`) could not complete due to known serial capture infrastructure limitation (same as SHELL-UX-001)

## Risks and Next Steps

- **No runtime validation**: QEMU serial capture is unavailable on this host, so cursor rendering and tab completion have not been tested interactively. Manual QEMU testing recommended.
- **Multi-line wrapping**: If the prompt + input exceeds screen width, cursor math may break. Not addressed in this pack.
- **Tab completion performance**: FAT directory enumeration on large directories could be slow. No caching implemented.
- **Next**: SHELL-UX-003 could add Ctrl+A/Ctrl+E (emacs bindings), Ctrl+K (kill to end), or filename-aware completion with path separators.
