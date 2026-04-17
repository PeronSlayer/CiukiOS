# Copilot Codex Task Pack - SHELL-UX-001 (DOS-like Shell UX and Visual Cleanup)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-001 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Improve the CiukiOS stage2 shell so it behaves more like classic DOS for day-to-day interactive use, while also making the shell output more readable and visually less chaotic.

This task is intentionally user-visible and workflow-oriented. After it lands, the shell should feel materially better for repeated interactive use, not just technically more complete.

## Current Relevant Context
1. The shell currently lives primarily in `stage2/src/shell.c`.
2. Today the user typically launches binaries through `run X.COM` or `run X.EXE`.
3. The shell already has path normalization, COM/MZ/BAT launch support, prompt rendering, and keyboard input primitives.
4. The shell already exposes DOS-like runtime behavior and must remain backwards compatible.
5. Direct execution by bare program name, command history on arrow keys, and output readability polish are not yet at the level desired.

## Scope (3 main tasks + validation)

### D1) Direct Program Launch By Bare Name
Implement DOS-like direct execution so the user can type a program name directly instead of always writing `run`.

Examples of expected behavior:
- `CIUKEDIT.COM` launches directly
- `CIUKEDIT` launches directly if the shell can resolve it to a runnable target
- `DOOM.EXE` launches directly
- `DOOM` launches directly if the shell can resolve it
- `AUTOEXEC.BAT` or another `.BAT` can launch directly if supported by current runtime rules

Required behavior:
1. Preserve the existing `run` command fully; it must continue working.
2. Add a direct-exec fallback in the shell command dispatch path:
   - if the first token is not a builtin shell command,
   - attempt to resolve it as an executable target in DOS-like order.
3. Resolution rules should be deterministic and documented. Preferred order:
   - exact path/name if already includes supported suffix
   - `.COM`
   - `.EXE`
   - `.BAT`
4. Reuse current path/canonicalization logic where possible.
5. If resolution succeeds, execute with the remaining tokens as the argument tail.
6. If resolution fails, print a clear DOS-like error line, for example:
   - `Bad command or file name`
   - or `Program or command not found`
   Choose one deterministic wording and use it consistently.
7. Emit deterministic serial/runtime markers for direct-exec success/failure, distinct enough to debug launch-path issues.

Constraints:
1. Do not break builtin commands such as `help`, `dir`, `type`, `copy`, `run`, `desktop`, `vmode`, etc.
2. Builtins must still take precedence over same-named binaries unless there is already an explicit policy otherwise.
3. Do not introduce ambiguous behavior silently; document final precedence in the handoff.

### D2) Command History With Arrow Keys
Add classic shell history navigation so pressing arrow keys recalls previous commands.

Required behavior:
1. Up arrow shows older commands.
2. Down arrow shows newer commands and eventually returns to the editable empty line.
3. Recalled commands must appear on the current prompt line, replacing the current input buffer visibly.
4. Editing a recalled command must work naturally with existing printable/backspace behavior.
5. History should ignore empty/whitespace-only submissions.
6. Consecutive duplicate commands should preferably be coalesced, unless existing architecture makes that too invasive.
7. Keep the implementation bounded and deterministic with a fixed-size ring buffer.

Recommended practical limits:
1. 16 to 32 history entries.
2. Reuse `SHELL_LINE_MAX` or compatible limits for stored lines.

Important technical note:
1. Arrow keys may currently arrive through keyboard scan/extended-key paths rather than plain ASCII.
2. The implementation should use the real shell input loop, not fake it through INT 21h buffered input.
3. Make sure prompt redraw is stable and does not leave stale characters on screen.

### D3) Visual Readability / Shell Presentation Cleanup
Improve the shell’s visual readability. The current shell output feels too dense and chaotic.

Goal:
Make the shell feel cleaner and more legible without turning it into a GUI or breaking the current text-mode workflow.

Required improvements:
1. Improve prompt/input redraw behavior so edited or recalled lines do not smear visually.
2. Reduce the feeling of lines being “stuck together” during normal shell interaction.
3. Ensure command output and next prompt have clean separation.
4. Make command errors, normal output, and prompt transitions easier to visually parse.

Suggested implementation directions (choose the minimal coherent set):
1. Standardize blank-line policy around command execution and prompt return.
2. Ensure commands that do not end with newline do not corrupt the next prompt line.
3. Add a small shell helper for clean line termination / prompt reset instead of ad hoc writes.
4. If text-mode cursor handling is part of the problem, tighten cursor bookkeeping rather than layering hacks.
5. Keep the look DOS-like and restrained; this is a readability polish task, not a redesign.

Non-goals:
1. No color theme system unless trivially safe and already supported.
2. No windowed UI.
3. No autocomplete in this task.

## Recommended Files To Inspect First
1. `stage2/src/shell.c`
2. `stage2/include/shell.h`
3. `stage2/src/keyboard.c`
4. `stage2/include/keyboard.h`
5. `stage2/src/video.c`
6. `stage2/include/video.h`
7. Any existing shell tests or scripts that validate command execution and prompt behavior.

## Acceptance Criteria
The task is complete when all of the following are true:
1. Typing a known binary name directly from the shell launches it without needing `run`.
2. Typing an unknown command produces one deterministic “not found” style error.
3. Arrow-up/arrow-down navigate command history reliably.
4. Recalled commands redraw correctly and remain editable.
5. Prompt/output spacing is visibly cleaner and does not regress normal shell usage.
6. Existing `run` behavior still works.
7. Existing DOS program execution path remains functional.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. Add or update at least one deterministic validation path for direct-exec resolution.
5. Add or update at least one deterministic validation path for history/redraw behavior if feasible within current test harness limits.

If fully automated history testing is not practical in the current harness, document exactly what was tested manually and why the automation gap remains.

## Deliverables
1. Code changes for shell dispatch, history, and redraw/readability.
2. Minimal documentation updates if command behavior changes.
3. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-001.md`

## Final Handoff Must Include
1. Files changed.
2. Direct-exec resolution order implemented.
3. Final not-found wording chosen.
4. History buffer policy and limits.
5. Visual/readability decisions made.
6. Test results.
7. Remaining gaps or follow-up ideas.
