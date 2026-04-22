# HANDOFF - FreeDOS Pipeline Validation Harness

## Date
`2026-04-16`

## Context
Task H1 from Codex: Add deterministic checks to ensure FreeDOS import/build artifacts are present and consistent. This harness provides tooling to gate CI/merge processes on validated FreeDOS pipeline state.

## Completed scope
1. Implemented `scripts/validate_freedos_pipeline.sh` - deterministic validation harness.
2. Added `make test-freedos-pipeline` target in Makefile.
3. Updated `docs/freedos-integration-policy.md` with "Validation and Testing" section.

## Technical decisions
1. **Script scope**:
   - Validates manifest exists and is well-formed.
   - Checks required files (marked `required=yes` in manifest) are present in `third_party/freedos/runtime/`.
   - Verifies freecom git repo is available (informational, not blocking).
   - Returns exit code 1 if any essential file is missing.

2. **Manifest as source of truth**:
   - All validation derives from `third_party/freedos/manifest.csv`.
   - Allows maintainers to update requirements declaratively without script edits.

3. **Non-blocking optional files**:
   - Script checks freecom git presence but does not fail if absent.
   - Optional files (memory *.EXE, utilities) are tracked but not validated.

## Touched files
1. `scripts/validate_freedos_pipeline.sh` (new)
2. `Makefile` (added test-freedos-pipeline target and .PHONY entry)
3. `docs/freedos-integration-policy.md` (added Validation section)

## ABI/contract changes
None. This is purely a validation/testing harness; no runtime behavior modified.

## Tests executed
1. **make test-stage2**:
   Result: ✅ PASS

2. **make test-fallback**:
   Result: ✅ PASS

3. **make test-fat-compat**:
   Result: ✅ PASS (12/12 checks)

4. **make test-int21**:
   Result: ✅ PASS (INT21 PSP/status markers confirmed)

5. **make test-freedos-pipeline** (new):
   Result: Expected failure (missing KERNEL.SYS, FDCONFIG.SYS, FDAUTO.BAT)
   - Validates correct detection of missing required files.
   - Exit code 1 as designed.

## Current status
1. Pipeline validation harness is operational.
2. Existing tests remain green.
3. New harness correctly identifies missing required FreeDOS components.
4. Ready for integration into CI pipelines and pre-commit checks.

## Risks / technical debt
1. Current manifest has 3 required files marked as missing (KERNEL.SYS, FDCONFIG.SYS, FDAUTO.BAT).
   - These must be imported before validation can consistently pass.
   - Script is designed to fail cleanly on these, signaling what is missing.

2. No SHA256 checksum validation implemented yet (checksums in manifest are partial).
   - Future enhancement: verify artifact integrity against manifest.

## Next steps (recommended order)
1. Import missing core FreeDOS files (KERNEL.SYS, FDCONFIG.SYS, FDAUTO.BAT) to make test-freedos-pipeline pass.
2. Implement SHA256 checksum validation in script.
3. Add test-freedos-pipeline to CI gate (after core files are imported).
4. Proceed with Task H2 (INT21 Compatibility Matrix).

## Notes for next agent
1. This harness is intentionally strict - it fails fast on missing essentials.
2. Script is idempotent and safe to call repeatedly.
3. Manifest.csv is the contract - update it when FreeDOS components are added/removed.
4. If merging before core files are available, disable test-freedos-pipeline in CI or mark as advisory-only.
