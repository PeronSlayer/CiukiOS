# HANDOFF - FreeDOS symbiotic integration bootstrap

## Context
User requested a symbiotic integration of FreeDOS into CiukiOS (not just optional notes), with practical project-level tooling.

## What Changed
1. Added FreeDOS asset import and provenance bootstrap:
   - `scripts/import_freedos.sh`
   - `third_party/freedos/README.md`
   - `third_party/freedos/manifest.csv`
   - `third_party/freedos/runtime/.gitkeep`
2. Added licensing/documentation scaffolding:
   - `docs/legal/freedos-licenses/README.md`
   - `docs/freedos-symbiotic-architecture.md`
3. Integrated FreeDOS bundle into image build/run path:
   - `run_ciukios.sh` now supports `CIUKIOS_INCLUDE_FREEDOS` (default `1`)
   - copies runtime bundle to `A:\FREEDOS\`
   - mirrors selected files to root when present (`COMMAND.COM`, `KERNEL.SYS`, `FDCONFIG.SYS`, `AUTOEXEC.BAT`)
4. Added DX tooling targets:
   - `make run`
   - `make run-nofreedos`
   - `make freedos-import FREEDOS_SRC=/path/...`
5. Updated collaboration/docs references:
   - `README.md`
   - `CLAUDE.md`
   - `docs/freedos-integration-policy.md`
   - `docs/roadmap-ciukios-doom.md`
   - `docs/roadmap-dos62-compat.md`
6. Added `.gitignore` rule to avoid accidental tracking of imported DOS binaries by default.

## Validation
Executed:
1. `bash -n scripts/import_freedos.sh` -> OK
2. `./scripts/import_freedos.sh --help` -> OK
3. `make test-stage2` -> PASS
4. `make test-fallback` -> PASS

## Decisions
1. Keep FreeDOS bundle optional but enabled by default when files exist.
2. Keep imported binaries outside git tracking by default (`runtime/*` ignored except `.gitkeep`).
3. Keep per-component manifest as the single provenance checkpoint.

## Risks / Limits
1. Import script uses filename discovery only; no package-level metadata parsing yet.
2. License field in manifest is a placeholder (`GPL-2.0-or-later?`) and requires verification per component/package.
3. Symbiosis is currently image-level and asset-level; execution-level symbiosis (`COMMAND.COM` real runtime handoff) is next.

## Immediate Next Step
1. Implement true DOS `.COM/.EXE` execution path and make imported `COMMAND.COM` runnable from CiukiOS runtime.
