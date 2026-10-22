# Native application event waits

The Windows 98 Resource Kit, chapter 28, `wrkc28_4.htm` (Process Scheduling
and Multitasking) describes preemptive Win32 threads and separate background
work in shell folders. `wrkc28_8.htm` (User) describes asynchronous input
delivery to thread message queues. Both were read from the owner's extracted
book in the main checkout's `build/rskt`, without copying its text or media.

Microsoft's primary [wait-functions reference](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions)
specifies waiting for synchronization objects, input and a timeout. Its
[message-loop reference](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-messages-and-message-queues)
shows retrieval and dispatch of queued messages. These support an application
event contract; no NT kernel implementation is inferred for Windows 98.
The Microsoft DDK-era [VxD thread discussion](https://techshelps.github.io/MSDN/TECHART/html/msdn_chicvxd.htm)
provides the historical preemptive-thread context. Upstream
[libnsfb's event interface](https://raw.githubusercontent.com/netsurf-browser/libnsfb/master/include/libnsfb_event.h)
defines zero as nonblocking and minus one as an infinite wait; preserve that
contract through NetSurf's scheduler and surface rather than clamping it.
The [POSIX poll specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/poll.html)
was requested but returned HTTP 403. The native wait and registration/recheck
contract already qualified in f2-88c is retained.

Repository comparison: libdeskui's loop still sleeps every 1–10 ms, private
jobs publish behind trylocks without a wakeup, Files uses completion timers,
Music has a separate 10 ms loop, and NetSurf caps its scheduled wait at 10 ms.
The desktop similarly caps its own service loop. A timeout change alone would
strand worker results.

Implementation decision: publish private results before a pollable completion
notification; consume notifications before servicing result predicates. Poll
input and output backpressure alongside those notifications and the nearest
actual deadline. Shared notification transport serializes send/close and
treats a full queue as already signaled; unexpected send failure closes the
peer to wake the consumer. Owners join workers before destroying transport.
Retain feature deadlines (clocks, process sampling, playback, device snapshots,
animations, protocol liveness); none is a generic completion retry interval.

NetSurf advances fetchers only in its existing main-turn pump before
`schedule_run()`. Its blocking surface wait collects socket readiness without
running fetch callbacks; otherwise a callback could schedule immediate work
behind an already-computed infinite timeout. DNS completion joins the same
wait set, and request/keepalive expiry supplies genuine network deadlines.
Partial TLS ciphertext waits for socket readiness; only buffered plaintext
can request an immediate continuation.

The native `channel_recv()` implementation in `src/kernel/proc/channel.c`
returns 1 for a delivered packet and 0 for EOF. Final inspection found that
Control Panel's decoder and completion drains, plus their host transports,
incorrectly used zero-success semantics. Correct both consumers and fixtures;
shutdown tokens must also be recognized on a positive receive. This fixes
native adoption and decoder shutdown without altering the ABI or adding a
retry path.

Ordinary Demo no longer manufactures self-PING traffic while idle and blocks
on its channel; its explicit crash-isolation workload retains its controller
cadence. Static DOS error windows likewise block on input instead of sleeping
every 50 ms. Active DOS presentation continues using its existing producer and
VGA-deadline wait; its startup/key pacing is unchanged.

Host evidence, exact commands, scope audit and remaining lead validation are
recorded in `build/codex/f2-98c-report.md`.
