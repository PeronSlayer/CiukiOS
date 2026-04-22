# Copilot Codex Task Pack - M6-NEXT-REAL-TARGET-001

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-m6-next-real-target-001 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Advance the protected-mode / DOS-extender path by replacing the current shallow smoke ceiling with the next real regression target that progresses farther than host detect, version query, raw-mode bootstrap discovery, LDT allocation, and free-memory smoke.

This task is intentionally separate from BIOS/runtime compatibility work assigned elsewhere.

## Context
Current state from roadmap + repo:
1. The M6 smoke chain already covers host detect, version, callable host presence, raw bootstrap slice, LDT allocation, memory allocation, and free-memory release.
2. The roadmap explicitly says the next milestone is a stronger DOS-extender regression target that reaches an interactive or near-interactive checkpoint.
3. The current gap is no longer a missing shallow API slice; it is the lack of a more meaningful binary that forces the protected-mode contract farther forward.

## Scope (5 tasks)

### M1) Choose The Next Real Regression Target
1. Inspect the current `com/m6_*` binaries, M6 test scripts, and requirements docs.
2. Select or build the next regression binary only if it exercises more than the existing shallow slices.
3. The new target must prove a stronger contract than simple presence/version/bootstrap/address-discovery behavior.

### M2) Implement The Minimum Missing Host Surface
1. Add only the protected-mode or DPMI behavior actually required by the new target.
2. Prefer the smallest coherent callable slice over a speculative subsystem expansion.
3. Preserve existing M6 smoke gates and current DOS runtime behavior.

### M3) Add A Stronger Gate
1. Add or update a deterministic gate dedicated to the new target.
2. The gate must distinguish a real progression milestone from the existing shallow smokes.
3. Reuse the current runtime/fallback harness style where appropriate.

### M4) Sync Roadmap And Requirements
1. Update the M6 requirements/status docs if the achieved contract meaning changes.
2. Keep wording precise about what is newly proven versus still deferred.

### M5) Documentation And Handoff
1. Create a handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-m6-next-real-target-001.md`
2. The handoff must explain:
   - chosen regression target
   - newly required host behavior
   - what stronger checkpoint was reached

## Constraints
1. Do not bump the project version.
2. Do not commit on `main`.
3. Stay in the protected-mode / DOS-extender category only.
4. Do not overlap with BIOS/runtime gap-closure work assigned to another agent.
5. Keep logs deterministic and gates reproducible.

## Recommended Files To Inspect First
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/roadmap-ciukios-doom.md`
5. `docs/m6-dos-extender-requirements.md`
6. `scripts/test_doom_readiness_m6.sh`
7. `scripts/test_m6_dpmi_free_smoke.sh`
8. `stage2/src/shell.c`
9. `stage2/src/stage2.c`
10. `com/m6_dpmi_smoke/`
11. `com/m6_dpmi_mem_smoke/`
12. `com/m6_dpmi_free_smoke/`

## Acceptance Criteria
The task is complete when all of the following are true:
1. A new regression target exists that proves a stronger protected-mode contract than the current smoke ceiling.
2. Only the minimum new host behavior required by that target is implemented.
3. A deterministic gate validates the stronger checkpoint.
4. Docs and handoff clearly separate newly achieved behavior from remaining gaps before a real DOS extender app reaches near-interactive state.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-doom-readiness-m6`
4. the new dedicated M6 gate you add

## Deliverables
1. Code/test changes for the next real M6 regression target.
2. Minimal docs updates if requirements meaning changed.
3. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-m6-next-real-target-001.md`

## Final Handoff Must Include
1. Files changed.
2. Chosen regression target and why it is stronger.
3. Newly implemented host behavior.
4. Gate behavior and test results.
5. Remaining gaps before a non-trivial DOS extender app reaches near-interactive mode.