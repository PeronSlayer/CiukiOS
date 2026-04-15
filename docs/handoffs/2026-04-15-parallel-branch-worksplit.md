# Handoff - Parallel Branch Worksplit Setup

## Context and Goal
Set up a parallel development workflow between Codex and Claude Code to speed up roadmap delivery with low merge conflict risk.

## What Was Set Up
1. Created branch for Codex work:
- `feature/codex-m1-com-loader-psp`
2. Created branch for Claude work:
- `feature/claude-m3-fat-io-hardening`
3. Added coordination docs:
- `docs/collab/branch-worksplit-plan.md`
- `docs/collab/claude-branch-brief-m3-fat-io-hardening.md`

## Scope Split Decided
1. Codex branch -> M1 `.COM` loader + PSP/process termination semantics.
2. Claude branch -> M3 FAT/path/handle behavior hardening and shell file command compatibility.

## Merge Strategy
1. Merge Codex M1 branch into `main` first.
2. Rebase Claude M3 branch on updated `main`.
3. Final merge Claude branch after compatibility checks.

## Validation
1. Branches created successfully.
2. Coordination docs available and versioned in repo.

## Risks
1. Potential ABI drift between loader and filesystem layers if shared structs change without notice.

## Immediate Next Step
1. Start implementation on `feature/codex-m1-com-loader-psp`.
2. Hand off Claude brief file for immediate execution on `feature/claude-m3-fat-io-hardening`.
