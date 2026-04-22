# 2026-04-17 Roadmap Closure Gates

## Context and goal
Close the remaining roadmap items still marked in progress by adding deterministic closure gates where the code already existed and syncing the roadmap docs to the validated implementation state.

## Files touched
- Makefile
- README.md
- Roadmap.md
- docs/roadmap-ciukios-doom.md
- docs/roadmap-dos62-compat.md
- scripts/test_gui_desktop.sh
- scripts/test_opengem_integration.sh
- scripts/test_startup_chain.sh
- scripts/test_fat32_edge_semantics.sh
- stage2/include/version.h

## Decisions made
1. Closed startup/batch roadmap work with a deterministic source-contract gate instead of a fragile framebuffer-only boot assertion.
2. Closed FAT32 parity hardening with a dedicated edge-semantics gate covering FSInfo corruption fallback, hint sanitization and fixed-root guards.
3. Tightened GUI and OpenGEM regression scripts so roadmap closure is backed by explicit, repeatable checks rather than documentation drift.

## Validation performed
- make test-startup-chain
- make test-fat32-edge
- make test-gui-desktop
- make test-opengem

## Risks and next step
- The startup-chain gate is deterministic/static because current startup markers are framebuffer-facing rather than serial-facing.
- Next useful increment is extending the DOS extender path past the `INT 2Fh AX=1687h` smoke toward a real DPMI service slice used by non-trivial DOS/4GW binaries.
