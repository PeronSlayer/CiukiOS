# OpenGEM Completion Execution Plan (v0.5.9)

Date: 2026-04-23
Scope: complete OpenGEM desktop functionality on CiukiOS full profile with deterministic validation and no DOS-core regressions.

## 1. Definition of Complete Functionality

OpenGEM is considered complete only when all conditions below are true:

1. Desktop boots reliably from the default full-profile launch path.
2. Mouse, keyboard, and window interaction are responsive.
3. Core OpenGEM apps can open and close without hangs.
4. Clean return to shell is reliable after desktop exit.
5. DOS runtime compatibility gates remain green.
6. Graphical regression gates are green across repeated runs.
7. At least one real legacy x86 machine passes the same acceptance flow.

## 2. Priority Backlog (Issue-Ready)

## P0 - Blocking for Milestone Closure

### OG-P0-01 - DOS syscall trace and compatibility matrix
Priority: P0
Estimate: 1.5 days
Dependencies: none

Steps:
1. Instrument OpenGEM startup and desktop session to capture INT 21h call sequence and return semantics.
2. Build a matrix for each critical function: expected DOS behavior, current behavior, mismatch, risk.
3. Mark hard blockers for launch, interaction, and clean exit.

Acceptance criteria:
1. A committed compatibility matrix exists with all high-frequency syscalls used by GEMVDI and GEM.
2. Every blocker has severity and an implementation task linked.

### OG-P0-02 - Memory manager stress hardening for GEM patterns
Priority: P0
Estimate: 2.0 days
Dependencies: OG-P0-01

Steps:
1. Add stress scenarios for AH=48h, AH=49h, AH=4Ah with nested EXEC-like allocation patterns.
2. Verify MCB chain integrity after each stress phase.
3. Validate PSP consistency and clean recovery paths after failures.

Acceptance criteria:
1. No MCB corruption detected in stress loops.
2. No false errors in memory allocation/free/resize sequences used by GEM.
3. Return-to-shell remains stable after stress runs.

### OG-P0-03 - GEM launch chain determinism and fallback correctness
Priority: P0
Estimate: 1.0 day
Dependencies: OG-P0-01

Steps:
1. Freeze the launch contract for CTMOUSE, GEMVDI, GEM.EXE, GEM.BAT and argument tail handling.
2. Validate expected behavior for each missing-payload scenario.
3. Keep deterministic diagnostics for each branch and failure code.

Acceptance criteria:
1. Launch chain behavior is deterministic across 20 runs.
2. Failure modes are explicit and map to known causes.
3. No silent hang in fallback branches.

### OG-P0-04 - Graphical acceptance gate (real desktop validation)
Priority: P0
Estimate: 2.0 days
Dependencies: OG-P0-02, OG-P0-03

Steps:
1. Add a dedicated OpenGEM acceptance script for full profile that validates desktop readiness and interaction markers.
2. Add repeated run loop (N>=20) with success-rate metrics.
3. Capture and report hang rate, launch latency, and shell-return reliability.

Acceptance criteria:
1. Launch success over 20 runs is at least 90 percent.
2. Clean return to shell over 20 runs is at least 95 percent.
3. Existing DOS/full profile gates still pass.

### OG-P0-05 - Milestone close gate and release criteria
Priority: P0
Estimate: 0.5 day
Dependencies: OG-P0-04

Steps:
1. Define and commit a final OpenGEM complete gate checklist.
2. Encode pass criteria in scripts and docs.
3. Require green status before milestone closure.

Acceptance criteria:
1. Single documented pass/fail gate exists for milestone closure.
2. Gate is reproducible on developer machine and CI-like local workflow.

## P1 - Completion Quality and Robustness

### OG-P1-01 - VDI/AES behavior completion for desktop fidelity
Priority: P1
Estimate: 3.0 days
Dependencies: OG-P0-01

Steps:
1. Identify missing or partially matching VDI/AES semantics used by real OpenGEM desktop operations.
2. Implement missing behavior incrementally with compatibility tests.
3. Validate text, clipping, fill, and coordinate semantics against expected GEM behavior.

Acceptance criteria:
1. Desktop interaction is visually and behaviorally stable.
2. No fallback to local stubs for mandatory desktop operations.

### OG-P1-02 - Long session soak tests
Priority: P1
Estimate: 1.0 day
Dependencies: OG-P0-04

Steps:
1. Run sustained sessions for 20 to 30 minutes with repeated app open/close cycles.
2. Monitor for hangs, memory drift, and degraded input responsiveness.
3. Store artifacts and summary metrics per run.

Acceptance criteria:
1. Zero crashes in defined soak campaign.
2. No measurable corruption indicators after session termination.

### OG-P1-03 - Real hardware validation lane
Priority: P1
Estimate: 2.0 days
Dependencies: OG-P0-04

Steps:
1. Define one legacy hardware target and execution checklist equal to QEMU acceptance flow.
2. Run launch, interaction, app lifecycle, and shell-return tests.
3. Document QEMU versus hardware differences and fix high-impact drift.

Acceptance criteria:
1. Hardware run passes the same functional checklist.
2. Any residual deltas are documented with risk and workaround.

### OG-P1-04 - Documentation and troubleshooting normalization
Priority: P1
Estimate: 0.5 day
Dependencies: OG-P0-05

Steps:
1. Align runtime docs with actual launch order and payload requirements.
2. Add troubleshooting section for known failure signatures and diagnostics.
3. Update roadmap/changelog status only after gate pass.

Acceptance criteria:
1. Docs are consistent with real behavior and scripts.
2. New contributors can reproduce the desktop run without hidden steps.

## P2 - Post-Milestone Hardening

### OG-P2-01 - Regression lock for known historical bugs
Priority: P2
Estimate: 1.0 day
Dependencies: OG-P0-05

Steps:
1. Add targeted regression checks for carry propagation, find-next behavior, alias handling, and memory resize/free paths.
2. Integrate checks into the aggregate full-profile suite.

Acceptance criteria:
1. Historical regressions are automatically detected before merge.

### OG-P2-02 - Performance baseline and budget tracking
Priority: P2
Estimate: 1.0 day
Dependencies: OG-P1-02

Steps:
1. Record baseline launch time, interaction readiness time, and memory footprint.
2. Define acceptable drift thresholds.
3. Add periodic budget checks.

Acceptance criteria:
1. Measured budgets exist and are enforced in routine validation.

## 3. Execution Order

1. OG-P0-01
2. OG-P0-02
3. OG-P0-03
4. OG-P0-04
5. OG-P0-05
6. OG-P1-01
7. OG-P1-02
8. OG-P1-03
9. OG-P1-04
10. OG-P2-01
11. OG-P2-02

## 4. Suggested Sprint Cut (Practical)

Sprint A (3 to 4 days): OG-P0-01, OG-P0-02, OG-P0-03
Sprint B (2 to 3 days): OG-P0-04, OG-P0-05
Sprint C (3 to 5 days): OG-P1-01, OG-P1-02, OG-P1-03, OG-P1-04
Sprint D (1 to 2 days): OG-P2-01, OG-P2-02

## 5. Milestone Exit Criteria

All items below are mandatory:

1. All P0 issues closed and validated.
2. Graphical acceptance gates pass with target rates.
3. DOS core baseline tests remain green.
4. At least one real hardware validation completed.
5. Documentation aligned with final runtime behavior.
