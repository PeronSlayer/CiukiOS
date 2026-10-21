# f5-21b — guest observers for the F5 core cases: reusable desktop fixture gate, kernel32 and the nine core fixtures

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead after f5-21 (partial, commit `632c9436`): the registration, aliases, write gates and the kernel32 fixture are done; the directive's "implement the missing observers" clause exceeded one session and lacked desktop authority (report `build/codex/f5-21-report.md`, "Decisions needed")
- **Worktree:** `../CiukiOS-wt-f5-21b`, branch `wt/f5-21b` from `wt/f5-21`

## Decision and authority

The desktop authority f5-21 asked for is granted: `apps/desktop/gate.h`, `apps/desktop/desktop.c`, `apps/desktop/v3_fixture.c` and a new `apps/desktop/w32_fixture_gate.c`. One reusable gate command (`GATE_W32_FIXTURE` with the fixture name as argument) starts w32srv, waits for a successful registry round trip (the f5-10C3 pattern already used by `w32-dialogs`), then spawns `/System/Win32/Fixtures/<name>.exe` and reports the pid; every probe below uses it instead of a per-probe copy. The kernel side keeps the C: write gate rule of f5-21 (a probe that starts w32srv opens it, with the `write-gate` predicate).

## Scope (fixed list)

Kernel observer + fixture PE + registration in the three tables and the loader model + removal from both known-pending lists, for: `w32-kernel32` (fixture from f5-21), `w32-fullscreen`, `w32-registry`, `w32-audio`, `w32-gdi32`, `w32-crash`, `w32-user32`, `w32-codecs`, `w32-ddraw`, `w32-input`. For each: a fixture under `src/probes/win32_<name>/` on the result-block protocol at a fixed free page (`0x10030000 + n·0x10000`, proven free after the real loader start as f5-21's test does), a probe in `src/kernel/probes/` that emits **exactly** the suite's predicates from kernel-observed facts (never application stdout), a host dry-run of the predicates; the suite predicates are unchanged. If a predicate cannot be observed from the kernel, say so in the report with the reason; do not relax it. The `[storage]` write gate, the deferred spawn and the loader table limits (`tests/host/test_boot_modes.py`) apply.

Out of scope, with owners: `w32-installer-nsis` (f5-15A2), `w32-restart` (a later f5-07 directive), the ten title/installer drivers (f5-17 continuation). They stay in the known-pending lists with those owners.

## Tests

`tests.host.test_runner_selectors`, `tests.host.win32.test_probe_registry` (pending lists reduced to the three owners above), `test_boot_modes`, each fixture's dry-run, `tests.host.win32.test_kernel_loader` result-page cases, `make CAP= apps` once; no full discovery suites (the lead runs them). Report `build/codex/f5-21b-report.md` with the case → probe → status table updated.

## Constraints

`rg`/`sed` only; no QEMU, no `systemd-run`, no image builds, no commits, no diary edits; host tests one at a time with `nice -n 19`; no new `panic()`; English.
