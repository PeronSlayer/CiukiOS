# Parallel Next Tasks (2026-04-15)

## Claude Code - Branch `feature/claude-m3-fat-io-hardening`

### Task C1 - Filesystem Hardening (DOS-like semantics)
Goal:
- Stabilize behavior of `mkdir/rmdir/ren/move/copy/del` across edge cases.

Acceptance:
1. Correct handling of `.`, `..`, root paths, invalid 8.3 names.
2. Clear and deterministic error messages for collisions, missing parents, non-empty dirs, cross-dir rename limits.
3. `make test-stage2`, `make test-fallback`, and `make test-fat-compat` pass.

### Task C2 - File Attributes and Enforcement (`attrib`)
Goal:
- Add DOS-like attribute inspection/update and enforce read-only/archive behavior in file commands.

Acceptance:
1. `attrib` can show attributes and set/clear at least read-only/archive.
2. `del/copy/ren/move` respect read-only flags.
3. Compatibility tests include attribute scenarios.

## Codex - Branch `feature/codex-m1-exe-mz-loader-mvp`

### Task X1 - EXE MZ Dispatch MVP
Goal:
- Move from load-only MZ path to executable dispatch path with controlled runtime contract.

Acceptance:
1. `run <name>.exe` reaches dispatch path (not just relocation/staging logs).
2. Clear diagnostics for unsupported runtime conditions.
3. Boot tests remain green.

### Task X2 - INT 21h Priority-A Subset (process/console baseline)
Goal:
- Implement/test minimal INT 21h subset needed by early DOS binaries.

Acceptance:
1. Stable return/exit behavior (`INT 20h`, `AH=4Ch`) with deterministic codes.
2. Initial process/console APIs documented and validated via small test binaries.
3. No regressions in shell/boot paths.

## Merge Policy
1. Keep branch scopes disjoint.
2. Require handoff file for each major multi-file change.
3. Re-run regression suite before each merge.
