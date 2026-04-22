# Parallel Next Tasks (2026-04-16) - oZone DOS App Integration (Optional Path)

Context:
- Goal: integrate `oZone GUI` as an optional DOS app inside CiukiOS/FreeDOS runtime.
- Keep CiukiOS core ownership intact; no hard dependency on oZone.
- This track must not block main roadmap (DOS compatibility + DOOM milestone).

Global constraints:
1. Do NOT change Stage2 boot ABI or low-level boot handoff contracts.
2. Do NOT break existing gates: `test-stage2`, `test-int21`, `test-freedos-pipeline`, `check-int21-matrix`.
3. Treat oZone as optional runtime payload (feature flag / presence-based behavior).
4. Keep licensing/provenance explicit for every imported artifact.
5. Keep changes small and reversible; one task per branch.

## Task O1 - Source and Package Provenance Intake
Suggested branch:
- `feature/copilot-ozone-provenance-intake`

Goal:
- Add reproducible metadata and provenance references for oZone package intake.

Deliverables:
1. New doc: `docs/ozone-integration-notes.md` with source URLs, package identity, platform notes, and trust assumptions.
2. Extend `third_party/freedos/manifest.csv` schema usage for oZone artifacts (`component=ozonegui` or equivalent).
3. Record checksums and source URL entries for imported files.

Acceptance:
1. Provenance is machine-readable in manifest.
2. No runtime behavior change yet.

---

## Task O2 - License and Redistribution Guardrails
Suggested branch:
- `feature/copilot-ozone-license-guardrails`

Goal:
- Ensure legal/compliance handling is consistent before runtime integration.

Deliverables:
1. Add license text/copying notices to `docs/legal/freedos-licenses/` for oZone package.
2. Update `docs/freedos-integration-policy.md` with an explicit oZone section (optional GUI app policy).
3. Add a short “what is redistributed” note in `third_party/freedos/README.md`.

Acceptance:
1. oZone licensing path is documented like FreeCOM assets.
2. No binary without provenance/license references.

---

## Task O3 - Import Script for oZone Runtime Payload
Suggested branch:
- `feature/copilot-ozone-import-script`

Goal:
- Provide deterministic import process similar to existing FreeDOS asset flow.

Deliverables:
1. New script: `scripts/import_ozonegui.sh`.
2. Inputs: source directory or archive extraction path.
3. Outputs: copy required oZone runtime files into `third_party/freedos/runtime/OZONE/`.
4. Update manifest rows automatically (with checksum refresh for copied files).

Acceptance:
1. Script is idempotent and safe on repeated runs.
2. Script fails clearly if required files are missing.

---

## Task O4 - Runtime Image Composition Hook
Suggested branch:
- `feature/copilot-ozone-image-composition`

Goal:
- Make oZone files available in disk image when present, without forcing dependency.

Deliverables:
1. Update `run_ciukios.sh` (or image assembly path) to copy `third_party/freedos/runtime/OZONE/` to image path (e.g. `A:\FREEDOS\OZONE\`).
2. Add environment toggle `CIUKIOS_INCLUDE_OZONE=1` (default on only if files exist, or explicit behavior documented).
3. Log marker in run output indicating oZone inclusion status.

Acceptance:
1. Builds still pass when OZONE payload is absent.
2. Deterministic copy when payload exists.

---

## Task O5 - FreeDOS Pipeline Validation Extension
Suggested branch:
- `feature/copilot-ozone-pipeline-gate`

Goal:
- Extend pipeline checks to include optional oZone payload validation.

Deliverables:
1. Update `scripts/validate_freedos_pipeline.sh` for optional checks:
   - If oZone feature flag is enabled, enforce required oZone files.
   - If disabled/absent, report INFO/WARN only.
2. Add clear pass/fail messages for oZone assets.

Acceptance:
1. `make test-freedos-pipeline` remains green in default setup.
2. Missing oZone files only fail when explicitly required.

---

## Task O6 - Shell Command Surface (`ozone` launcher)
Suggested branch:
- `feature/copilot-ozone-shell-command`

Goal:
- Add Stage2 shell command to launch oZone via DOS runtime path.

Deliverables:
1. Add `ozone` command in `stage2/src/shell.c` help and dispatch.
2. Default launch target path: `A:\FREEDOS\OZONE\OZONE.EXE` (or package-verified main binary).
3. Graceful error messages when not present.
4. Serial marker: `[ app ] ozone launch requested` and result marker.

Acceptance:
1. `ozone` command does not crash if files are missing.
2. Existing shell commands unaffected.

---

## Task O7 - Runtime Compatibility Probe (Preflight)
Suggested branch:
- `feature/copilot-ozone-preflight-probe`

Goal:
- Detect and report likely runtime blockers before attempting launch.

Deliverables:
1. Implement lightweight preflight check before `ozone` launch:
   - executable present
   - key config/resources present (from package layout)
   - optional DOS API readiness markers where possible
2. Preflight summary printed to shell and serial.

Acceptance:
1. Failure path is actionable (“missing X file”, “runtime unsupported Y”).
2. No side effects on normal boot.

---

## Task O8 - Smoke Test Harness for oZone Launch Path
Suggested branch:
- `feature/copilot-ozone-smoke-test`

Goal:
- Add non-interactive smoke verification around command and markers.

Deliverables:
1. New test helper: `scripts/test_ozone_integration.sh`.
2. Validate at least:
   - launch command exists
   - expected markers are emitted
   - no panic/exception in log
3. Optional mode: if oZone payload absent, test returns PASS with SKIP semantics.

Acceptance:
1. Test is CI-safe and deterministic.
2. Doesn’t require human keyboard interaction.

---

## Task O9 - User Docs and Ops Notes
Suggested branch:
- `feature/copilot-ozone-docs-ops`

Goal:
- Document usage and maintenance path.

Deliverables:
1. README section: optional oZone integration (high-level only, aligned with alpha policy).
2. New short doc: `docs/ozone-ops.md` with:
   - import flow
   - expected runtime paths
   - troubleshooting markers
3. Note that this is optional and does not replace CiukiOS native GUI roadmap.

Acceptance:
1. Documentation matches actual script names/flags.
2. No contradiction with alpha/no-public-build-instructions policy.

---

## Task O10 - Final Integration PR Gate
Suggested branch:
- `feature/copilot-ozone-final-gate`

Goal:
- Consolidate and enforce final quality gate before merge.

Deliverables:
1. Run and capture results for:
   - `make test-stage2`
   - `make test-int21`
   - `make check-int21-matrix`
   - `make test-freedos-pipeline`
   - `./scripts/test_ozone_integration.sh`
2. Add integration handoff:
   - `docs/handoffs/2026-04-16-copilot-ozone-dos-app.md`

Acceptance:
1. No regression on core DOS compatibility flow.
2. oZone path is optional, documented, and test-covered.

## Merge gate (required)
1. `make test-stage2`
2. `make test-int21`
3. `make check-int21-matrix`
4. `make test-freedos-pipeline`
5. `make test-gui-desktop`
6. `./scripts/test_ozone_integration.sh` (PASS or PASS-with-SKIP semantics)

## Handoff rule
Create handoff file:
- `docs/handoffs/2026-04-16-copilot-ozone-dos-app.md`

Must include:
1. Imported files and checksums
2. License/provenance sources
3. Runtime path and launcher command
4. Test results and residual risks
