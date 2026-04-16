# HANDOFF - INT21 Compatibility Matrix + Automated Gate

## Date
`2026-04-16`

## Context
Task H2 from Codex: Create a single compatibility matrix source and an automated sanity gate for implemented/stubbed INT21 functions. This provides a declarative single source of truth for INT21h feature coverage and prevents documentation drift.

## Completed scope
1. Added formal `INT21h Compatibility Matrix` section in `docs/int21-priority-a.md`.
2. Implemented `scripts/check_int21_matrix.sh` - automated matrix validation gate.
3. Added `make check-int21-matrix` target in Makefile.
4. Updated `docs/int21-priority-a.md` with "Matrix Validation Gate" section explaining CI integration.

## Technical decisions
1. **Matrix as declarative spec**:
   - All INT21h function status (IMPLEMENTED, DETERMINISTIC_STUB, UNSUPPORTED) is documented in table format.
   - Table is human-readable and machine-parseable.
   - Single source of truth prevents duplication.

2. **Script validation logic**:
   - Extracts matrix block from documentation using sed/grep.
   - Validates all required priority-A functions have a documented row.
   - Checks status values conform to enum (IMPLEMENTED|DETERMINISTIC_STUB|UNSUPPORTED).
   - Reports implementation coverage as advisory (non-fail-on-low warning).

3. **Non-breaking change**:
   - No code generation or runtime ABI changes.
   - Purely documentation + validation infrastructure.
   - Existing INT21 behavior completely unchanged.

## Touched files
1. `docs/int21-priority-a.md` (added Matrix section + Validation Gate section)
2. `scripts/check_int21_matrix.sh` (new - 101 lines)
3. `Makefile` (added check-int21-matrix target and .PHONY entry)

## ABI/contract changes
None. This is purely documentation and tooling.

## Tests executed
1. **make test-stage2**:
   Result: ✅ PASS

2. **make test-fallback**:
   Result: ✅ PASS

3. **make test-fat-compat**:
   Result: ✅ PASS

4. **make test-int21**:
   Result: ✅ PASS

5. **make check-int21-matrix** (new):
   Result: ✅ PASS
   - Validates 16 matrix entries
   - Confirms all 16 required functions documented
   - Verifies 13 implemented, 3 deterministic stubs
   - All status values valid

## Current status
1. INT21h Compatibility Matrix is operational and passing validation.
2. All existing tests remain green.
3. Documentation accurately reflects current implementation (13 implemented + 3 stubs).
4. Gate is ready for CI integration.

## Risks / technical debt
1. Matrix is currently manually updated - no auto-sync from code.
   - Future: could parse shell.c dispatcher to detect stale matrix.
2. Status column shows implementation intent, not full test coverage per function.
   - Each IMPLEMENTED function still needs regression test confirmation.

## Next steps (recommended order)
1. Integrate `check-int21-matrix` into CI/pre-commit.
2. Add keyboard status/flush INT21 APIs (`AH=0Bh`, `AH=0Ch`) to matrix as DETERMINISTIC_STUB.
3. Expand deterministic stubs to handle-based file API subset (`3Ch..42h`).
4. Proceed with additional Phase-1/Phase-2 items from roadmap.

## Notes for next agent
1. When adding new INT21 functions, **always** update this matrix first.
2. Matrix format is strict: "FN  | Status | Implementation Details".
3. Script is idempotent and safe to run repeatedly.
4. If matrix check fails, error messages clearly indicate what is missing.
5. Consider adding matrix entries for all future INT21h functions, not just priority-A.
