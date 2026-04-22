# Handoff - FreeCOM COMMAND.COM Build with Fallback

## Context and Goal
Enable a reliable path to populate `third_party/freedos/runtime/COMMAND.COM` from FreeCOM, even when local `ia16-elf` toolchain runtime headers/libraries are incomplete.

## Files Touched
1. `scripts/build_freecom.sh` (new)
2. `Makefile` (`freecom-build` target)
3. `README.md`
4. `CLAUDE.md`
5. `third_party/freedos/README.md`
6. `docs/freedos-integration-policy.md`
7. `docs/freedos-symbiotic-architecture.md`
8. `third_party/freedos/manifest.csv` (runtime provenance updated)
9. `third_party/freedos/runtime/COMMAND.COM` (imported binary)
10. `docs/legal/freedos-licenses/freecom-license.txt` (copied license text)

## Decisions Made
1. Added a single command pipeline: `make freecom-build`.
2. `scripts/build_freecom.sh` workflow:
   - sync FreeCOM source (`scripts/sync_freecom_repo.sh`)
   - attempt source build (`build.sh gcc`)
   - if source build fails, fallback to official FreeDOS package:
     `https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/freecom.zip`
   - extract `COMMAND.COM` and copy to `third_party/freedos/runtime/COMMAND.COM`
   - update `third_party/freedos/manifest.csv` row for `core,COMMAND.COM`
   - copy FreeCOM license text into `docs/legal/freedos-licenses/freecom-license.txt`
3. Kept fallback enabled by default to avoid blocking CI/local progress.

## Validation Performed
1. `bash -n scripts/build_freecom.sh` -> OK
2. `make freecom-build` -> OK (source build failed due missing ia16 libc headers, fallback succeeded)
3. Verified artifact:
   - `file third_party/freedos/runtime/COMMAND.COM` -> `MS-DOS executable, MZ for MS-DOS`
4. Boot regressions:
   - `make test-stage2` -> PASS
   - `make test-fallback` -> PASS

## Known Risk / Open Point
1. Current local `ia16-elf-gcc` is configured `--without-headers`, so source build currently fails on missing `stdlib.h`.
2. Fallback keeps delivery unblocked, but reproducible source build still requires installing a compatible ia16 libc/libi86 toolchain.

## Suggested Next Step
1. Add a dedicated script/docs section to install/verify ia16 libc for this distro so `build.sh gcc` can succeed without fallback.
2. Then start integrating real runtime handoff path to execute imported `COMMAND.COM` from Stage2 DOS loader path.
