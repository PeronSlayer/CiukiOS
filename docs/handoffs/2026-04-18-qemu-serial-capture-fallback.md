# Handoff: QEMU Serial Capture Fallback for Runtime Gates

**Date:** 2026-04-18
**Scope:** `run_ciukios.sh`, `scripts/test_stage2_boot.sh`, `scripts/test_dosrun_simple_program.sh`, `documentation.md`

## Context and Goal

Runtime gates on this Linux desktop host were repeatedly failing in `INFRA` state because
headless QEMU runs produced an empty serial sink, even though the system could boot and
emit markers when QEMU had a normal graphical display.

Goal: make `make test-stage2` and `make test-dosrun-simple` reliably classifiable on this
host without weakening the existing serial-marker based validation model.

## Changes Made

1. **Adjusted headless QEMU launch path in `run_ciukios.sh`**
   - Replaced `-nographic` with `-display none -monitor none` for headless runs.
   - When a serial file sink is requested in headless mode, QEMU now emits serial on
     stdout and the runner mirrors it into the requested file via `tee`.
   - This keeps GOP/VGA available while still producing a persistent serial artifact.

2. **Added automatic retry logic to `test_stage2_boot.sh`**
   - First attempt still runs headless.
   - If no runtime markers are captured, the script automatically retries with
     `CIUKIOS_QEMU_HEADLESS=0` and the same serial capture contract.
   - Pass/fail matching now uses the combined evidence log that contains both runner
     output and appended serial output.

3. **Added the same retry logic to `test_dosrun_simple_program.sh`**
   - Headless-first, graphical-fallback-second.
   - Required/forbidden patterns now evaluate against the combined evidence log.

4. **Updated durable documentation**
   - `documentation.md` now records that runtime gates fall back to graphical QEMU on
     hosts where headless serial capture is silent.

## Validation Performed

1. `make all`
2. `TIMEOUT_SECONDS=90 make test-stage2`
   - headless attempt silent
   - graphical fallback captured full serial markers
   - gate passed
3. `TIMEOUT_SECONDS=90 make test-dosrun-simple`
   - headless attempt silent
   - graphical fallback captured shell + dosrun markers
   - gate passed

## Decisions

1. Kept headless as the first attempt to preserve the existing fast/quiet flow where it works.
2. Used a graphical fallback instead of weakening pattern matching or suppressing infra failures.
3. Kept serial-marker based validation as the source of truth; only the transport/capture path changed.

## Residual Risks

1. The fallback depends on QEMU being able to start with a graphical display on the host.
2. On fully headless CI runners with no display backend, gates still depend on headless serial working.
3. `build/debugcon.log` still remains sparse/noisy on this host and is only a secondary diagnostic path.

## Next Step

If needed later, generalize the fallback into a reusable helper for other QEMU-based gates so
all runtime validation scripts share the same capture policy.