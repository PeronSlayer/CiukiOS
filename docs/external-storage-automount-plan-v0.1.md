# CiukiOS External Storage Automount Plan v0.1

## 1. Current objective
Define and execute a practical implementation plan for external storage support in CiukiOS, covering USB mass-storage devices, additional IDE/SATA disks, and floppy auto-mount, with deterministic DOS drive-letter assignment and safe media-change handling.

Scope includes:
1. Drive-letter policy proposal and runtime mapping rules.
2. Boot-time and runtime automount behavior.
3. Removable media change detection and cache/handle safety.
4. Failure and fallback handling with deterministic user-visible behavior.
5. Regression strategy with verifiable commands and artifacts.

## 2. Step-by-step plan
Critical path: Step 1 -> Step 2 -> Step 3 -> Step 4 -> Step 6.

Parallel stream (independent after Step 3): Step 5 can run in parallel with Step 4 because failure UX and fallback command behavior depend on stable mount-state interfaces, not on detector internals.

| Step | Actions | Dependencies | Critical path | Verifiable output |
| --- | --- | --- | --- | --- |
| 1. Baseline and storage observability contract | Capture current disk behavior in `floppy` and `full` profiles. Define serial/log markers for storage lifecycle (`[STOR-ENUM]`, `[DRIVE-MAP]`, `[AUTOMOUNT]`, `[MEDIA-CHANGE]`, `[STOR-FAILSAFE]`). | None | Yes | Existing checks pass: `make qemu-test-floppy`, `make qemu-test-full`, `make qemu-test-stage1`. Baseline logs produced in `build/logs/storage/` (new lane artifact directory). |
| 2. Device discovery + drive-letter allocator policy | Implement normalized device inventory for floppy, fixed disks, and BIOS-visible USB mass storage. Apply this policy: `A:` floppy 0 (reserved, `NOT READY` if absent), `B:` floppy 1 (reserved, `NOT READY` if absent), `C:` primary boot volume when non-floppy; if boot is floppy, assign `C:` to first fixed disk if available, otherwise keep `C:` unavailable; `D:` onward for remaining fixed disks (BIOS order), then removable media (first-detected order). Preserve letter stability until reboot; on removable reinsertion with matching signature, reuse prior letter. | Step 1 | Yes | Deterministic map marker visible on boot and rescan (`[DRIVE-MAP] A=... B=... C=...`). New automated lane proposed: `bash scripts/qemu_test_storage_drive_letters.sh`. |
| 3. Automount manager (boot + runtime) | Add boot-time mount pass for all present volumes and runtime automount for removable media insertion. Define mount states: `MOUNTED_RW`, `MOUNTED_RO`, `NOT_READY`, `UNSUPPORTED_FS`, `IO_ERROR`. Add explicit manual rescan hook for operators (`MOUNT /R` command path or equivalent shell verb). | Step 2 | Yes | Boot log shows deterministic automount sequence (`[AUTOMOUNT] begin/end`, per-letter result lines). New lane proposed: `bash scripts/qemu_test_storage_automount.sh`. |
| 4. Removable media change detection and safety | Implement policy: floppy media-change check at each new open/root directory operation plus periodic prompt-loop poll; USB removable check through periodic poll + explicit rescan command. On change, invalidate FAT/block cache for that drive, mark stale handles invalid, and return deterministic DOS error paths instead of hanging. | Step 3 | Yes | Media-change events logged with previous/new signature and drive letter (`[MEDIA-CHANGE]`). New lane proposed: `bash scripts/qemu_test_storage_media_change.sh`. |
| 5. Failure/fallback behavior and user surface | Implement deterministic fallback handling: enumeration failure does not block boot; failed mount keeps letter state visible with explicit error; removed media transitions drive to `NOT READY`; optional retry path (`MOUNT /R`) remains available. Keep shell messages concise and machine-checkable. | Step 3 | No (parallel with Step 4) | Fallback markers and shell responses are reproducible (`[STOR-FAILSAFE] reason=... action=...`). New lane proposed: `bash scripts/qemu_test_storage_failure_fallback.sh`. |
| 6. Integration gates and rollout | Integrate new storage lanes into aggregate validation flow (direct scripts + `make qemu-test-all`). Archive evidence in `build/logs/storage/` with pass/fail summary per scenario (boot mount, hot-insert, hot-remove, unsupported FS, I/O error). Update docs after lane stabilization. | Step 4 and Step 5 | Yes | Full lane run produces deterministic PASS summary and marker checks. Candidate command set: `make qemu-test-all`, plus all new storage scripts. |

