# Changelog

All notable project-level changes are tracked here.
This changelog is intentionally concise and includes only major milestones.

## pre-Alpha v0.5.9-final (2026-04-24) — OpenGEM milestone closure

### OG-P0 — Gate validation
1. Added `scripts/opengem_gate_final.sh` (OG-P0-05): official single-verdict gate; 20 runs, 100% launch, 100% return-to-shell, 0 hangs → **PASS**.
2. Added `scripts/opengem_trace_full.sh`: deterministic DOS/OpenGEM trace artifacts generator (serial, INT log, INT21 summary).
3. Added `scripts/opengem_acceptance_full.sh`: N-run acceptance loop with per-run logs and summary report.
4. Added failure classification counters to acceptance and soak scripts (`launch_without_return`, `qemu_fail`, `infra_fail`, `unexpected_exit`).

### OG-P1 — Hardening, soak, hardware
5. OG-P1-01: hardened Stage2 VDI/AES primitives with coordinate clipping guards for pixel/line/rect paths; upgraded INT33h handler from minimal stub to deterministic stateful behavior (reset/status/position/range/version).
6. OG-P1-01: extended full-profile Stage2 OpenGEM launch diagnostics with explicit `try GEM.EXE` and `try GEM.BAT` markers.
7. OG-P1-01: added `src/com/opengem_vdi_validation.asm` — VDI validation module for offline harness testing.
8. OG-P1-02: added long-session soak runner `scripts/opengem_soak_full.sh` (20-30 min, 100 runs) with per-run NDJSON and JSON/TXT summary; **PASS** (100/100, 0 errors).
9. OG-P1-03: added real hardware lane package: `docs/opengem-hardware-validation-lane.md`, execution/evidence templates, `scripts/opengem_hardware_lane_pack.sh`.
10. OG-P1-03: **hardware evidence collected on real x86 hardware** (HP w19 monitor, legacy PC): `[HW] PASS: OpenGEM autorun completed`, `[HW] PASS: returned to CiukiDOS shell`.
11. OG-P1-04: added runtime normalization guide `docs/opengem-runtime-normalization.md` covering launch order, payload requirements, and troubleshooting signatures.
12. Added Makefile targets: `opengem-soak-full`, `opengem-hardware-lane-pack`, `opengem-final-bundle`.

### OG-P2 — Quality lock
13. OG-P2-01: added `scripts/opengem_regression_lock.sh` with 10 deterministic regression checks (carry, find-next, alias path, memory free/resize, autorun markers) → **PASS** 10/10.
14. OG-P2-02: added `scripts/opengem_perf_baseline.sh` and `scripts/opengem_perf_budget_check.sh`; baseline and budget enforcement with desktop readiness drift tracking → **PASS**.
15. OG-P2-02: added `docs/opengem-performance-budget.json` with latency thresholds and desktop readiness metric.

### Final bundle
16. Added `scripts/opengem_final_validation_bundle.sh`: aggregates gate/acceptance/soak/hardware evidence into single PASS/FAIL verdict; **final-closure-ready verdict: PASS**.
17. Added CD-ROM profile scaffolding: `scripts/build_full_cd.sh`, `src/boot/full_cd_mbr.asm`.

### Prior v0.5.9 work (2026-04-23)
18. Restored descriptive shell messages in Stage1 (banner with version, full `help` listing, `Unknown command` text).
19. Expanded Stage1 loader budget to 24 sectors in both full and floppy profiles (was 23/22 respectively); fixes a silent 330-byte overflow in the floppy profile.
20. Fixed `INT 21h AH=49h` block2 free path: `.free_done` was aliased to `.invalid`, causing all secondary-block frees to return error 9.
21. Added block2 path to `INT 21h AH=4Ah` resize: secondary heap block can now be resized without corrupting block1 MCB state.
22. Improved MCB/List-of-Lists coherence: block2 now writes a real MCB header, block1 toggles `M/Z` type correctly, `INT 21h AH=52h` returns valid first-MCB pointer.
23. Completed keyboard-backed DOS input path (`INT 21h AH=06h/07h/08h/0Ah`) via BIOS `INT 16h`, replacing forced-CR stubs.
24. Added `INT 21h AH=51h` support and corrected PSP reporting (`AH=62h`) to use `current_psp_seg`.
25. Switched full-profile defaults to real OpenGEM execution (`CIUKIOS_OPENGEM_TRY_EXEC=1`, `CIUKIOS_STAGE2_AUTORUN=1`) with Stage2 autorun diagnostics.

## pre-Alpha v0.5.9 (2026-04-23)
1. Fixed carry-flag preservation in DOS I/O done paths (`INT 21h AH=3Fh/40h/42h`) so handle-swap housekeeping no longer corrupts success/error status.
2. Removed false `3F:02` read-error signature in OpenGEM/GEMVDI runtime tracing caused by flag clobbering after successful reads.
3. Relaxed special GEM probe `find-next` behavior to avoid immediate `0x12` termination in OpenGEM driver-discovery flow.
4. Added `VD*` open alias handling to `SDPSC9.VGA` for better compatibility with bundled GEM driver payload names.
5. Kept Stage1 full-profile payload within the 29-sector budget while preserving OpenGEM launch diagnostics stability.

## pre-Alpha v0.5.8 (2026-04-23)
1. Fixed nested `INT 21h AH=4Bh` execution flow for OpenGEM chainload (`GEMVDI -> GEM.EXE`) by separating parent/child load segments and preserving parent PSP context on return.
2. Corrected DOS find-first compatibility in root scanning by copying the matched 11-byte FAT name before DTA emission, restoring OpenGEM file-discovery expectations.
3. Extended Stage1 DOS heap ceiling (`0x9A00 -> 0x9F00`) and introduced two-block allocation behavior in `INT 21h AH=48h/49h` to reduce immediate memory-allocation failures during GEM runtime.
4. Increased Stage1 reserved slot from 22 to 23 sectors in full and floppy profiles to absorb runtime growth while keeping build and test flows stable.
5. Consolidated OpenGEM launch-path diagnostics and runtime hardening to move execution beyond the previous post-`[OPENGEM] try GEMVDI` hang condition.

## pre-Alpha v0.5.7 (2026-04-22)
1. Extended Stage1 FAT16 runtime to handle multi-sector clusters in core DOS paths (`open/read/write/exec`) and fixed cluster-to-LBA mapping inconsistencies.
2. Reworked full-profile FAT16 image assembly to align built-in payload FAT entries and data placement with 8-sector cluster geometry.
3. Expanded DOS compatibility surface with additional `INT 21h` handlers (`2Ah`, `2Ch`, `33h`, `34h`, `36h`, `52h`, `54h`, `58h`) plus minimal `INT 2Fh` multiplex support.
4. Corrected `INT 21h` register return semantics for `ES:BX`-returning functions and added serial diagnostics for unsupported `INT 21h` calls.
5. Added OpenGEM payload dual-layout injection (`root` and `GEMAPPS/GEMSYS`) for the full profile image.
6. Improved Stage2 OpenGEM launcher flow with deterministic progress markers, primary GEM path attempts, and explicit guarded-safe mode to avoid hard shell hangs.
7. Increased Stage1 reserved slot from 21 to 22 sectors in both full and floppy profiles to absorb DOS runtime growth while keeping regression gates green.

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
