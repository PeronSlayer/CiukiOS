# Local Git configuration and release hook

Repository-specific editor settings, assistant configuration, and hook
installations live in the root `.github/`, `.githooks/`, `.claude/`, `.codex/`,
and `.vscode/` directories. These directories are ignored so local workspace
state does not become part of the shared tree. Git's ignore rules only affect
untracked paths; already tracked entries must be removed from the index with
`git rm --cached`, which leaves their working copies in place. The migration
removes these paths from the current tree without rewriting earlier history.

The origin/main prerelease behavior remains available in each clone through
the tracked hook source at `scripts/hooks/pre-push`. Run
`bash scripts/install_release_push_hook.sh` to copy it into the ignored local
`.githooks/` directory, mark it executable, and set the repository-local
`core.hooksPath`. The installer leaves other local hook files alone and stops
if a different hook already occupies `pre-push` or a different
`core.hooksPath` is configured.

This follows Git's documented behavior: `.gitignore` does not affect files
already tracked, `git rm --cached` removes a path from the index while keeping
the working copy, and hooks are loaded from the directory named by
`core.hooksPath` when executable. References: [gitignore](https://git-scm.com/docs/gitignore),
[git-rm](https://git-scm.com/docs/git-rm), [githooks](https://git-scm.com/docs/githooks),
and [git-config `core.hooksPath`](https://git-scm.com/docs/git-config#Documentation/git-config.txt-corehooksPath).
