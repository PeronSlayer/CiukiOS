# Copilot Codex Task Pack - SHELL-UX-002 (Shell Productivity and Line Editing)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-002 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Deliver a second wave of shell improvements for CiukiOS focused on productivity, discoverability, and more complete DOS-like line editing.

SHELL-UX-001 already covered direct execution by bare program name, command history on arrows, and basic readability cleanup. This task must build on that baseline rather than redoing it.

## Scope (4 tasks)

### D1) Inline Cursor Editing
Extend the shell input editor so the current command line supports more complete interactive editing.

Required behavior:
1. Left arrow moves the cursor one character left within the current line.
2. Right arrow moves the cursor one character right within the current line.
3. Home jumps to start of editable input.
4. End jumps to end of current input.
5. Backspace deletes the character to the left of the cursor.
6. Delete removes the character under the cursor.
7. Inserted printable characters should be placed at the cursor position, shifting the tail right rather than only appending.
8. Redraw logic must remain stable and not leave stale characters on screen.

Constraints:
1. Stay within fixed `SHELL_LINE_MAX` bounds.
2. Preserve existing history behavior from SHELL-UX-001.
3. Do not regress prompt rendering or command submission.

### D2) Tab Completion
Add minimal, deterministic tab completion to reduce typing friction.

Required behavior:
1. Pressing Tab attempts to complete the current token.
2. Completion should work at least for:
   - builtin shell commands
   - runnable program names resolvable by the shell
   - optionally FAT file names in current directory if practical
3. If exactly one completion exists, fill it in-place.
4. If multiple completions share a longer common prefix, extend to that common prefix.
5. If multiple completions remain ambiguous, print a compact list and redraw the prompt + current input cleanly.
6. Completion behavior must be deterministic in ordering.

Suggested ordering:
1. builtins
2. runnable programs
3. files/directories

### D3) Discoverability Commands
Add one or two lightweight shell commands that make the new execution model easier to understand.

Preferred options:
1. `history` command:
   - prints recent command history in order
2. `which X` or `where X`:
   - explains how a token would resolve
   - identifies builtin vs `.COM` vs `.EXE` vs `.BAT`

Minimum requirement:
1. Implement at least one of `history` or `which/where`.
2. If both are straightforward, implement both.
3. Output must be deterministic and concise.

### D4) Help / UX Polish
Update shell help and any relevant docs so the user can discover the newer shell capabilities.

Required behavior:
1. `help` must mention:
   - direct program launch
   - history arrows
   - any new line-editing keys supported
   - tab completion if implemented
   - new discoverability command(s)
2. Keep help compact and readable.
3. If command semantics changed, update a durable doc or handoff accordingly.

## Recommended Files To Inspect First
1. `stage2/src/shell.c`
2. `stage2/include/shell.h`
3. `stage2/src/keyboard.c`
4. `stage2/include/keyboard.h`
5. `stage2/src/video.c`
6. `stage2/include/video.h`

## Acceptance Criteria
The task is complete when all of the following are true:
1. The shell supports true in-line cursor movement/editing with Left/Right/Home/End/Delete in addition to existing input behavior.
2. Tab completion works deterministically for at least builtins and runnable programs.
3. At least one discoverability command (`history`, `which`, or `where`) is implemented and useful.
4. Prompt redraw remains visually stable after editing and completion.
5. Existing direct execution and command history continue to work.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. Add or update at least one deterministic validation path for new shell editing/completion behavior if feasible.

If some interaction behavior cannot be fully automated with the current harness, document exact manual validation performed.

## Deliverables
1. Code changes for shell line editing and completion.
2. Minimal doc/help updates.
3. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-002.md`

## Final Handoff Must Include
1. Files changed.
2. Supported editing keys and final behavior.
3. Completion sources and precedence.
4. New command(s) added and their output model.
5. Test results.
6. Remaining follow-up ideas.
