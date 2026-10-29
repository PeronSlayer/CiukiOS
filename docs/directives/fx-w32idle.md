# fx-w32idle: w32srv idle CPU with GUI threads waiting for messages

- **Written:** 2026-10-29 02:47 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-w32idle`, branch `wt/fx-w32idle` from `wt/integ9`.
- **Observation:** found during fx-hlis5 on QEMU. With any Win32 window open and idle, w32srv uses about 10% of the CPU. Each GUI thread asks w32srv for messages every 250 ms, and after each reply w32srv keeps running for about 20 ms instead of blocking. On the Pentium III target this cost scales with the number of open windows.

## Scope

1. **Measure first.** In a host harness, show the two mechanisms with figures: number of requests per second and time w32srv spends runnable per idle GUI thread. Find in `apps/w32srv/` and `sdk/win32/user32/` where the 250 ms poll and the 20 ms post-reply spin come from.
2. **Research and record the Windows model** in the user32 design document, with links:
   - `GetMessage` and `WaitMessage` block the thread until input, a posted or sent message, a timer or a paint arrives;
   - `MsgWaitForMultipleObjects` adds kernel objects to the same wait;
   - `WM_TIMER` and `WM_PAINT` are synthesized when the queue is empty, not polled.

   Use Microsoft Learn for `GetMessage`, `MsgWaitForMultipleObjects` and "About Messages and Message Queues", and Wine's `dlls/win32u/message.c` (wait on the server queue handle) as the reference implementation.
3. **Implement the blocking model:**
   - an idle GUI thread waits on a kernel object or channel that w32srv signals when its queue gains input, a posted or sent message, a due timer or a paint. No periodic poll.
   - w32srv blocks when it has no work. No spin or busy wait after replying.

   Timer accuracy and the order of message delivery stay exactly as they are; the existing USER queue tests must pass unchanged.
4. **Acceptance in the host harness:**
   - an idle thread with an open window causes zero requests per second;
   - w32srv shows no runnable time while idle;
   - timers (`SetTimer` at 10, 55 and 1000 ms) and `PostMessage`/`SendMessage` from another thread wake the waiter within one scheduler tick.
5. **Tests:** run the win32 suite (`python3 -B -m unittest discover -s tests/host/win32 -t .`) and the USER queue tests, sequentially, with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- New syscalls, channel operations or abi.h records get their census and abi_layout rows (K8-6).
- Write the report to `build/codex/fx-w32idle-report.md`.
