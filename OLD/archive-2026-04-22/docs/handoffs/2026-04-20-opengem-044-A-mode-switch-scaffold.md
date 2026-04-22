# 2026-04-20 — OPENGEM-044 split + Task A scaffolding

## Context and goal

Runtime OPENGEM-043 confirmed architectural impossibility of the compat-mode host task (Intel SDM 3A §20.1: v86 not available in IA-32e). User selected **Path 1 (full legacy mode-switch)** because long-term target is real retro hardware (no VT-x dependency). User also requested that the subsystem be split across 3 agents working in parallel on dedicated branches, with merges controlled only by explicit user request.

## Files touched

Branch: `feature/opengem-044-A-mode-switch`.

- `docs/agent-directives.md` — new "Multi-Agent Parallel Task Rule" section (6 clauses).
- `docs/opengem-044-mode-switch-split.md` — NEW. Full multi-agent split spec: contracts for Task A / B / C, reserved magics (0xC1D39440/50/60) and sentinels (0x0440/50/60), file ownership boundaries, handoff contract, assignments table.
- `stage2/include/mode_switch.h` — NEW. Task A public API: `mode_switch_run_legacy_pm`, arm/disarm/is_armed, probe. Constants MODE_SWITCH_ARM_MAGIC=0xC1D39440u, MODE_SWITCH_SENTINEL=0x0440u. Return codes including MODE_SWITCH_ERR_NOT_IMPLEMENTED (until asm trampoline lands).
- `stage2/src/mode_switch.c` — NEW. Disarmed scaffold: arm-gate state, probe with 5 disarmed/input-validation cases. `run_legacy_pm` arm-checks FIRST, then validates body, then returns NOT_IMPLEMENTED (boot-safe: no CR/MSR/LGDT/LIDT/LTR writes until asm ships in a follow-up commit on this branch).
- `scripts/test_mode_switch.sh` — NEW. 25 static checks: sentinels, magic, API signatures, arm-flag default 0, magic enforcement, arm-check-FIRST ordering, forbidden CR/MSR/descriptor writes in C, boot-path isolation (no external callers), probe coverage.
- `Makefile` — added `test-mode-switch` target.

## Decisions made

1. **Task split geometry chosen: A/B/C = mode-switch engine / legacy-PM v86 host / dispatcher+loader.** Each has a stable interface contract so B can stub against A and C can stub against B while the three work in parallel.
2. **Reserved namespace:** 0xC1D39440/50/60 for magics, 0x0440/50/60 for sentinels. No overlap with 017..043.
3. **040/041 compat-entry files preserved** as historical reference. Task C may later remove them after the long↔legacy↔v86 stack is fully merged and verified.
4. **Task A ships in two stages** on the same branch:
   - Stage 1 (THIS commit): disarmed scaffold + API contract + gate. Engine returns NOT_IMPLEMENTED. No CR/MSR/descriptor writes anywhere. 26/26 gates green, stage2 builds clean.
   - Stage 2 (next commit on this branch): `stage2/src/mode_switch_asm.S` trampoline (long→PM32→long round-trip). At that point NOT_IMPLEMENTED is replaced by OK when the round-trip completes. Probe is extended to exercise the asm path host-driven with a tiny PM32 body that writes a marker.
5. **Assignments:** Task A claimed by this agent. Task B and Task C open for other agents — the split doc lists "unassigned" for both.

## Validation performed

- `bash scripts/test_mode_switch.sh` → OK=25 FAIL=0.
- `bash /tmp/run_gates2.sh` → 26/26 PASS (017..041 + 044-A). Zero regression.
- `make build/stage2.elf` → clean compile and link. `mode_switch.o` integrated via existing `STAGE2_C_SRCS := $(shell find stage2/src -type f -name '*.c')` discovery.
- Boot path unaffected (arm flag default 0, no external caller, grep verified).

## Risks and next step

**Risks:**
- None at this stage: the engine is explicitly NOT_IMPLEMENTED. It cannot triple-fault because it does not touch any privileged register. Runtime regression impossible.
- Next-stage risk (mode_switch_asm.S): first long-mode exit in project history. Likely needs multiple iterations. Mitigation: probe host-driven first (write marker in PM32, return); integrate into shell only after round-trip proven.

**Next step options:**
1. Continue on THIS branch: implement `mode_switch_asm.S`, extend probe to actually execute, reach Stage 2 of Task A.
2. Park Task A at scaffold level, open branches for Task B (`feature/opengem-044-B-legacy-v86-host`) and Task C (`feature/opengem-044-C-dispatch-loader`) so multi-agent work can begin in parallel.
3. User requests `fai il merge` of Task A scaffold now, then choose follow-up.

The split document (`docs/opengem-044-mode-switch-split.md`) is the single source of truth for Task B and Task C agents. Any new prompt for another agent must instruct them to read it alongside `CLAUDE.md`, `docs/agent-directives.md`, `docs/collab/diario-di-bordo.md`.
