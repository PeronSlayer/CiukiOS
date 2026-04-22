# Copilot Claude Opus Task Pack - BIOS-RUNTIME-GAP-001

## Mandatory Branch Isolation
Claude Opus must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-claude-bios-runtime-gap-001 origin/main
```

No direct commits on `main`. No force-push on shared branches.

## Mission
Close the next BIOS and DOS runtime compatibility gaps that are most likely to block the frozen DOOM startup path, but do it from evidence instead of broad speculative parity work.

This task is intentionally separate from the next DOS extender target work. Do not work on the protected-mode regression binary area assigned elsewhere.

## Context
Current state from roadmap + repo:
1. The project already emits boot markers for `INT 10h`, `INT 16h`, `INT 1Ah`, and `INT 2Fh`.
2. The roadmap still lists BIOS/runtime gaps as a remaining blocker between the current baseline and real DOOM startup.
3. Existing gates cover shallow BIOS presence, but the broader trace-driven behavior used by real DOS tools and startup paths is still incomplete.
4. The target remains frozen to user-supplied shareware `DOOM.EXE` v1.9 + `DOOM1.WAD`, so this work must stay tightly focused on startup/runtime behaviors that help that path.

## Scope (5 tasks)

### B1) Capture The Next Real BIOS/DOS Footprint
1. Inspect the current DOOM/startup-related harnesses, markers, and any existing runtime evidence.
2. Identify the most plausible next BIOS and DOS API calls that occur between `binary_found` and `video_init` / `menu_reached`.
3. Prefer evidence from current repo scripts, harnesses, and observable runtime traces over generic emulator folklore.
4. Produce a short, deterministic list of the next missing or weakly-covered calls.

### B2) Strengthen BIOS Compatibility Tests
1. Add or extend a deterministic gate that validates BIOS behaviors actually relevant to startup paths.
2. Focus on `INT 10h`, `INT 16h`, `INT 1Ah`, and `INT 2Fh` behaviors that are still too shallow for real-binary confidence.
3. Reuse existing runtime-gate style and fallback strategy where possible.
4. Avoid inventing a large generic framework.

### B3) Close The Smallest Useful Runtime Gaps
1. Implement only the minimum BIOS/DOS behavior required by the calls identified in B1.
2. Keep behavior deterministic and marker-driven.
3. Avoid speculative expansion into unrelated DOS APIs.
4. Preserve existing shell, GUI, and DOS runtime behavior.

### B4) Improve Observability
1. Add explicit markers for the new BIOS/runtime behaviors when useful.
2. Make it easy to distinguish:
   - behavior stub present only
   - behavior invoked
   - behavior returned coherent data
3. Do not spam logs with noisy or redundant output.

### B5) Documentation And Handoff
1. Update the relevant roadmap or status doc only if the achieved compatibility meaning materially changes.
2. Create a handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-claude-bios-runtime-gap-001.md`
3. The handoff must clearly separate:
   - newly covered BIOS/runtime behaviors
   - what evidence drove those choices
   - what still remains before the DOOM startup path is materially improved

## Constraints
1. Do not bump the project version.
2. Do not commit on `main`.
3. Do not drift into DOS extender redesign or mode `13h` graphics implementation.
4. Keep the work in the BIOS/runtime compatibility category only.
5. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.

## Recommended Files To Inspect First
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/roadmap-ciukios-doom.md`
5. `docs/int21-priority-a.md`
6. `scripts/test_doom_boot_harness.sh`
7. `scripts/test_doom_readiness_m6.sh`
8. `scripts/check_int21_matrix.sh`
9. `stage2/src/shell.c`
10. `stage2/src/stage2.c`

## Acceptance Criteria
The task is complete when all of the following are true:
1. The next BIOS/runtime behaviors most relevant to startup are chosen from evidence, not guesswork.
2. At least one deterministic gate becomes stronger or broader in a way that covers real startup-relevant behavior.
3. The implemented behavior is minimal, coherent, and does not overlap with the separate protected-mode regression task.
4. The handoff explains both coverage gained and what still remains missing.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. any updated BIOS/runtime gate you add
4. any relevant DOOM-startup-facing harness you strengthen

## Deliverables
1. Code/test changes for BIOS/runtime gap closure.
2. Minimal docs updates if compatibility meaning changed.
3. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-claude-bios-runtime-gap-001.md`

## Final Handoff Must Include
1. Files changed.
2. Which BIOS/runtime behaviors were strengthened.
3. What evidence justified those choices.
4. Test results.
5. Remaining gaps before meaningful DOOM startup progress.