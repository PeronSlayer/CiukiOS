# Prompt For Assigned Agent - SHELL-UX-003

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-003 origin/main
```

You are implementing a third wave of shell UX improvements for the CiukiOS stage2 shell.

Read first:
1. `CLAUDE.md`
2. `docs/copilot-task-codex-shell-ux-003.md`
3. `docs/handoffs/2026-04-18-copilot-codex-shell-ux-001.md`
4. `docs/handoffs/2026-04-18-copilot-codex-shell-ux-002.md`
5. `stage2/src/shell.c`
6. relevant keyboard, FAT, and video headers / implementation files

Context:
SHELL-UX-001 already added direct execution by bare name, command history on arrows, and readability cleanup.
SHELL-UX-002 already added inline cursor editing, tab completion, and `history` / `which` / `where` style discoverability.
Do not reimplement those features. Build directly on them.

Your mission is to improve the shell in four concrete areas:

1. Path-aware execution and completion
Extend the resolver/completion path so the shell handles directory-prefixed runnable targets cleanly, not just flat names in the current directory. Reuse existing canonicalization and suffix probing logic. Completion should append a trailing `\\` for directories so navigation can continue.

2. Advanced editing shortcuts
Add a compact but valuable set of shortcuts: `Esc` to clear line, `Ctrl+A`, `Ctrl+E`, `Ctrl+U`, and `Ctrl+K`. Keep redraw stable and do not regress the existing editor behavior from SHELL-UX-002.

3. Resolver / completion introspection
Extend `which` / `where` or add a tightly scoped equivalent so the shell can explain how a token resolves, including builtin vs file target class and, when applicable, the final chosen suffix/path.

4. Deterministic validation hooks
The current host has incomplete QEMU serial capture for some runtime gates. Improve observability from the shell side by adding deterministic markers or selftests for the new resolver/completion logic. Prefer extending existing shell selftests or serial markers instead of inventing a large new framework.

Hard requirements:
1. Preserve backwards compatibility for existing shell builtins and DOS runtime behavior.
2. Keep changes coherent and centered around the shell input/resolution flow.
3. Keep DOS-like semantics; do not drift into Unix-style behavior.
4. Do not bump project version.
5. Do not commit on `main`.

Implementation guidance:
1. Use `stage2/src/shell.c` as the primary integration point.
2. Reuse existing completion and resolver helpers where possible instead of creating parallel logic.
3. Be explicit about precedence and fallback order for path-aware resolution.
4. If parent-directory traversal or some path form is unsafe in the current architecture, keep behavior deterministic and document the limitation instead of guessing.
5. If keyboard decoding needs additional control-key recognition, keep it minimal and document exact mappings.
6. Do not spend the task budget trying to fully solve host-specific QEMU capture issues; add deterministic evidence that helps validate behavior anyway.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. any new or updated deterministic validation you add for resolver/completion/editing behavior

Deliverables:
1. Working code on the dedicated branch
2. Any minimal docs/help/test updates needed
3. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-003.md`

In the final handoff, explicitly report:
1. path-aware resolution rules and suffix order
2. supported new editing shortcuts and exact behavior
3. how directory completion behaves
4. any `which` / `where` / `resolve` output changes
5. deterministic validation markers or selftests added
6. automated vs manual validation performed