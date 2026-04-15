# CiukiOS

Educational operating system project built from scratch with a DOS-compatibility direction.

## Current Direction
The active north star is:
1. Run real DOS executables on CiukiOS.
2. Reach the first major game milestone: run DOS DOOM from CiukiOS.

## Key Docs
1. DOS-to-DOOM roadmap: `docs/roadmap-ciukios-doom.md`
2. DOS 6.2 compatibility roadmap: `docs/roadmap-dos62-compat.md`
3. FreeDOS integration and licensing policy: `docs/freedos-integration-policy.md`
4. FreeDOS symbiotic architecture: `docs/freedos-symbiotic-architecture.md`
5. Handoff workflow: `docs/handoffs/README.md`
6. Claude/Codex shared session readme: `CLAUDE.md`

## Build and Run
1. `./run_ciukios.sh`
2. Boot regression tests:
   - `make test-stage2`
   - `make test-fallback`

## FreeDOS Symbiotic Mode (Optional)
1. Sync FreeCOM source:
   - `make freecom-sync`
2. Build/import `COMMAND.COM` from FreeCOM:
   - `make freecom-build`
   - Note: if local `ia16-elf` libc headers are missing, this target falls back to the official FreeDOS `freecom.zip` package.
3. Import FreeDOS files:
   - `./scripts/import_freedos.sh --source /path/to/freedos/files`
4. Run with integration enabled (default):
   - `CIUKIOS_INCLUDE_FREEDOS=1 ./run_ciukios.sh`
5. Runtime files are copied inside image under `A:\\FREEDOS\\` and selected files are mirrored to DOS-style root.

## Credits
Developed collaboratively with Claude Code and Codex.

Dedicated to one of the best dogs ever, Jack.
