# HANDOFF - GitHub Copilot (Claude Haiku 4.5) Onboarding

## Date
`2026-04-16`

## Context
Add a third coding agent (GitHub Copilot using Claude Haiku 4.5) to the CiukiOS collaborative workflow, with a clear and minimal operational context so it can contribute safely without breaking boot/runtime compatibility.

## Completed scope
1. Consolidated current project state after latest merges on `main`.
2. Captured architecture, active milestones, and guardrails for a new agent.
3. Defined collaboration expectations, ownership boundaries, and test gates for Copilot contributions.

## Touched files
1. `docs/handoffs/2026-04-16-github-copilot-haiku-onboarding.md`
2. `docs/collab/copilot-haiku-prompt.md`

## Technical decisions
1. Decision:
Use `main` as shared integration baseline; new work must happen on feature branches.
Reason:
Protects stability while allowing parallel throughput across Codex, Claude Code, and Copilot.
Impact:
Lower merge risk and easier review/rollback.

2. Decision:
Assign Copilot first to bounded, testable subsystems (scripts/docs/small compatibility deltas) before core loader ABI work.
Reason:
Fast onboarding with lower blast radius.
Impact:
Quicker productive output with reduced regression probability.

3. Decision:
Keep strict regression gates before merging any Copilot branch.
Reason:
Boot/runtime regressions are expensive in OS projects.
Impact:
Predictable integration quality.

## ABI/contract changes
1. None in this handoff (documentation/process only).

## Tests executed
1. Command:
`make test-stage2 && make test-fallback && make test-fat-compat && make test-int21`
Result:
PASS on current `main` baseline before this onboarding handoff.

## Current status
1. `main` includes both latest task streams:
   - Claude branch merge: FAT semantics hardening (`d432f89`)
   - Codex branch merge: INT21/BIOs test harness (`19b2844`, `2eca0bb`, merge `291f3f7`)
2. DOS-like Stage2 shell and FAT cache RW path are active and tested.
3. FreeDOS symbiotic integration pipeline exists (sync/build/import scripts) and is documented.

## Risks / technical debt
1. Multi-agent parallel edits can collide in core files (`stage2/src/shell.c`, `stage2/src/fat.c`, ABI headers).
2. Compatibility drift risk if docs/tests are updated inconsistently across branches.
3. DOS extender/protected-mode path remains high complexity and should stay tightly scoped.

## Next steps (recommended order)
1. Keep Copilot on one clearly scoped branch with explicit file ownership.
2. Require green checks before merge:
   - `make test-stage2`
   - `make test-fallback`
   - `make test-fat-compat`
   - `make test-int21`
3. For every substantial Copilot change, add a new handoff in `docs/handoffs/`.

## Notes for GitHub Copilot (Claude Haiku 4.5)
1. Read first:
   - `CLAUDE.md`
   - `docs/roadmap-ciukios-doom.md`
   - latest handoffs in `docs/handoffs/`
2. Keep changes incremental and deterministic; avoid broad refactors unless explicitly requested.
3. Prefer compatibility-safe behavior over speculative optimization.
4. If a change touches boot/runtime ABI, document it and call out migration impact explicitly.
