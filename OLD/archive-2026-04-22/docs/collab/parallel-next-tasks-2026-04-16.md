# Parallel Next Tasks (2026-04-16)

Roadmap alignment target:
1. M2 - Memory/API compatibility core
2. M3 - FAT semantics hardening
3. M4 - BIOS compatibility tests

## Claude Code - Branch `feature/claude-m3-fat-semantics-v2`

### Task C3 - FAT Write Semantics Finalization (Roadmap M3)
Goal:
- Close remaining write-path edge cases in FAT12/16 to match DOS-like behavior.

Scope (owned files):
- `stage2/src/fat.c`
- `stage2/include/fat.h`
- `scripts/test_fat_compat.sh`

Acceptance:
1. Writes fail safely and deterministically on invalid chains, full disk, and directory misuse.
2. No silent corruption on partial write scenarios.
3. `make test-stage2`, `make test-fallback`, `make test-fat-compat` all PASS.

### Task C4 - Path/Error Semantics Hardening (Roadmap M3)
Goal:
- Standardize DOS-like path and error semantics for filesystem APIs used by shell/runtime.

Scope (owned files):
- `stage2/src/fat.c`
- `stage2/include/fat.h`
- `docs/handoffs/*` (new handoff for this task)

Acceptance:
1. Clear distinction among: not found, already exists, invalid name, not empty, access denied.
2. `.`/`..` and root edge cases handled explicitly and tested.
3. New/updated regression checks documented in handoff.

## Codex - Branch `feature/codex-m2-m4-int21-bios-tests`

### Task X3 - INT 21h Priority-A Expansion (Roadmap M2/M5 bridge)
Goal:
- Extend and normalize current `INT 21h` subset with strict flag/error behavior.

Scope (owned files):
- `stage2/src/shell.c`
- `boot/proto/services.h` (only if ABI extension needed)
- `docs/int21-priority-a.md`

Acceptance:
1. Stable behavior for currently targeted core calls (console/process baseline + compatibility-safe returns).
2. Deterministic carry/AX error behavior for unsupported vs invalid usage.
3. Existing boot/FAT regressions remain green.

### Task X4 - BIOS Compatibility Test Harness (Roadmap M4)
Goal:
- Add explicit tests for baseline BIOS interrupt compatibility expectations.

Scope (owned files):
- `scripts/test_stage2_boot.sh`
- `scripts/test_kernel_fallback_boot.sh` / related test scripts if needed
- `docs/handoffs/*` (new handoff for this task)

Acceptance:
1. Tests assert expected markers for `INT 10h`, `INT 16h`, `INT 1Ah` compatibility path (or explicit TODO markers, not silent gaps).
2. Test output is deterministic and CI-friendly.
3. No regressions in existing suites.

## Merge Policy
1. Keep ownership/file scopes disjoint where possible.
2. One handoff markdown per major task block.
3. Re-run full regression trio before merge:
   - `make test-stage2`
   - `make test-fallback`
   - `make test-fat-compat`
