# Copilot Task Pack - OT-DEMO-001 (Demo-Oriented Shell Cleanup and Real-Time Graphics Showcase)

## Mandatory Branch Isolation
The assigned agent must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/third-agent-ot-demo-001 origin/main
```

No commits on `main`. No force-push on shared branches.

## Mission
Prepare a more presentable short-form CiukiOS demo by improving two visible areas of the product surface:

1. make the shell feel less like an internal test harness and more like a product-facing environment
2. add a small graphical demo program that draws a deterministic real-time scene for roughly 30 seconds

This is intentionally demo-oriented work. It should improve the current public-facing impression of CiukiOS without broadening into speculative architecture rewrites.

## Context
Current state from roadmap + code:
1. CiukiOS already boots reliably into Stage2 and exposes a DOS-like shell with many commands, including some that exist mainly for bring-up, diagnostics, or transitional testing.
2. The system already has a graphics path with a mode `0x13` scaffold and sample programs, but there is not yet a polished short visual demo meant to showcase the current state of the project to a viewer.
3. For a short video demo, the visible user-facing shell surface matters as much as the low-level capabilities.

This task should stay focused on making the current state look intentional and demoable.

## Scope (4 tasks)

### D1) Shell Command Surface Cleanup
Required behavior:
1. Review the current shell command list and identify commands that are clearly internal-only, transitional, or redundant for a public-facing demo.
2. Remove, hide from `help`, or otherwise demote commands that make the shell feel like a test harness instead of a product.
3. Preserve commands that are needed for believable everyday interaction during a demo, such as navigation, inspection, launching programs, and a small set of clearly useful utilities.
4. Keep the final shell command surface coherent and intentionally limited rather than maximal.

Constraints:
1. Do not remove commands that are still required by current boot/test/runtime flows unless they can be safely hidden from the primary help/discoverability surface.
2. Do not break existing scripted startup behavior.
3. Prefer product-oriented presentation over exposing every low-level tool.

### D2) Help and Shell Presentation Polish
Required behavior:
1. Make the visible shell help/output feel cleaner and more final-product-oriented.
2. Group or rewrite help text so the shell looks curated rather than like an internal debug menu.
3. If the shell prints command lists or startup discoverability text, make that output shorter, clearer, and more demo-friendly.
4. Preserve deterministic output and current runtime behavior.

Constraints:
1. Do not redesign the full shell UX.
2. Keep text changes concise and compatible with existing regression expectations where practical.
3. Avoid broad scope creep into unrelated shell editing/resolver work.

### D3) Real-Time Graphics Demo Program
Required behavior:
1. Add one dedicated graphical demo program intended specifically for a short video capture.
2. The program should draw an animated deterministic scene in real time for roughly 30 seconds.
3. The animation should be visually richer than a static gradient, for example using moving bars, waves, palette effects, geometric motion, layered color regions, or other simple but intentional real-time effects.
4. The demo must not rely on external user-supplied assets.
5. The program should emit clear runtime markers so a future gate or capture script can prove the main phases were reached.

Constraints:
1. Keep the implementation freestanding and compatible with the existing CiukiOS graphics/runtime path.
2. Do not broaden into WAD parsing, audio, or game-engine style architecture.
3. Prefer deterministic animation over random behavior.

### D4) Demo Launch and Minimal Validation
Required behavior:
1. Ensure the new graphics demo is easy to launch during a recorded session.
2. Add or update a small validation path proving the demo program is packaged and runnable.
3. If a runtime gate is practical, use the established runtime+fallback pattern already used elsewhere in the repo.
4. Update minimal docs/handoff notes so another agent or the user can reproduce the demo flow.

Constraints:
1. Do not spend the task redesigning generic QEMU infrastructure.
2. Keep validation pragmatic: enough to prove the demo is launchable and reaches the intended frame/animation path.

## Recommended Files To Inspect First
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/roadmap-ciukios-doom.md`
5. `stage2/src/shell.c`
6. `stage2/src/stage2.c`
7. `stage2/src/gfx_modes.c`
8. `stage2/include/gfx_modes.h`
9. existing graphics sample programs under `com/`
10. relevant test scripts under `scripts/`

## Acceptance Criteria
The task is complete when all of the following are true:
1. The shell surface shown to a user in a demo is visibly cleaner and more product-oriented.
2. Low-value internal/test-only commands are either removed from the primary surface or clearly demoted.
3. A dedicated real-time graphics demo program exists and runs for about 30 seconds with deterministic animated output.
4. The demo program is easy to launch and its main phases are observable through deterministic markers.
5. Existing shell/runtime behavior needed by CiukiOS does not regress.
6. The handoff explains exactly what was polished and how to reproduce the short demo flow.

## Validation
Run before handoff:
1. `make all`
2. `make test-stage2`
3. Any direct runtime validation path added for the new graphics demo
4. Any updated shell/help validation path if touched

If host-specific QEMU capture still limits runtime confirmation, document exactly what was observed and what fell back to static validation.

## Deliverables
1. Shell cleanup/presentation changes suitable for a public-facing demo
2. A new real-time graphics demo program
3. Any minimal docs/test updates needed
4. A handoff file:
   - `docs/handoffs/YYYY-MM-DD-third-agent-ot-demo-001.md`

## Final Handoff Must Include
1. Files changed.
2. Final shell command/help surface and what was removed or hidden.
3. Final graphics demo behavior over its ~30 second run.
4. Runtime markers and what they prove.
5. Tests run and their status.
6. Remaining rough edges before the demo can be considered fully presentation-ready.