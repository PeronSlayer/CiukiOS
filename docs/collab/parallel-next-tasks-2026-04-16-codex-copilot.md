# Parallel Next Tasks (2026-04-16, Codex + Copilot)

Context:
- Claude Code is temporarily unavailable.
- Active agents: Codex + GitHub Copilot (Claude Haiku 4.5).

## Codex Tasks (Executed)

### Task CX1 - INT21 PSP/Return-Status Extension (M2 compatibility)
Status: DONE
Goal:
- Extend deterministic INT 21h subset with PSP and process-status primitives.

Scope (owned files):
- `stage2/src/shell.c`
- `docs/int21-priority-a.md`

Delivered:
1. Added `AH=51h` and `AH=62h` to return current PSP segment in `BX`.
2. Added `AH=4Dh` to return last process status (`AL=code`, `AH=type`).
3. Added selftest coverage in `stage2_shell_selftest_int21_baseline()`.

### Task CX2 - Regression Harness + Marker Sync (M0 discipline)
Status: DONE
Goal:
- Ensure boot/test harness explicitly validates the new INT21 compatibility path.

Scope (owned files):
- `stage2/src/stage2.c`
- `scripts/test_stage2_boot.sh`
- `scripts/test_int21_priority_a.sh`

Delivered:
1. Added serial marker:
   - `[ compat ] INT21h PSP/status path ready (AH=51h/62h/4Dh)`
2. Added required checks for this marker in both stage2 and int21 tests.

## GitHub Copilot Tasks (Planned)

### Task H1 - FreeDOS Pipeline Validation Harness
Status: TODO
Suggested branch:
- `feature/copilot-freedos-pipeline-validation`
Goal:
- Add deterministic checks to ensure FreeDOS import/build artifacts are present and consistent.

Scope (owned files):
- `scripts/` (new validation script)
- `Makefile` (new test target)
- `docs/freedos-integration-policy.md` (test usage section)

Acceptance:
1. New script validates expected files/manifest under `third_party/freedos/runtime/`.
2. New make target (example: `test-freedos-pipeline`) returns non-zero on missing essentials.
3. Existing boot tests remain green.

### Task H2 - INT21 Compatibility Matrix Doc + Gate
Status: TODO
Suggested branch:
- `feature/copilot-int21-matrix-gate`
Goal:
- Create a single compatibility matrix source and an automated sanity gate for implemented/stubbed INT21 functions.

Scope (owned files):
- `docs/int21-priority-a.md`
- `scripts/` (new matrix-check script)

Acceptance:
1. Matrix explicitly marks: implemented / deterministic stub / unsupported.
2. Script fails if matrix is missing entries for currently claimed priority-A functions.
3. No change in runtime behavior; documentation/test quality improvement only.

## Merge Gate (Both Agents)
1. `make test-stage2`
2. `make test-fallback`
3. `make test-fat-compat`
4. `make test-int21`
