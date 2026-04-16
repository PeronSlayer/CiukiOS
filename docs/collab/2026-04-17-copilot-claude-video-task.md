# Copilot Claude Task - Video Driver Next Updates (Parallel Track)

Date: 2026-04-17
Owner: Copilot Claude (video track)
Requester: mainline coordination

## Objective
Advance the video sub-roadmap beyond current `1024x768` compatibility floor, while preserving existing boot/runtime stability and current regression gates.

## Current Baseline (must preserve)
1. `GOP` mode catalog handoff is active.
2. Loader emits `GOP: policy1024 available=... selected=... result=PASS/FAIL`.
3. `make test-video-1024` is green.
4. `make test-mz-regression` and `make test-phase2` must remain green.

## Requested Scope (Claude)
1. Implement larger/dynamic backbuffer allocation policy in stage2 video path.
2. Extend compatibility policy above `1024x768` without forcing direct-render fallback by default.
3. Add deterministic tests for resolution scaling behavior (prefer non-interactive host checks).
4. Update roadmap status and add one handoff doc for the video increment.

## Guardrails
1. Do not regress loader/stage2 handoff ABI.
2. Keep `make all` green.
3. Keep existing video policy marker and tests compatible.
4. If a runtime QEMU capture issue appears, classify as infra with explicit diagnostics (do not hide failures).

## Suggested Validation Matrix
1. `make all`
2. `make test-video-1024`
3. `make test-video-mode` (or equivalent pipeline gate)
4. `make test-mz-regression`
5. `make test-phase2`

## Expected Deliverables
1. Code changes for dynamic/larger backbuffer policy.
2. New/updated tests proving behavior across multiple resolutions.
3. Updated roadmap entries in `Roadmap.md` and/or `docs/roadmap-ciukios-doom.md`.
4. Handoff doc: `docs/handoffs/2026-04-17-<video-topic>.md`.
