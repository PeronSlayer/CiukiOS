# Copilot Codex Task Pack - SR-DOSRUN-001 (Simple DOS Program Milestone)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-sr-dosrun-001 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Close the sub-milestone “run a simple DOS program” with deterministic tests and clear runtime markers.

## Scope (5 tasks)

### D1) Deterministic Program Corpus (Minimal)
1. Add a tiny DOS program corpus for smoke tests (at least one `.COM`; optional tiny `.EXE`).
2. Ensure assets are reproducible and documented.
3. Keep binaries legally safe (self-built or clearly sourced with license note).

### D2) Run Path Contract Hardening
1. Harden `run` command flow for clear result classes:
- success
- file not found
- unsupported/bad format
- runtime failure
2. Emit explicit serial markers for each class.

Marker examples:
- `[dosrun] launch path=... type=COM`
- `[dosrun] result=ok code=0x..`
- `[dosrun] result=error class=not_found|bad_format|runtime`

### D3) Return-Code Parity Mini Gate
1. Add deterministic checks for `AH=4Ch` termination + one-shot `AH=4Dh` status retrieval.
2. Ensure second read of `AH=4Dh` behaves as expected (already one-shot baseline exists, extend with launch path integration).

### D4) End-to-End Non-Interactive Test
1. Add `scripts/test_dosrun_simple_program.sh`.
2. Test must validate:
- boot reaches shell
- simple DOS program launches
- return marker captured
- no `#UD`/panic
3. Add Make target: `test-dosrun-simple`.

### D5) Documentation + Roadmap Sync
1. Update `docs/subroadmap-sr-dosrun-001.md` with delivered items.
2. Update `Roadmap.md` SR section status.
3. Add brief changelog line in README for this sub-milestone if completed.

## Constraints
1. Do not regress existing FreeDOS/OpenGEM/INT21/video gates.
2. Keep deterministic log markers (no random strings/timestamps).
3. Keep stage2 shell behavior backwards compatible.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-mz-regression`
4. `make test-dosrun-simple`

## Final Handoff
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-codex-sr-dosrun-001.md`

Include:
1. changed files
2. markers added
3. test outputs
4. unresolved gaps
5. next tasks
