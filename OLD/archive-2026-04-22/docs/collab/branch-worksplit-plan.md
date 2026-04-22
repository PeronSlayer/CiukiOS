# CiukiOS Parallel Branch Worksplit Plan

## Date
2026-04-15

## Objective
Work in parallel on two independent roadmap tracks to accelerate progress while keeping merge conflicts low.

## Branches
1. `feature/codex-m1-com-loader-psp` (owned by Codex)
2. `feature/claude-m3-fat-io-hardening` (owned by Claude Code)

## Scope Split
1. Codex branch scope (Roadmap M1):
- True DOS `.COM` execution path (PSP-oriented semantics)
- Clean terminate/return path (`INT 20h`, `INT 21h AH=4Ch`) in runtime contract
- Minimal compatibility harness for tiny `.COM` binaries

2. Claude branch scope (Roadmap M3):
- FAT12/16 path semantics hardening (8.3, case-insensitive DOS behavior)
- Handle/file APIs behavior alignment (`3Ch-42h` family semantics where already exposed)
- Shell-facing compatibility checks for `DIR/TYPE/COPY/DEL` behavior

## Boundaries (Do Not Overlap)
1. Codex does not touch FAT cache internals unless strictly required by M1 loader contract.
2. Claude does not redesign loader ABI or process lifecycle internals.
3. Shared structures/protocol changes require explicit handoff note before merge.

## Merge Order
1. Merge Codex M1 branch first into `main`.
2. Rebase Claude branch onto updated `main`.
3. Resolve any ABI drift and merge Claude branch.

## Mandatory Validation Before Merge
1. `make test-stage2`
2. `make test-fallback`
3. Any new targeted test introduced by each branch owner

## Communication Contract
1. Every major multi-file change produces a handoff in `docs/handoffs/`.
2. Keep `CLAUDE.md` in sync if global project state changed.
3. Mention explicit risks and next step in each handoff.