Test strategy and evidence commands:
1. Baseline regressions (already available):
   - `make qemu-test-floppy`
   - `make qemu-test-full`
   - `make qemu-test-stage1`
2. New dedicated storage lanes (to add):
   - `bash scripts/qemu_test_storage_drive_letters.sh`
   - `bash scripts/qemu_test_storage_automount.sh`
   - `bash scripts/qemu_test_storage_media_change.sh`
   - `bash scripts/qemu_test_storage_failure_fallback.sh`
3. Evidence checks (example):
   - `grep -E "\[DRIVE-MAP\]|\[AUTOMOUNT\]|\[MEDIA-CHANGE\]|\[STOR-FAILSAFE\]" build/logs/storage/*.log`

## 3. Assignments
| Workstream | Owner role | Deliverable | Status |
| --- | --- | --- | --- |
| Storage enumeration and identity | Runtime storage owner | Device inventory + signatures + marker contract | Planned |
| Drive-letter allocator | DOS compatibility owner | Deterministic letter mapping and stability rules | Planned |
| Automount lifecycle | Runtime I/O owner | Boot/runtime mount manager + state transitions | Planned |
| Media-change safety | Filesystem owner | Cache invalidation + stale-handle protection | Planned |
| Failure UX and shell integration | Shell owner | Operator-facing messages + manual rescan flow | Planned |
| Regression automation | QA/validation owner | New storage scripts + aggregation in CI/QEMU lane | Planned |
| Documentation alignment | Documentation owner | Policy and validation evidence updates in docs | Planned |

## 4. Risks and mitigations
1. Risk: BIOS USB exposure differs across emulators/hardware and may appear as a generic fixed disk.
   Mitigation: classify by capability flags + removable hints, and validate on both `floppy` and `full` QEMU lanes before hardware rollout.
2. Risk: drive letters may shift after hot-plug operations, breaking DOS expectations.
   Mitigation: persist in-memory letter bindings for session lifetime and prefer signature-based reassignment on reinsertion.
3. Risk: stale file handles after media replacement may cause corruption or hangs.
   Mitigation: force per-drive cache invalidation and deterministic stale-handle failure path on detected signature change.
4. Risk: automount retries can increase boot latency or block shell startup.
   Mitigation: bounded retries with timeout budget and non-blocking transition to `NOT_READY` plus manual rescan.
5. Risk: insufficient failure-path coverage can hide regressions.
   Mitigation: add dedicated failure-lane scripts and require marker-based PASS evidence in `build/logs/storage/`.

## 5. Completion criteria
1. Drive-letter policy is implemented exactly as specified and verified by deterministic `[DRIVE-MAP]` markers in both `floppy` and `full` profiles.
2. Boot automount and runtime removable automount work without shell hangs and produce deterministic `[AUTOMOUNT]` traces.
3. Removable media changes trigger `[MEDIA-CHANGE]` events, cache invalidation, and deterministic stale-handle error behavior.
4. Failure/fallback flows are reproducible (`NOT_READY`, `UNSUPPORTED_FS`, `IO_ERROR`) and visible through shell + serial markers.
5. Storage test lanes run with explicit PASS/FAIL outputs and are callable alongside `make qemu-test-all`.
6. Documentation and changelog updates are complete for the delivered milestone and reflect only major project-level impact.

## 6. Next action
Create execution tickets for Step 1 and Step 2, then run and archive baseline evidence before implementation changes:
1. `mkdir -p build/logs/storage`
2. `make qemu-test-floppy`
3. `make qemu-test-full`
4. `make qemu-test-stage1`

Expected immediate output: baseline log bundle and approved marker contract for storage instrumentation.