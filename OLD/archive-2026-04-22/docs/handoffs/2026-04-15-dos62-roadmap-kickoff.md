# HANDOFF - DOS62 roadmap kickoff

## Date
`2026-04-15`

## Context
The project switches to the "high DOS compatibility" path (DOS 6.2 functional equivalent, including progressive binary compatibility for `.COM/.EXE`).

## Completed scope
1. Defined full phase roadmap (0-10) for DOS compatibility.
2. Defined phase-0 operational kickoff with deliverables and DoD.
3. Defined draft ABI handoff `UEFI -> stage2`.
4. Defined `INT 21h` priority-A list.
5. Updated historical phase document.

## Touched files
1. `docs/roadmap-dos62-compat.md`
2. `docs/phase-0-kickoff.md`
3. `docs/abi-uefi-stage2.md`
4. `docs/int21-priority-a.md`
5. `docs/phase-1.md`
6. `docs/handoffs/README.md`
7. `docs/handoffs/HANDOFF_TEMPLATE.md`

## Technical decisions
1. Decision: use UEFI only as modern bootstrap.
Reason: keep boot reliable in QEMU/OVMF and isolate DOS compatibility layer.
Impact: introduce dedicated stage2.

2. Decision: implement through small milestones with explicit exit criteria.
Reason: reduce risk in a long/complex path.
Impact: frequent, verifiable deliveries.

## ABI/contract changes
1. New proposed contract `handoff_v0_t` (document draft).

## Tests executed
1. Documentation consistency review.
Result: docs present and aligned with project direction.

## Current status
1. Existing boot base works and starts kernel.
2. DOS compatibility planning is ready.
3. Stage2 not implemented yet (at that time).

## Risks / technical debt
1. Detailed CPU-transition spec toward DOS-like runtime was still missing.
2. Automated compatibility API tests were still missing.

## Next steps (recommended order)
1. Stage2 scaffolding (banner + halt) with integrated build.
2. Shared ABI header between loader and stage2.
3. Real loader -> stage2 handoff with debug checkpoints.
4. Automatic pass/fail boot test script.

## Notes for Claude Code
Start from phase 0 iteration 1. Avoid scope creep on filesystem/INT 21h until stage2 handoff is stable.
