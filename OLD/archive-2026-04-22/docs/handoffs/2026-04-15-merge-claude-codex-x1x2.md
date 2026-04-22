# Handoff - Claude + Codex integration (2026-04-15)

## Branches merged into `main`

- `feature/claude-m3-fat-io-hardening` (`d58db65`)
- `feature/codex-x1-x2-mz-dispatch-int21-mvp` (`4a22e3b`)

## What is now in `main`

### Claude side (M3 hardening)
- `attrib` command available in shell.
- FAT layer hardening improvements (`fat.c`/`fat.h`).
- Stage2 boot tests updated.

### Codex side (X1/X2 runtime ABI completion)
- Service ABI extended with DOS-like `int21` register block in `boot/proto/services.h`.
- Service table now exposes `int21(ctx, regs)` callback for COM/MZ runtime contracts.
- `INIT.COM` sample updated to:
  - print via `INT 21h, AH=09h` when available,
  - terminate via `INT 21h, AH=4Ch` when available,
  - fallback safely to legacy service path.

## Validation run

All checks passed after merge on `main`:

- `make test-stage2`
- `make test-fallback`
- `make test-fat-compat`

## Suggested next integration checkpoints

1. Connect `run <name>` to FAT name-based COM/EXE loading path (remove single-image assumptions).
2. Expand `INT 21h` subset (`AH=3Dh/3Fh/40h/42h/4Eh/4Fh`) for FreeCOM/FreeDOS compatibility ramp-up.
3. Keep MZ dispatch split explicit: CIUKEX64 marker path vs true 16-bit path.
