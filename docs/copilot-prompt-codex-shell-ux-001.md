# Prompt For Assigned Agent - SHELL-UX-001

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-001 origin/main
```

You are implementing a user-visible CiukiOS shell UX upgrade in the stage2 shell.

Read first:
1. `CLAUDE.md`
2. `docs/copilot-task-codex-shell-ux-001.md`
3. `stage2/src/shell.c`
4. relevant keyboard/video headers and implementation files

Your mission is to improve the shell in three concrete ways:

1. Direct execution by program name
The user should be able to type `CIUKEDIT`, `CIUKEDIT.COM`, `DOOM`, `DOOM.EXE`, etc. directly at the shell prompt without needing `run` first. If the command is not a builtin, the shell must attempt DOS-like executable resolution and launch the target if found. If not found, print one deterministic DOS-like error message. Keep builtin commands and the existing `run` command working.

2. Command history on arrow keys
Add classic shell history navigation with up/down arrows. Recalled commands must redraw cleanly on the current prompt line and remain editable. Use a fixed-size bounded history buffer and keep behavior deterministic.

3. Visual readability cleanup
Improve shell readability and reduce the current cramped/chaotic feeling. Focus on prompt redraw correctness, clean separation between command output and the next prompt, and preventing stale characters or malformed line transitions. Keep the style DOS-like and conservative.

Hard requirements:
1. Preserve backwards compatibility for existing shell builtins and DOS runtime behavior.
2. Prefer minimal, coherent changes in existing shell code instead of scattering shell UX logic across unrelated subsystems.
3. Add deterministic markers/logging when useful for direct-exec debugging.
4. Do not bump project version.
5. Do not commit on `main`.

Implementation guidance:
1. Use `stage2/src/shell.c` as the main integration point.
2. Reuse existing path canonicalization and launch code instead of cloning it.
3. For direct-exec resolution, prefer deterministic suffix probing order: exact target, then `.COM`, then `.EXE`, then `.BAT`.
4. Builtins should still win over same-named binaries unless you find an existing explicit contrary rule.
5. For history, implement a fixed ring buffer and robust line redraw logic.
6. If the current keyboard path exposes scan/extended keys, integrate there instead of inventing a fake abstraction.
7. If fully automated testing for arrow-key history is difficult in the current harness, add what deterministic coverage you can and document the remaining manual validation clearly.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. Any new or updated validation you add for direct-exec behavior

Deliverables:
1. Working code on the dedicated branch
2. Any minimal docs/test updates needed
3. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-001.md`

In the final handoff, explicitly report:
1. exact direct-exec precedence and fallback order
2. chosen not-found message
3. history capacity and duplicate policy
4. readability/prompt redraw changes
5. automated vs manual validation performed
