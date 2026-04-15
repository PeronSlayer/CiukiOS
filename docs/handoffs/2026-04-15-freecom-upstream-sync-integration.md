# HANDOFF - FreeCOM upstream sync integration

## Context
User shared the official FreeCOM repository:
- `https://github.com/FDOS/freecom`

Goal: integrate this upstream source into CiukiOS FreeDOS symbiotic workflow.

## What Changed
1. Added upstream sync script:
   - `scripts/sync_freecom_repo.sh`
2. Added Make target:
   - `make freecom-sync`
3. Created source mirror location:
   - `third_party/freedos/sources/freecom/` (git clone)
4. Updated docs and workflows to reference FreeCOM upstream sync:
   - `README.md`
   - `CLAUDE.md`
   - `third_party/freedos/README.md`
   - `docs/freedos-integration-policy.md`
   - `docs/freedos-symbiotic-architecture.md`
5. Updated manifest provenance with synced source row:
   - `source,freecom.git,...`

## Synced Upstream Commit
- `ec6c63f13be0b254151c76cbbbec3c80ae33741b`

## Validation
Executed:
1. `./scripts/sync_freecom_repo.sh` -> OK
2. `make test-stage2` -> PASS

## Notes
1. This step syncs FreeCOM source only; it does not yet build `COMMAND.COM` from source inside CiukiOS pipeline.
2. Runtime DOS binary import still uses `scripts/import_freedos.sh` from user-provided FreeDOS package files.

## Immediate Next Step
1. Add a reproducible path to produce/obtain `COMMAND.COM` from FreeCOM source workflow and feed it into `third_party/freedos/runtime/COMMAND.COM`.
