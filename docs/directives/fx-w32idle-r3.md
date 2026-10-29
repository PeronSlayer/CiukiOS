# fx-w32idle round 3: power wake after a session turn, overlapping deadlines

- **Written:** 2026-10-29 08:47 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-w32idle`, branch `wt/fx-w32idle` at `f4d34a155`.
- **Input:** the review `build/lead-scratch/fx-w32idle-review-0647.md` in the main checkout. It confirms that the three round-2 P1 paths are fixed.

## Scope

Fix each finding. For each, add a test that fails on `f4d34a155` and passes after the fix. Keep the round-2 fixes and tests unchanged.

1. **P1 — a power wake is lost after a session turn** (`apps/w32srv/session_impl.h:193`).
   - The bug: a thread consumes its power latch while another thread owns the end-session turn. Its session poll returns empty, yet the power turn keeps `notified=1`. When the session resets, no new notification goes out, and the undelivered power message can expire.
   - The required behaviour: a pending power message that has not been delivered gets a new notification whenever the turn that blocked it ends (session reset, release or completion). Deduplication may suppress repeat signals only while the message is still deliverable to a thread that has not yet consumed its latch.
   - Test: the review's sequence (session turn on thread A, power latch consumed by thread B, session reset). After the reset, B receives exactly one replacement notification, and the power message is delivered before it expires.
2. **P2 — overlapping session and power deadlines make `w32srv` spin** (`session_impl.h:189`, `:208`).
   - The bug: an active HOLD session stops power from advancing, but `wr_deadline()` keeps choosing the expired power deadline, so the main loop does zero-time waits.
   - The required behaviour: `wr_deadline()` ignores a deadline that cannot advance while the session holds it, and returns the next deadline that can fire (the session's, a timer's, or none). When the hold ends, the deferred power deadline is evaluated at once against the refreshed clock.
   - Test: drive the main loop, not only `wr_deadline()`. Under an active HOLD session with an expired power deadline, the loop performs a bounded number of waits, none of them zero-time after the first, until the session releases. Then the power step runs.
3. Run the win32 suite and the USER queue tests sequentially with `nice -n 19`. Report the idle figures again.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-w32idle-r3-report.md`.
