# Prompt For Assigned Agent - BIOS-RUNTIME-GAP-001

Work on a dedicated branch only:

```bash
git fetch origin
git switch -c feature/copilot-claude-bios-runtime-gap-001 origin/main
```

You are Claude Opus and you are implementing the next BIOS/DOS runtime compatibility slice for CiukiOS.

Read first:
1. `CLAUDE.md`
2. `docs/agent-directives.md`
3. `docs/collab/diario-di-bordo.md`
4. `docs/copilot-task-claude-bios-runtime-gap-001.md`
5. `docs/roadmap-ciukios-doom.md`
6. `docs/int21-priority-a.md`
7. `scripts/test_doom_boot_harness.sh`
8. `scripts/test_doom_readiness_m6.sh`
9. `stage2/src/shell.c`
10. `stage2/src/stage2.c`

Before starting implementation, confirm in `docs/collab/diario-di-bordo.md` that no other agent is already working on the same project area. If it is already occupied, stop and choose a different section instead of overlapping work.

Context:
CiukiOS already has baseline BIOS markers and deterministic runtime gates, but the roadmap still shows BIOS and DOS runtime gaps between the current shell/runtime baseline and the frozen DOOM startup path. This task must improve the next startup-relevant BIOS/runtime behaviors from evidence, not by broad speculative parity work.

Your mission is to:
1. identify the next startup-relevant BIOS/DOS behaviors to strengthen
2. implement the smallest useful compatibility slice for those behaviors
3. strengthen deterministic validation and markers around that slice

Hard requirements:
1. Do not bump the project version.
2. Do not commit on `main`.
3. Stay in the BIOS/runtime compatibility category only.
4. Do not overlap with the separate protected-mode regression target task.
5. Preserve deterministic logs and current runtime behavior.
6. Update `docs/collab/diario-di-bordo.md` at the end of the task, and do not add that file to Git.

Validation required before handoff:
1. `make all`
2. `make test-stage2`
3. any updated BIOS/runtime gate you add
4. any relevant DOOM-startup-facing harness you strengthen

Deliverables:
1. Working code on the dedicated branch
2. Updated BIOS/runtime validation coverage
3. Any minimal docs updates needed
4. A handoff file named:
   - `docs/handoffs/YYYY-MM-DD-copilot-claude-bios-runtime-gap-001.md`

In the final handoff, explicitly report:
1. which BIOS/runtime behaviors were strengthened
2. what evidence drove the choices
3. markers and tests added or updated
4. tests run and their status
5. remaining gaps before meaningful DOOM startup progress