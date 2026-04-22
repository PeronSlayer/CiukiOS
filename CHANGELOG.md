# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## Unreleased
1. Delivered a BIOS `floppy` stage0->stage1 chain-loader baseline with a real 16-bit boot sector and stage1 payload jump.
2. Upgraded floppy build pipeline to assemble stage0 and stage1, enforce stage1 slot size, and inject both payloads into deterministic sectors.
3. Upgraded floppy QEMU smoke test to validate both stage0 and stage1 execution markers.
4. Added a real-mode Stage1 BIOS core monitor with startup diagnostics for `INT 10h`, `INT 13h`, `INT 16h`, and `INT 1Ah`.
5. Added a minimal interactive Stage1 command loop (`help`, `cls`, `ticks`, `drive`, `reboot`, `halt`) for BIOS bring-up checks.
6. Defined `DOS Core Specification v0.1` and `DOS Core Implementation Plan v0.1` as the normative baseline for MS-DOS/PC DOS style core development.

## pre-Alpha v0.5.0 (2026-04-22)
1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Archived the previous codebase state to `OLD/archive-2026-04-22/` for historical reference.
3. Introduced two official build profiles: `floppy` (1.44MB bring-up) and `full` (extended runtime).
4. Added build and QEMU smoke-test scripts for both image profiles.
5. Established new AI/development operating directives for branch discipline, merge approvals, and documentation standards.
