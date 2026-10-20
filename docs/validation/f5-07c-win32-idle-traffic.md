# Win32 service idle traffic (f5-07C)

## Sources and implementation decisions

The f5-07C directive names the local f5-07A/B implementation and review,
but gives no external primary-source URLs. The following primary sources
were checked for the behavior affected by this change:

- Microsoft [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession)
  and [WM_ENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession):
  the query response determines whether the session ends; ENDSESSION carries
  that outcome. USER still calls the owning window procedure and replies to
  the existing coordinator. Only polling cadence and buffer storage change.
  These current API references are not evidence for modern shutdown UI or
  timeout rules on Windows 98; the directive's existing 10 s policy remains.
- Wine 10.0 [ntdll Unix server transport](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/ntdll/unix/server.c),
  `server_init_process`, `init_thread_pipe`, and `server_init_thread`: connection
  and thread initialization are separate from ordinary requests. CiukiOS
  retains its own protocol and process-wide FastPebLock. CLIENT is announced
  after connection establishment, rather than before each registry/USER RPC.
  Wine is a transport comparison, not a claim about Windows 9x internals.
- GNU libc [pthread_mutex_trylock implementation](https://raw.githubusercontent.com/bminor/glibc/master/nptl/pthread_mutex_trylock.c):
  contention returns EBUSY. The desktop uses trylock for both launch and idle
  result handoff; the worker releases the mailbox before entering RPC.

Repository checks: `src/kernel/proc/win32/service.c` closes a client's old
endpoint on server exit and CONNECT may reuse its numeric descriptor.
`sdk/win32/ntdll/server.c` therefore retains the established connection until
a transport failure, rather than treating an unchanged FD number as a server
generation. It holds a private F_DUPFD_CLOEXEC reference using the existing
`desktop_fcntl` implementation in `src/kernel/proc/surface.c`. Kernel cleanup
can reuse the original tracked FD without redirecting the private reference;
its channel instead reports the closed peer. That reference is closed on
invalidation. A failed first SEND can reconnect and retry safely; a partially
sent request or lost reply invalidates the connection without replaying the
operation. The next call reconnects and announces. The whole handshake and
BEGIN/WRITE/READ sequence remain under FastPebLock. There is no kernel ABI
change and no retry based on ambiguous registry errors such as invalid handle.

`sdk/win32/user32/session.c` uses a 42-byte request and 40-byte reply on its
stack, with no per-poll allocation. Each thread sleeps 250 ms between idle
polls. A delivered notification allows the next pump to poll immediately.
An empty poll keeps the fast phase until an empty result at least 20 ms after
the last notification; other polling during that phase is at most 20 ms apart.
Tick comparisons use signed differences for DWORD wraparound. Reentrant pumps
remain guarded and the caller's last error is restored.

`apps/desktop/win32_native.c` posts at most one request to one persistent
condition-variable worker. The desktop schedules every 500 ms when an HWND
identity or pending/in-progress launch exists, otherwise every 5 s. The first
poll includes the boot pending-work check. Results and restart UI are applied
only by the desktop; a service-exit epoch discards obsolete results and rearms
the boot check. An active restart retains its existing desktop-thread stages.
No worker accesses the desktop's client list or changes restart state.

`apps/w32srv/directory.c` partitions stale process-local atoms and windows in
memory and saves once if anything was removed. Global atoms, next-atom and
durable reservation counters survive. Live windowless clients are reconstructed
from their surviving marker without saving or incrementing its reference count.
Fresh-client reset followed by reconcile still produces just one save. A failed
startup commit returns its error before endpoint registration and retains all
objects for cleanup/retry. Per-process runtime exit behavior is unchanged.

## Focused host evidence

The fixtures cover N registry requests with one CLIENT; service destroy/reload
and numeric FD reuse; refusal and lost-reply behavior; eight windows on one
USER thread (accept, last-window veto and force); 250 ms idle pacing and heap
allocation counts; a 2 s blocked native exchange with prompt desktop frames;
500 ms / 5 s scheduling; and 50 stale owners with one write, one marker patch
and two syncs, including all four commit failure stages.

Exact command results and environmental limitations are recorded in
`build/codex/f5-07c-report.md`. No image, QEMU, hardware, or full-suite validation
is claimed here; integration and hardware evidence belong to the lead.
