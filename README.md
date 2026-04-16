![Splashscreen CiukiOS](misc/CiukiOS_SplashScreen.png)

# CiukiOS

Educational operating system project built from scratch with a DOS-compatibility direction.

## Current Version
`CiukiOS Stage2 v0.5`  
Focus: INT21 compatibility expansion + testable desktop session entry.

## Changelog (Latest)
### v0.5.1
1. Improved desktop readability with layout grid v2 and clearer window chrome.
2. Upgraded desktop interaction flow (focus/navigation feedback and launcher clarity).
3. Added launcher/dock visual pass v2 with better selection visibility.
4. Added GUI regression helper script: `make test-gui-desktop`.
5. Added Copilot handoffs for desktop polish tasks D1-D5.

### v0.5
1. Added INT21 compatibility set for console/drive/DTA paths (`AH=06h/07h/0Ah/0Eh/1Ah/2Fh`) with deterministic tests.
2. Extended boot/test gates for INT21 matrix and compatibility markers.
3. Added interactive desktop session from shell (`desktop` command).
4. Added desktop controls (`TAB`, `UP/DOWN`, `J/K`, `ENTER`, `ESC`) and startup hint for GUI testing.
5. Kept boot/fallback/FAT/INT21 automated regression flow green.

### v0.4
1. Added graphic splash renderer (framebuffer, centered scaling, ASCII-to-grayscale mapping).
2. Added explicit framebuffer metadata in stage handoff ABI.
3. Added shell preview command: `gsplash` (alias `splash`).
4. Kept ASCII splash as automatic fallback path.

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

## Third-Party and Licensing (FreeDOS Notice)
1. This repository can include and use third-party FreeDOS components in `third_party/freedos/`.
2. FreeDOS packages are distributed under their own licenses (often GPL-family, but not a single license for all files).
3. Keep license/provenance files with imported components and validate redistribution rights per package.
4. See:
   - `docs/freedos-integration-policy.md`
   - `docs/legal/freedos-licenses/`

## Credits
Developed collaboratively with Claude Code and Codex.

Dedicated to one of the best dogs ever, Jack.
