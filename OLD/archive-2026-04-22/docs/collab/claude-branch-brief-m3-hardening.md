# Claude Branch Brief - M3 Filesystem Hardening

## Branch
`feature/claude-m3-fat-io-hardening`

## Goal
Harden DOS-like filesystem command behavior for `mkdir/rmdir/ren/move/copy/del`.

## Required Work
1. Path semantics edge cases:
- `.` / `..`
- root path behavior
- canonicalization consistency

2. Name validation:
- strict 8.3 constraints for created/renamed targets
- clear invalid-name errors

3. Directory semantics:
- `mkdir` parent existence checks
- `rmdir` only on empty directories
- correct errors for non-empty directories

4. Rename/move semantics:
- clear policy for same-dir vs cross-dir rename
- deterministic collision handling

## Non-Goals
1. No COM/EXE loader ABI changes.
2. No protected-mode/extender work.
3. No LFN support.

## Acceptance Criteria
1. `mkdir/rmdir/ren/move/copy/del` behave predictably on edge paths.
2. Messages are short, stable, and DOS-like.
3. Tests pass:
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`
4. Add at least one new test case file for edge paths.

## Deliverables
1. Code changes in FAT/shell modules.
2. Updated tests.
3. Handoff in `docs/handoffs/` with decisions, tests, risks, next step.
