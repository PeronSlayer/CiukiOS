# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## pre-Alpha v0.5.3 (2026-04-30)
1. Stabilized shell `move/mv` path-command handling across floppy (FAT12) and full (FAT16) runtime profiles.
2. Fixed `INT 21h AH=56h` rename/move behavior to preserve deterministic DOS file-move semantics.
3. Revalidated shell command regression lanes after the fixes to confirm runtime stability.

## pre-Alpha v0.5.2 (2026-04-29)
1. Closed Stage1 DOS command regressions across floppy (FAT12) and full (FAT16) profiles.
2. Stabilized INT 21h runtime return behavior in critical read/write/seek paths for deterministic file I/O outcomes.
3. Fixed floppy image build write path to preserve root/runtime payload integrity during regression runs.

## pre-Alpha v0.5.0 (2026-04-28)
1. Stabilized Stage1 startup and shell-entry runtime behavior for more deterministic boot bring-up.
2. Hardened shell prompt/input paths, including drive and current-directory state handling.
3. Improved QEMU stderr observability to speed up runtime diagnostics and regression triage.

## pre-Alpha v0.5.0 (2026-04-22)
1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Archived the previous codebase state to `OLD/archive-2026-04-22/` for historical reference.
3. Introduced two official build profiles: `floppy` (1.44MB bring-up) and `full` (extended runtime), both shell-first.
4. Added build and QEMU smoke-test scripts for both image profiles.
5. Established branch discipline and documentation standards for reproducible delivery.
6. Set real floppy bootability and shell reliability as the baseline acceptance criteria.
