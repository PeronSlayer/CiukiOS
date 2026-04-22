# Copilot Codex Task Pack - SHELL-UX-003 (Path-Aware Shell Ergonomics and Deterministic Validation)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-shell-ux-003 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Deliver a third wave of CiukiOS shell improvements focused on three areas that are still limiting day-to-day use:

1. smarter path-aware execution and completion
2. more complete keyboard editing shortcuts
3. deterministic validation hooks that reduce reliance on fragile interactive capture

SHELL-UX-001 already introduced direct execution, basic history, and readability cleanup.
SHELL-UX-002 already introduced inline editing, tab completion, and `history` / `which` discoverability.
This task must extend those behaviors instead of redoing them.

## Scope (4 tasks)

### D1) Path-Aware Execution and Completion
Make shell execution and completion behave better when the user works with subdirectories instead of only flat names.

Required behavior:
1. Direct execution must continue working for bare names exactly as before.
2. Add support for deterministic resolution of runnable targets when the typed token already includes a path component.
3. At minimum, handle these forms cleanly if the underlying FAT path helpers already support them:
   - `SUBDIR\\APP`
   - `SUBDIR\\APP.COM`
   - `.\\APP`
   - `..\\APP` if parent traversal is already supported by current canonicalization rules
4. Reuse the existing shell path normalization and suffix probing logic rather than cloning a second resolver.
5. Tab completion must understand tokens that include directory prefixes.
6. If a completion result is a directory, append a trailing `\\` so the user can keep completing inside it.
7. Completion ordering must remain deterministic.

Constraints:
1. Do not silently change existing precedence between builtins and runnable files.
2. Do not invent Unix-style path rules; stay aligned with current DOS-like path semantics in CiukiOS.
3. If parent-directory traversal is unsafe or unsupported, document that clearly and keep behavior deterministic.

### D2) Advanced Editing Shortcuts
Add a small, coherent set of higher-value editing shortcuts so the shell feels less primitive during repeated interactive work.

Required behavior:
1. `Esc` clears the current input line and returns to a clean prompt line.
2. `Ctrl+A` moves to the start of input.
3. `Ctrl+E` moves to the end of input.
4. `Ctrl+U` clears from cursor back to the start of input.
5. `Ctrl+K` clears from cursor to the end of input.
6. Existing Left/Right/Home/End/Delete/Backspace/history behavior must keep working.
7. Redraw logic must remain bounded and must not leave stale characters on screen.

Recommended if cleanly achievable:
1. `Ctrl+L` for clear screen + prompt redraw.
2. simple word-wise cursor or delete motion, but only if it fits the current architecture naturally.

Constraints:
1. Do not add a full readline clone.
2. Keep the implementation concentrated in the existing shell input editor path.
3. If keyboard decoding needs new control-key mappings, keep them explicit and documented.

### D3) Completion and Resolver Introspection
The smarter shell needs a compact way to explain what it is doing.

Required behavior:
1. Extend `which` / `where` or add a closely related helper so the shell can report:
   - whether a token resolves as a builtin
   - whether it resolves through COM catalog / FAT file path
   - what final canonical target path or suffix was chosen
2. The output must stay concise and deterministic.
3. If path-aware completion is implemented, ambiguous completion listings should clearly distinguish files from directories when practical.

Suggested directions:
1. enrich `which`
2. or add `resolve <token>` if that is cleaner than overloading `which`

Non-goal:
1. Do not build a full debugger for the shell resolver.

### D4) Deterministic Validation Hooks
Current QEMU shell validation is partially blocked on host-specific serial capture behavior. Improve testability from inside the shell/runtime codebase.

Required behavior:
1. Add at least one deterministic validation surface for the new resolver/completion logic that does not depend purely on interactive manual typing.
2. Preferred approach: factor small resolver/completion helpers so they can be exercised by an existing selftest path or deterministic serial markers.
3. If shell selftests already exist, extend them rather than inventing a new framework.
4. At minimum, add serial/runtime markers that make it possible to distinguish:
   - direct bare-name resolution
   - path-aware resolution
   - chosen suffix / target class
   - completion ambiguity vs single-match completion
5. Update or add at least one script/test path if feasible without destabilizing the current harness.

Constraints:
1. Do not spend the whole task fighting host QEMU infrastructure.
2. The goal is to make shell behavior more observable and testable, not to redesign the whole test harness.

## Recommended Files To Inspect First
1. `stage2/src/shell.c`
2. `stage2/include/shell.h`
3. `stage2/src/keyboard.c`
4. `stage2/include/keyboard.h`
5. `stage2/src/fat.c`
6. `stage2/include/fat.h`
7. `docs/handoffs/2026-04-18-copilot-codex-shell-ux-001.md`
8. `docs/handoffs/2026-04-18-copilot-codex-shell-ux-002.md`
9. existing shell-related test scripts under `scripts/`

## Acceptance Criteria
The task is complete when all of the following are true:
1. The shell can resolve and complete runnable targets with directory prefixes in a deterministic DOS-like way.
2. Directory completion can continue naturally after appending a trailing `\\`.
3. `Esc`, `Ctrl+A`, `Ctrl+E`, `Ctrl+U`, and `Ctrl+K` work reliably on the interactive input line.
4. Existing shell editing/history/direct-exec behavior does not regress.
5. The shell exposes at least one clear deterministic validation path or marker set for the new resolver/completion behavior.
6. `help` or an equivalent discoverability surface documents the new shortcuts if user-visible behavior changed.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-dosrun-simple`
4. Any new or updated deterministic validation path added for resolver/completion/editing behavior

If host infrastructure still prevents full QEMU runtime confirmation, document exactly what was blocked and what deterministic evidence was still produced.

## Deliverables
1. Code changes for path-aware resolution/completion and advanced editing shortcuts.
2. Minimal help/docs updates if user-visible behavior changed.
3. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-shell-ux-003.md`

## Final Handoff Must Include
1. Files changed.
2. Path-aware resolution rules and suffix probing order.
3. Supported new editing shortcuts and final behavior.
4. Any resolver/completion introspection command changes.
5. Deterministic validation markers or selftests added.
6. Test results.
7. Remaining gaps or follow-up ideas.