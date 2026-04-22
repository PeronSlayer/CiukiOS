# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## pre-Alpha v0.5.6 (2026-04-22)
1. Phase 2 work in progress: move from embedded `.COM` demo payloads to FAT-backed `.COM` loading and execution.
2. Fixed Stage1 command dispatch to correctly parse `dos21` and `comdemo` from the interactive prompt.
3. Fixed Stage1 floppy LBA read path to preserve target buffer register semantics during CHS conversion.
4. Added Stage1 `.EXE` MZ execution baseline with relocation handling and PSP-linked launch flow.
5. Added deterministic Stage1 regression gate for `.EXE` runtime (`MZDEMO`) and integrated it into `qemu-test-stage1`.
6. Added Stage1 `INT 21h` file-handle baseline (`AH=3Dh/3Eh/3Fh/42h`) with DOS-like read path over FAT root entries.
7. Added deterministic `FILEIO` runtime regression marker in Stage1 selftest and `qemu-test-stage1`.
8. Expanded Stage1 loader slot from 6 to 14 sectors to support Phase 2 DOS runtime growth.
9. Added `INT 21h AH=4Bh` execute baseline for DOS path-driven `.COM/.EXE` launch in Stage1.
10. Switched Stage1 `comdemo` and `mzdemo` flows to execute through DOS `AH=4Bh` instead of direct internal launch paths.
11. Added minimal MCB-compatible memory block header behavior for `INT 21h AH=48h/49h/4Ah`.
12. Added Stage1 `INT 21h` DTA and directory search baseline (`AH=1Ah/4Eh/4Fh`) with deterministic `FIND-SERIAL` regression markers.
13. Added Stage1 default-drive and CWD baseline services (`INT 21h AH=19h/0Eh/3Bh/47h`) for DOS runtime compatibility.
14. Added compile-time FAT12/FAT16 Stage1 FAT cache path with FAT16 cluster walker support (`FAT_TYPE=16`) and proper FAT16 EOF handling.
15. Reworked full profile image assembly to inject real runtime payloads (`COMDEMO`, `MZDEMO`, `FILEIO`, `DELTEST`) into FAT16 data clusters with matching root directory entries.
16. Added deterministic full-profile Stage1 regression gate (`scripts/qemu_test_full_stage1.sh`) and integrated it into the aggregate test suite.
17. Stabilized QEMU serial capture in test mode using file chardev wiring (`-chardev file ... -serial chardev:... -monitor none`) for reliable marker collection.

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
