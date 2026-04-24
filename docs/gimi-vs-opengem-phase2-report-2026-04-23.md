# CiukiOS Technical Report
## OpenGEM vs GIMI and Phase 2 Execution Plan

Date: 2026-04-23
Scope: Evaluate whether switching from OpenGEM to GIMI is easier, then define a complete Phase 2 plan to execute safely.

---

## 1. Executive Summary

Recommendation: keep OpenGEM as primary desktop path and treat GIMI as a Phase 2 exploratory track, not as an immediate replacement.

Why:
1. OpenGEM integration is already deep in CiukiOS runtime and build flow.
2. Existing code and assets are aligned to GEM/AES/VDI behavior and boot chain.
3. A direct switch to GIMI would create a parallel compatibility stack with uncertain payoff.
4. The fastest path to a stable desktop milestone is to complete current OpenGEM stabilization and evaluate GIMI via bounded PoC.

Decision confidence: medium-high (based on current repository evidence plus public project metadata).

---

## 2. Current CiukiOS Baseline (Evidence)

### 2.1 Strategic and Architecture Constraints
1. DOS core is the mandatory contract; GUI is optional and layered above DOS services.
2. Compatibility-first policy: additive features must not break DOS-visible behavior.
3. Required interactive surfaces already include video, keyboard, timer, and mouse contracts.

Reference docs:
- docs/dos-core-spec-v0.1.md
- docs/dos-core-implementation-plan-v0.1.md

### 2.2 OpenGEM Integration Already Implemented
Evidence in repository indicates non-trivial progress:
1. Full image payload pipeline and launcher flow are OpenGEM-specific.
2. Stage2 launch chain already targets GEM startup path.
3. Runtime fixes already invested in GEM startup behavior and DOS allocation edge cases.

Key references:
- assets/full/opengem/README.md
- README.md (runtime stabilization notes)
- docs/phase3-completion.md
- src/com/opengem.asm
- src/com/opengem_stub.asm
- src/com/opengem_desktop.asm

### 2.3 Observed Technical Debt / Caveats
1. OpenGEM desktop stability is still pending in graphics runtime completeness.
2. Some local desktop stub files are exploratory and should not be confused with final GEM path.
3. Headless/serial gate coverage is strong; full graphical behavioral coverage still needs expansion.

---

## 3. GIMI Snapshot (What We Know)

Public project metadata (SourceForge):
1. Positioning: GUI/multitasking interpreter.
2. Historical stack: QuickBASIC-centric ecosystem with own scripting model.
3. License: GPLv2.
4. Last update shown as 2015-11-14.

Practical implication:
- GIMI appears to be a standalone DOS environment/toolchain model, not a drop-in GEM/AES/VDI compatible replacement.

---

## 4. Technical Comparison Matrix

Scoring scale: 1 (bad) to 5 (best for CiukiOS current goals)

| Criterion | OpenGEM | GIMI | Notes |
|---|---:|---:|---|
| Reuse of current CiukiOS work | 5 | 1 | OpenGEM path already integrated in build/runtime |
| Near-term implementation speed | 4 | 2 | GIMI requires new integration assumptions |
| DOS core contract alignment | 4 | 3 | Both run on DOS, but OpenGEM already mapped to current surfaces |
| API/model predictability for desktop apps | 4 | 2 | GEM model is known in current repo; GIMI app model less aligned |
| Risk of regressions | 3 | 2 | Switching stack now introduces broad regression surface |
| Long-term maintainability | 3 | 3 | Both legacy; maintainability depends on internal wrappers/tests |
| Licensing clarity for distribution | 3 | 3 | Both require GPL-compliant handling in packaging/docs |
| Aesthetic/custom UX potential | 3 | 4 | GIMI may be visually appealing but needs validation effort |

Weighted outcome (current milestone priorities): OpenGEM clearly wins for delivery speed and risk control.

---

## 5. Complexity and Effort Estimate

### 5.1 If Continuing OpenGEM (Recommended Path)
Estimated effort to reach stable desktop milestone: 2 to 5 weeks equivalent engineering focus.

Main work packages:
1. Complete memory manager hardening for GEM workload patterns.
2. Expand INT and VDI/AES behavioral completeness where GEM depends on exact semantics.
3. Add graphical regression gates (not only serial markers).
4. Validate on at least one real legacy target.

### 5.2 If Switching to GIMI Now (Not Recommended)
Estimated effort for parity with current OpenGEM progress: 6 to 12+ weeks, with higher uncertainty.

Main unknowns/costs:
1. Runtime contract mapping from CiukiOS DOS core to GIMI expectations.
2. Packaging and launcher semantics redesign.
3. App ecosystem path and shell integration redesign.
4. New diagnostics and test harness definition.
5. Possible rework of existing OpenGEM-specific fixes and launch chain.

Uncertainty factor: high, because docs/behavioral contracts for exact integration path are not yet validated in-tree.

---

## 6. Risk Register

### 6.1 OpenGEM Path Risks
1. Risk: Desktop rendering/event-loop edge-case instability.
   - Mitigation: introduce deterministic UI smoke scripts and frame-based assertions.
2. Risk: Memory allocation corner cases in nested EXEC flows.
   - Mitigation: stress tests for INT 21h AH=48h/49h/4Ah with GEM-like sequences.
3. Risk: Emulation vs real hardware behavior drift.
   - Mitigation: weekly hardware validation lane.

### 6.2 GIMI Path Risks
1. Risk: Integration contract mismatch (non-drop-in architecture).
   - Mitigation: bounded PoC before any migration commitment.
