# 2026-04-20 — OPENGEM-044 Stage 3A shell wire-up

## Context and goal

Stage 3A adds a low-risk shell surface to exercise only the API arm/disarm gates and host-driven probes of OPENGEM-044 Task A/B/C. The command must remain runtime-inert: no legacy mode-switch trampoline arm, no legacy v86 entry, and no actual mode transition.

## Files touched

- `stage2/src/shell.c` — new `mstest` command with `probe`, `arm`, `disarm` subcommands and serial markers.
- `scripts/test_mstest_shell.sh` — new static gate for help/handler presence, ordering, and forbidden call checks.
- `Makefile` — added `test-mstest-shell` target.
- `docs/collab/diario-di-bordo.md` — local coordination update.
- `docs/handoffs/2026-04-20-opengem-044-stage3A-shell-wireup.md` — this local handoff.

## Decisions made

1. `mstest probe` prints raw signed return codes to serial for all three probes, instead of normalizing them, so Task A/B/C diagnostics remain visible as-is.
2. `mstest arm` arms only the published API gates (`MODE_SWITCH_ARM_MAGIC`, `LEGACY_V86_ARM_MAGIC`, `V86_DISPATCH_ARM_MAGIC`). It intentionally never touches any trampoline-live control.
3. `mstest disarm` clears the three API gates in reverse dependency order (`C`, `B`, `A`) and emits a single success marker.
4. The command is independent from the existing `gem` path and does not reuse its runtime loop or call `legacy_v86_enter()`.

## Validation performed

- `make test-mstest-shell`
- `make build/stage2.elf clean`
- full regression via `/tmp/run_gates2_main.sh` when available on host

## Risks and next step

Risks:
- None expected at runtime; the new surface only toggles existing API arm flags and invokes host-driven probes.

Next step:
- Stage 3B/3C can build on `mstest` for operator-facing diagnostics or staged runtime checks without coupling them to the high-risk `gem` execution path.