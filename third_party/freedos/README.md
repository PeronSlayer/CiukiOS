# FreeDOS Runtime Bundle (Symbiotic Mode)

This directory hosts user-imported FreeDOS runtime files used by CiukiOS image builds.

## Layout
1. `runtime/` -> imported DOS binaries/scripts (not tracked by git by default)
2. `manifest.csv` -> provenance, checksums, and license tracking

## Import Flow
1. Sync FreeCOM source (COMMAND.COM upstream):
   `./scripts/sync_freecom_repo.sh`
2. Build/import `COMMAND.COM`:
   `./scripts/build_freecom.sh`
3. Obtain official FreeDOS packages/releases.
4. Run:
   `./scripts/import_freedos.sh --source /path/to/freedos/files`
5. Run CiukiOS with FreeDOS integration:
   `CIUKIOS_INCLUDE_FREEDOS=1 ./run_ciukios.sh`

## Upstream FreeCOM Source
1. Repo: `https://github.com/FDOS/freecom`
2. Local mirror path: `third_party/freedos/sources/freecom/`

## Image Placement
When enabled, `run_ciukios.sh` copies:
1. all files from `runtime/` to `A:\FREEDOS\`
2. selected compatibility files to DOS-style root when present:
   - `COMMAND.COM`
   - `KERNEL.SYS`
   - `FDCONFIG.SYS`
   - `FDAUTO.BAT` -> `AUTOEXEC.BAT`

## Notes
1. This is a compatibility asset layer; CiukiOS remains the host OS/runtime.
2. Check `docs/freedos-integration-policy.md` for licensing/process rules.