2. Risk: Unknown runtime assumptions from QuickBASIC/scripting model.
   - Mitigation: instrumented DOS syscall and memory trace capture.
3. Risk: Schedule slip due to rebuilding test infrastructure.
   - Mitigation: strict phase gates with explicit stop criteria.

---

## 7. Recommended Strategy

Use a dual-track strategy:
1. Primary track: finish OpenGEM milestone in current roadmap.
2. Secondary track (Phase 2): execute a low-cost GIMI feasibility program.

This preserves delivery while still evaluating whether GIMI can become a future optional desktop profile.

---

## 8. Phase 2 Plan (Complete Work Plan)

Goal of Phase 2:
- Determine in a measurable way whether GIMI can be integrated as an optional desktop profile without derailing OpenGEM roadmap.

Timebox:
- 10 working days (2 weeks) hard cap.

Team mode:
- 1 engineer full-time equivalent or 2 engineers part-time.

### 8.1 Deliverables
1. D1: Integration feasibility memo (contracts, blockers, legal/package notes).
2. D2: Minimal boot-to-launch PoC for GIMI on full profile.
3. D3: Comparative benchmark sheet (boot success, launch latency, memory footprint, stability loops).
4. D4: Go/No-Go recommendation for Phase 3+ roadmap branch.

### 8.2 Workstreams

#### WS1 - Source and Packaging Audit (Day 1-2)
1. Collect GIMI binaries/source provenance and verify reproducible acquisition path.
2. Define licensing distribution checklist (GPL obligations in CiukiOS artifacts).
3. Create asset layout proposal under full profile without touching current OpenGEM flow.

Exit criteria:
- Artifacts and license obligations are clearly documented and reproducible.

#### WS2 - Runtime Contract Mapping (Day 2-4)
1. Map required DOS interrupts and memory assumptions from GIMI startup.
2. Capture syscall trace during startup attempts.
3. Build compatibility gap table (supported, partial, missing).

Exit criteria:
- A concrete list of missing runtime behaviors with severity and implementation cost.

#### WS3 - Launcher and Shell Integration PoC (Day 4-6)
1. Add isolated command path (for example a separate shell command) gated behind build/profile flag.
2. Keep OpenGEM launch path untouched.
3. Implement diagnostics markers for deterministic gate parsing.

Exit criteria:
- GIMI can be launched from CiukiOS shell in QEMU full profile at least once with deterministic logs.

#### WS4 - Stability and UX Smoke Gates (Day 6-8)
1. Create scripted run loops (N launches, timeout bounded).
2. Measure crash/hang rate, return-to-shell reliability, and visual readiness marker.
3. Record memory behavior before/after launch.

Exit criteria:
- Quantified stability data suitable for decision.

#### WS5 - Decision Package (Day 9-10)
1. Consolidate matrix: engineering cost, risk, UX gain, compatibility impact.
2. Produce Go/No-Go decision with branch strategy.
3. If Go: draft Phase 3 migration branch plan.
4. If No-Go: archive findings and continue OpenGEM-only path.

Exit criteria:
- Decision approved with explicit next roadmap action.

### 8.3 Non-Goals for Phase 2
1. Do not replace OpenGEM in mainline.
2. Do not redesign DOS core APIs purely for GIMI.
3. Do not merge unstable desktop code into default boot path.

### 8.4 Gate Metrics
1. Launch success rate over 20 runs >= 90% in QEMU full profile.
2. Clean return to shell over 20 runs >= 95%.
3. No new regressions in existing DOS/OpenGEM baseline gates.
4. Additional memory pressure within acceptable envelope (define target during WS2 baseline).

### 8.5 Branching and Integration Policy
1. Use dedicated feature branch for GIMI feasibility.
2. Keep all changes isolated behind feature flags/profile switches.
3. Merge only documentation and harmless tooling until Go decision.

### 8.6 Suggested Repository Outputs for Phase 2
1. docs/gimi-feasibility-notes.md
2. docs/gimi-compat-gap-matrix.md
3. scripts/qemu_test_gimi_poc.sh
4. assets/full/gimi/README.md
5. Optional command source isolated from current opengem command path.

---

## 9. Go/No-Go Decision Framework

Go to broader integration only if all are true:
1. GIMI launch and shell-return reliability targets are met.
2. Compatibility gaps are small/medium and bounded to <= 2 weeks extra implementation.
3. No regressions are introduced into OpenGEM and DOS core mandatory gates.
4. Team accepts GPL packaging obligations and maintenance cost.

Otherwise: No-Go, keep GIMI as archived experimental option and continue OpenGEM roadmap.

---

## 10. Final Recommendation for Current Milestone

1. Continue OpenGEM as primary delivery target.
2. Approve a strict, timeboxed Phase 2 GIMI feasibility study (2 weeks) with hard gates.
3. Decide based on measured data, not aesthetics only.

This approach preserves velocity, limits risk, and still gives room to adopt GIMI later if it proves technically superior in CiukiOS context.

---

## Appendix A - Quick Action Checklist

Week 1:
1. Audit GIMI artifacts/license.
2. Map runtime contracts and missing services.
3. Achieve first deterministic launch attempt.

Week 2:
1. Run stability loops and gather metrics.
2. Build comparative report.
3. Produce Go/No-Go decision.

## Appendix B - Data Sources Used

Internal repository evidence:
- docs/dos-core-spec-v0.1.md
- docs/dos-core-implementation-plan-v0.1.md
- docs/phase3-completion.md
- README.md
- assets/full/opengem/README.md
- src/com/opengem.asm
- src/com/opengem_stub.asm
- src/com/opengem_desktop.asm

External metadata:
- https://sourceforge.net/projects/gimi/
