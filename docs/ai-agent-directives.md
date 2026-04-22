# AI Agent Operating Directives (v2)

## Mandatory Workflow Rules
1. Every task must run on a dedicated branch, never directly on `main`.
2. At task completion, ask for explicit approval before merging to `main`.
3. Push to remote only after explicit user approval.

## Documentation and Changelog Rule
1. All documentation must be written in clear, concise English.
2. All changelog entries must be written in clear, concise English.

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
