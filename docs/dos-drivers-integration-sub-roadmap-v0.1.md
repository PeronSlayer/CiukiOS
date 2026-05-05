# DOS Drivers Integration Sub-Roadmap v0.1

## Current objective
Complete runtime-ready integration of DOS generic drivers into CiukiOS, starting from the already stable full-build packaging baseline under SYSTEM/DRIVERS.

### Progress delta (as of 2026-05-05)
- Completed: full build packaging path injects drivers under SYSTEM/DRIVERS.
- Completed: baseline build evidence exists from full build runs.
- Deferred by decision gate: stage1 boot-time autoload remains disabled due to stage1 size gate risk.
- Completed: runtime activation helper is available via SYSTEM/DRIVERS/DRVLOAD.COM with fail-open behavior and markers.
- Completed: automated runtime smoke helper is available to invoke DRVLOAD from shell and verify deterministic serial markers.
- Completed: DOOM taxonomy automation now invokes DRVLOAD before launching DOOM from the full-profile shell.
- Completed: DOOM is playable on the full FAT16 profile despite DRVLOAD remaining a fail-open helper for unavailable DEVLOAD/MSCDEX activation.
- In progress: DRVLOAD evidence mode now includes a native `.SYS` loader slice. It loads `QCDROM.SYS`, calls the driver INIT strategy/interrupt entry points, links the device header from DRVLOAD, and QCDROM detects the QEMU DVD-ROM as `QCDROM1`; MSCDEX launches but remains blocked at child exit `0x11` until kernel-owned DOS List-of-Lists/CDS/device-handle compatibility is implemented.

## Step-by-step plan
Dependency chain: Phase 1 -> Phase 2 -> Phase 3A -> Phase 4 -> Phase 5.
Critical path: Phase 1, Phase 2, Phase 3A, Phase 4, Phase 5.
Parallelization model: only 2 independent streams in Phase 3 (A implementation, B validation prep), merged at Phase 4.

1. Phase 1 - Confirm and freeze packaging baseline [Status: Completed]
- Dependencies: none.
- Critical path: yes (entry gate).
- Activities: rerun full build; capture inventory snapshot for SYSTEM/DRIVERS as immutable baseline evidence.
- Phase acceptance criteria (measurable):
	- scripts/build_full.sh exits with code 0.
	- Expected driver files are present in SYSTEM/DRIVERS in full artifact output.
	- Baseline inventory file is generated with timestamp.
- Evidence command checklist:
	- bash scripts/build_full.sh
	- mdir -i build/full/ciukios-full.img ::/SYSTEM
	- mdir -i build/full/ciukios-full.img ::/SYSTEM/DRIVERS
	- scripts/verify_full_drivers_payload.sh

2. Phase 2 - Define runtime activation contract [Status: In Progress]
- Dependencies: Phase 1 evidence.
- Critical path: yes.
- Activities: define deterministic load order, activation trigger, fallback behavior, and boot-time error signaling.
- Phase acceptance criteria (measurable):
	- Contract is reviewed and approved by implementation, validator, and reviewer roles.
	- Deterministic marker sequence is specified for every activation path.
	- Missing/invalid driver handling is defined with expected runtime outcome.
- Evidence command checklist:
	- rg -n "SYSTEM/DRIVERS|driver|load order|fallback|marker" docs src
	- git diff -- docs/dos-drivers-integration-sub-roadmap-v0.1.md

3. Phase 3 - Execute implementation and validation preparation (parallel, 2 streams) [Status: Pending]
- Dependencies: Phase 2 approved contract.
- Critical path: Stream A yes; Stream B no (support stream).
- Independence rationale: Stream A modifies runtime activation logic; Stream B prepares validation matrix assets independently until merged in Phase 4.
- Stream A (implementation): integrate runtime activation path for drivers from SYSTEM/DRIVERS and emit deterministic activation markers.
- Stream A acceptance criteria (measurable):
	- Runtime logs show marker sequence in expected load order.
	- Missing/invalid driver path triggers fallback without blocking boot.
- Stream B (validation prep): build matrix and procedure pack for normal, missing, invalid, and mixed driver scenarios.
- Stream B acceptance criteria (measurable):
	- Matrix includes scenario ID, command set, expected result, and pass/fail field.
	- 100% mandatory scenarios are defined before execution.
- Evidence command checklist:
	- rg -n "DRIVER|SYSTEM/DRIVERS|fallback|marker" src docs
	- find build -type f | rg "validation|marker|drivers"

4. Phase 4 - Run runtime validation matrix and regression checks [Status: Pending]
- Dependencies: Phase 3A complete and Phase 3B complete.
- Critical path: yes.
- Activities: execute mandatory scenarios and collect logs/artifacts.
- Mandatory scenarios: normal boot; missing driver; invalid/corrupted driver; mixed valid and invalid set.
- Phase acceptance criteria (measurable):
	- 100% mandatory scenarios executed and recorded.
	- Every scenario has explicit pass/fail plus linked evidence artifact.
	- No unresolved high-severity regression remains open, or it is explicitly deferred with owner/date.
