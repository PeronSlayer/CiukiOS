# Claude Branch Brief - M3 FAT I/O Hardening

## Branch
`feature/claude-m3-fat-io-hardening`

## Context
CiukiOS is progressing toward DOS compatibility and DOOM execution. This branch focuses on Roadmap M3 (filesystem + handle behavior), in parallel with Codex working on M1 loader semantics.

## Primary Goal
Improve DOS-like FAT and file I/O behavior so real DOS workflows (`DIR`, `TYPE`, `COPY`, `DEL`) behave predictably.

## Tasks
1. Harden DOS path semantics:
- 8.3 normalization and case-insensitive lookup
- Consistent handling of `.` and `..`
- Robust error mapping for missing/invalid paths

2. Harden handle/file behavior (where APIs already exist in runtime):
- Create/open/close/read/write/seek behavior consistency
- EOF and partial read semantics
- DOS-like error/return behavior for invalid handle, denied operations, missing file

3. Shell command compatibility checks:
- Verify `dir`, `type`, `copy`, `del` against realistic edge cases
- Ensure no regressions on current mini shell loop

## Explicit Non-Goals
1. Do not redesign loader/PSP/process lifecycle internals (owned by Codex M1 branch).
2. Do not introduce protected-mode or extender work in this branch.

## Acceptance Criteria
1. `make test-stage2` passes.
2. `make test-fallback` passes.
3. Add at least one focused filesystem compatibility test script covering path/handle edge cases.
4. Document behavior differences still open vs DOS target.

## Suggested Files to Touch
1. `stage2/src/*` in FAT/filesystem/shell command modules
2. `scripts/test_*` for added coverage
3. `docs/` for short compatibility notes

## Deliverables Required
1. Code changes in branch scope.
2. Updated or new tests.
3. One handoff file in `docs/handoffs/` with:
- files changed
- decisions
- tests run
- risks
- next step

## Coordination Notes
1. If a shared ABI or struct needs change, write it clearly in handoff before merge.
2. Keep branch conflict surface small and avoid unrelated refactors.
