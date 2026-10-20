# f5-07C — Win32 service traffic at idle and during end-session

- **Owner:** Codex, `gpt-6.1-sol`, effort `high`
- **Written:** 2026-10-20 by the lead (Claude), after the f5-07B integration (`main 29070119`) and a read-only Codex review of its runtime cost (`build/lead-scratch/f5-07b-perf-review.md`, findings quoted below)
- **Worktree:** `../CiukiOS-wt-f5-07c`, branch `wt/f5-07c`
- **Target:** ThinkPad T23 (Pentium III 1.13 GHz, single core, slow PATA/CF disk). The owner reported desktop lag on that machine; the next hardware image must not add idle service traffic or blocking on the desktop thread.

## Background

f5-07B added end-session coordination by polling. The lead fix `29070119` throttled it (desktop idle POLL 500 ms, USER per-thread poll 250 ms, the `WININIT.INI` stat only for the boot check). The review then found:

1. `apps/desktop/win32_restart.c` / `win32_native.c`: the desktop thread still performs a synchronous w32srv exchange every 500 ms (two packet round trips, `rpc_lock` without deadline), even with no Win32 program running; a busy w32srv (hive commit, `SERVICE.CRS` write, DDE disk work) freezes input and painting for the whole wait.
2. `sdk/win32/user32/session.c`: each poll allocates and zeroes a ~70 KB response buffer and costs a CLIENT plus an ENDSESSION transaction (four packet round trips), including while `GetMessage` waits with no messages.
3. `sdk/win32/ntdll/server.c` `w32_server_exchange`: a W32R_CLIENT announcement precedes **every** service request (registry, directory, fonts, gdi32 bridge), doubling the transactions of every small RPC.
4. Regression of `29070119`: the 250 ms delay is armed before each notification is handled; one thread with N top-level windows needs about N x 500 ms for QUERY and END, against one 10 s deadline.
5. `apps/w32srv/main.c` `--fresh-clients` / `wr_reconcile` and `apps/w32srv/directory.c`: stale process objects are removed one at a time through `wr_exit()`, each one a whole-store serialization, a commit-marker patch and two fsyncs, before the endpoint is registered.

## Required changes

1. **Announce once per connection** (finding 3). ntdll sends W32R_CLIENT once per service connection lifetime and again only after the connection is re-established (server restart, endpoint change, a refused request that proves the server lost the identity). No permanent process-local boolean that would survive a w32srv restart. Keep the FastPebLock rule of f5-07A amendment 1 (one lock across BEGIN/WRITE/READ).
2. **USER end-session delivery** (findings 2 and 4). Idle: at most one poll per `U32_SESSION_POLL_MS` (250 ms) per UI thread. After a poll returns a notification, the thread polls again at its next pump without delay, and keeps a fast interval (at most 20 ms) until a poll returns nothing for one full fast interval after the last notification of that session. The reply buffer of the poll is sized for the poll's reply, not the 70 KB service maximum; no allocation per idle poll.
3. **Desktop poll off the desktop thread** (finding 1). The idle W32R_SHUTDOWN POLL (and the boot pending-work check) runs on one persistent worker thread; the desktop thread only takes the latest result through a nonblocking handoff (the existing `launch_lock` trylock pattern in `win32_native.c` is the model) and keeps every restart state change, dialog and notice on the desktop thread. Interval: 500 ms while at least one desktop client carries a Win32 HWND identity (`v3_win32_hwnd`) or a Win32 launch is pending, 5 s otherwise (bounded latency for a windowless caller). The worker never takes a lock the desktop thread holds across a frame. Restart stages after `desk_restart_begin` may stay on the desktop thread as now.
4. **Stale identities in one commit** (finding 5). Fresh-client reset and reconcile remove all stale process-owned objects in memory and persist the directory once, before the endpoint is registered. Preserve global atoms and the durable identity reservation counters (conversation IDs and HWNDs must still never alias across service restarts).

Out of scope (later directive): DDE timeouts and bounded PROGMAN filesystem steps (review finding 6); the w32srv 20 ms active-yield tail.

## Acceptance (host tests)

- A fixture counts transactions: N registry requests from one process produce exactly one W32R_CLIENT; after a simulated w32srv restart the next request re-announces once.
- USER: one thread with 8 top-level windows completes QUERY and END for all of them within 8 x 2 x 20 ms + 250 ms of fixture ticks, with accept, veto and force cases; idle pumping produces at most one poll per 250 ms and no allocation per poll.
- Desktop: with a fake exchange that blocks for 2 s, the desktop poll function returns immediately and the next frame is not delayed; intervals 500 ms / 5 s as specified; the restart state machine still passes `tests/host/win32/shell` session cases.
- w32srv: M = 50 stale objects from a previous boot are cleared with exactly one directory save (count io writes and syncs); identity counters preserved; existing PID-reuse and windowless-client cases still pass.
- Existing focused suites green: `tests.host.win32.user32`, `tests/host/win32/shell`, the desktop Win32 adapter, restart and startup tests, the w32srv registry and directory tests.

## Rules

Host tests only; the lead runs the full suites, the image build and the hardware trial. No new kernel ABI. Report in `build/codex/f5-07c-report.md`.