- Evidence command checklist:
	- make build-full
	- make build-full-cd
	- make qemu-test-full
	- make qemu-test-full-cd
	- make qemu-test-full-cd-shell-drive
	- make qemu-test-full-shell-stability
	- make qemu-test-full-drvload-smoke
	- make qemu-test-setup-runtime-hdd-install
	- make qemu-test-all
	- DOOM_TAXONOMY_DISPLAY_MODE=none DOOM_TAXONOMY_SCREENSHOT=build/full/doom_stability.ppm QEMU_TIMEOUT_SEC=120 DOOM_TAXONOMY_OBSERVE_SEC=45 DOOM_TAXONOMY_MIN_STAGE=runtime_stable make qemu-test-full-doom-taxonomy
	- find build/validation -type f | sort
	- rg -n "DRIVER|ERROR|FALLBACK|SYSTEM/DRIVERS" build/validation build/full

5. Phase 5 - Close integration cycle and publish handoff [Status: Pending]
- Dependencies: Phase 4 complete.
- Critical path: yes.
- Activities: consolidate technical decisions, open risks, final status, and prioritized follow-up.
- Phase acceptance criteria (measurable):
	- Handoff includes decision summary, execution status (done/in progress/blocked), and open risks.
	- Prioritized TODO list is present and actionable for next cycle.
	- Reviewer sign-off is recorded.
- Evidence command checklist:
	- git diff -- docs/dos-drivers-integration-sub-roadmap-v0.1.md
	- rg -n "Status:|acceptance criteria|Next action|Test status" docs/dos-drivers-integration-sub-roadmap-v0.1.md

## Assignments
Workload split by worker specialization for this cycle:
- Mapper worker (15%): map driver set, activation order constraints, and dependency notes.
- Implementation worker (40%): runtime activation flow, fallback behavior, and marker instrumentation.
- Validator worker (25%): validation matrix design/execution, evidence capture, and defect triage.
- Doc worker (10%): roadmap maintenance, decision traceability, and handoff consistency.
- Reviewer worker (10%): gate approvals on Phase 2 contract and Phase 4 outcomes.

Phase ownership:
- Phase 1: Mapper + Validator.
- Phase 2: Mapper + Reviewer.
- Phase 3A: Implementation.
- Phase 3B: Validator + Doc.
- Phase 4: Validator + Reviewer.
- Phase 5: Doc + Reviewer.

## Risks and mitigations
- Risk: runtime boot instability due to early driver activation failures.
- Mitigation: staged activation with fail-safe fallback and explicit boot error signaling; validate first on controlled smoke scenarios.
- Risk: driver ordering or compatibility conflicts causing non-deterministic behavior.
- Mitigation: enforce deterministic load order and add activation markers for traceability.
- Risk: incomplete validation coverage hides regressions.
- Mitigation: matrix must include mandatory negative and mixed scenarios before closure gate, with 100% scenario execution tracking.

## Completion criteria
- Phase 1 to Phase 5 each meet their measurable acceptance criteria.
- Full build evidence confirms targeted generic drivers are present under SYSTEM/DRIVERS.
- Runtime activation behavior is deterministic and evidenced by marker logs.
- Runtime validation matrix has 100% mandatory scenario execution with evidence links.
- No unresolved high-severity runtime regression remains at cycle closure.
- Handoff package includes decisions, status, risks, test outcome summary, and prioritized next-cycle TODOs.

## Next action
Define the next driver-runtime hardening slice after the QCDROM `.SYS` INIT milestone: keep DRVLOAD fail-open behavior stable, then complete MSCDEX compatibility by hardening DOS List-of-Lists/CDS/device-chain internals and IOCTL forwarding without regressing the full-profile DOOM lane.

## Test status
- Full build integration status (SYSTEM/DRIVERS): Completed/PASS in full build packaging baseline.
- Runtime activation status: Available through DRVLOAD.COM helper lane (manual invocation).
- Runtime activation smoke status: Available through scripts/qemu_test_full_drvload_smoke.sh (serial markers BEGIN/TRY/DONE); `DRVLOAD_ARGS=/DEVLOAD make qemu-test-full-drvload-smoke` now verifies the native `.SYS` loader reaches QCDROM INIT and detects the QEMU DVD-ROM, while MSCDEX still exits with `0x11`.
- DOOM automation integration status: Available through scripts/qemu_test_full_doom_taxonomy.sh; Phase 4 gameplay playable milestone is closed in v0.6.1 and the 2026-05-05 stability run reached `visual_gameplay` with minimum stage `runtime_stable`.
- Stage1 boot autoload status: Deferred by size gate decision.
- Regression matrix status: Active full/full-CD stability baseline passed on 2026-05-05; QCDROM `.SYS` INIT evidence now passes, and real MSCDEX drive-letter activation remains pending on kernel-owned DOS List-of-Lists/CDS/device-handle and IOCTL compatibility.
