# 2026-04-20 — OPENGEM-044-B stage-1 scaffold

## Context and goal

Task B owns the legacy-PM v86 host layer sitting on top of Task A's long↔legacy mode-switch engine. Stage 1 is intentionally a boot-safe scaffold: publish the API, keep the arm-gate default disarmed, route through Task A's `mode_switch_run_legacy_pm`, and map Task A's current `MODE_SWITCH_ERR_NOT_IMPLEMENTED` into a structured legacy-v86 fault so the branch is buildable and testable before the real PM32→v86 path exists.

## Files touched

Branch: `feature/opengem-044-B-legacy-v86-host`.

- `stage2/include/legacy_v86.h` — new public Task B contract with magic `0xC1D39450u`, sentinel `0x0450u`, guest frame/exit structs, arm API, and dedicated stage-1 fault codes.
- `stage2/src/legacy_v86.c` — new arm-gated scaffold implementation. `legacy_v86_enter()` requires Task B armed first, then calls `mode_switch_run_legacy_pm(legacy_v86_pm32_body, &context)`, mapping Task A `NOT_ARMED` and `NOT_IMPLEMENTED` into `LEGACY_V86_EXIT_FAULT` while preserving the input frame in `out->frame`.
- `stage2/src/legacy_v86_pm32.S` — new placeholder PM32 body that writes `OPENGEM-044-B` to port `0xE9` and returns. No TSS/IDT/GDT/v86 entry yet.
- `scripts/test_legacy_v86.sh` — new static gate with >20 checks covering API, sentinel/magic, arm-first discipline, forbidden-write scan in C, boot-path isolation, asm placeholder shape, and probe coverage.
- `Makefile` — added `test-legacy-v86` target only.
- `docs/collab/diario-di-bordo.md` — local coordination entry for Task B scaffold completion.
- `docs/handoffs/2026-04-20-opengem-044-B-scaffold.md` — this local handoff.

## Decisions made

1. Stage 1 returns structured faults, not raw mode-switch errors. Task A's `MODE_SWITCH_ERR_NOT_IMPLEMENTED` is translated to `LEGACY_V86_EXIT_FAULT` with `LEGACY_V86_FAULT_MODE_SWITCH_NOT_IMPLEMENTED` as required by the split contract.
2. `legacy_v86_enter()` treats Task A disarmed separately (`LEGACY_V86_FAULT_MODE_SWITCH_NOT_ARMED`) so probe coverage can verify the double arm-cascade without touching Task A files.
3. The PM32 body lives in a dedicated `.S` file already, even though it is only a marker writer today. This keeps all future privileged instructions out of the C scaffold and matches the ownership boundary from the split doc.
4. A small `legacy_v86_context_t` is passed to Task A now so Stage 2 can grow into real PM32/v86 state marshaling without changing the public API.

## Validation performed

- `bash scripts/test_legacy_v86.sh`
- `bash /tmp/run_gates2.sh` if present, otherwise manual extra step for `test-legacy-v86`
- `make build/stage2.elf clean`
- grep-based verification inside the gate that no boot-path caller references `legacy_v86_*`

## Risks and next step

Risks:
- Stage 1 never enters v86; even if Task A later lands its asm trampoline first, the placeholder PM32 body only writes a serial marker and returns. That path is intentionally classified as `LEGACY_V86_EXIT_FAULT` (`LEGACY_V86_FAULT_PM32_BODY_RETURNED`) until Stage 2 installs the real PM32 host.
- Task B still depends transitively on Task A's final trampoline before runtime validation can move beyond structured fault mapping.

Next step:
- Once Task A lands the real long↔legacy trampoline, extend `legacy_v86_pm32.S` into a real legacy PM host with GDT32/IDT32/TSS32, `IRETL` into v86, and #GP-driven exit classification (`NORMAL`, `GP_INT`, `HALT`, `FAULT`) as specified in `docs/opengem-044-mode-switch-split.md` §3.2.