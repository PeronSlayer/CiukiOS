# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## Unreleased
1. Delivered a BIOS-bootable `floppy` stage0 baseline with a real 16-bit boot sector.
2. Upgraded floppy build pipeline to assemble and inject boot sector code into LBA0.
3. Upgraded floppy QEMU smoke test to validate a real boot marker instead of timeout-only execution.
4. Defined `DOS Core Specification v0.1` and `DOS Core Implementation Plan v0.1` as the normative baseline for MS-DOS/PC DOS style core development.

## pre-Alpha v0.5.0 (2026-04-22)
1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Archived the previous codebase state to `OLD/archive-2026-04-22/` for historical reference.
3. Introduced two official build profiles: `floppy` (1.44MB bring-up) and `full` (extended runtime).
4. Added build and QEMU smoke-test scripts for both image profiles.
5. Established new AI/development operating directives for branch discipline, merge approvals, and documentation standards.
