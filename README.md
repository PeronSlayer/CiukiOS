# CiukiOS (Legacy Reset)

CiukiOS is restarting from a clean baseline with a legacy-first x86 architecture.
The goal is native BIOS boot and native DOS/pre-NT software execution, with no CPU emulation layer in the final runtime path.

## Primary Goals
1. Run on real legacy x86 hardware (Intel/AMD) with BIOS boot.
2. Run DOS software natively, including OpenGEM and DOOM, and progressively support Windows pre-NT (up to Windows 98).
3. Maintain two build profiles:
   - `floppy`: minimal 1.44MB image for early bring-up.
   - `full`: extended image for full runtime and desktop milestones.

## Repository State
1. Previous codebase archived in `OLD/archive-2026-04-22/`.
2. New documentation baseline in `docs/`.
3. Historical files preserved at root: `CHANGELOG.md`, handoff history, license files.

## Key Documents
1. `docs/architecture-legacy-x86-v1.md`
2. `Roadmap.md`
3. `docs/diario-bordo-v2.md`
4. `docs/ai-agent-directives.md`

## Quick Commands
```bash
make help
make build-floppy
make build-full
make qemu-test-floppy
make qemu-test-full
```

Current images are scaffolds (not yet fully bootable). QEMU targets are smoke-test entry points for the new pipeline.
