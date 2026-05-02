# AI Agent Operating Directives (v2)

## Mandatory Workflow Rules
1. Every task must run on a dedicated branch, never directly on `main`.
2. At task completion, ask for explicit approval before merging to `main`.
3. Push to remote only after explicit user approval.

## Documentation and Changelog Rules
1. All documentation must be written in clear, concise English.
2. All changelog entries must be written in clear, concise English.
3. Changelog entries must report major project-level changes only.
4. Minor/internal edits must not be listed as standalone changelog items.

## Versioning Rules
1. Versioning baseline is reset to `pre-Alpha v0.5.0`.
2. Version bumps are patch-only (`x.x.1`).
3. Apply a version bump only when explicitly requested by the user.

## Commit Message Rules
1. Commit messages must describe fundamental project changes.
2. Do not use commit messages for trivial/internal file-maintenance details.
3. Prefer concise, outcome-focused wording.

## Anti-loop Rules
1. If a solution does not converge, stop repeating the same attempt.
2. Perform targeted technical research to identify a definitive solution.
3. Document assumptions, evidence, and the final decision.

## Parallelization Rules
1. Large tasks must be split into at most 3 concurrent agents or 3 sequential phases.
2. Each agent/phase must own a clear, non-overlapping scope.
3. Integrate and verify results in one final validation pass.

## Delivery Rules
1. No implicit merges.
2. No destructive changes without explicit consent.
3. Every change must include test status, or a clear reason when tests cannot run.

## Build Profile Rules
1. The `full` profile is the default and only required build/test target from now on.
2. Do not run `floppy` build/test lanes unless the user explicitly requests them.
3. Any new validation checklist must prioritize `full` profile evidence first.

## UX Invariance Rules
1. UX is frozen by default: do not change layouts, visual behavior, prompts, flows, labels, colors, spacing, navigation, or interaction patterns unless the user explicitly approves the UX change first.
2. If a fix requires UX impact, stop and request explicit approval before any UX-affecting edit.
3. Prefer internal/behavioral fixes that preserve existing UX output exactly.
4. Any approved UX change must be documented with explicit scope and rationale in the task recap.
