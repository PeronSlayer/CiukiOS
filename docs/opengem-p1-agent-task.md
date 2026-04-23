# Task For Parallel Agent - Complete OG-P1

Date: 2026-04-24
Owner: Parallel agent
Scope: complete OG-P1 end-to-end, without touching OG-P2 implementation files.

## Mission

Close all OG-P1 items from `docs/opengem-completion-execution-plan-v0.5.9.md`:
1. OG-P1-01 VDI/AES behavior completion
2. OG-P1-02 long session soak tests
3. OG-P1-03 real hardware validation lane (docs + checklist + evidence placeholders)
4. OG-P1-04 docs/troubleshooting normalization

## Constraints (Important)

1. Do not edit `scripts/opengem_regression_lock.sh`.
2. Do not edit `scripts/qemu_test_all.sh` lines related to OG-P2 regression lock integration.
3. Keep runtime changes focused on OG-P1 goals only.
4. Do not revert existing unrelated changes.
5. Keep outputs reproducible with non-interactive commands.

## Required Deliverables

1. Code/runtime updates for OG-P1-01 in relevant stage1/stage2/desktop paths.
2. New soak test runner and report format for OG-P1-02.
3. Hardware lane package for OG-P1-03:
   - hardware checklist doc
   - execution template
   - evidence collection format
4. Documentation normalization for OG-P1-04:
   - launch order
   - payload requirements
   - troubleshooting signatures
5. Makefile targets for OG-P1 flows.
6. Updated roadmap/changelog/doc status where appropriate.

## Acceptance Criteria (Must Pass)

1. Desktop interaction stability improved with no mandatory stub fallback for required operations.
2. Soak campaign runs 20-30 minutes and outputs machine-readable report.
3. Hardware validation lane is executable by another developer from docs only.
4. Existing full-profile smoke and stage1 gates still run.
5. OG-P1 artifacts are committed and pushed with clear commit grouping.

## Suggested Execution Plan

1. Baseline capture:
   - run current OpenGEM gate + acceptance and archive outputs.
2. Implement OG-P1-01 increments with small commits.
3. Add OG-P1-02 soak harness and validate reports.
4. Create OG-P1-03 hardware lane docs and templates.
5. Finalize OG-P1-04 docs + troubleshooting.
6. Re-run validation matrix and publish summary.

## Mandatory Final Report From Parallel Agent

Provide a concise report containing:
1. Files modified/created.
2. Commands run and outcomes.
3. OG-P1 acceptance evidence and metrics.
4. Remaining risks with severity.
5. Confirmation that OG-P2 files were not modified.
