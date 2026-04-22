# Copilot Claude Task Pack - SR-DOSRUN-001 Closure v1 (Simple DOS Programs)

## Mandatory Branch Isolation
Claude must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-claude-sr-dosrun-closure-v1 origin/main
```

No direct commits on `main`. No force-push on shared branches.

## Mission
Close the remaining ~30% of simple DOS-program compatibility by finishing `SR-DOSRUN-001`.

Primary target:
- Move `SR-DOSRUN-001` from `IN PROGRESS` to `DONE` for practical simple DOS app execution.

## Scope (5 heavy tasks)

### D1) Deterministic Minimal EXE/MZ Smoke Artifact + Launch Path
1. Add a deterministic minimal `.EXE` smoke payload (e.g. `CIUKMZ.EXE`) that can be executed by current runtime path.
2. Ensure artifact is reproducible from source in repo (no opaque binary-only drop).
3. Integrate into boot image build flow similarly to existing DOS smoke assets.
4. Extend `run` flow markers for MZ path.

Required markers:
- `[dosrun] launch path=CIUKMZ.EXE type=MZ`
- `[dosrun] result=ok code=0x..`

### D2) DOS Command Tail / Args Bridge (COM + MZ)
1. Implement deterministic command-tail propagation (PSP tail semantics baseline) so `run <prog> <args>` reaches payload.
2. Support at least:
- no args
- simple args with spaces
- quoted arg baseline (minimal deterministic behavior, document exact parsing rules)
3. Ensure no regressions for current `run CIUKSMK.COM` path.

Required markers:
- `[dosrun] argv tail len=...`
- `[dosrun] argv parse=PASS`

### D3) INT21 Coverage for Simple Utility Class (Targeted)
Implement missing/high-impact INT21 subset commonly used by simple utilities (deterministic baseline, not full DOS):
1. `AH=2Ah` get date
2. `AH=2Ch` get time
3. `AH=44h` IOCTL minimal baseline (`AL=00h` device info for std handles)

Requirements:
- deterministic return conventions
- DOS-like error mapping for unsupported subfunctions
- update INT21 matrix doc and gate expectations

Required markers:
- `[compat] INT21h date/time ready (AH=2Ah/2Ch)`
- `[compat] INT21h ioctl baseline ready (AH=44h/AL=00h)`

### D4) Run-Path Error Taxonomy Hardening
1. Expand run-path error classes to distinguish practical failures:
- `not_found`
- `bad_format`
- `runtime`
- `unsupported_int21`
- `args_parse`
2. Ensure deterministic mapping from failure point -> marker class.
3. Keep backward compatibility for existing class names used by current tests.

Required markers:
- `[dosrun] result=error class=...` (extended set)

### D5) Gate Expansion + Roadmap Closure
1. Add new deterministic non-interactive gate:
- `scripts/test_dosrun_mz_simple.sh`
2. Expand existing `scripts/test_dosrun_simple_program.sh` to validate both COM and MZ smoke success paths.
3. Add Make targets:
- `test-dosrun-mz`
- keep `test-dosrun-simple`
4. Update docs:
- `docs/subroadmap-sr-dosrun-001.md` -> mark closure items `DONE`
- `Roadmap.md` -> set SR-DOSRUN-001 to `DONE` with explicit note of baseline scope
- `README.md` changelog software-only entry (public-facing, no internal orchestration references)

## Constraints
1. Do not regress existing gates:
- `make test-stage2`
- `make test-int21`
- `make test-mz-regression`
- `make test-phase2`
- `make test-video-mode`
2. Keep deterministic markers (no timestamps/random IDs).
3. Keep shell UX backward-compatible (`run`, `help`, existing commands).
4. Keep freestanding-safe implementation.

## Validation (must run before handoff)
1. `make all`
2. `make test-dosrun-simple`
3. `make test-dosrun-mz`
4. `make test-mz-regression`
5. `make test-int21`
6. `make test-stage2`

## Exit Criteria
1. COM smoke and MZ smoke both PASS in non-interactive gates.
2. `run` returns deterministic markers for success/failure classes.
3. SR-DOSRUN-001 can be marked `DONE` with documented baseline limitations.

## Final Handoff
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-claude-sr-dosrun-closure-v1.md`

Include:
1. changed files
2. artifacts added (COM/MZ smoke)
3. markers added/changed
4. tests executed + outcomes
5. residual limitations (max 5)
