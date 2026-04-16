# FreeDOS Integration Policy for CiukiOS

## Goal
Use FreeDOS components to accelerate compatibility work while respecting licensing obligations.

## Short Answer
Yes, integrating FreeDOS files is possible and can be very useful.

## Licensing Reality
1. FreeDOS is not one single license across all files.
2. Many core components are GPL-compatible, but some tools can have different free licenses.
3. Treat each imported binary/tool as a separate licensing unit.

## Practical Safe Rules
1. Import only files you can trace to official FreeDOS packages/releases.
2. Keep license text and attribution for each imported component.
3. Keep a manifest in repo documenting source URL, package version, and license.
4. If you redistribute GPL binaries in images, provide corresponding source access path and build/repro notes.
5. Do not mix unknown third-party DOS binaries into distributed images.

## Recommended Import Set (First Pass)
1. `KERNEL.SYS`
2. `COMMAND.COM`
3. `FDCONFIG.SYS`
4. `FDAUTO.BAT`
5. `HIMEMX.EXE` (or equivalent XMS provider from FreeDOS stack)
6. `JEMM386.EXE` (optional later for advanced memory)
7. Core utilities for testing: `MEM`, `MODE`, `EDIT`, `FDISK`, `FORMAT`, `XCOPY`

## Suggested Repository Structure
1. `third_party/freedos/README.md` with acquisition instructions.
2. `third_party/freedos/manifest.csv` with component, version, license, source URL, checksum.
3. `scripts/import_freedos.sh` to reproduce import process.
4. `docs/legal/freedos-licenses/` for copied license texts.

## Implemented Pipeline (Current)
1. Import command:
   - `./scripts/import_freedos.sh --source /path/to/freedos/files`
2. Runtime bundle location:
   - `third_party/freedos/runtime/`
3. FreeCOM upstream sync:
   - `./scripts/sync_freecom_repo.sh`
   - source repo: `https://github.com/FDOS/freecom`
4. FreeCOM COMMAND.COM build/import:
   - `./scripts/build_freecom.sh`
   - tries source build from `third_party/freedos/sources/freecom`
   - falls back to official `freecom.zip` if local ia16 runtime headers/libraries are missing
5. Image integration toggle:
   - `CIUKIOS_INCLUDE_FREEDOS=1 ./run_ciukios.sh`
6. Image copy behavior:
   - all files copied to `A:\\FREEDOS\\`
   - selected files mirrored to root when present (`COMMAND.COM`, `KERNEL.SYS`, `FDCONFIG.SYS`, `FDAUTO.BAT -> AUTOEXEC.BAT`)

## Image Build Policy
1. Default public image should include only components with verified redistribution clarity.
2. Optional local-only image profile may include user-supplied DOS assets.
3. Separate target in build scripts:
   - `make image-freedos` for verified FreeDOS bundle.
   - `make image-local-assets` for user-private additions.

## Validation and Testing
1. **Automated Pipeline Validation**:
   - `make test-freedos-pipeline`: runs deterministic checks to ensure FreeDOS import/build artifacts are present and consistent.
   - Validates that `third_party/freedos/manifest.csv` is well-formed.
   - Checks that all required files (marked `required=yes`) are present in `third_party/freedos/runtime/`.
   - Returns non-zero exit code on missing essentials; zero on all checks passing.
2. **Integration with CI**:
   - Called before boot and compatibility tests to ensure sanity of FreeDOS dependencies.
   - Recommend adding to pre-commit checks or CI gates for public images.


## Notes on Microsoft DOS Files
1. Keep Microsoft DOS files out of public redistribution unless rights are explicit.
2. For personal testing, use local user-supplied files only.

## Optional GUI App Policy (oZone)
1. oZone GUI is treated as an optional runtime payload within the FreeDOS ecosystem.
2. It is NOT a core dependency — CiukiOS must boot and function without it.
3. Import flow: `./scripts/import_ozonegui.sh --source /path/to/extracted/ozone`
4. Runtime location: `third_party/freedos/runtime/OZONE/`
5. Disk image path: `A:\FREEDOS\OZONE\`
6. Feature toggle: `CIUKIOS_INCLUDE_OZONE=1` (default: auto-detect from file presence)
7. License tracking: `docs/legal/freedos-licenses/ozonegui-license.txt`
8. The oZone integration does NOT replace the CiukiOS native GUI roadmap.
9. Provenance details: `docs/ozone-integration-notes.md`

## Optional GUI App Policy (OpenGEM)
1. OpenGEM (FreeGEM distribution) is treated as an optional runtime payload within the FreeDOS ecosystem.
2. It is NOT a core dependency — CiukiOS must boot and function without it.
3. Import flow: `./scripts/import_opengem.sh` (uses default zip at `third_party/freedos/sources/opengem/opengem.zip`)
4. Runtime location: `third_party/freedos/runtime/OPENGEM/`
5. Disk image path: `A:\FREEDOS\OPENGEM\`
6. Feature toggle: `CIUKIOS_INCLUDE_OPENGEM=1` (default: auto-detect from file presence)
7. License: GPL-2.0-or-later; tracking: `docs/legal/freedos-licenses/opengem-license.txt`
8. The OpenGEM integration does NOT replace the CiukiOS native GUI roadmap.
9. Provenance details: `docs/opengem-integration-notes.md`
10. Source code included in archive as `SOURCE/OPENGEM/SOURCES.ZIP`.

## Disclaimer
This is an engineering policy, not legal advice. For public distribution decisions, verify licenses with a legal review.
