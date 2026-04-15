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

## Image Build Policy
1. Default public image should include only components with verified redistribution clarity.
2. Optional local-only image profile may include user-supplied DOS assets.
3. Separate target in build scripts:
   - `make image-freedos` for verified FreeDOS bundle.
   - `make image-local-assets` for user-private additions.

## Notes on Microsoft DOS Files
1. Keep Microsoft DOS files out of public redistribution unless rights are explicit.
2. For personal testing, use local user-supplied files only.

## Disclaimer
This is an engineering policy, not legal advice. For public distribution decisions, verify licenses with a legal review.
