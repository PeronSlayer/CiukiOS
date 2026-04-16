# HANDOFF - INT21 PSP/Status Extension + Test Harness Sync

## Date
`2026-04-16`

## Context
With Claude temporarily unavailable, Codex advanced a low-risk DOS compatibility increment: extend INT21 baseline with PSP/status primitives and make tests assert the new path.

## Completed scope
1. Added INT21 functions `AH=51h`, `AH=62h`, and `AH=4Dh` in stage2 COM runtime dispatcher.
2. Added deterministic in-process bookkeeping for last exit status exposure via `AH=4Dh`.
3. Extended selftest coverage in `stage2_shell_selftest_int21_baseline()`.
4. Added explicit compatibility marker in boot serial log and enforced it in regression scripts.
5. Updated INT21 documentation to match implementation.

## Touched files
1. `stage2/src/shell.c`
2. `stage2/src/stage2.c`
3. `scripts/test_stage2_boot.sh`
4. `scripts/test_int21_priority_a.sh`
5. `docs/int21-priority-a.md`
6. `docs/collab/parallel-next-tasks-2026-04-16-codex-copilot.md`
7. `docs/handoffs/2026-04-16-codex-int21-psp-status-extension.md`

## Technical decisions
1. Decision:
Expose PSP via both `AH=51h` and `AH=62h` as compatibility aliases.
Reason:
Common DOS software expects at least one of these paths; adding both is cheap and safe.
Impact:
Improved compatibility surface without ABI changes.

2. Decision:
Implement `AH=4Dh` using stage2-maintained last-exit state (`AL` code, `AH` type).
Reason:
Needed for parent/child-like compatibility semantics and diagnostics.
Impact:
Provides deterministic process-status query behavior.

3. Decision:
Add a dedicated serial marker and test assertions for this extension.
Reason:
Prevents silent regressions in CI-style log checks.
Impact:
Higher confidence when parallel merges happen.

## ABI/contract changes
1. No boot ABI changes (`boot/proto/services.h` unchanged).
2. Runtime INT21 semantic surface expanded in stage2 dispatcher only.

## Tests executed
1. Command:
`make test-stage2`
Result:
PASS

2. Command:
`make test-fallback`
Result:
PASS

3. Command:
`make test-fat-compat`
Result:
PASS

4. Command:
`make test-int21`
Result:
PASS

## Current status
1. INT21 baseline now includes PSP getters and last-exit status getter.
2. New marker is asserted by regression scripts.
3. Existing boot/fallback/FAT tests remain green.

## Risks / technical debt
1. `AH=4Dh` currently uses a simplified termination-type model (always normal type 0 in published runtime path).
2. Full DOS process tree semantics are not yet implemented.

## Next steps (recommended order)
1. Add keyboard status/flush INT21 APIs (`AH=0Bh`, `AH=0Ch`) with deterministic behavior.
2. Expand handle-based INT21 file API subset (`3Ch..42h`) in deterministic phases.
3. Add matrix-based test guard for documented INT21 compatibility claims.

## Notes for Claude Code
1. This change is intentionally compatibility-safe and incremental.
2. If extending INT21 further, keep deterministic carry/AX behavior first, then replace stubs with real backends.
3. Preserve current log markers because test scripts now assert them.
