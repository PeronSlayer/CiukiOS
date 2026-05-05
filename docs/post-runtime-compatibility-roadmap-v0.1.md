# Post-Runtime Compatibility Roadmap v0.1

## Objective
Define the product-priority order after the Stage1/runtime split foundation so CiukiOS grows toward a stronger DOS-compatible runtime before opening Windows pre-NT scope.

## Strategic Order
1. Continue the Stage1/runtime split with small, validated runtime-owned slices.
2. Improve general DOS program compatibility so arbitrary real DOS software can launch from CiukiOS.
3. Add legacy audio capability for DOOM and other DOS-era workloads.
4. Explore legacy networking only after DOS runtime and audio compatibility are materially stronger.
5. Enter Windows pre-NT milestones only after the previous layers are proven.

## Phase A - Runtime Split Consolidation
Goal: keep Stage1 loader-first and continue moving low-risk ownership into `\SYSTEM\RUNTIME.BIN`.

Target slices:
1. runtime-owned immutable constants or diagnostic providers
2. small read-only status/profile queries
3. low-risk live helpers with no allocator, PSP/MCB, EXEC, or file-I/O core ownership

Acceptance gates:
1. `make build-full`
2. `make build-full-cd`
3. `make qemu-test-full`
4. `make qemu-test-full-cd`
5. `make qemu-test-full-runtime-probe`
6. `make qemu-test-all`
7. `DOOM_TAXONOMY_MIN_STAGE=runtime_stable make qemu-test-full-doom-taxonomy` whenever runtime/shared paths are touched

## Phase B - DOS Application Compatibility
Goal: make CiukiOS a believable host for arbitrary DOS software, not only curated milestone apps.

Workstreams:
1. define a compatibility matrix of real DOS programs across categories
2. add repeatable launch smoke paths for representative external programs
3. classify failures by runtime subsystem: PSP, MCB, handles, env, IOCTL, seek/read, path semantics, termination, extender compatibility
4. prioritize fixes that broaden compatibility across multiple programs, not only one title

Suggested program categories:
1. command-line utilities
2. text editors and file managers
3. real-mode games
4. DOS extender applications
5. installers or setup utilities

Exit signal:
CiukiOS can launch a broader mixed set of DOS binaries directly from the full/full-CD environment with documented failure classes and steadily improving pass rate.

## Phase C - Legacy Audio Bring-up
Goal: close the current "video yes, audio no" gap for DOOM and strengthen DOS multimedia compatibility.

Priority targets:
1. sound-device detection paths used by DOOM and similar software
2. baseline Sound Blaster and AdLib compatibility expectations
3. timer, IRQ, DMA, and port-I/O behaviors needed for conservative audio bring-up
4. evidence-based testing that distinguishes detection, initialization, and audible playback

Exit signal:
DOOM and at least one additional audio-capable DOS workload can pass documented audio initialization milestones, with at least one practical playback success target.

## Phase D - Legacy Networking
Goal: explore whether packet-driver or other DOS-era networking paths are worth supporting after the runtime is stronger.

Scope rules:
1. networking is not a blocker for DOS compatibility or audio
2. choose a narrow first target with practical value
3. avoid opening broad network scope before runtime/application compatibility is mature

Possible first targets:
1. packet-driver compatibility investigation
2. minimal DOS TCP/IP tool bring-up
3. controlled evidence lane for one networking stack or utility

## Phase E - Windows pre-NT Readiness
Goal: approach Windows 3.x and later pre-NT milestones from a stronger DOS base.

Entry requirements:
1. Stage1/runtime split has progressed beyond proof-of-concept
2. DOS application compatibility is broad enough to trust CiukiOS with non-curated programs
3. legacy audio is no longer a known major gap for DOOM-class software
4. shell return paths, process execution, file/path semantics, and handle behavior are materially stable

First Windows target:
1. Windows 3.x bootstrap and runtime investigation

Later targets:
1. Windows 95
2. Windows 98

## Non-Priority Items During This Roadmap
1. FAT32 is future scope, not an active prerequisite for the phases above.
2. The Windows 3.11-style GUI demo branch is exploratory and does not change the mainline critical path.
3. Floppy/FAT12 is not part of the active engineering baseline unless the owner explicitly reopens it.
