# Prompt For Assigned Agent - SHELL-UX-002

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-002 origin/main
```

You are implementing a second wave of shell UX improvements for the CiukiOS stage2 shell.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-codex-shell-ux-002.md`
5. `docs/handoffs/2026-04-18-copilot-codex-shell-ux-001.md`
6. `stage2/src/shell.c`
7. relevant keyboard/video headers and implementation files

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
SHELL-UX-001 already introduced direct execution by program name, command history on up/down arrows, and basic readability cleanup. Do not reimplement those features; build on them.

Your mission is to improve the shell in four concrete areas:

1. Inline cursor editing
Add real line editing with Left/Right/Home/End/Delete and insertion at cursor position. Keep redraw stable and bounded by existing shell limits.

2. Tab completion
Implement deterministic tab completion for at least builtin commands and runnable program names. If current architecture allows it cleanly, extend to FAT file names in the current directory as well. Single-match complete, common-prefix extend, ambiguous-match list + redraw.

3. Discoverability commands
Add at least one lightweight command that helps users understand the smarter shell behavior. Preferred options are `history` and `which`/`where`. If both are cleanly achievable, implement both.

4. Help polish
Update `help` so users can discover direct execution, history arrows, new line-editing keys, tab completion, and any newly added commands.

Hard requirements:
1. Preserve backwards compatibility for existing shell builtins and DOS runtime behavior.
2. Keep changes coherent and concentrated around the shell input/dispatch flow.
3. Do not bump project version.
4. Do not commit on `main`.
5. Reuse the SHELL-UX-001 behavior as the baseline rather than bypassing it.
6. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.

Implementation guidance:
1. Use `stage2/src/shell.c` as the main integration point.
2. Prefer a clear internal model for input buffer + cursor position + redraw rather than layering one-off conditionals.
3. Keep completion ordering deterministic.
4. If interaction automation is limited in the current harness, still add what deterministic coverage you can and document manual validation precisely.
5. Avoid overengineering: this is a productivity upgrade, not a shell rewrite.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. Any new or updated validation you add for editing/completion/discoverability behavior

Deliverables:
1. Working code on the dedicated branch
2. Any minimal docs/help/test updates needed
3. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-002.md`

In the final handoff, explicitly report:
1. supported editing keys and cursor behavior
2. completion sources and precedence
3. whether `history` and/or `which/where` were added
4. redraw/readability decisions
5. automated vs manual validation performed
