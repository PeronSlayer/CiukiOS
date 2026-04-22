# Copilot Codex Task Pack - M6 Closure (Protected Mode / DOS Extender Readiness)

## Mandatory Branch Isolation
Codex must work only on a dedicated branch and must not touch `main` directly.

```bash
git fetch origin
git switch -c feature/copilot-codex-m6-closure-v1 origin/main
```

No direct commits on `main`. No force-push on shared branches.

## Mission
Close `M6` in `Roadmap.md` by implementing the remaining in-progress pieces and promoting planned items to implemented baseline contract.

Target: move all M6 bullets to `DONE` (with explicit baseline scope and deterministic tests).

## Scope (5 heavy tasks)

### M6-C1) Transition State Block + Runtime Markers v2
1. Add explicit `pmode_transition_state` structure shared by stage2 runtime modules.
2. Track and log:
- pre-transition descriptor snapshots (GDTR/IDTR)
- intended CR0 transition flags
- return-path status
3. Add deterministic markers for each step.

Markers:
- `[m6] transition state init: PASS`
- `[m6] gdt/idt snapshot: PASS`
- `[m6] cr0 transition contract: PASS`
- `[m6] return-path contract: PASS`

### M6-C2) Real-Mode Entry Infrastructure Baseline (A20 + Descriptor Hygiene)
1. Implement A20 status probe utility and controlled enable path (contract-level baseline).
2. Validate GDT/IDT baseline ownership for transition handoff.
3. Add failure reason markers (no silent fallback).

Markers:
- `[m6] a20 probe=on|off`
- `[m6] a20 enable result=PASS|FAIL`
- `[m6] descriptor baseline ready=1`

### M6-C3) DOS Extender Host Interface Skeleton (Deterministic)
1. Add minimal host-interface layer for DOS extender readiness:
- DPMI detect query skeleton path
- real-mode callback registration skeleton
- interrupt reflection skeleton
2. Must be non-crashing, deterministic, and explicitly marked as baseline skeleton where full behavior is pending.

Markers:
- `[m6] dpmi detect skeleton ready`
- `[m6] rm callback skeleton ready`
- `[m6] int reflect skeleton ready`

### M6-C4) Pmode Memory Accounting Baseline
1. Introduce pmode memory accounting domain separated from DOS conventional arena.
2. Add guardrails to prevent overlap with stage2 runtime buffers.
3. Emit accounting markers with ranges and status.

Markers:
- `[m6] pmem range base=0x... size=0x...`
- `[m6] pmem overlap check: PASS`

### M6-C5) M6 Gate Expansion + Docs Closure
1. Extend `scripts/test_m6_pmode_contract.sh` to assert new markers.
2. Add script: `scripts/test_m6_transition_contract_v2.sh`.
3. Include new gate in `scripts/test_doom_readiness_m6.sh`.
4. Update docs:
- `docs/m6-dos-extender-requirements.md`
- `docs/pmode-transition-contract.md`
- `Roadmap.md` M6 section -> `DONE` (baseline closure)
5. Update README changelog (software-only, public-facing).

## Constraints
1. No regressions to:
- INT21h, MZ runtime, DOS run path
- video pipeline
- FreeDOS/OpenGEM integration tests
2. Deterministic markers only (no random/timestamps).
3. No ABI break without documented migration note.

## Validation (must run)
1. `make all`
2. `make test-m6-pmode`
3. `bash scripts/test_m6_transition_contract_v2.sh`
4. `bash scripts/test_doom_readiness_m6.sh`
5. `make test-phase2`

## Final Handoff
Create:
- `docs/handoffs/YYYY-MM-DD-copilot-codex-m6-closure-v1.md`

Include:
1. changed files
2. implemented vs skeleton details
3. test outputs
4. residual risks
5. next tasks (max 5)
