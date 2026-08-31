# SHELL.COM Migration Status

## Status

The prototype described on 2026-05-17 has become the default product shell for the active full and full-CD profiles.

Stage1 now behaves as a loader in the visible boot path:

1. load and validate SYSTEM/CIUKIDOS.SYS
2. initialize the runtime and its service table
3. execute SYSTEM/SHELL.COM
4. enter the bounded loader-fatal halt if the runtime or shell is missing/invalid, or if SHELL.COM returns

The old interactive Stage1 shell is not the supported fallback contract.

## Current Shell Boundary

SHELL.COM is a normal DOS COM process. It uses DOS INT 21h and BIOS services rather than Stage1-private labels or buffers.

The current command surface includes:

1. prompt, line editing, history, and completion
2. help/version/echo/clear commands
3. current directory, drive, PATH, WHERE, RUN, and bare external execution
4. directory listing and directory creation/removal
5. copy, type, delete, rename, and move
6. reboot, shutdown, and mouse helper integration
7. disabled EXIT/QUIT behavior in the normal interactive session

The full shell regression covers absolute and relative multi-component COM/MZ execution, extension search, PATH resolution, file operations, high FAT16 clusters, subdirectories, and prompt recovery. The full-CD D: lane now also covers COM/MZ, CIUKRTST/PSTACK, mouse state, power commands, prompt recovery, and a deterministic read beyond LBA 65,535. It remains an internal workflow rather than the general external-application matrix required by Phase 6.

## Completion Status

The migration completion signal is satisfied: `SHELL.COM` is the only interactive product shell, the loader contains no required interactive command processor or normal DOS owner, CIUKIDOS owns the DOS services used by shell/children, and the full/full-CD positive plus fatal-negative lanes protect the contract.

## Remaining Shell Compatibility Work

1. Keep command behavior independent of hard-coded C: assumptions when running from full-CD D:.
2. Keep missing-runtime, missing-shell, invalid-shell, and unexpected-return tests aligned with the fatal-loader contract.
3. Add compatibility features such as batch/config processing only through explicit Phase 6 requirements and tests.
4. Treat new parser, environment, PATH, and child-return defects as Phase 6 shell compatibility work, not unfinished migration.
