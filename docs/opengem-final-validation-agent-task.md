# Task For Parallel Agent - Close OpenGEM Final Validation

Date: 2026-04-24
Owner: Parallel agent
Scope: close the remaining validation and evidence gaps for OpenGEM completion on CiukiOS full profile.

## Mission

Close the remaining non-green items after OG-P0/OG-P1/OG-P2 implementation work:
1. Official final gate must pass.
2. Acceptance campaign over 20 runs must pass.
3. Soak campaign over 20 to 30 minutes must pass.
4. Hardware lane must be executed with committed evidence.

This is not a broad feature task. The current gap is final validation closure, mainly around clean return-to-shell and hang elimination.

## Current Evidence (Starting Point)

Use these committed/local artifacts as the baseline truth:

1. Official final gate currently fails:
   - `build/full/opengem-gate-final.smokecheck.report.txt`
   - current result: `Verdict: FAIL`
   - current problem: `return_to_shell_rate_percent: 0.00`, `hang_count: 1`

2. 20-run acceptance currently fails milestone criteria:
   - `build/full/opengem-acceptance-full.p1-post.report.txt`
   - current result: `launch_success_rate_percent: 100.00`, `return_to_shell_rate_percent: 0.00`, `hang_count: 20`

3. 20-minute soak currently fails milestone criteria:
   - `build/full/opengem-soak-full.p1.report.txt`
   - current result: `launch_success_rate_percent: 100.00`, `return_to_shell_rate_percent: 0.00`, `hang_count: 100`

4. Hardware lane docs/templates exist, but no committed executed evidence exists yet:
   - `docs/opengem-hardware-validation-lane.md`
   - `docs/templates/opengem-hardware-execution-template.md`
   - `docs/templates/opengem-hardware-evidence-template.json`

## Hard Problem To Solve

The desktop launches, but does not return cleanly to DOS/shell under validation loops. Treat this as the main blocker unless evidence proves otherwise.

Primary suspected surfaces:
1. Stage2 OpenGEM launch/return path.
2. Desktop exit behavior and exit propagation.
3. Runtime state cleanup after GEM/GEMVDI/desktop completion.
4. Any path where QEMU exits only by timeout instead of normal runtime return.

## Constraints

1. Do not revert unrelated changes.
2. Do not weaken thresholds in existing gates just to get a green report.
3. Keep OG-P2 regression/performance tooling intact unless a strictly necessary compatibility fix is needed.
4. Keep all commands non-interactive and reproducible.
5. Do not fabricate hardware evidence. If real hardware is unavailable, stop and report the blocker explicitly.

## Required Deliverables

1. Runtime/code fixes needed to make clean return-to-shell reliable.
2. Fresh passing official gate report.
3. Fresh passing 20-run acceptance report.
4. Fresh passing 20 to 30 minute soak report.
5. Executed hardware lane package with committed evidence files.
6. Final summary note with metrics and residual risks.

## Required Commands

Run these commands after fixes and archive the resulting artifacts:

1. Official final gate:
   - `bash scripts/opengem_gate_final.sh --label final-closure --runs 20 --timeout-sec 12`

2. Acceptance 20 runs:
   - `bash scripts/opengem_acceptance_full.sh --label final-closure-acc --runs 20 --timeout-sec 12 --no-build`

3. Soak 20 to 30 minutes:
   - `bash scripts/opengem_soak_full.sh --label final-closure-soak --duration-min 20 --run-timeout-sec 12 --no-build`

4. Hardware lane package preparation:
   - `bash scripts/opengem_hardware_lane_pack.sh hw-final-closure`

## Acceptance Criteria (Must Pass)

1. Official final gate report says `Verdict: PASS`.
2. Acceptance 20-run report meets:
   - launch success >= 90 percent
   - return to shell >= 95 percent
   - hangs within gate limits
3. Soak report for 20 to 30 minutes shows:
   - zero crashes
   - no corruption indicators
   - clean return behavior documented and acceptable
4. Hardware execution evidence is committed, not only templated.
5. Existing DOS/full smoke behavior is not regressed.

## Hardware Evidence Requirements

Commit all of the following from a real executed run:

1. Filled execution template copied to a run-specific file, for example:
   - `docs/hardware/opengem-hardware-execution-2026-04-24.md`
2. Filled JSON evidence file, for example:
   - `docs/hardware/opengem-hardware-evidence-2026-04-24.json`
3. Short markdown summary of QEMU vs hardware deltas, for example:
   - `docs/hardware/opengem-hardware-delta-2026-04-24.md`

If photos or videos are available and acceptable to store in-repo, commit them. If they are too large or not suitable for the repo, commit filenames, hashes, and storage location references in the markdown summary.

## Suggested Execution Order

1. Reproduce the failing return-to-shell behavior from the existing reports.
2. Fix the runtime/desktop/stage2 return path with small, reviewable commits.
3. Re-run official gate.
4. Re-run acceptance 20 runs.
5. Re-run soak 20 to 30 minutes.
6. Execute hardware lane and commit evidence.
7. Push with clear commit grouping.

## Mandatory Final Report From Parallel Agent

Provide a concise report containing:

1. Files modified/created.
2. Root cause found for the return-to-shell/hang issue.
3. Commands run and outcomes.
4. Final metrics from gate, acceptance, soak, and hardware lane.
5. Remaining risks with severity.
6. Confirmation whether any OG-P2 file had to be touched.