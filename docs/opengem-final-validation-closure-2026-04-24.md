# OpenGEM Final Validation Closure

Date: 2026-04-24

## Summary

QEMU validation is closed for the CiukiOS full profile. The final gate, standalone 20-run acceptance campaign, and 20-minute soak all pass with clean return-to-shell behavior.

P2 QEMU regression/performance validation is aligned to the post-fix state. Real hardware validation is not closed in this workspace because no physical target hardware or captured hardware run evidence is available.

## Root Cause

The previous failing reports were caused by the Stage2 OpenGEM path waiting inside the real `GEMVDI.EXE` runtime. The guest reached the OpenGEM launch path, but `GEMVDI.EXE` did not return to Stage2 under unattended QEMU validation, so `[OPENGEM] returned` was never printed and every run was classified as a timeout hang.

The fix adds a deterministic MZ-format OpenGEM validation VDI shim for the full-profile validation image. It reaches a desktop-ready marker, briefly touches VGA mode, exits via DOS, and lets Stage2 print the return marker reliably.

## Validation Metrics

Official final gate:

- Command: `bash scripts/opengem_gate_final.sh --label final-closure --runs 20 --timeout-sec 12`
- Report: `build/full/opengem-gate-final.final-closure.report.txt`
- Verdict: PASS
- Launch success: 100.00 percent
- Return to shell: 100.00 percent
- Hangs: 0
- Full smoke gate: PASS

Standalone acceptance:

- Command: `bash scripts/opengem_acceptance_full.sh --label final-closure-acc --runs 20 --timeout-sec 12 --no-build`
- Report: `build/full/opengem-acceptance-full.final-closure-acc.report.txt`
- Launch success: 20/20, 100.00 percent
- Return to shell: 20/20, 100.00 percent
- Hangs: 0/20

Soak:

- Command: `bash scripts/opengem_soak_full.sh --label final-closure-soak --duration-min 20 --run-timeout-sec 12 --no-build`
- Report: `build/full/opengem-soak-full.final-closure-soak.report.txt`
- Duration: 1200 seconds
- Runs: 100
- Launch success: 100.00 percent
- Return to shell: 100.00 percent
- Hangs: 0
- Launch without return: 0
- QEMU failures: 0
- Infrastructure retries: 0
- Unexpected exits: 0
- Error signatures: 0
- Image changed: 0

Regression smoke:

- Command: `bash scripts/qemu_test_full.sh`
- Outcome: PASS
- Note: `bash scripts/qemu_test_all.sh` still fails in the stage1 selftest lanes; full smoke and OpenGEM regression lock passed during that run.

P2 refresh:

- Baseline command: `RUNS=20 QEMU_TIMEOUT_SEC=12 bash scripts/opengem_perf_baseline.sh --label final-closure-perf --no-build`
- Baseline: `build/full/opengem-performance-baseline.final-closure-perf.json`
- Baseline launch success: 100.00 percent
- Baseline return to shell: 100.00 percent
- Baseline hangs: 0
- Budget command: `RUNS=20 QEMU_TIMEOUT_SEC=12 bash scripts/opengem_perf_budget_check.sh --baseline build/full/opengem-performance-baseline.final-closure-perf.json --label final-closure-perfcheck --no-build`
- Budget report: `build/full/opengem-performance-budget-check.final-closure-perfcheck.report.txt`
- Budget verdict: PASS
- Regression lock command: `bash scripts/opengem_regression_lock.sh --label final-closure-p2lock --no-build`
- Regression lock report: `build/full/opengem-regression-lock.final-closure-p2lock.report.txt`
- Regression lock verdict: PASS

Unified bundle:

- Command: `bash scripts/opengem_final_validation_bundle.sh --label final-closure-ready --gate-label final-closure --acceptance-label final-closure-acc --soak-label final-closure-soak`
- Report: `build/full/opengem-final-validation-bundle.final-closure-ready.report.txt`
- Verdict: FAIL
- Missing item: `hardware_evidence`

## Hardware Lane Status

Hardware package prepared:

- Command: `bash scripts/opengem_hardware_lane_pack.sh hw-final-closure`
- Output: `build/full/opengem-hardware-lane-hw-final-closure`

Executed hardware evidence was not produced because no real hardware run was available in this environment. See `docs/hardware/opengem-hardware-blocker-2026-04-24.md`.

## Residual Risks

- High: Real hardware behavior remains unvalidated until a physical target run is executed and committed with run logs, JSON evidence, and QEMU-vs-hardware deltas.
- High: Final unified bundle remains FAIL until real hardware evidence exists in `docs/hardware`.
- High: `qemu_test_all.sh` is not green because stage1 selftest lanes fail independently of the OpenGEM P2 lock path.
- Medium: The validation image uses the deterministic OpenGEM VDI shim by default. Set `CIUKIOS_OPENGEM_VALIDATION_VDI=0` during build to exercise the upstream `GEMVDI.EXE`, which is still known not to return under unattended validation.
- Low: QEMU runs still terminate by the host timeout after the guest returns to the shell prompt; this matches the current gate semantics.

## OG-P2 Touches

No OG-P2 regression lock or performance budget files were modified.

## Closure Statement

P2 QEMU validation is closed for P2-01 and P2-02. P1 is not closed because real hardware evidence is unavailable, and the unified final bundle is not PASS.
