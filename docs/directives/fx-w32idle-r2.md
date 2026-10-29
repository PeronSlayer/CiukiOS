# fx-w32idle round 2: the three P1 review findings

- **Written:** 2026-10-29 05:59 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-w32idle`, branch `wt/fx-w32idle` at `271b63121`.
- **Input:** the review `build/lead-scratch/fx-w32idle-review-0447.md` in the main checkout.

## Scope

Fix each finding and add a test that fails on round 1 and passes after the fix.

1. **Timer due between publication and sleep** (`sdk/win32/user32/messages.c:527`).
   - The wait deadline is always the earliest due timer, computed after the last queue check.
   - A deadline already reached returns at once and synthesizes `WM_TIMER`. It never falls into an `INFINITE` wait.
   - Test: make the clock reach the deadline between `u32_mark()` and the wait.
2. **Duplicate session notification spin in a nested loop** (`messages.c:548`, `session.c:23`).
   - One pending session notification is consumed once, by whichever loop is active, nested loops included.
   - `MsgWaitForMultipleObjects` re-checks the queue and the elapsed timeout before it retries. A finite wait always ends by its timeout.
   - Prefer that the server signals once per pending state rather than at both dispatch and loop tail.
   - Test: a session callback that enters a modal loop while a duplicate notification is pending.
3. **Stale server time after an idle sleep** (`apps/w32srv/main.c:203`).
   - Refresh `s.now` after every wake and before any request is dispatched.
   - Session, power and shutdown deadlines are computed from the refreshed clock.
   - Test: the main-loop order, not only `wr_dispatch`. After 60 s idle, a session request keeps its full window, and a shutdown countdown starts from the request time.
4. Run the win32 suite and the USER queue tests sequentially with `nice -n 19`. Report the before/after idle figures again.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-w32idle-r2-report.md`.
