# OpenGEM Real Hardware Validation Lane (OG-P1-03)

Date: 2026-04-24  
Scope: reproducible hardware validation flow equivalent to full-profile QEMU acceptance.

## Target Hardware Profile

1. Legacy x86 BIOS machine (Pentium III/Pentium 4 era or equivalent retro-compatible board).
2. VGA-compatible display path.
3. Keyboard mandatory, PS/2 mouse recommended.
4. Boot media compatible with CiukiOS full image path.

## Preconditions

1. Build full image on dev host:
   `bash scripts/build_full.sh`
2. Prepare boot media from `build/full/ciukios-full.img`.
3. Print or copy these docs locally:
   - `docs/opengem-hardware-validation-lane.md`
   - `docs/templates/opengem-hardware-execution-template.md`
   - `docs/templates/opengem-hardware-evidence-template.json`

## Execution Checklist

1. Boot hardware from prepared media.
2. Confirm boot markers and shell readiness.
3. Launch OpenGEM path and verify visual responsiveness:
   - keyboard interaction
   - pointer motion/button behavior if mouse present
   - desktop/app open-close cycle
4. Verify clean return-to-shell reliability.
5. Repeat for at least 20 runs.
6. Capture anomalies with timestamp and run index.

## Required Evidence

1. Filled execution template (run-by-run status).
2. JSON evidence file with metrics and risk annotations.
3. Photos/video of at least:
   - boot marker phase
   - OpenGEM desktop ready state
   - return-to-shell prompt
4. Note deltas between QEMU and hardware behavior.

## Recommended Command Set (Host Side)

1. Baseline trace (host):
   `bash scripts/opengem_trace_full.sh --label hw-baseline --timeout-sec 12`
2. Acceptance baseline (host):
   `bash scripts/opengem_acceptance_full.sh --label hw-baseline --runs 20 --timeout-sec 12 --no-build`
3. Prepare hardware package:
   `bash scripts/opengem_hardware_lane_pack.sh hw-<date>`

## Exit Criteria

1. Hardware lane executed with >=20 runs.
2. Launch + interaction + shell-return outcomes documented.
3. Residual deltas classified by severity with workaround/proposed fix.
