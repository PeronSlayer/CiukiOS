# Handoff: OpenGEM GUI Integration (OPG-01 .. OPG-10)

**Date:** 2026-04-16
**Author:** Copilot (Claude Opus 4.6)
**Branch:** main (direct commits)

## Context and Goal
Integrate OpenGEM (FreeGEM distribution, Release 7 RC3) as an optional FreeDOS GUI payload in CiukiOS, following the same deterministic patterns established by the oZone integration. The integration must be fully optional — CiukiOS boots and functions without OpenGEM present.

## Implemented Features (OPG-01 through OPG-10)

### OPG-01: Import Script
- **File:** `scripts/import_opengem.sh`
- Supports `--zip` (default: `third_party/freedos/sources/opengem/opengem.zip`) and `--source` modes
- Auto-detects OpenGEM root within archive (`GUI/OPENGEM/`)
- Copies full runtime tree preserving directory structure
- Detects launch entry in priority order: GEM.BAT, GEM.EXE, DESKTOP.APP, OPENGEM.BAT, OPENGEM.EXE
- Computes sha256 checksums and updates `manifest.csv`
- Prints actionable "next commands" on success

### OPG-02: Runtime Composition
- **File:** `run_ciukios.sh`
- Added `CIUKIOS_INCLUDE_OPENGEM=auto|0|1` environment gate
- Recursive `mcopy` of `OPENGEM/` tree to `::FREEDOS/OPENGEM/` in FAT image
- Existing oZone composition block unchanged

### OPG-03: Pipeline Validation
- **File:** `scripts/validate_freedos_pipeline.sh`
- Added Check 7: OpenGEM payload detection with optional/strict modes
- `CIUKIOS_REQUIRE_OPENGEM=1` fails if payload or entry missing
- Runnable entry search uses same priority list as import script
- Existing oZone checks (Check 6) unchanged

### OPG-04: Shell Command
- **Files:** `stage2/src/shell.c`, `stage2/src/stage2.c`, `scripts/test_stage2_boot.sh`
- Added `opengem` command with 5-path preflight probe:
  - `/FREEDOS/OPENGEM/GEM.BAT`
  - `/FREEDOS/OPENGEM/GEM.EXE`
  - `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/DESKTOP.APP`
  - `/FREEDOS/OPENGEM/OPENGEM.BAT`
  - `/FREEDOS/OPENGEM/OPENGEM.EXE`
- FAT ready check, diagnostic output to serial and screen
- Launches using exact found path
- Updated shell ready marker to include `opengem`
- Used `static const` for path array to avoid memcpy link error

### OPG-05: Integration Smoke Test
- **Files:** `scripts/test_opengem_integration.sh`, `Makefile`
- SKIP semantics when payload absent (CI-safe)
- Checks: payload presence, shell command surface, boot integrity (panic/UD/GPF), preflight markers
- Added `make test-opengem` target

### OPG-06: Image Content Probe
- **File:** `scripts/check_opengem_in_image.sh`
- Verifies `::FREEDOS/OPENGEM/` exists in image via `mdir`
- Checks for runnable entry and GEMAPPS subdirectory
- Actionable remediation messages on failure

### OPG-07: Licensing/Provenance Docs
- **Files:**
  - `docs/opengem-integration-notes.md` — Provenance, package identity, trust assumptions
  - `docs/opengem-ops.md` — Operations guide with import flow, env vars, serial markers, recovery
  - `docs/legal/freedos-licenses/opengem-license.txt` — GPL-2.0-or-later notice
  - `docs/freedos-integration-policy.md` — Added "Optional GUI App Policy (OpenGEM)" section
  - `third_party/freedos/README.md` — Added OpenGEM section

### OPG-08: README Update
- **File:** `README.md`
- Bumped to `v0.5.4`
- Added changelog entry for OpenGEM integration
- Added OpenGEM to Key Docs section
- Expanded Third-Party and Licensing section

### OPG-09: Regression Safety
All gates pass:

| Gate | Result |
|------|--------|
| `make test-stage2` | PASS |
| `make test-int21` | PASS |
| `make check-int21-matrix` | PASS |
| `make test-freedos-pipeline` | PASS |
| `make test-gui-desktop` | PASS |
| `scripts/test_ozone_integration.sh` | PASS (SKIP - payload absent) |
| `make test-opengem` | PASS |

## Files Changed

### New Files
| File | Purpose |
|------|---------|
| `scripts/import_opengem.sh` | Import script |
| `scripts/test_opengem_integration.sh` | Smoke test |
| `scripts/check_opengem_in_image.sh` | Image content probe |
| `docs/opengem-integration-notes.md` | Provenance doc |
| `docs/opengem-ops.md` | Operations guide |
| `docs/legal/freedos-licenses/opengem-license.txt` | License notice |

### Modified Files
| File | Change |
|------|--------|
| `run_ciukios.sh` | +OpenGEM composition block |
| `scripts/validate_freedos_pipeline.sh` | +Check 7 (OpenGEM) |
| `stage2/src/shell.c` | +opengem command, help line |
| `stage2/src/stage2.c` | Updated shell ready marker |
| `scripts/test_stage2_boot.sh` | Updated marker pattern |
| `Makefile` | +test-opengem target |
| `README.md` | +v0.5.4, OpenGEM sections |
| `docs/freedos-integration-policy.md` | +OpenGEM policy section |
| `third_party/freedos/README.md` | +OpenGEM section |
| `third_party/freedos/manifest.csv` | +opengem entries |

## How to Test Manually
```bash
# 1. Import OpenGEM (zip must exist at default location)
./scripts/import_opengem.sh

# 2. Validate pipeline
make test-freedos-pipeline
CIUKIOS_REQUIRE_OPENGEM=1 make test-freedos-pipeline

# 3. Build and run
CIUKIOS_INCLUDE_OPENGEM=1 ./run_ciukios.sh

# 4. At shell prompt
C:\> opengem

# 5. Run smoke test
make test-opengem

# 6. Check image content
./scripts/check_opengem_in_image.sh
```

## Known Limitations
1. **16-bit execution**: OpenGEM is a 16-bit DOS application requiring real-mode DOS APIs. CiukiOS cannot actually execute it yet — preflight will pass but GEM.BAT batch processing and GEM.EXE execution require unimplemented subsystems.
2. **Batch file support**: GEM.BAT is the primary launch entry but CiukiOS shell does not yet support .BAT execution. The `shell_run` path will attempt to load it as a binary.
3. **Deep directory tree**: OpenGEM has ~243 files in a deep tree. Image composition uses recursive mcopy which may be slow on large images.
4. **No interactive test**: The smoke test only checks serial markers, not actual GEM desktop rendering.

## Next 3 Recommended Tasks
1. **Batch file interpreter**: Implement basic .BAT execution in stage2 shell to support GEM.BAT launch (and other DOS batch files).
2. **INT 21h batch primitives**: Add DOS functions needed by batch files (environment variables, ERRORLEVEL, ECHO, CALL).
3. **OpenGEM runtime probe**: Add a deeper preflight that checks for GEMSYS/GEM.EXE, GEMVDI.EXE and critical .RSC files to give more specific diagnostics about what's missing.
