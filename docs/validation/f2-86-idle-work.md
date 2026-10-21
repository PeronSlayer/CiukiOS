# f2-86 timer invalidation and idle service

Research checked on 2026-10-20 before implementation and rechecked on
2026-10-21 when resuming the interrupted WIP:

- [Microsoft WM_TIMER](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer)
  describes timer notification/callback delivery, with low-priority queue semantics.
- [Microsoft InvalidateRect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-invalidaterect)
  separately describes accumulating changed rectangles for later painting.
- [POSIX nanosleep](https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html)
  specifies an interruptible relative wait; scheduling and clock resolution can
  extend it. A requested sleep is therefore a polling bound, not hardware latency
  evidence. The page was available through search; direct retrieval returned 403.

The directive and `build/idle-cpu-review.md` name no external primary sources.
These references support separating notification from invalidation, rather than
claiming that CiukiOS implements the Windows message queue. In this tree,
`du_emit` invalidates even empty timer callbacks; the client already gates rendering
on dirty state and suppresses unchanged surface damage. Preserve those gates,
leave timer dirty state untouched, and let callbacks describe changed content.
Files' scan result is consumed on the UI thread; arm its 20 ms poll only while a
request is outstanding. Build dialog Places into scratch storage and compare
visible entries before publishing and invalidating its pane.

The desktop has no timed input read/poll ABI. Use a 10 ms service wait when idle,
shortened for queued input and real frame/animation/heartbeat/shell deadlines.
A stationary held button or drag, and input already consumed in the current
turn, do not create a deadline or justify continuous 1 ms polling. Keep input
drains before IPC and before rendering, and the existing input-lag telemetry.
During shutdown, ordinary shell ticks and child-close timeouts are suspended;
ignore their stale deadlines and use the active shutdown phase's close/retry
deadline. The static Busy panel otherwise keeps the 10 ms idle cadence.
Host tests check policy and production-loop fake time;
physical CPU/latency validation remains with the lead. See
`build/codex/f2-86-report.md` for commands, counts and limitations.
