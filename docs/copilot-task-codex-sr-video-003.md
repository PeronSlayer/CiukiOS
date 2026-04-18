# Copilot Codex Task Pack - SR-VIDEO-003 (First Real VGA Mode 13h Checkpoint)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-sr-video-003 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Replace the current static VGA mode 13h readiness scaffold with a first real graphics checkpoint that proves CiukiOS can:

1. switch to DOS-style mode `0x13`
2. draw a deterministic indexed-color frame into the 320x200 plane
3. present that frame through the current GOP-backed video path
4. expose deterministic serial markers and a stronger regression gate than the current grep-only baseline

This is not the full DOOM graphics milestone yet. The goal is to move from "API/scaffold present" to "real frame rendered through the mode 13h path" without broadening scope into audio, VBE completeness, or DOS extender redesign.

## Context
Current state from roadmap + code:
1. `stage2/src/gfx_modes.c` already contains a mode `0x13` plane, palette table, indexed blit helpers, and `gfx_mode_present()`.
2. `com/dosmode13/dosmode13.c` already exercises the current services ABI by setting mode `0x13`, drawing a gradient, and calling `present()`.
3. `scripts/test_vga13_baseline.sh` is still a static grep gate that only validates shell/help/startup marker wiring.
4. The roadmap explicitly says the scaffold exists but the real draw/render path checkpoint is still pending.

This task must focus on making that checkpoint real and observable.

## Scope (4 tasks)

### V1) Strengthen the Mode 13h Runtime Surface
Required behavior:
1. Keep `gfx_mode_set(0x13)` and `gfx_mode_present()` as the core path.
2. Ensure the mode `0x13` plane can be deterministically cleared, drawn into, and presented without depending on shell text rendering side effects.
3. Add lightweight runtime markers for the important transitions:
   - mode switch success/failure
   - plane draw path reached
   - present success/failure
   - frame counter or present count when practical
4. If the current implementation has silent failure cases, make them explicit through deterministic markers.

Constraints:
1. Do not redesign the whole video stack.
2. Do not broaden into VBE mode switching beyond what the current mode `0x13` contract already uses.
3. Keep text-mode shell behavior intact when returning from the sample path.

### V2) Turn the Existing Sample Into a Real Checkpoint Binary
Required behavior:
1. Keep `DOSMODE13.COM` as the reference runtime sample unless a clearly better name is needed.
2. Upgrade the sample so it produces a more meaningful deterministic frame than a bare gradient alone.
3. At minimum, the sample should draw a frame with multiple visually distinct regions, for example:
   - full-screen background fill
   - one or more rectangles/bands
   - a gradient or palette sweep region
   - a small text/marker area if that fits the current surface naturally
4. Emit serial markers that prove the sample reached the frame-complete point.
5. If returning to text mode is already supported by the current ABI, do it cleanly; otherwise document the limitation precisely.

Constraints:
1. Do not pull in WAD parsing or DOOM-specific assets here.
2. Do not require external user-supplied assets.
3. Keep the sample deterministic and freestanding.

### V3) Replace the Static Baseline With a Stronger Gate
Required behavior:
1. Replace or extend `scripts/test_vga13_baseline.sh` so the test validates more than string presence in source files.
2. Preferred direction:
   - keep a static fallback if host runtime capture is still imperfect
   - but add a real runtime path that launches `DOSMODE13.COM` and greps deterministic markers from runtime evidence
3. The gate must clearly distinguish:
   - mode `0x13` entered
   - deterministic frame draw completed
   - present path executed
4. If host capture remains incomplete, preserve a static fallback path similar in spirit to other CiukiOS runtime gates.

Constraints:
1. Do not spend the whole task reworking generic QEMU infrastructure.
2. Reuse the current runtime-gate pattern where possible.

### V4) Documentation and Handoff
Required behavior:
1. Update the relevant roadmap/status doc if the meaning of the VGA baseline materially changes.
2. Create a handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-sr-video-003.md`
3. The handoff must explain:
   - final mode `0x13` sample behavior
   - markers emitted
   - what the new gate proves vs what remains deferred

## Recommended Files To Inspect First
1. `CLAUDE.md`
2. `docs/roadmap-ciukios-doom.md`
3. `docs/subroadmap-sr-video-002.md`
4. `stage2/include/gfx_modes.h`
5. `stage2/src/gfx_modes.c`
6. `stage2/src/stage2.c`
7. `stage2/src/shell.c`
8. `com/dosmode13/dosmode13.c`
9. `scripts/test_vga13_baseline.sh`
10. `Makefile`

## Acceptance Criteria
The task is complete when all of the following are true:
1. CiukiOS has a deterministic runtime sample that switches to mode `0x13`, draws a real indexed frame, and presents it.
2. Runtime serial markers prove the draw/present path was reached.
3. The VGA gate validates a real runtime path when possible, with static fallback only as backup.
4. Existing shell/stage2/runtime behavior does not regress.
5. The handoff clearly explains what was achieved and what still remains before the DOOM frame/menu milestone.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. `make test-vga13-baseline`
4. Any direct runtime command/script added for `DOSMODE13.COM`

If host-specific capture still blocks a full runtime confirmation, document exactly which marker set was still observed and which part fell back to static validation.

## Deliverables
1. Code changes for the first real mode `0x13` checkpoint.
2. Updated gate for VGA validation.
3. Minimal docs updates if the baseline semantics changed.
4. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-copilot-codex-sr-video-003.md`

## Final Handoff Must Include
1. Files changed.
2. Final `DOSMODE13.COM` behavior.
3. Runtime markers and their meaning.
4. Gate behavior: runtime path vs fallback path.
5. Test results.
6. Remaining gaps before a true DOOM first-frame/menu milestone.