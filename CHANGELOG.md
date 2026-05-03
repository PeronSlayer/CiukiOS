# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## Unreleased (2026-05-03)
1. Added a full-profile DOOM taxonomy harness and Makefile target to classify launch progress stages deterministically.
2. Added local-only DOOM payload packaging in the full image build lane and guarded proprietary assets from publication.
3. Fixed INT 21h MZ loading to use header-declared module size, removing the previous 4B:08 launch failure and advancing DOOM to extender startup diagnostics.
4. Closed the Phase 4 installer execution lane with deterministic scenario coverage (success, media swap, timeout, missing media, and insufficient space).
5. Hardened installer manifest-source diagnostics, including explicit `MANIFEST_MEDIA_HEX` reporting for normal and fallback parse paths.
6. Synchronized project documentation to reflect installer-lane closure while keeping the runtime/DOOM lane active.

## pre-Alpha v0.5.4 (2026-05-01)
1. Improved shell input stability for hold-key repeat, line wrap, and backspace behavior.
2. Stabilized FAT16 shell footer telemetry (`CPU/DSK/RAM`) with corrected non-stuck stat refresh behavior.
3. Revalidated cross-profile build and regression lanes on floppy (FAT12) and full (FAT16) profiles.

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
