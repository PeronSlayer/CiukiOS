# Claude Branch Brief - M3 Copy/Move/Rename Management

## Branch
`feature/claude-m3-fat-io-hardening`

## Goal
After directory support, implement DOS-like file management commands for copy and rename workflows.

## Priority Order
1. `copy <src> <dst>`
2. `ren <old> <new>` / `rename <old> <new>`
3. Optional: `move <src> <dst>` (can be implemented as copy+delete for same-volume FAT)

## Required Features
1. `copy`:
- Copy file content and metadata needed by current FAT model.
- Proper errors for missing source, destination exists (if overwrite not supported), invalid name, disk full.
- Keep behavior deterministic with 8.3 names.

2. `ren` / `rename`:
- Rename inside the same directory (MVP).
- Reject cross-directory rename in first step unless explicitly supported.
- Case-insensitive lookup with DOS-like normalization.

3. `move` (optional in this step):
- If implemented, define clearly whether cross-directory move is supported.
- If not implemented, return clear message and keep command disabled.

## Shell UX
1. Add commands to `help` output.
2. Keep error messages short and consistent (`File not found`, `Already exists`, `Invalid name`, etc.).

## Non-Goals
1. No COM loader/PSP/termination ABI changes.
2. No LFN support.
3. No wildcard support (`*`, `?`) in this step unless already easy and safe.

## Acceptance Criteria
1. `copy A.TXT B.TXT` produces byte-identical file.
2. `copy` handles text and binary files correctly.
3. `ren OLD.TXT NEW.TXT` updates directory listing as expected.
4. Error handling validated for missing source, invalid destination, and collisions.
5. Regression checks:
- `make test-stage2` PASS
- `make test-fallback` PASS
- update/add one compatibility test script for copy/rename scenarios.

## Suggested Tests
1. create source -> copy -> verify `type` / size / content.
2. rename source -> verify old missing and new present.
3. copy into existing name -> expected error or explicit overwrite behavior.
4. binary payload copy check using deterministic test bytes.

## Deliverables
1. FAT + shell command updates scoped to file management.
2. Updated tests.
3. Handoff in `docs/handoffs/` with changed files, decisions, tests, risks, next step.
