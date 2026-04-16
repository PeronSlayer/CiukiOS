# FreeDOS Symbiotic Architecture in CiukiOS

## Intent
Integrate FreeDOS as a compatibility partner inside CiukiOS, not as a replacement for CiukiOS core ownership.

## Symbiotic Model
1. CiukiOS owns boot, hardware abstraction, memory, interrupt routing, and compatibility runtime.
2. FreeDOS contributes mature DOS userland/system binaries where useful (`COMMAND.COM`, utilities, memory helpers).
3. CiukiOS provides compatibility contracts expected by DOS binaries; FreeDOS components run as guests in that contract.

## Layered View
1. Host layer: UEFI loader + Stage2 + CiukiOS runtime core.
2. DOS compatibility layer: `.COM/.EXE` loader, PSP/MCB, `INT 21h`, BIOS interrupt bridge.
3. FreeDOS layer: imported command interpreter and tools from `third_party/freedos/runtime/`.
4. Application layer: DOS programs and games (target: DOOM).

## Current Integration State
1. Import pipeline available: `scripts/import_freedos.sh`.
2. FreeCOM source sync available: `scripts/sync_freecom_repo.sh`.
3. FreeCOM build/import pipeline available: `scripts/build_freecom.sh` (source build with package fallback).
4. Upstream orchestration + lock available: `scripts/sync_freedos_upstreams.sh` -> `third_party/freedos/upstreams.lock`.
5. Reproducible runtime package index: `third_party/freedos/runtime-manifest.csv`.
6. Asset bundle directory: `third_party/freedos/runtime/`.
7. Build/run integration: `run_ciukios.sh` copies bundle into image (`A:\FREEDOS\` + selected root mirrors).

## Runtime Contract (Near Term)
1. FreeDOS `COMMAND.COM` should be launchable from CiukiOS DOS execution path.
2. Filesystem semantics must match DOS expectations (paths, attributes, errors).
3. Critical `INT 21h` functions must be behaviorally compatible (flags/error codes).

## Integration Modes
1. `Embedded tools mode`: use FreeDOS utilities as external commands from CiukiOS shell.
2. `Command interpreter mode`: hand off command loop control to FreeDOS `COMMAND.COM`.
3. `Compatibility stress mode`: run FreeDOS utilities as regression suite for API correctness.

## Boundaries
1. CiukiOS must remain independently bootable and debuggable without FreeDOS files.
2. FreeDOS integration is optional and controlled by image composition.
3. Licensing/provenance tracking is mandatory for distributable bundles.

## Immediate Next Technical Step
1. Expand runtime behavioral coverage using imported FreeDOS/OpenGEM tools as compatibility stress workloads.
