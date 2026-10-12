# runner-01 — Parallel runner slots (two QEMU at a time) with per-scope ownership

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-runner-01` from `wt/integ2`. Host-testable (the runner's host tests use fake QEMU); the lead qualifies on real suites.
- **When:** after the current campaign; the runner host tests share the common lock with live suites and must not run during one.

## Decision (owner, 2026-10-12)

With memory freed on the workstation (each TCG guest costs about 1.1 GiB), two suites may run at the same time. The architecture rule "one QEMU at a time" (`docs/design/test-architecture.md`) becomes "at most N runner slots, N = 2 by default", still one QEMU per case and still refusing when memory is short.

## Scope

1. `scripts/test/resources.py`: replace the single `build/test-runs/.qemu.lock` with N slot locks (`.qemu.slot0`, `.qemu.slot1`; N from `CIUKI_RUNNER_SLOTS`, default 2, max 4); `preflight` takes the first free slot instead of refusing on any `qemu-system` process; the memory check requires 2 GiB per additional running slot; `heavy_builds()` unchanged.
2. Ownership: `qemu_count()`/`unowned_qemu_count()` and the teardown check ("owned children survived teardown") must count only QEMU processes whose systemd scope (or parent) belongs to this runner, never another slot's guest; a manual QEMU outside any slot still blocks as today.
3. Scratch and evidence directories stay per run id (already); `summary.json` writes must not race between slots (write per suite under a lock or atomic rename).
4. `scripts/test/run.py --slot N` is not needed; slots are taken automatically. Document the behaviour in `docs/design/test-architecture.md` and `docs/build-and-run.md`.
5. Host tests: `tests/host/test_runner.py` (fake QEMU) covers two concurrent runners, slot exhaustion refusal, memory refusal per slot, a foreign manual QEMU still refused, teardown counting only own children.

## Constraints

No change to evidence semantics (image SHA, overlays, TCG flags, record grammar); no QEMU, no commits, no diary. Report to `build/codex/runner-01-report.md` with the verbatim runner test summary.
