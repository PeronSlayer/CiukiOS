# Claude Branch Brief - M3 Attributes (`attrib`) and Enforcement

## Branch
`feature/claude-m3-fat-io-hardening`

## Goal
Add DOS-like file attribute support and enforce it in filesystem commands.

## Scope
1. Add shell command:
- `attrib <path>` show flags
- `attrib +r|-r <path>`
- `attrib +a|-a <path>`

2. FAT attribute integration:
- read/write directory entry attribute bits
- ensure updates persist in cached FAT image

3. Enforcement rules:
- `del` must refuse read-only files
- `copy/ren/move` must handle read-only destination constraints consistently

## Non-Goals
1. Hidden/system policy beyond display is optional for now.
2. Wildcards (`*`, `?`) not required in this step.
3. No LFN.

## Acceptance Criteria
1. `attrib` correctly displays and toggles read-only/archive.
2. Enforcement behavior is deterministic and documented.
3. Tests pass:
- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`
4. Add attribute-focused compatibility tests.

## Deliverables
1. FAT + shell changes for attribute support.
2. Test updates.
3. Handoff documenting edge cases and remaining gaps.
