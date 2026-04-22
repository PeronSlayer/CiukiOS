# OPENGEM-016 — Design document kickoff (no-code)

## Context and goal
OPENGEM-015 closed the OpenGEM observability series with the full MZ header probe on `gem.exe`, which lands on `requires-extender / mz-max-alloc-64k`. The next step is architectural and cannot be a single-session code commit. OPENGEM-016 is therefore a **design-only deliverable**: a formal design document that (a) picks the execution strategy for 16-bit code on CiukiOS, (b) declares the long-term Windows DOS-based scope, (c) declares the permanent Windows NT non-goal, and (d) plans the incremental implementation series OPENGEM-017+.

## Files touched
- `docs/opengem-016-design.md` — new design document (strategy, tier map, architectural contract, phase plan, approval gates).
- `docs/roadmap-windows-dosbased.md` — new companion roadmap (T0..T6 compatibility tiers for Windows DOS-based family).
- `CLAUDE.md` — Project North Star extended with long-term Windows DOS-based objective and permanent NT non-goal; Source of Truth list extended to include the two new documents; Last Updated → 2026-04-19.

## Decisions
1. **Strategy A (v8086 monitor) is chosen.** DPMI-only and software-emulator strategies are rejected on the record.
2. **Long-term Windows DOS-based scope.** Targets: Windows 1.x / 2.x / 3.0 / 3.1 / WfW 3.11 / 95 / 98 / ME.
3. **Permanent non-goal: Windows NT and all its descendants** (NT 3.x, 4.0, 2000, XP, Vista, 7, 8, 10, 11) plus ReactOS.
4. **Tier map T0..T6** codified in the design document. Each tier is a CiukiOS milestone.
5. **Three-level execution model**: long mode (host) ↔ 32-bit protected mode (compatibility host) ↔ virtual-8086 (guest). Mode-switch is the first real deliverable (OPENGEM-017).
6. **Approval gate before OPENGEM-017**: four explicit user approvals required (document reviewed, Strategy A confirmed, NT non-goal confirmed, tier map confirmed). No 16-bit execution code lands until those approvals are recorded.

## Validation performed
- Zero-code change verification: no source files in `stage2/`, no scripts in `scripts/`, no Makefile targets modified.
- Existing regression stack untouched; no gates re-run required (per CiukiOS policy, design-only changes do not gate on runtime).
- Document cross-references consistent: `opengem-016-design.md` references `roadmap-windows-dosbased.md` and vice versa; `CLAUDE.md` references both.

## Risks
- **Approval risk**: the design document itself is the deliverable; if the user does not approve Strategy A, OPENGEM-017+ cannot begin. No mitigation other than the document's quality.
- **Scope expansion risk**: Windows DOS-based is a significant broadening of the CiukiOS objective. The non-goal clause mitigates scope creep into NT but does not mitigate the effort scaling to hit Win9x quality.
- **Documentation drift risk**: three files now encode the scope (`opengem-016-design.md`, `roadmap-windows-dosbased.md`, `CLAUDE.md`). Any future update must touch the relevant set together.

## Next step
- **User action**: review `docs/opengem-016-design.md` and confirm the four approval gates listed in §8.
- **Agent action upon approval**: open `feature/opengem-017-mode-switch-scaffold` and deliver the long-mode ↔ 32-bit PE mode-switch scaffold as pure observability (no v8086 yet), per §6.1 of the design document.

## Branch + commit
- Branch: `feature/opengem-016-design` (from `main` tip after OPENGEM-015 merge).
- Awaiting explicit `fai il merge`. Do not merge into main automatically.
