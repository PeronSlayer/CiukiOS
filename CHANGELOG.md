# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## Unreleased
1. Phase 2 work in progress: move from embedded `.COM` demo payloads to FAT-backed `.COM` loading and execution.

## pre-Alpha v0.5.5 (2026-04-22)
1. Completed Stage1 milestone with deterministic `stage0 -> stage1` handoff and stable BIOS diagnostics (`INT 10h/13h/16h/1Ah`).
2. Delivered first DOS runtime surface in Stage1: `INT 21h` baseline services (`AH=02h`, `09h`, `4Ch`, `4Dh`) plus deterministic smoke paths.
3. Added `.COM` execution baseline with PSP-compatible memory setup and reproducible `comdemo` validation path.
4. Unified QEMU run/test flows per profile and introduced Stage1 boot selftest regression gate (`qemu-test-stage1`) integrated in `qemu-test-all`.
5. Finalized project bootstrap contracts (`DOS Core Specification v0.1` + `DOS Core Implementation Plan v0.1`) as normative references for next-phase DOS compatibility work.

## pre-Alpha v0.5.0 (2026-04-22)
1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Archived the previous codebase state to `OLD/archive-2026-04-22/` for historical reference.
3. Introduced two official build profiles: `floppy` (1.44MB bring-up) and `full` (extended runtime).
4. Added build and QEMU smoke-test scripts for both image profiles.
5. Established new AI/development operating directives for branch discipline, merge approvals, and documentation standards.
