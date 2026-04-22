# Claude Branch Brief - M3 Directory Management

## Branch
`feature/claude-m3-fat-io-hardening`

## Goal
Add DOS-like directory management to CiukiOS shell and FAT layer, starting with directory creation.

## Priority Order
1. `mkdir <path>` / `md <path>`
2. `rmdir <path>` / `rd <path>` (only empty directories)
3. Optional: improve `dir` output to clearly mark directories

## Required Features
1. Create directory entry (`ATTR_DIRECTORY`) with correct `.` and `..` initialization in new cluster.
2. Support nested path creation when parent exists.
3. Return clear user errors:
- directory already exists
- parent path not found
- invalid 8.3 name
- disk full / no free cluster
4. Keep DOS-like case-insensitive behavior for path lookup.

## Shell Commands
1. Add aliases:
- `mkdir` and `md`
- `rmdir` and `rd`
2. Keep command help updated (`help`).

## Non-Goals
1. Do not touch COM loader/PSP/termination ABI.
2. Do not implement recursive delete in this step.
3. Do not implement long file names (LFN).

## Acceptance Criteria
1. `mkdir TESTDIR` creates directory visible in `dir`.
2. `mkdir TESTDIR` again returns already-exists error.
3. `mkdir A/B/C` works only if parents exist (no implicit multi-level create unless explicitly implemented).
4. `rmdir TESTDIR` works only when empty.
5. `rmdir` on non-empty dir returns clear error.
6. Regression checks:
- `make test-stage2` PASS
- `make test-fallback` PASS
- add/update one filesystem compatibility script for directory lifecycle.

## Suggested Tests (script)
1. create dir -> list -> remove dir
2. create dir -> create file inside -> fail rmdir until file deleted
3. invalid names (`A*`, very long, empty)
4. root and parent traversal checks (`.`, `..`)

## Deliverables
1. Code changes in FAT + shell modules.
2. Updated test script(s).
3. Handoff in `docs/handoffs/` with decisions, tests, risks, next step.
